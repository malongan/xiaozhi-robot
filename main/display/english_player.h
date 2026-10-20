#ifndef ENGLISH_PLAYER_H
#define ENGLISH_PLAYER_H

#include <cstdint>
#include <string>
#include <vector>

// 英语学习播放器：按固定间隔依次播放 TF 卡里的单词发音。
//
// 两种模式：
//   听写模式（kDictation）— 播完一个词后等待 interval 秒再播下一个，
//                          单词默认隐藏（真听写），可切换成"看词听写"。
//   单词卡模式（kCards）   — 一直显示单词/音标/释义，每隔 interval 秒翻一张卡，
//                          适合预习。
//
// 与服务器 TTS 的协调：
//   - 服务器语音优先：收到新的服务器音频时暂停本地播放，播完后继续
//   - 本地播放期间若设备进入对话状态，暂停并等空闲再续播
// 所有状态变更都通过 Application::Schedule() 在主任务里执行，避免跨任务竞争。

class EnglishPlayer {
public:
    enum Mode {
        kDictation = 0,  // 报听写
        kCards = 1,      // 单词卡
    };

    // 播报语言
    enum Speak {
        kSpeakEnglish = 0,  // 只播英文
        kSpeakChinese = 1,  // 只播中文
        kSpeakBoth = 2,     // 先英文再中文
    };

    static EnglishPlayer& GetInstance();

    // 开始学习。unit=0 表示不限定单元；count<=0 表示到词库末尾。
    // interval 为词与词之间的等待秒数（不含发音本身时长）。
    bool Start(int unit, int count, int interval_seconds, bool show_word, Mode mode = kDictation);

    void Stop();
    void Next();
    void Prev();
    void Repeat();
    // 标记当前词听写正确（长按"下一个"触发）
    void MarkCorrect();
    // 切换显示模式（真听写 <-> 看词听写）
    void ToggleShowWord();
    // 在报听写与单词卡之间切换
    void ToggleMode();
    // 在 5/10/15/20/30 秒之间轮换间隔
    void CycleInterval();
    // 轮换播报语言（英文 / 中文 / 中英）
    void CycleSpeak();

    bool Active() const { return active_; }
    bool Paused() const { return paused_; }
    int CurrentIndex() const { return current_; }
    // 当前是第几个词（1 起）
    int Position() const { return current_ - start_ + 1; }
    int Total() const { return total_; }
    int Interval() const { return interval_; }
    bool ShowWord() const { return show_word_; }
    // 听写成绩
    int CorrectCount() const { return correct_; }
    int AnsweredCount() const { return answered_; }
    Mode CurrentMode() const { return mode_; }
    Speak CurrentSpeak() const { return speak_; }
    // 播报语言名（用于界面显示）
    static const char* SpeakName(Speak speak);
    // 距离下一个词还有几秒（-1 表示不在等待中）
    int Countdown() const;
    // 词库就绪且有词
    static bool Available();
    // 由主循环时钟事件驱动：推进到下一个词 / 结束
    void Tick();

private:
    void PlayCurrent();

    bool active_ = false;
    bool paused_ = false;
    bool show_word_ = false;
    Mode mode_ = kDictation;
    Speak speak_ = kSpeakEnglish;
    int unit_ = 0;
    int count_ = 0;
    int interval_ = 10;
    int current_ = -1;
    int total_ = 0;
    int start_ = 0;
    int end_ = 0;
    int played_ = 0;
    int correct_ = 0;
    int answered_ = 0;
    bool pending_cn_ = false;
    uint32_t next_tick_ = 0;
    std::vector<uint8_t> audio_;
};

#endif  // ENGLISH_PLAYER_H
