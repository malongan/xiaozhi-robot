#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

/* 待机分级。
 *
 *   Active ──seconds_to_sleep──> 档1 省电（降频 + 降背光，保持唤醒词）
 *      │
 *      └────seconds_to_deep_save──> 档2 深省电（背光归零 + Wi-Fi modem sleep，
 *                                    麦克风与唤醒词仍然工作，喊一声就能醒）
 *      │
 *      └────seconds_to_shutdown───> 请求深度休眠（deep sleep，只能按键唤醒）
 *
 * 三个计时都从「最后一次交互」算起：任何一次唤醒/触摸都会把 ticks_ 清零。
 * seconds_* 传 -1 表示关闭该档。
 */
class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1,
                   bool allow_light_sleep = true, bool keep_wake_word = false,
                   int seconds_to_deep_save = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    /* 运行时改档位。三个 seconds 的语义与构造函数一致（<=0 关闭该档）。
       设置页改完 NVS 必须调这个，否则新值要等重启才生效。 */
    void SetIntervals(int seconds_to_sleep, int seconds_to_deep_save, int seconds_to_shutdown);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnEnterDeepSaveMode(std::function<void()> callback);
    void OnExitDeepSaveMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    /* deep_save_allowed：返回 false 时跳过档 2 与自动深度休眠（档 1 不受影响）。
       接外部供电检测用——插着 USB 时既不需要省电，也不该让设备自己关机。 */
    void SetDeepSaveAllowed(std::function<bool()> allowed);
    void WakeUp();
    /* 只退档 2、回落到档 1，用于「档 2 期间插上外部供电」：
       不必整机唤醒到满亮度，但屏幕要从全黑恢复到待机亮度。 */
    void ExitDeepSaveMode();

    bool in_sleep_mode() const { return in_sleep_mode_; }
    bool in_deep_save_mode() const { return in_deep_save_mode_; }

private:
    void PowerSaveCheck();
    void EnterSleepMode();
    void EnterDeepSaveMode();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool in_deep_save_mode_ = false;
    bool is_wake_word_running_ = false;
    /* on_shutdown_request_ 每秒都会被判定一次，必须只触发一次 */
    bool shutdown_requested_ = false;
    /* 上次的「允许档 2/自动休眠」判定，用来捕捉插拔外部供电的边沿 */
    bool last_deep_allowed_ = true;
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    int seconds_to_deep_save_;
    bool allow_light_sleep_;
    /* keep_wake_word_: 进入省电后仍保留唤醒词检测与音频输入。
       彩屏常驻设备需要随时能被喊醒，只靠降背光/降频省电。 */
    bool keep_wake_word_;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_enter_deep_save_;
    std::function<void()> on_exit_deep_save_;
    std::function<void()> on_shutdown_request_;
    std::function<bool()> deep_save_allowed_;
};
