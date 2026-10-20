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
}  // namespace

EnglishPlayer& EnglishPlayer::GetInstance() {
    static EnglishPlayer instance;
    return instance;
}

bool EnglishPlayer::Available() {
    auto& store = EnglishStore::GetInstance();
    return store.Ready() && store.WordCount() > 0;
}

bool EnglishPlayer::Start(int unit, int count, int interval_seconds, bool show_word, Mode mode) {
    auto& store = EnglishStore::GetInstance();
    if (!store.Ready() || store.WordCount() == 0) {
        ESP_LOGW(TAG, "英语词库未就绪");
        return false;
    }
    int start = 0;
    int end = store.WordCount();
    if (unit > 0) {
        start = store.UnitStart(unit);
        end = store.UnitEnd(unit);
        if (start < 0 || end <= start) {
            ESP_LOGW(TAG, "单元 %d 没有词", unit);
            return false;
        }
    }
    if (count > 0 && start + count < end) {
        end = start + count;
    }
    if (end <= start) {
        return false;
    }
    if (interval_seconds < 1) {
        interval_seconds = 1;
    }
    if (interval_seconds > 120) {
        interval_seconds = 120;
    }

    unit_ = unit;
    count_ = count;
    interval_ = interval_seconds;
    show_word_ = show_word;
    mode_ = mode;
    start_ = start;
    end_ = end;
    total_ = end - start;
    current_ = start;
    played_ = 0;
    paused_ = false;
    active_ = true;
    next_tick_ = 0;

    ESP_LOGI(TAG, "开始%s：%d 词，间隔 %d 秒，%s", mode_ == kCards ? "单词卡" : "听写", total_,
             interval_, show_word_ ? "显示单词" : "只发音");
    PlayCurrent();
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

void EnglishPlayer::CycleInterval() {
    static const int kSteps[] = {5, 10, 15, 20, 30};
    int idx = 0;
    for (int i = 0; i < 5; ++i) {
        if (kSteps[i] == interval_) {
            idx = (i + 1) % 5;
            break;
        }
    }
    interval_ = kSteps[idx];
    ESP_LOGI(TAG, "间隔改为 %d 秒", interval_);
    // 立刻按新间隔重新计时
    if (active_) {
        next_tick_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + interval_ + 1;
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

void EnglishPlayer::Stop() {
    if (!active_) {
        return;
    }
    active_ = false;
    paused_ = false;
    current_ = -1;
    total_ = 0;
    played_ = 0;
    if (answered_ > 0) {
        ESP_LOGI(TAG, "停止听写，本次成绩 %d/%d", correct_, answered_);
    }
    correct_ = 0;
    answered_ = 0;
    ESP_LOGI(TAG, "停止听写");
}

void EnglishPlayer::Next() {
    if (!active_) {
        return;
    }
    if (current_ + 1 < end_) {
        current_++;
        PlayCurrent();
    } else {
        ESP_LOGI(TAG, "已是最后一个词");
    }
}

void EnglishPlayer::Prev() {
    if (!active_) {
        return;
    }
    if (current_ > start_) {
        current_--;
        PlayCurrent();
    } else {
        ESP_LOGI(TAG, "已是第一个词");
    }
}

void EnglishPlayer::Repeat() {
    if (!active_) {
        return;
    }
    PlayCurrent();
}

int EnglishPlayer::Countdown() const {
    if (!active_ || paused_ || next_tick_ == 0) {
        return -1;
    }
    const int64_t remain =
        static_cast<int64_t>(next_tick_) - static_cast<int64_t>(esp_timer_get_time() / 1000);
    if (remain <= 0) {
        return 0;
    }
    return static_cast<int>((remain + 999) / 1000);
}

void EnglishPlayer::PlayCurrent() {
    auto& store = EnglishStore::GetInstance();
    if (current_ < 0 || current_ >= store.WordCount()) {
        return;
    }
    // 播放前先确保服务器语音已放完、设备空闲，避免抢麦
    auto& app = Application::GetInstance();
    const bool server_busy = !app.GetAudioService().IsPlaybackIdle();
    const bool state_busy =
        app.GetDeviceState() != kDeviceStateIdle && app.GetDeviceState() != kDeviceStateListening;
    if (server_busy || state_busy) {
        // 服务器还在说话或设备正在对话，稍后由 Tick 重试
        next_tick_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 1;
        return;
    }
    if (!store.ReadAudio(current_, audio_)) {
        ESP_LOGW(TAG, "读取音频失败：%s", store.WordAt(current_).data());
        audio_.clear();
    } else if (!audio_.empty()) {
        app.PlaySound(
            std::string_view(reinterpret_cast<const char*>(audio_.data()), audio_.size()));
    }
    played_++;
    // 发音播放完（约 0.5s）后开始倒计时等待；单词卡模式整张卡停留 interval 秒
    next_tick_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + interval_ + 1;
}

void EnglishPlayer::Tick() {
    if (!active_) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (next_tick_ == 0 || now < next_tick_) {
        return;
    }
    if (current_ + 1 < end_) {
        current_++;
        PlayCurrent();
    } else {
        ESP_LOGI(TAG, "学习完成：共 %d 词", played_);
        Stop();
    }
}
