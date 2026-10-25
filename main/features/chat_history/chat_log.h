#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 对话记录存储：把每一轮对话追加写到 TF 卡，供离线回看。
//
// 文件布局（一天一个文件）：
//   /sdcard/chatlog/yyyymmdd.log
//   记录 = 24 字节定长头 + 变长 UTF-8 正文
//
// 定长头的意义：回看时可以用 fseek 直接跳到第 N 条，不必顺序解析；
// 追加写也只会动文件尾部，断电最多丢掉最后一条。
class ChatLog {
public:
    struct Entry {
        uint32_t seq = 0;
        uint32_t ts = 0;   // Unix 时间戳（秒），0 表示未知
        uint8_t role = 0;  // 0=用户 1=助手 2=系统
        uint8_t flags = 0;
        std::string text;
    };

    // 单条正文上限。超长回复在屏幕上也只显示前几行，截断能保证存储可控。
    static constexpr size_t kMaxTextBytes = 512;
    // 记录头长度。
    static constexpr size_t kHeaderBytes = 24;
    // 单日文件上限，超过后丢弃当天最早的记录。
    static constexpr uint32_t kMaxDayBytes = 2u * 1024u * 1024u;
    // 保留天数。
    static constexpr int kKeepDays = 7;
    // 攒批刷盘：攒够这么多条或超过这个间隔才真正落盘，减少 TF 卡写入。
    static constexpr int kFlushCount = 10;
    static constexpr uint32_t kFlushIntervalSec = 30;

    static ChatLog& GetInstance();

    // 挂载 SD 卡之后调用。会创建目录、清理过期文件。
    bool Init();

    bool Ready() const { return ready_; }

    // 追加一条。role 用 "user"/"assistant"/"system"。
    // 失败不抛出，返回 false，调用方不必处理：记录是尽力而为的能力。
    bool Append(const char* role, const char* content);

    // 把攒着的记录真正写到卡上。关机前、回看前、攒够条数时调用。
    void Flush();

    // 可回看的天数（新的在前）。
    int DayCount() const;
    // 第 index 天的日期（yyyymmdd），越大越新。
    uint32_t DayAt(int index) const;
    // 读取某天的全部记录。返回条数，失败返回 -1。
    int LoadDay(uint32_t day, std::vector<Entry>& out) const;
    // 最新一天的记录（没有记录时返回 -1）。
    int LoadLatestDay(std::vector<Entry>& out) const;

    // 统计：已存条数（不含未刷盘的）、占用字节
    int TotalCount() const { return total_count_; }
    void PrintStats() const;

private:
    ChatLog() = default;
    ChatLog(const ChatLog&) = delete;
    ChatLog& operator=(const ChatLog&) = delete;

    struct Pending {
        uint32_t ts;
        uint8_t role;
        std::string text;
    };

    bool EnsureDir();
    bool AppendToFile(const Pending* batch, int count, uint32_t day);
    void PruneOldFiles();
    static uint32_t TodayKey();
    static std::string DayPath(uint32_t day);
    static uint32_t TsToDay(uint32_t ts);

    bool ready_ = false;
    uint32_t cur_day_ = 0;    // 当前追加的文件日期
    uint32_t day_bytes_ = 0;  // 当前文件已有字节数
    uint32_t next_seq_ = 1;
    int total_count_ = 0;
    uint32_t last_flush_sec_ = 0;
    std::vector<Pending> pending_;
};
