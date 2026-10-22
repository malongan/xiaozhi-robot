#ifndef _WEB_FILE_SERVER_H_
#define _WEB_FILE_SERVER_H_

#include <cstddef>
#include <functional>
#include <string>

// Serves the SD card over HTTP to the local network: a tiny single page app
// plus a JSON / raw-byte API.  Deliberately independent from the display and
// network layers so a board can start and stop it purely from network events.
class WebFileServer {
public:
    using StatusCallback = std::function<void(bool running)>;
    // Upload activity.  `total` is 0 when the size is unknown; `done` and
    // `error` terminate a transfer.  Invoked from the HTTP task.
    using ActivityCallback =
        std::function<void(const std::string& name, size_t sent, size_t total, bool done, bool error)>;

    static WebFileServer& GetInstance();

    bool Start();
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }
    // "http://<sta-ip>" (no port) or an empty string while offline.
    std::string Url() const;

    void SetStatusCallback(StatusCallback callback) { status_callback_ = std::move(callback); }
    void SetActivityCallback(ActivityCallback callback) { activity_callback_ = std::move(callback); }

private:
    friend class WebFileServerAccess;  // HTTP handlers defined in the .cc

    WebFileServer() = default;
    WebFileServer(const WebFileServer&) = delete;
    WebFileServer& operator=(const WebFileServer&) = delete;

    void NotifyStatus();
    void NotifyActivity(const std::string& name, size_t sent, size_t total, bool done, bool error);

    void* server_ = nullptr;
    StatusCallback status_callback_;
    ActivityCallback activity_callback_;
};

#endif  // _WEB_FILE_SERVER_H_
