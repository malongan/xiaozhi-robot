#include "web_file_server.h"

// Weak symbol so builds without the embedded page still link (defensive only;
// the Kconfig option and the CMake rule are enabled together).
#if CONFIG_WEB_FILE_SERVER

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <lwip/sockets.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#include "wifi_manager.h"

namespace {

constexpr const char* TAG = "WebFileServer";
constexpr const char* kSdRoot = "/sdcard";
constexpr size_t kIoChunk = 16 * 1024;
constexpr size_t kMaxPathLen = 384;
constexpr size_t kMaxBodyLen = 2048;
constexpr int kMaxListEntries = 512;
constexpr int kMaxRemoveDepth = 8;
constexpr int kMaxRemoveEntries = 5000;
constexpr int64_t kProgressIntervalUs = 300 * 1000;
constexpr uint64_t kUploadSlack = 64 * 1024;
constexpr const char* kPin = CONFIG_WEB_FILE_SERVER_PIN;

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

std::string FsPath(const std::string& canon) {
    return canon == "/" ? std::string(kSdRoot) : std::string(kSdRoot) + canon;
}

std::string BaseName(const std::string& canon) {
    const size_t slash = canon.find_last_of('/');
    return slash == std::string::npos ? canon : canon.substr(slash + 1);
}

std::string ParentOf(const std::string& canon) {
    const size_t slash = canon.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return canon.substr(0, slash);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool PercentDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false;
        }
        const int hi = HexValue(in[i + 1]);
        const int lo = HexValue(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

// Canonical SD-root-relative path: always starts with '/', no "..", no drive
// letters and no backslashes.  "/" is the SD root itself.
bool NormalizePath(const std::string& raw, std::string& canon) {
    std::string decoded;
    if (!PercentDecode(raw, decoded) || decoded.empty() || decoded[0] != '/') {
        return false;
    }
    if (decoded.size() > kMaxPathLen) {
        return false;
    }
    if (decoded.find('\\') != std::string::npos || decoded.find(':') != std::string::npos) {
        return false;
    }
    std::string out;
    size_t i = 0;
    while (i <= decoded.size()) {
        size_t end = decoded.find('/', i);
        if (end == std::string::npos) {
            end = decoded.size();
        }
        const std::string seg = decoded.substr(i, end - i);
        if (seg == "..") {
            return false;
        }
        if (!seg.empty() && seg != ".") {
            out += '/';
            out += seg;
            if (out.size() > kMaxPathLen) {
                return false;
            }
        }
        if (end == decoded.size()) {
            break;
        }
        i = end + 1;
    }
    canon = out.empty() ? "/" : out;
    return true;
}

bool CheckAuth(httpd_req_t* req) {
    if (kPin[0] == '\0') {
        return true;
    }
    char value[128];
    char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", value, sizeof(value)) == ESP_OK &&
        strcmp(value, kPin) == 0) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", value, sizeof(value)) == ESP_OK &&
        strcmp(value, kPin) == 0) {
        return true;
    }
    return false;
}

bool AlsoRequireAuth(httpd_req_t* req) {
    // Small JSON responses would otherwise wait for Nagle/delayed-ACK, which
    // is very visible over a high-latency link.
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (CheckAuth(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return false;
}

void SendError(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    cJSON* root = cJSON_CreateObject();
    if (root != nullptr) {
        cJSON_AddStringToObject(root, "error", message);
        char* text = cJSON_PrintUnformatted(root);
        if (text != nullptr) {
            httpd_resp_sendstr(req, text);
            cJSON_free(text);
        } else {
            httpd_resp_sendstr(req, "{\"error\":\"internal\"}");
        }
        cJSON_Delete(root);
    } else {
        httpd_resp_sendstr(req, "{\"error\":\"internal\"}");
    }
}

void SendJson(httpd_req_t* req, cJSON* root, const char* status) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        SendError(req, "500 Internal Server Error", "encode failed");
        return;
    }
    httpd_resp_sendstr(req, text);
    cJSON_free(text);
}

bool GetPathParam(httpd_req_t* req, std::string& canon, bool required) {
    char query[512];
    char value[kMaxPathLen * 3 + 4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "path", value, sizeof(value)) == ESP_OK) {
        return NormalizePath(value, canon);
    }
    if (required) {
        return false;
    }
    canon = "/";
    return true;
}

bool SdStats(uint64_t& free_bytes, uint64_t& total_bytes) {
    free_bytes = 0;
    total_bytes = 0;
    // FATFS VFS does not implement statvfs(); esp_vfs_fat_info() is the
    // supported way to query total/free bytes.
    if (esp_vfs_fat_info(kSdRoot, &total_bytes, &free_bytes) != ESP_OK) {
        return false;
    }
    return total_bytes > 0;
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len == 0 || req->content_len > kMaxBodyLen) {
        return false;
    }
    body.assign(req->content_len, '\0');
    size_t got = 0;
    int timeouts = 0;
    while (got < body.size()) {
        const int ret = httpd_req_recv(req, &body[got], body.size() - got);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return false;
            }
            continue;
        }
        if (ret <= 0) {
            return false;
        }
        timeouts = 0;
        got += static_cast<size_t>(ret);
    }
    return true;
}

std::string MakeUniquePath(const std::string& canon) {
    struct stat info = {};
    if (stat(FsPath(canon).c_str(), &info) != 0) {
        return canon;
    }
    const size_t slash = canon.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string("/") : canon.substr(0, slash + 1);
    const std::string name = BaseName(canon);
    const size_t dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    for (int i = 1; i < 200; ++i) {
        const std::string candidate = dir + stem + "." + std::to_string(i) + ext;
        if (stat(FsPath(candidate).c_str(), &info) != 0) {
            return candidate;
        }
    }
    return canon;
}

void AppendRfc5987(std::string& out, const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    for (const unsigned char c : text) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
}

std::string ContentDisposition(const std::string& name) {
    std::string fallback;
    for (const unsigned char c : name) {
        fallback.push_back(c >= 0x20 && c < 0x7F && c != '"' && c != '\\' ? static_cast<char>(c) : '_');
    }
    std::string header = "attachment; filename=\"" + fallback + "\"; filename*=UTF-8''";
    AppendRfc5987(header, name);
    return header;
}

bool HasEntries(const std::string& fs_path) {
    DIR* dir = opendir(fs_path.c_str());
    if (dir == nullptr) {
        return true;
    }
    bool found = false;
    while (struct dirent* ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

// Returns 0 on success, otherwise an HTTP status code.
int RemovePath(const std::string& canon, bool recursive, int depth, int& budget) {
    if (canon == "/") {
        return 400;
    }
    if (depth > kMaxRemoveDepth) {
        return 400;
    }
    if (--budget < 0) {
        return 400;
    }
    const std::string fs_path = FsPath(canon);
    struct stat info = {};
    if (stat(fs_path.c_str(), &info) != 0) {
        return 404;
    }
    if (!S_ISDIR(info.st_mode)) {
        return unlink(fs_path.c_str()) == 0 ? 0 : 500;
    }
    if (!recursive) {
        if (HasEntries(fs_path)) {
            return 409;
        }
        return rmdir(fs_path.c_str()) == 0 ? 0 : 500;
    }
    std::vector<std::string> children;
    if (DIR* dir = opendir(fs_path.c_str())) {
        while (struct dirent* ent = readdir(dir)) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                children.emplace_back(canon == "/" ? std::string("/") + ent->d_name
                                                   : canon + "/" + ent->d_name);
            }
        }
        closedir(dir);
    } else {
        return 500;
    }
    for (const auto& child : children) {
        const int ret = RemovePath(child, true, depth + 1, budget);
        if (ret != 0) {
            return ret;
        }
    }
    return rmdir(fs_path.c_str()) == 0 ? 0 : 500;
}

struct ListEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

}  // namespace

// Handlers reach the singleton's callbacks through this friend.
class WebFileServerAccess {
public:
    static esp_err_t HandleIndex(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        const size_t len = static_cast<size_t>(index_html_gz_end - index_html_gz_start);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        return httpd_resp_send(req, reinterpret_cast<const char*>(index_html_gz_start),
                                static_cast<ssize_t>(len));
    }

    static esp_err_t HandleInfo(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            SendError(req, "500 Internal Server Error", "out of memory");
            return ESP_OK;
        }
        cJSON_AddStringToObject(root, "name", BOARD_TYPE);
        cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
        cJSON_AddBoolToObject(root, "running", WebFileServer::GetInstance().IsRunning());
        cJSON_AddStringToObject(root, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
        uint64_t free_bytes = 0;
        uint64_t total_bytes = 0;
        const bool mounted = SdStats(free_bytes, total_bytes);
        cJSON* sd = cJSON_AddObjectToObject(root, "sd");
        if (sd != nullptr) {
            cJSON_AddBoolToObject(sd, "mounted", mounted);
            cJSON_AddNumberToObject(sd, "free", static_cast<double>(free_bytes));
            cJSON_AddNumberToObject(sd, "total", static_cast<double>(total_bytes));
        }
        SendJson(req, root, "200 OK");
        cJSON_Delete(root);
        return ESP_OK;
    }

    static esp_err_t HandleList(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        std::string canon;
        if (!GetPathParam(req, canon, false)) {
            SendError(req, "400 Bad Request", "invalid path");
            return ESP_OK;
        }
        const std::string fs_path = FsPath(canon);
        DIR* dir = opendir(fs_path.c_str());
        if (dir == nullptr) {
            const int err = errno;
            if (err == ENOENT || err == ENOTDIR) {
                SendError(req, "404 Not Found", err == ENOTDIR ? "not a directory" : "not found");
            } else {
                SendError(req, "503 Service Unavailable", "SD card unavailable");
            }
            return ESP_OK;
        }
        std::vector<ListEntry> entries;
        while (struct dirent* ent = readdir(dir)) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (static_cast<int>(entries.size()) >= kMaxListEntries) {
                break;
            }
            ListEntry item;
            item.name = ent->d_name;
            struct stat info = {};
            const std::string child = fs_path == "/" ? fs_path + item.name : fs_path + "/" + item.name;
            if (stat(child.c_str(), &info) != 0) {
                continue;
            }
            item.is_dir = S_ISDIR(info.st_mode);
            item.size = static_cast<uint64_t>(info.st_size);
            item.mtime = static_cast<int64_t>(info.st_mtime);
            entries.push_back(std::move(item));
        }
        closedir(dir);
        std::sort(entries.begin(), entries.end(), [](const ListEntry& a, const ListEntry& b) {
            if (a.is_dir != b.is_dir) {
                return a.is_dir;
            }
            return a.name < b.name;
        });
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            SendError(req, "500 Internal Server Error", "out of memory");
            return ESP_OK;
        }
        cJSON_AddStringToObject(root, "path", canon.c_str());
        cJSON* list = cJSON_AddArrayToObject(root, "entries");
        if (list != nullptr) {
            for (const auto& item : entries) {
                cJSON* node = cJSON_CreateObject();
                if (node == nullptr) {
                    break;
                }
                cJSON_AddStringToObject(node, "name", item.name.c_str());
                cJSON_AddStringToObject(node, "type", item.is_dir ? "dir" : "file");
                cJSON_AddNumberToObject(node, "size", static_cast<double>(item.size));
                cJSON_AddNumberToObject(node, "mtime", static_cast<double>(item.mtime));
                cJSON_AddItemToArray(list, node);
            }
        }
        SendJson(req, root, "200 OK");
        cJSON_Delete(root);
        return ESP_OK;
    }

    static esp_err_t HandleFileGet(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        std::string canon;
        if (!GetPathParam(req, canon, true) || canon == "/") {
            SendError(req, "400 Bad Request", "invalid path");
            return ESP_OK;
        }
        const std::string fs_path = FsPath(canon);
        struct stat info = {};
        if (stat(fs_path.c_str(), &info) != 0) {
            SendError(req, "404 Not Found", "not found");
            return ESP_OK;
        }
        if (!S_ISREG(info.st_mode)) {
            SendError(req, "400 Bad Request", "not a file");
            return ESP_OK;
        }
        FILE* file = fopen(fs_path.c_str(), "rb");
        if (file == nullptr) {
            SendError(req, "500 Internal Server Error", "open failed");
            return ESP_OK;
        }
        const std::string disposition = ContentDisposition(BaseName(canon));
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        std::vector<char> buffer(kIoChunk);
        esp_err_t result = ESP_OK;
        while (true) {
            const size_t read = fread(buffer.data(), 1, buffer.size(), file);
            if (read == 0) {
                break;
            }
            if (httpd_resp_send_chunk(req, buffer.data(), read) != ESP_OK) {
                result = ESP_FAIL;
                break;
            }
        }
        fclose(file);
        if (result == ESP_OK) {
            httpd_resp_send_chunk(req, nullptr, 0);
        }
        return ESP_OK;
    }

    static esp_err_t HandleFilePut(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        std::string canon;
        if (!GetPathParam(req, canon, true) || canon == "/") {
            SendError(req, "400 Bad Request", "invalid path");
            return ESP_OK;
        }
        const size_t total = req->content_len;
        if (total == 0) {
            SendError(req, "411 Length Required", "content-length required");
            return ESP_OK;
        }
        // Reject when the file cannot fit; keep a little slack for FAT metadata.
        uint64_t free_bytes = 0;
        uint64_t total_bytes = 0;
        if (!SdStats(free_bytes, total_bytes)) {
            SendError(req, "503 Service Unavailable", "SD card unavailable");
            return ESP_OK;
        }
        if (free_bytes < static_cast<uint64_t>(total) + kUploadSlack) {
            SendError(req, "507 Insufficient Storage", "not enough space");
            return ESP_OK;
        }
        const std::string parent = ParentOf(canon);
        struct stat info = {};
        if (stat(FsPath(parent).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            SendError(req, "404 Not Found", "parent directory missing");
            return ESP_OK;
        }
        const std::string target = MakeUniquePath(canon);
        const std::string fs_path = FsPath(target);
        FILE* file = fopen(fs_path.c_str(), "wb");
        if (file == nullptr) {
            SendError(req, "500 Internal Server Error", "create failed");
            return ESP_OK;
        }
        const std::string name = BaseName(target);
        auto& owner = WebFileServer::GetInstance();
        std::vector<char> buffer(kIoChunk);
        size_t received = 0;
        int timeouts = 0;
        int64_t last_report = 0;
        bool failed = false;
        while (received < total) {
            const size_t want = std::min(buffer.size(), total - received);
            const int ret = httpd_req_recv(req, buffer.data(), want);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > 5) {
                    failed = true;
                    break;
                }
                continue;
            }
            if (ret <= 0) {
                failed = true;
                break;
            }
            timeouts = 0;
            if (fwrite(buffer.data(), 1, static_cast<size_t>(ret), file) != static_cast<size_t>(ret)) {
                failed = true;
                break;
            }
            received += static_cast<size_t>(ret);
            const int64_t now = esp_timer_get_time();
            if (now - last_report >= kProgressIntervalUs) {
                last_report = now;
                owner.NotifyActivity(name, received, total, false, false);
            }
        }
        const bool flush_error = fflush(file) != 0;
        fclose(file);
        if (failed || flush_error || received != total) {
            unlink(fs_path.c_str());
            owner.NotifyActivity(name, received, total, true, true);
            SendError(req, "500 Internal Server Error", failed ? "transfer interrupted" : "short write");
            return ESP_OK;
        }
        owner.NotifyActivity(name, received, total, true, false);
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            SendError(req, "500 Internal Server Error", "out of memory");
            return ESP_OK;
        }
        cJSON_AddStringToObject(root, "path", target.c_str());
        cJSON_AddNumberToObject(root, "size", static_cast<double>(received));
        SendJson(req, root, "201 Created");
        cJSON_Delete(root);
        return ESP_OK;
    }

    static esp_err_t HandleMkdir(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        std::string body;
        if (!ReadBody(req, body)) {
            SendError(req, "400 Bad Request", "invalid body");
            return ESP_OK;
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            SendError(req, "400 Bad Request", "invalid json");
            return ESP_OK;
        }
        std::string canon;
        const cJSON* path = cJSON_GetObjectItem(root, "path");
        const bool valid = cJSON_IsString(path) && NormalizePath(path->valuestring, canon) && canon != "/";
        cJSON_Delete(root);
        if (!valid) {
            SendError(req, "400 Bad Request", "invalid path");
            return ESP_OK;
        }
        const std::string fs_path = FsPath(canon);
        if (mkdir(fs_path.c_str(), 0777) == 0) {
            cJSON* out = cJSON_CreateObject();
            if (out != nullptr) {
                cJSON_AddStringToObject(out, "path", canon.c_str());
                SendJson(req, out, "201 Created");
                cJSON_Delete(out);
            } else {
                SendError(req, "500 Internal Server Error", "out of memory");
            }
            return ESP_OK;
        }
        if (errno == EEXIST) {
            SendError(req, "409 Conflict", "already exists");
        } else {
            SendError(req, "500 Internal Server Error", "mkdir failed");
        }
        return ESP_OK;
    }

    static esp_err_t HandleRm(httpd_req_t* req) {
        if (!AlsoRequireAuth(req)) {
            return ESP_OK;
        }
        std::string body;
        if (!ReadBody(req, body)) {
            SendError(req, "400 Bad Request", "invalid body");
            return ESP_OK;
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            SendError(req, "400 Bad Request", "invalid json");
            return ESP_OK;
        }
        std::string canon;
        const cJSON* path = cJSON_GetObjectItem(root, "path");
        const cJSON* recursive = cJSON_GetObjectItem(root, "recursive");
        const bool valid = cJSON_IsString(path) && NormalizePath(path->valuestring, canon) && canon != "/";
        const bool deep = cJSON_IsTrue(recursive);
        cJSON_Delete(root);
        if (!valid) {
            SendError(req, "400 Bad Request", "invalid path");
            return ESP_OK;
        }
        int budget = kMaxRemoveEntries;
        const int status = RemovePath(canon, deep, 0, budget);
        if (status == 0) {
            cJSON* out = cJSON_CreateObject();
            if (out != nullptr) {
                cJSON_AddBoolToObject(out, "ok", true);
                SendJson(req, out, "200 OK");
                cJSON_Delete(out);
            } else {
                SendError(req, "500 Internal Server Error", "out of memory");
            }
            return ESP_OK;
        }
        switch (status) {
            case 404:
                SendError(req, "404 Not Found", "not found");
                break;
            case 409:
                SendError(req, "409 Conflict", "directory not empty");
                break;
            case 400:
                SendError(req, "400 Bad Request", "refused");
                break;
            default:
                SendError(req, "500 Internal Server Error", "delete failed");
                break;
        }
        return ESP_OK;
    }
};

WebFileServer& WebFileServer::GetInstance() {
    static WebFileServer instance;
    return instance;
}

bool WebFileServer::Start() {
    if (server_ != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_FILE_SERVER_PORT;
    config.ctrl_port = 32770;
    config.stack_size = 6144;
    config.max_open_sockets = 3;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.lru_purge_enable = true;

    httpd_handle_t handle = nullptr;
    const esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }
    static const httpd_uri_t kUris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = &WebFileServerAccess::HandleIndex, .user_ctx = nullptr},
        {.uri = "/api/info",
         .method = HTTP_GET,
         .handler = &WebFileServerAccess::HandleInfo,
         .user_ctx = nullptr},
        {.uri = "/api/list",
         .method = HTTP_GET,
         .handler = &WebFileServerAccess::HandleList,
         .user_ctx = nullptr},
        {.uri = "/api/file",
         .method = HTTP_GET,
         .handler = &WebFileServerAccess::HandleFileGet,
         .user_ctx = nullptr},
        {.uri = "/api/file",
         .method = HTTP_PUT,
         .handler = &WebFileServerAccess::HandleFilePut,
         .user_ctx = nullptr},
        {.uri = "/api/mkdir",
         .method = HTTP_POST,
         .handler = &WebFileServerAccess::HandleMkdir,
         .user_ctx = nullptr},
        {.uri = "/api/rm", .method = HTTP_POST, .handler = &WebFileServerAccess::HandleRm, .user_ctx = nullptr},
    };
    for (const auto& uri : kUris) {
        const esp_err_t reg = httpd_register_uri_handler(handle, &uri);
        if (reg != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed: %s", uri.uri, esp_err_to_name(reg));
        }
    }
    server_ = handle;
    ESP_LOGI(TAG, "serving %s on port %d", kSdRoot, CONFIG_WEB_FILE_SERVER_PORT);
    NotifyStatus();
    return true;
}

void WebFileServer::Stop() {
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(static_cast<httpd_handle_t>(server_));
    server_ = nullptr;
    ESP_LOGI(TAG, "stopped");
    NotifyStatus();
}

std::string WebFileServer::Url() const {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected()) {
        return {};
    }
    const std::string ip = wifi.GetIpAddress();
    if (ip.empty()) {
        return {};
    }
#if CONFIG_WEB_FILE_SERVER_PORT != 80
    return "http://" + ip + ":" + std::to_string(CONFIG_WEB_FILE_SERVER_PORT);
#else
    return "http://" + ip;
#endif
}


void WebFileServer::NotifyStatus() {
    if (status_callback_) {
        status_callback_(server_ != nullptr);
    }
}

void WebFileServer::NotifyActivity(const std::string& name, size_t sent, size_t total, bool done,
                                   bool error) {
    if (activity_callback_) {
        activity_callback_(name, sent, total, done, error);
    }
}

#else

WebFileServer& WebFileServer::GetInstance() {
    static WebFileServer instance;
    return instance;
}

bool WebFileServer::Start() { return false; }
void WebFileServer::Stop() {}
std::string WebFileServer::Url() const { return {}; }
void WebFileServer::NotifyStatus() {}
void WebFileServer::NotifyActivity(const std::string&, size_t, size_t, bool, bool) {}

#endif  // CONFIG_WEB_FILE_SERVER

