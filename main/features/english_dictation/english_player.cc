#include "english_player.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "device_state.h"
#include "english_tool.h"

namespace {
constexpr const char* TAG = "EnglishPlayer";

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}  // namespace

EnglishPlayer& EnglishPlayer::GetInstance() {
    static EnglishPlayer instance;
    return instance;
}

bool EnglishPlayer::Available() {
    auto& store = EnglishStore::GetInstance();
    return store.Ready() && store.WordCount() > 0;
}

bool EnglishPlayer::Start(int unit, int topic, int count, int interval_seconds, bool show_word,
                          Mode mode) {
    auto& store = EnglishStore::GetInstance();
    if (!store.Ready() || store.WordCount() == 0) {
        ESP_LOGW(TAG, "英语词库未就绪");
        return false;
    }
    order_.clear();
    if (topic > 0) {
        for (int i = 0; i < store.WordCount(); ++i) {
            if (store.TopicAt(i) != topic) {
                continue;
            }
            if (unit > 0 && store.UnitAt(i) != unit) {
                continue;
            }
            order_.push_back(i);
        }
    } else if (unit > 0) {
        const int start = store.UnitStart(unit);
        const int end = store.UnitEnd(unit);
        if (start < 0 || end <= start) {
            ESP_LOGW(TAG, "单元 %d 没有词", unit);
            return false;
        }
        for (int i = start; i < end; ++i) {
            order_.push_back(i);
        }
    } else {
        for (int i = 0; i < store.WordCount(); ++i) {
            order_.push_back(i);
        }
    }
    if (count > 0 && order_.size() > (size_t)count) {
        order_.resize(count);
    }
    if (order_.empty()) {
        ESP_LOGW(TAG, "筛选结果为空：单元 %d，分类 %d", unit, topic);
        return false;
    }
    if (interval_seconds < 0) {
        interval_seconds = 0;
    }
    if (interval_seconds > 120) {
        interval_seconds = 120;
    }

    unit_ = unit;
    topic_ = topic;
    count_ = count;
    interval_ = interval_seconds;
    show_word_ = show_word;
    mode_ = mode;
    total_ = (int)order_.size();
    pos_ = 0;
    current_ = order_[0];
    played_ = 0;
    paused_ = false;
    active_ = true;
    pending_cn_ = false;
    retry_current_ = false;
    paused_remaining_ms_ = 0;
    last_repeat_ = 0;
    next_tick_ = 0;

    // 听写期间退出服务器监听，避免麦克风把播放内容/环境声送给助手。
    Application::GetInstance().SetDictationActive(true);

    if (interval_ > 0) {
        ESP_LOGI(TAG, "开始%s：%d 词（单元 %d，分类 %d），间隔 %d 秒，%s",
                 mode_ == kCards ? "单词卡" : "听写", total_, unit_, topic_, interval_,
                 show_word_ ? "显示单词" : "只发音");
    } else {
        ESP_LOGI(TAG, "开始%s：%d 词（单元 %d，分类 %d），手动翻页，%s",
                 mode_ == kCards ? "单词卡" : "听写", total_, unit_, topic_,
                 show_word_ ? "显示单词" : "只发音");
    }
    PlayCurrent();
    if (refresh_cb_ != nullptr) {
        refresh_cb_();
    }
    return true;
}

void EnglishPlayer::ToggleShowWord() {
    show_word_ = !show_word_;
    ESP_LOGI(TAG, "显示单词：%s", show_word_ ? "开" : "关");
}

void EnglishPlayer::ToggleMode() {
    if (mode_ == kDictation) {
        mode_ = kCards;
        show_word_ = true;  // 单词卡始终显示单词
    } else {
        mode_ = kDictation;
        show_word_ = false;  // 回到真听写
    }
    ESP_LOGI(TAG, "模式切换为 %s", mode_ == kCards ? "单词卡" : "报听写");
    // 立刻重播当前词
    if (active_) {
        PlayCurrent();
    }
}

const char* EnglishPlayer::SpeakName(Speak speak) {
    switch (speak) {
        case kSpeakChinese:
            return "报中文";
        case kSpeakBoth:
            return "中英";
        default:
            return "报英文";
    }
}

void EnglishPlayer::SetSpeak(Speak speak) {
    switch (speak) {
        case kSpeakEnglish:
        case kSpeakChinese:
        case kSpeakBoth:
            break;
        default:
            speak = kSpeakEnglish;
            break;
    }
    if (speak_ == speak) {
        return;
    }
    speak_ = speak;
    ESP_LOGI(TAG, "播报语言：%s", SpeakName(speak_));
}

void EnglishPlayer::CycleSpeak() {
    SetSpeak(static_cast<Speak>((speak_ + 1) % 3));
    if (active_) {
        PlayCurrent();
    }
}

void EnglishPlayer::CycleInterval() {
    static const int kSteps[] = {5, 10, 15, 20, 30, 0};
    constexpr int kCount = sizeof(kSteps) / sizeof(kSteps[0]);
    int idx = 0;
    for (int i = 0; i < kCount; ++i) {
        if (kSteps[i] == interval_) {
            idx = (i + 1) % kCount;
            break;
        }
    }
    interval_ = kSteps[idx];
    if (interval_ > 0) {
        ESP_LOGI(TAG, "间隔改为 %d 秒", interval_);
    } else {
        ESP_LOGI(TAG, "间隔改为手动");
    }
    // 立刻按新间隔重新计时（next_tick_ 用毫秒）；手动模式不再自动翻页
    if (active_) {
        if (pending_cn_) {
            next_tick_ = NowMs() + 200;
        } else {
            next_tick_ =
                interval_ > 0 ? NowMs() + static_cast<uint32_t>(interval_) * 1000 + 1000 : 0;
        }
    }
}

void EnglishPlayer::MarkCorrect() {
    if (!active_ || mode_ != kDictation) {
        return;
    }
    correct_++;
    answered_++;
    ESP_LOGI(TAG, "标记答对：%d/%d", correct_, answered_);
    Next();
}

bool EnglishPlayer::RequestNext() {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - last_manual_ < 800) {
        ESP_LOGI(TAG, "忽略过于频繁的翻页请求");
        return false;
    }
    last_manual_ = now;
    Next();
    return true;
}

bool EnglishPlayer::RequestPrev() {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - last_manual_ < 800) {
        ESP_LOGI(TAG, "忽略过于频繁的翻页请求");
        return false;
    }
    last_manual_ = now;
    Prev();
    return true;
}

void EnglishPlayer::Stop() {
    Application::GetInstance().SetDictationActive(false);
    if (!active_) {
        return;
    }
    active_ = false;
    paused_ = false;
    pending_cn_ = false;
    retry_current_ = false;
    paused_remaining_ms_ = 0;
    current_ = -1;
    total_ = 0;
    pos_ = 0;
    order_.clear();
    played_ = 0;
    if (answered_ > 0) {
        ESP_LOGI(TAG, "停止听写，本次成绩 %d/%d", correct_, answered_);
    }
    correct_ = 0;
    answered_ = 0;
    ESP_LOGI(TAG, "停止听写");
}

void EnglishPlayer::Pause() {
    if (!active_ || paused_) {
        return;
    }
    const uint32_t now = NowMs();
    const bool playing =
        pending_cn_ || !Application::GetInstance().GetAudioService().IsPlaybackIdle();
    paused_ = true;
    if (playing) {
        // 当前词被打断：继续后先重播这个词，避免漏词。
        retry_current_ = true;
        next_tick_ = now;
        paused_remaining_ms_ = 0;
    } else {
        paused_remaining_ms_ = next_tick_ > now ? next_tick_ - now : 0;
    }
    ESP_LOGI(TAG, "听写暂停");
}

void EnglishPlayer::Resume() {
    if (!active_ || !paused_) {
        return;
    }
    paused_ = false;
    if (next_tick_ != 0) {
        next_tick_ = NowMs() + paused_remaining_ms_;
    }
    ESP_LOGI(TAG, "听写继续");
    if (refresh_cb_ != nullptr) {
        refresh_cb_();
    }
}

void EnglishPlayer::SetInterval(int interval_seconds) {
    if (interval_seconds < 0) {
        interval_seconds = 0;
    }
    if (interval_seconds > 120) {
        interval_seconds = 120;
    }
    if (interval_ == interval_seconds) {
        return;
    }
    interval_ = interval_seconds;
    if (!active_) {
        return;
    }
    const uint32_t now = NowMs();
    if (paused_) {
        // 暂停中改间隔：继续后按新间隔等待；若当前词被打断，先补播再计时。
        if (interval_ <= 0) {
            next_tick_ = 0;
            paused_remaining_ms_ = 0;
        } else {
            // 手动模式原本 next_tick_ 为 0，给 Resume 一个非零锚点。
            if (next_tick_ == 0) {
                next_tick_ = now;
            }
            paused_remaining_ms_ = retry_current_
                                       ? 0
                                       : static_cast<uint32_t>(interval_) * 1000 + 1000;
        }
    } else if (!pending_cn_) {
        next_tick_ =
            interval_ > 0 ? now + static_cast<uint32_t>(interval_) * 1000 + 1000 : 0;
    }
    ESP_LOGI(TAG, "间隔改为 %d 秒", interval_);
    if (refresh_cb_ != nullptr) {
        refresh_cb_();
    }
}

void EnglishPlayer::Next() {
    if (!active_) {
        return;
    }
    if (pos_ + 1 < total_) {
        pos_++;
        current_ = order_[pos_];
        ESP_LOGI(TAG, "推进到第 %d 词", pos_ + 1);
        PlayCurrent();
        if (refresh_cb_ != nullptr) {
            refresh_cb_();
        }
    } else {
        ESP_LOGI(TAG, "已是最后一个词");
    }
}

void EnglishPlayer::Advance() {
    // 设备自己按间隔推进，不受手动翻页防抖限制
    if (!active_) {
        return;
    }
    if (pos_ + 1 < total_) {
        pos_++;
        current_ = order_[pos_];
        if (!PlayCurrent()) {
            pos_--;
            current_ = order_[pos_];
            return;
        }
        if (refresh_cb_ != nullptr) {
            refresh_cb_();
        }
    } else {
        ESP_LOGI(TAG, "学习完成：共 %d 词", played_);
        Stop();
    }
}

void EnglishPlayer::Prev() {
    if (!active_) {
        return;
    }
    if (pos_ > 0) {
        pos_--;
        current_ = order_[pos_];
        PlayCurrent();
    } else {
        ESP_LOGI(TAG, "已是第一个词");
    }
}

void EnglishPlayer::Repeat() {
    if (!active_) {
        return;
    }
    const uint32_t now = NowMs();
    if (last_repeat_ != 0 && now - last_repeat_ < 800) {
        ESP_LOGI(TAG, "忽略过于频繁的重播请求");
        return;
    }
    last_repeat_ = now;
    PlayCurrent();
}

bool EnglishPlayer::Restart() {
    if (!active_ || order_.empty()) {
        return false;
    }
    pos_ = 0;
    current_ = order_[0];
    played_ = 0;
    correct_ = 0;
    answered_ = 0;
    paused_ = false;
    pending_cn_ = false;
    retry_current_ = false;
    last_repeat_ = 0;
    next_tick_ = 0;
    ESP_LOGI(TAG, "重新开始：%d 词", total_);
    PlayCurrent();
    if (refresh_cb_ != nullptr) {
        refresh_cb_();
    }
    return true;
}

int EnglishPlayer::Countdown() const {
    if (!active_ || paused_ || next_tick_ == 0 || interval_ <= 0) {
        return -1;
    }
    const int64_t remain =
        static_cast<int64_t>(next_tick_) - static_cast<int64_t>(esp_timer_get_time() / 1000);
    if (remain <= 0) {
        return 0;
    }
    return static_cast<int>((remain + 999) / 1000);
}

bool EnglishPlayer::PlayCurrent() {
    if (paused_) {
        return false;
    }
    auto& store = EnglishStore::GetInstance();
    if (current_ < 0 || current_ >= store.WordCount()) {
        return false;
    }
    // 播放前先确保服务器语音已放完、设备空闲，避免抢麦。
    // 忙时只做短间隔重试（不推进词），真正播成功后 interval 才重新起算，
    // 这样既不会丢词，也不会把间隔压缩成连播。
    auto& app = Application::GetInstance();
    const bool server_busy = !app.GetAudioService().IsPlaybackIdle();
    const bool state_busy =
        app.GetDeviceState() != kDeviceStateIdle && app.GetDeviceState() != kDeviceStateListening;
    // 忙状态超时兜底：长对话结束后 IsPlaybackIdle() 有可能迟迟不返回 true
    // （解码/输出标志残留），此时如果一直"稍后重试"，听写就永久卡住。
    // 连续忙超过 10 秒就认为空闲判断不可信，放行播放。
    const uint32_t now_ms = NowMs();
    bool force_play = false;
    if (server_busy || state_busy) {
        if (busy_since_ms_ == 0) {
            busy_since_ms_ = now_ms;
        } else if (now_ms - busy_since_ms_ >= 10000) {
            ESP_LOGW(TAG, "设备忙超过 10 秒（server_busy=%d state_busy=%d state=%d），放行播放",
                     (int)server_busy, (int)state_busy, (int)app.GetDeviceState());
            busy_since_ms_ = 0;
            force_play = true;
        }
        if (!force_play) {
            ESP_LOGW(TAG, "设备忙，稍后重试：%s (server_busy=%d state_busy=%d state=%d)",
                     store.WordAt(current_).data(), (int)server_busy, (int)state_busy,
                     (int)app.GetDeviceState());
            next_tick_ = now_ms + 500;
            retry_current_ = true;
            return false;
        }
    } else {
        busy_since_ms_ = 0;
    }
    // 真正播出去了，清掉“忙时重试”标记。双语分支下面会进入 pending_cn_，
    // 若这里不清，Tick 会每 200ms 再走一次重试，导致同一个词无限重播。
    retry_current_ = false;
    // 按播报语言决定播放哪一段（重播/切换语言时清掉上一词的待播中文）
    pending_cn_ = false;
    bool want_en = (speak_ == kSpeakEnglish || speak_ == kSpeakBoth);
    bool want_cn = (speak_ == kSpeakChinese || speak_ == kSpeakBoth);

    if (want_cn && !want_en) {
        // 只播中文
        if (!store.ReadChineseAudio(current_, audio_)) {
            ESP_LOGW(TAG, "没有中文音频：%s", store.WordAt(current_).data());
            audio_.clear();
        } else {
            app.PlaySound(
                std::string_view(reinterpret_cast<const char*>(audio_.data()), audio_.size()));
        }
        played_++;
        next_tick_ = interval_ > 0 ? NowMs() + static_cast<uint32_t>(interval_) * 1000 + 1000 : 0;
        retry_current_ = false;
        return true;
    }

    // 英文（和中英双语的第一段）
    if (!store.ReadAudio(current_, audio_)) {
        ESP_LOGW(TAG, "读取音频失败：%s", store.WordAt(current_).data());
        audio_.clear();
    } else if (!audio_.empty()) {
        app.PlaySound(
            std::string_view(reinterpret_cast<const char*>(audio_.data()), audio_.size()));
    }
    played_++;
    ESP_LOGI(TAG, "播放第 %d/%d 词：%s", pos_ + 1, total_, store.WordAt(current_).data());

    if (want_cn) {
        // 中英双语：等英文真正播完后再播中文（Tick 轮询播放队列）
        pending_cn_ = true;
        next_tick_ = NowMs() + 200;
        return true;
    }
    // 发音约 1 秒，之后再等 interval 秒到下一个词；手动模式下等用户翻页
    next_tick_ = interval_ > 0 ? NowMs() + static_cast<uint32_t>(interval_) * 1000 + 1000 : 0;
    retry_current_ = false;
    return true;
}

void EnglishPlayer::Tick() {
    if (!active_ || paused_) {
        return;
    }
    const uint32_t now = NowMs();
    if (retry_current_ && (next_tick_ == 0 || now >= next_tick_)) {
        // 上一轮因设备忙没播出去：原地重试当前词，不推进
        if (!PlayCurrent()) {
            return;
        }
        if (refresh_cb_ != nullptr) {
            refresh_cb_();
        }
        return;
    }
    if (next_tick_ == 0 || now < next_tick_) {
        return;
    }
    if (pending_cn_) {
        // 等英文播完再播中文释义，避免两段叠在一起
        if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
            next_tick_ = now + 200;
            return;
        }
        pending_cn_ = false;
        auto& store = EnglishStore::GetInstance();
        if (store.ReadChineseAudio(current_, audio_)) {
            Application::GetInstance().PlaySound(
                std::string_view(reinterpret_cast<const char*>(audio_.data()), audio_.size()));
        }
        // 中文播完后才开始倒计时；手动模式下停在这里等用户翻页
        next_tick_ = interval_ > 0 ? now + static_cast<uint32_t>(interval_) * 1000 + 1000 : 0;
        if (refresh_cb_ != nullptr) {
            refresh_cb_();
        }
        return;
    }
    if (interval_ <= 0) {
        // 手动模式：不自动推进
        return;
    }
    if (pos_ + 1 >= total_) {
        ESP_LOGI(TAG, "学习完成：共 %d 词", played_);
        Stop();
        if (refresh_cb_ != nullptr) {
            refresh_cb_();
        }
        return;
    }
    pos_++;
    current_ = order_[pos_];
    if (!PlayCurrent()) {
        // 设备忙：保留已推进的 current_，由 retry_current_ 在空闲后补播。
        // 不能回滚并清掉 retry_current_，否则会丢掉"补播下一个词"的状态。
        return;
    }
    if (refresh_cb_ != nullptr) {
        refresh_cb_();
    }
}
