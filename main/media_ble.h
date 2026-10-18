#ifndef MEDIA_BLE_H
#define MEDIA_BLE_H

#include <cstdint>
#include <functional>

// Six consumer-control keys exposed over BLE HID.
enum class MediaBleKey : uint8_t {
    kNext = 0,
    kPrevious = 1,
    kPlayPause = 2,
    kMute = 3,
    kVolumeUp = 4,
    kVolumeDown = 5,
    kHome = 6,  // Android: Consumer AC Home
    kBack = 7,  // Android: Consumer AC Back
    kMenu = 8,  // Android: keyboard Application key
};

struct MediaBlePeer {
    uint8_t type;  // 0 = public, 1 = random
    uint8_t addr[6];
};

// How media keys are delivered: both reports, mixed report only (macOS),
// or standalone consumer report only (Android).
enum class MediaSendMode : uint8_t {
    kBoth = 0,
    kMixedOnly = 1,
    kConsumerOnly = 2,
};

class MediaBle {
public:
    using StatusCallback = std::function<void(bool connected)>;

    static MediaBle& GetInstance();

    // Brings up NimBLE, the HID service and advertising. Idempotent.
    void Start();
    void SetStatusCallback(StatusCallback callback);

    // Pulses one consumer-control bit (press, 20 ms, release).
    bool SendKey(MediaBleKey key);
    bool SendShortcut(uint8_t modifiers, uint8_t keycode);
    void SetSendMode(MediaSendMode mode) { send_mode_ = mode; }
    MediaSendMode GetSendMode() const { return send_mode_; }
    bool SendKeycode(uint8_t keycode, uint8_t modifiers = 0);

    bool IsConnected() const { return connected_; }

    // Bonded peers, newest first.
    int GetBondedPeers(MediaBlePeer* peers, int max_peers);
    bool Unpair(const MediaBlePeer& peer);
    int UnpairAll();
    bool GetConnectedPeer(MediaBlePeer* peer);
    bool Disconnect();

private:
    MediaBle() = default;
    MediaBle(const MediaBle&) = delete;
    MediaBle& operator=(const MediaBle&) = delete;

    void InitGattServices();
    void StartAdvertising();
    void OnConnect(uint16_t conn_handle);
    void OnDisconnect();

    static int GapEvent(struct ble_gap_event* event, void* arg);
    static void HostTask(void* param);
    static void OnSync();
    static void OnReset(int reason);

    bool started_ = false;
    volatile bool connected_ = false;
    uint16_t conn_handle_ = 0xFFFF;
    uint16_t report_val_handle_ = 0;    // keyboard input report
    uint16_t consumer_val_handle_ = 0;  // media/consumer input report
    uint16_t subscribed_handle_ = 0;
    bool subscribed_notify_ = false;
    StatusCallback status_callback_;
    MediaSendMode send_mode_ = MediaSendMode::kBoth;
};

#endif  // MEDIA_BLE_H
