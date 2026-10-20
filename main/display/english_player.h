#ifndef ENGLISH_PLAYER_H
#define ENGLISH_PLAYER_H

#include <cstdint>
#include <string>
#include <vector>

// 英语听写播放器：按固定间隔依次播放 TF 卡里的单词发音。
//
// 与服务器 TTS 的协调：
//   - 服务器语音优先：收到新的服务器音频时暂停本地播放，播完后继续
//   - 本地播放期间若设备进入对话状态，暂停并等空闲再续播
// 所有状态变更都通过 Application::Schedule() 在主任务里执行，避免跨任务竞争。

class EnglishPlayer {
public:
    static EnglishPlayer& GetInstance();

    // 开始报听写。unit=0 表示不限定单元；count<=0 表示到词库末尾。
    // interval 为词与词之间的等待秒数（不含发音本身时长）。
    bool Start(int unit, int count, int interval_seconds, bool show_word);

    void Stop();
    void Next();
    void Prev();
    void Repeat();

    bool Active() const { return active_; }
    bool Paused() const { return paused_; }
    int CurrentIndex() const { return current_; }
    // 当前是第几个词（1 起）
    int Position() const { return current_ - start_ + 1; }
    int Total() const { return total_; }
    int Interval() const { return interval_; }
    bool ShowWord() const { return show_word_; }
    // 距离下一个词还有几秒（-1 表示不在等待中）
    int Countdown() const;
    // 词库就绪且有词
    static bool Available();
    // 由主循环时钟事件驱动：推进到下一个词 / 结束听写
    void Tick();

private:
    void PlayCurrent();

    bool active_ = false;
    bool paused_ = false;
    bool show_word_ = false;
    int unit_ = 0;
    int count_ = 0;
    int interval_ = 10;
    int current_ = -1;
    int total_ = 0;
    int start_ = 0;
    int end_ = 0;
    int played_ = 0;
    uint32_t next_tick_ = 0;
    uint32_t tick_period_ = 0;
    std::vector<uint8_t> audio_;
};

#endif  // ENGLISH_PLAYER_H
