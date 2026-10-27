#include "power_save_timer.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PowerSaveTimer"


PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown,
                               bool allow_light_sleep, bool keep_wake_word,
                               int seconds_to_deep_save)
    : cpu_max_freq_(cpu_max_freq), seconds_to_sleep_(seconds_to_sleep),
      seconds_to_shutdown_(seconds_to_shutdown), seconds_to_deep_save_(seconds_to_deep_save),
      allow_light_sleep_(allow_light_sleep), keep_wake_word_(keep_wake_word) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

PowerSaveTimer::~PowerSaveTimer() {
    esp_timer_stop(power_save_timer_);
    esp_timer_delete(power_save_timer_);
}

void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", true)) {
            ESP_LOGI(TAG, "Power save timer is disabled by settings");
            return;
        }

        ticks_ = 0;
        shutdown_requested_ = false;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

void PowerSaveTimer::OnEnterDeepSaveMode(std::function<void()> callback) {
    on_enter_deep_save_ = callback;
}

void PowerSaveTimer::OnExitDeepSaveMode(std::function<void()> callback) {
    on_exit_deep_save_ = callback;
}

void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

void PowerSaveTimer::EnterSleepMode() {
    ESP_LOGI(TAG, "Enabling power save mode");
    in_sleep_mode_ = true;
    if (on_enter_sleep_mode_) {
        on_enter_sleep_mode_();
    }

    if (cpu_max_freq_ != -1) {
        auto& app = Application::GetInstance();
        if (!keep_wake_word_) {
            // Disable wake word detection
            auto& audio_service = app.GetAudioService();
            is_wake_word_running_ = audio_service.IsWakeWordRunning();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(false);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // Disable audio input
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec) {
                codec->EnableInput(false);
            }
        }

        esp_pm_config_t pm_config = {
            .max_freq_mhz = cpu_max_freq_,
            .min_freq_mhz = 40,
            .light_sleep_enable = allow_light_sleep_,
        };
        esp_pm_configure(&pm_config);
    }
}

void PowerSaveTimer::EnterDeepSaveMode() {
    ESP_LOGI(TAG, "Entering deep save mode: backlight off, wake word kept");
    in_deep_save_mode_ = true;
    if (on_enter_deep_save_) {
        on_enter_deep_save_();
    }
}

void PowerSaveTimer::PowerSaveCheck() {
    auto& app = Application::GetInstance();
    const bool can_enter = app.CanEnterSleepMode();

    if (!can_enter) {
        /* 省电期间出现交互（喊醒、对话、播放），立刻恢复满频与背光，
           不必等用户去点屏幕或进设置。 */
        if (in_sleep_mode_ || in_deep_save_mode_) {
            WakeUp();
        }
        ticks_ = 0;
        return;
    }

    ticks_++;

    if (!in_sleep_mode_ && seconds_to_sleep_ > 0 && ticks_ >= seconds_to_sleep_) {
        EnterSleepMode();
    }
    if (!in_deep_save_mode_ && seconds_to_deep_save_ > 0 && ticks_ >= seconds_to_deep_save_) {
        EnterDeepSaveMode();
    }
    if (!shutdown_requested_ && seconds_to_shutdown_ > 0 && ticks_ >= seconds_to_shutdown_) {
        shutdown_requested_ = true;
        ESP_LOGI(TAG, "Idle %d s, requesting shutdown", ticks_);
        if (on_shutdown_request_) {
            on_shutdown_request_();
        }
    }
}

void PowerSaveTimer::WakeUp() {
    ticks_ = 0;
    shutdown_requested_ = false;

    /* 先退档 2：恢复指示灯与 Wi-Fi 省电策略。
       背光由档 1 的回调统一恢复，避免两处各写一次亮度。 */
    if (in_deep_save_mode_) {
        in_deep_save_mode_ = false;
        ESP_LOGI(TAG, "Exiting deep save mode");
        if (on_exit_deep_save_) {
            on_exit_deep_save_();
        }
    }

    if (in_sleep_mode_) {
        ESP_LOGI(TAG, "Exiting power save mode");
        in_sleep_mode_ = false;

        if (cpu_max_freq_ != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = cpu_max_freq_,
                .min_freq_mhz = cpu_max_freq_,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            // Enable wake word detection
            auto& app = Application::GetInstance();
            auto& audio_service = app.GetAudioService();
            if (!keep_wake_word_ && is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
        }

        if (on_exit_sleep_mode_) {
            on_exit_sleep_mode_();
        }
    }
}
