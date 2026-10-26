#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1,
                   bool allow_light_sleep = true, bool keep_wake_word = false);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    void WakeUp();

private:
    void PowerSaveCheck();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool is_wake_word_running_ = false;
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    bool allow_light_sleep_;
    /* keep_wake_word_: 进入省电后仍保留唤醒词检测与音频输入。
       彩屏常驻设备需要随时能被喊醒，只靠降背光/降频省电。 */
    bool keep_wake_word_;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
