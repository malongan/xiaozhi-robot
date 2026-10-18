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
    kStop = 6,
    kRewind = 7,
    kFastForward = 8,
};

struct MediaBlePeer {
    uint8_t type;  // 0 = public, 1 = random
    uint8_t addr[6];
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

    bool IsConnected() const { return connected_; }

    // Bonded peers, newest first.
    int GetBondedPeers(MediaBlePeer* peers, int max_peers);
    bool Unpair(const MediaBlePeer& peer);
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
    uint16_t report_val_handle_ = 0;
    uint16_t subscribed_handle_ = 0;
    bool subscribed_notify_ = false;
    StatusCallback status_callback_;
};

#endif  // MEDIA_BLE_H
