#include "freenove_power_monitor.h"

#include <esp_log.h>
#include <driver/usb_serial_jtag.h>

#include <algorithm>
#include <vector>

static const char* TAG = "FreenovePower";

namespace {

// 与 espressif/adc_battery_estimation 的 OCV_SOC Model 1 保持一致
constexpr struct {
    float voltage;
    int capacity;
} kBatteryPoints[] = {
    {4.16f, 100}, {4.07f, 90}, {3.99f, 80}, {3.90f, 70}, {3.82f, 60}, {3.72f, 50},
    {3.61f, 40},  {3.53f, 30}, {3.38f, 20}, {3.20f, 10}, {2.85f, 0},
};

}  // namespace

FreenovePowerMonitor::FreenovePowerMonitor(gpio_num_t charging_pin)
    : charging_pin_(charging_pin) {
    if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << charging_pin_,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // TP4054 CHRG 为开漏，需要上拉
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        ESP_LOGI(TAG, "charging detect pin: GPIO%d (active low)", charging_pin_);
    }

    InitAdc();
    Sample();

    esp_timer_create_args_t timer_cfg = {
        .callback = [](void* arg) { static_cast<FreenovePowerMonitor*>(arg)->Sample(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "freenove_power",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &timer_handle_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, kSampleIntervalUs));
}

FreenovePowerMonitor::~FreenovePowerMonitor() {
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
    if (adc_handle_ != nullptr) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (adc_cali_handle_ != nullptr) {
        adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
        adc_cali_handle_ = nullptr;
    }
#endif
}

void FreenovePowerMonitor::InitAdc() {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &adc_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    // GPIO9 = ADC1_CH8，BAT_ADC
    if (adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_8, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel init failed");
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_8,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle_) != ESP_OK) {
        ESP_LOGW(TAG, "ADC curve fitting calibration unavailable");
        adc_cali_handle_ = nullptr;
    }
#endif
    adc_ready_ = true;
}

int FreenovePowerMonitor::ReadBatteryMillivolts() const {
    // 取 9 次采样的中位数，压掉单点噪声
    int raw[9] = {};
    for (int& value : raw) {
        if (adc_oneshot_read(adc_handle_, ADC_CHANNEL_8, &value) != ESP_OK) {
            return battery_mv_;
        }
    }
    std::sort(std::begin(raw), std::end(raw));
    int adc_mv = 0;
    if (adc_cali_handle_ != nullptr) {
        if (adc_cali_raw_to_voltage(adc_cali_handle_, raw[4], &adc_mv) != ESP_OK) {
            return battery_mv_;
        }
    } else {
        adc_mv = raw[4] * 3300 / 4095;
    }
    // R14 = R15 = 200K -> 分压比 0.5，电池端电压是 ADC 的两倍
    return adc_mv * 2;
}

void FreenovePowerMonitor::Sample() {
    if (!adc_ready_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (last_sample_us_ != 0 && now - last_sample_us_ < kSampleIntervalUs) {
        return;
    }
    last_sample_us_ = now;

    const int mv = ReadBatteryMillivolts();
    if (mv <= 0) {
        return;
    }
    battery_mv_ = mv;
    battery_present_ = mv >= kBatteryPresentMv;
    history_.push_back({now, mv});
    while (!history_.empty() && now - history_.front().time_us > kHistoryUs) {
        history_.pop_front();
    }

    UpdateLevel();
    UpdateChargingState();

    if (charging_pin_ == GPIO_NUM_NC) {
        ESP_LOGD(TAG, "battery %d mV, %u%%, charging=%d, usb=%d, samples=%u", battery_mv_, level_,
                 charging_, usb_serial_jtag_is_connected(), static_cast<unsigned>(history_.size()));
    } else {
        ESP_LOGD(TAG, "battery %d mV, %u%%, charging=%d (pin)", battery_mv_, level_, charging_);
    }
}

void FreenovePowerMonitor::UpdateLevel() {
    if (!battery_present_) {
        level_ = 0;
        return;
    }
    const float voltage = battery_mv_ / 1000.0f;
    constexpr size_t count = sizeof(kBatteryPoints) / sizeof(kBatteryPoints[0]);
    if (voltage >= kBatteryPoints[0].voltage) {
        level_ = 100;
        return;
    }
    for (size_t i = 0; i + 1 < count; ++i) {
        const auto& hi = kBatteryPoints[i];
        const auto& lo = kBatteryPoints[i + 1];
        if (voltage <= hi.voltage && voltage >= lo.voltage) {
            const float span = hi.voltage - lo.voltage;
            const float ratio = span > 0.0f ? (voltage - lo.voltage) / span : 0.0f;
            level_ = static_cast<uint8_t>(lo.capacity + ratio * (hi.capacity - lo.capacity));
            return;
        }
    }
    level_ = 0;
}

bool FreenovePowerMonitor::HasSampleIn(int64_t from_us, int64_t to_us) const {
    for (const auto& sample : history_) {
        if (sample.time_us >= from_us && sample.time_us <= to_us) {
            return true;
        }
    }
    return false;
}

int FreenovePowerMonitor::MedianMillivolts(int64_t from_us, int64_t to_us) const {
    std::vector<int> values;
    for (const auto& sample : history_) {
        if (sample.time_us >= from_us && sample.time_us <= to_us) {
            values.push_back(sample.mv);
        }
    }
    if (values.empty()) {
        return battery_mv_;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void FreenovePowerMonitor::UpdateChargingState() {
    if (!battery_present_) {
        // 没接电池：不显示充电图标，也不缓存旧状态
        state_known_ = false;
        SetCharging(false);
        return;
    }

    // 有硬件 CHRG 引脚时直接读：TP4054 开漏，低电平 = 充电中
    if (charging_pin_ != GPIO_NUM_NC) {
        SetCharging(gpio_get_level(charging_pin_) == 0);
        return;
    }

    const int64_t now = esp_timer_get_time();

    // 1) 插着 USB 主机（数据线/电脑）：直接认为外部供电，电池正在补电。
    //    注意 usb_serial_jtag_is_connected() 依赖主机 SOF，纯充电头读不到，
    //    所以它只作为“确定插电”的快速信号，下面仍用电压变化兜底。
    if (usb_serial_jtag_is_connected()) {
        SetCharging(battery_mv_ < 4190);  // 满电截止后显示充满而不是充电中
        state_known_ = true;
        return;
    }

    // 2) 阶跃检测：插拔瞬间电池端口电压会有几十 mV 的跳变
    const int64_t recent_from = now - kStepRecentUs;
    const int64_t base_to = now - kStepBaseUs;
    const int64_t base_from = base_to - kStepBaseSpanUs;
    if (HasSampleIn(recent_from, now) && HasSampleIn(base_from, base_to)) {
        const int recent = MedianMillivolts(recent_from, now);
        const int base = MedianMillivolts(base_from, base_to);
        const int delta = recent - base;
        if (delta >= kStepThresholdMv) {
            SetCharging(true);
            state_known_ = true;
            return;
        }
        if (delta <= -kStepThresholdMv) {
            SetCharging(false);
            state_known_ = true;
            return;
        }
    }

    // 3) 斜率检测：开机就插着电时没有阶跃，用 5 分钟基线兜底
    const int64_t slope_from = now - kSlopeWindowUs;
    if (HasSampleIn(slope_from, now - kSlopeWindowUs + kStepRecentUs) &&
        HasSampleIn(recent_from, now)) {
        const int now_mv = MedianMillivolts(recent_from, now);
        const int old_mv = MedianMillivolts(slope_from, slope_from + 30 * 1000 * 1000);
        const int delta = now_mv - old_mv;
        if (delta >= kSlopeThresholdMv) {
            SetCharging(true);
            state_known_ = true;
        } else if (delta <= -kSlopeThresholdMv) {
            SetCharging(false);
            state_known_ = true;
        }
    }
}

void FreenovePowerMonitor::SetCharging(bool charging) {
    if (charging == charging_) {
        return;
    }
    charging_ = charging;
    if (on_charging_status_changed_) {
        on_charging_status_changed_(charging_);
    }
}
