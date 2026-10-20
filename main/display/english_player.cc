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

bool EnglishPlayer::Start(int unit, int count, int interval_seconds, bool show_word) {
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
    start_ = start;
    end_ = end;
    total_ = end - start;
    current_ = start;
    played_ = 0;
    paused_ = false;
    active_ = true;
    next_tick_ = 0;

    ESP_LOGI(TAG, "开始听写：%d 词，间隔 %d 秒，%s", total_, interval_,
             show_word_ ? "显示单词" : "只发音");
    PlayCurrent();
    return true;
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
        ESP_LOGW(TAG, "读取音频失败：%s", store.WordAt(current_).c_str());
        audio_.clear();
    } else if (!audio_.empty()) {
        app.PlaySound(
            std::string_view(reinterpret_cast<const char*>(audio_.data()), audio_.size()));
    }
    played_++;
    // 发音播放完（约 0.5s）后开始倒计时等待
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
        ESP_LOGI(TAG, "听写完成：共 %d 词", played_);
        Stop();
    }
}
