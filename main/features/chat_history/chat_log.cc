#include "chat_log.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* TAG = "ChatLog";
constexpr const char* kRoot = "/sdcard/chatlog";
constexpr uint8_t kMagic = 0xC7;  // 记录头校验字节

uint32_t NowSec() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

// 正文按字节截断，但不能把一个 UTF-8 多字节字符切一半。
size_t Utf8SafeTruncate(const std::string& text, size_t limit) {
    if (text.size() <= limit) {
        return text.size();
    }
    size_t cut = limit;
    // 回退到字符边界：UTF-8 续字节形如 10xxxxxx
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

bool ParseDayFile(const char* name, uint32_t* day) {
    // 期望 yyyymmdd.log
    size_t len = strlen(name);
    if (len != 12 || strcmp(name + 8, ".log") != 0) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    *day = static_cast<uint32_t>(strtoul(std::string(name, 8).c_str(), nullptr, 10));
    return true;
}
}  // namespace

ChatLog& ChatLog::GetInstance() {
    static ChatLog instance;
    return instance;
}

bool ChatLog::EnsureDir() {
    if (mkdir(kRoot, 0775) == 0) {
        return true;
    }
    // 已存在也算成功
    return errno == EEXIST;
}

std::string ChatLog::DayPath(uint32_t day) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%08u.log", kRoot, (unsigned)day);
    return std::string(path);
}

uint32_t ChatLog::TodayKey() {
    // 设备没有 RTC 时用开机秒数兜底，保证文件仍能按天切分（虽不准确）。
    // 有 SNTP 的情况下 Application 会把时间同步给 build 时间戳，这里优先用系统时间。
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {  // 2000 年之后
        return static_cast<uint32_t>((tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 +
                                     tm_now.tm_mday);
    }
    // 没有有效时间：用开机天数 + 19700101 偏移，避免所有记录挤在一个文件里
    return 19700101u + static_cast<uint32_t>(NowSec() / 86400u);
}

uint32_t ChatLog::TsToDay(uint32_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm tm_v = {};
    localtime_r(&t, &tm_v);
    if (tm_v.tm_year > 100) {
        return static_cast<uint32_t>((tm_v.tm_year + 1900) * 10000 + (tm_v.tm_mon + 1) * 100 +
                                     tm_v.tm_mday);
    }
    return TodayKey();
}

bool ChatLog::Init() {
    ready_ = false;
    pending_.clear();
    total_count_ = 0;
    cur_day_ = 0;
    day_bytes_ = 0;
    next_seq_ = 1;

    if (!EnsureDir()) {
        ESP_LOGE(TAG, "创建目录 %s 失败 errno=%d", kRoot, errno);
        return false;
    }
    PruneOldFiles();

    // 以最新一天为当前追加文件
    const int days = DayCount();
    if (days > 0) {
        cur_day_ = DayAt(0);
        struct stat st = {};
        if (stat(DayPath(cur_day_).c_str(), &st) == 0) {
            day_bytes_ = static_cast<uint32_t>(st.st_size);
        }
        std::vector<Entry> entries;
        if (LoadDay(cur_day_, entries) > 0) {
            next_seq_ = entries.back().seq + 1;
            total_count_ = static_cast<int>(entries.size());
        }
    } else {
        cur_day_ = TodayKey();
    }
    last_flush_sec_ = NowSec();
    ready_ = true;
    ESP_LOGW(TAG, "聊天记录就绪：%d 天，最新 %08u.log，%d 条", days, (unsigned)cur_day_,
             total_count_);
    return true;
}

void ChatLog::PruneOldFiles() {
    std::vector<uint32_t> days;
    DIR* dir = opendir(kRoot);
    if (dir == nullptr) {
        return;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        uint32_t day = 0;
        if (ParseDayFile(ent->d_name, &day)) {
            days.push_back(day);
        }
    }
    closedir(dir);
    if (static_cast<int>(days.size()) <= kKeepDays) {
        return;
    }
    std::sort(days.begin(), days.end(), std::greater<uint32_t>());
    for (size_t i = kKeepDays; i < days.size(); ++i) {
        const std::string path = DayPath(days[i]);
        if (unlink(path.c_str()) == 0) {
            ESP_LOGI(TAG, "清理过期记录 %s", path.c_str());
        }
    }
}

bool ChatLog::Append(const char* role, const char* content) {
    if (!ready_ || content == nullptr || content[0] == '\0') {
        return false;
    }
    uint8_t role_id = 1;
    if (strcmp(role, "user") == 0) {
        role_id = 0;
    } else if (strcmp(role, "system") == 0) {
        role_id = 2;
    }

    std::string text(content);
    const size_t keep = Utf8SafeTruncate(text, kMaxTextBytes);
    text.resize(keep);

    Pending item;
    item.ts = static_cast<uint32_t>(time(nullptr));
    item.role = role_id;
    item.text = std::move(text);
    pending_.push_back(std::move(item));

    // 攒批刷盘：够条数或够久才写卡
    const uint32_t now = NowSec();
    if (static_cast<int>(pending_.size()) >= kFlushCount ||
        now - last_flush_sec_ >= kFlushIntervalSec) {
        Flush();
    }
    return true;
}

void ChatLog::Flush() {
    if (!ready_ || pending_.empty()) {
        last_flush_sec_ = NowSec();
        return;
    }
    const uint32_t day = TodayKey();
    if (day != cur_day_) {
        // 跨天：新开文件
        cur_day_ = day;
        day_bytes_ = 0;
    }
    if (day_bytes_ >= kMaxDayBytes) {
        ESP_LOGW(TAG, "当日记录超过上限，丢弃最早的部分");
        // 简单策略：整天的文件轮转到一个临时名后删除，保证不无限增长
        const std::string path = DayPath(cur_day_);
        unlink(path.c_str());
        day_bytes_ = 0;
    }
    const int flushed = static_cast<int>(pending_.size());
    if (AppendToFile(pending_.data(), flushed, day)) {
        total_count_ += flushed;
        ESP_LOGW(TAG, "已写入 %d 条记录，累计 %d 条（%08u.log %u B）", flushed, total_count_,
                 (unsigned)day, (unsigned)day_bytes_);
    }
    pending_.clear();
    last_flush_sec_ = NowSec();
}

bool ChatLog::AppendToFile(const Pending* batch, int count, uint32_t day) {
    const std::string path = DayPath(day);
    FILE* f = fopen(path.c_str(), "ab");
    if (f == nullptr) {
        ESP_LOGW(TAG, "打开 %s 失败 errno=%d", path.c_str(), errno);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const Pending& item = batch[i];
        uint8_t head[kHeaderBytes] = {};
        const uint32_t seq = next_seq_++;
        const uint16_t len = static_cast<uint16_t>(item.text.size());
        memcpy(head + 0, &seq, 4);
        memcpy(head + 4, &item.ts, 4);
        head[8] = item.role;
        head[9] = 0;  // flags
        head[10] = static_cast<uint8_t>(len & 0xFF);
        head[11] = static_cast<uint8_t>((len >> 8) & 0xFF);
        head[12] = kMagic;
        head[13] = 0;
        head[14] = 0;
        head[15] = 0;
        uint16_t crc = 0xFFFF;
        for (size_t k = 0; k < item.text.size(); ++k) {
            crc ^= static_cast<uint8_t>(item.text[k]);
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : (crc >> 1);
            }
        }
        head[16] = static_cast<uint8_t>(crc & 0xFF);
        head[17] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        if (fwrite(head, 1, kHeaderBytes, f) != kHeaderBytes) {
            ok = false;
            break;
        }
        if (len > 0 && fwrite(item.text.data(), 1, len, f) != len) {
            ok = false;
            break;
        }
        day_bytes_ += static_cast<uint32_t>(kHeaderBytes + len);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok;
}

int ChatLog::DayCount() const {
    int n = 0;
    DIR* dir = opendir(kRoot);
    if (dir == nullptr) {
        return 0;
    }
    struct dirent* ent = nullptr;
    uint32_t day = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (ParseDayFile(ent->d_name, &day)) {
            ++n;
        }
    }
    closedir(dir);
    return n;
}

uint32_t ChatLog::DayAt(int index) const {
    std::vector<uint32_t> days;
    DIR* dir = opendir(kRoot);
    if (dir == nullptr) {
        return 0;
    }
    struct dirent* ent = nullptr;
    uint32_t day = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (ParseDayFile(ent->d_name, &day)) {
            days.push_back(day);
        }
    }
    closedir(dir);
    std::sort(days.begin(), days.end(), std::greater<uint32_t>());
    if (index < 0 || index >= static_cast<int>(days.size())) {
        return 0;
    }
    return days[index];
}

int ChatLog::LoadDay(uint32_t day, std::vector<Entry>& out) const {
    out.clear();
    const std::string path = DayPath(day);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return -1;
    }
    for (;;) {
        uint8_t head[kHeaderBytes];
        if (fread(head, 1, kHeaderBytes, f) != kHeaderBytes) {
            break;
        }
        if (head[12] != kMagic) {
            ESP_LOGW(TAG, "记录头损坏，停止读取");
            break;
        }
        Entry e;
        memcpy(&e.seq, head + 0, 4);
        memcpy(&e.ts, head + 4, 4);
        e.role = head[8];
        e.flags = head[9];
        const uint16_t len = static_cast<uint16_t>(head[10] | (head[11] << 8));
        if (len > kMaxTextBytes) {
            ESP_LOGW(TAG, "记录长度异常 %u，停止读取", (unsigned)len);
            break;
        }
        std::string text(len, '\0');
        if (len > 0 && fread(&text[0], 1, len, f) != len) {
            break;  // 尾部被截断（断电），保留已读到的
        }
        e.text = std::move(text);
        out.push_back(std::move(e));
    }
    fclose(f);
    return static_cast<int>(out.size());
}

int ChatLog::LoadLatestDay(std::vector<Entry>& out) const {
    const uint32_t day = DayAt(0);
    if (day == 0) {
        out.clear();
        return -1;
    }
    return LoadDay(day, out);
}

void ChatLog::PrintStats() const {
    ESP_LOGI(TAG, "聊天记录统计：%d 天，%d 条，待刷盘 %u 条", DayCount(), total_count_,
             (unsigned)pending_.size());
}
