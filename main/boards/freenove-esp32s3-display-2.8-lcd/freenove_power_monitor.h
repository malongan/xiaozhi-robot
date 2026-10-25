#ifndef _FREENOVE_POWER_MONITOR_H_
#define _FREENOVE_POWER_MONITOR_H_

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include <cstdint>
#include <deque>
#include <functional>

// 电池监测（Freenove ESP32-S3 2.8"，ES3C28P / FNK0104）
//
// 硬件事实：电池电压 BAT+ 经 R14/R15 200K/200K 分压到 GPIO9（ADC1_CH8），
// 充电 IC 是 TP4054，但它的 CHRG 状态脚没有引到 ESP32 任何 GPIO，
// 因此固件只能从电池电压的变化推断充电状态：
//   1) 插拔 Type-C 的瞬间，电池从"放电供整机"变成"被充电"，BAT+ 会有
//      几十 mV 的阶跃（负载电流方向反转），可用来秒级判定；
//   2) 开机时就已经插着电，没有阶跃可看，用几分钟的电压斜率兜底。
// 如果以后把 TP4054 的 CHRG 焊到空闲脚（IO2/IO3/IO14/IO21），把这个引脚
// 传给构造函数即可切换成硬件直读（CHRG 开漏：低=充电中）。
class FreenovePowerMonitor {
public:
    explicit FreenovePowerMonitor(gpio_num_t charging_pin = GPIO_NUM_NC);
    ~FreenovePowerMonitor();

    bool IsCharging() const { return charging_; }
    bool IsDischarging() const { return !charging_; }
    bool IsBatteryPresent() const { return battery_present_; }
    uint8_t GetBatteryLevel() const { return level_; }
    int GetBatteryMillivolts() const { return battery_mv_; }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = std::move(callback);
    }

private:
    struct Sample {
        int64_t time_us;
        int mv;  // 电池端电压（已乘回分压比）
    };

    static constexpr int64_t kSampleIntervalUs = 2 * 1000 * 1000;  // 2s
    static constexpr int64_t kHistoryUs = 12 * 60 * 1000 * 1000;    // 保留 12 分钟
    static constexpr int kBatteryPresentMv = 2500;                  // 低于此值视为没接电池
    static constexpr int kStepThresholdMv = 12;                     // 插拔阶跃阈值
    static constexpr int kSlopeThresholdMv = 8;                     // 慢速斜率阈值
    static constexpr int64_t kStepRecentUs = 6 * 1000 * 1000;       // 最近 6s
    static constexpr int64_t kStepBaseUs = 20 * 1000 * 1000;        // 20s 前作为阶跃基线
    static constexpr int64_t kStepBaseSpanUs = 8 * 1000 * 1000;     // 基线取 12~20s 前
    static constexpr int64_t kSlopeWindowUs = 5 * 60 * 1000 * 1000; // 5 分钟基线

    gpio_num_t charging_pin_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool adc_ready_ = false;

    std::deque<Sample> history_;
    int64_t last_sample_us_ = 0;
    int battery_mv_ = 0;
    uint8_t level_ = 0;
    bool battery_present_ = false;
    bool charging_ = false;
    bool state_known_ = false;
    std::function<void(bool)> on_charging_status_changed_;

    void InitAdc();
    void Sample();
    int ReadBatteryMillivolts() const;
    void UpdateLevel();
    void UpdateChargingState();
    int MedianMillivolts(int64_t from_us, int64_t to_us) const;
    bool HasSampleIn(int64_t from_us, int64_t to_us) const;
    void SetCharging(bool charging);
};

#endif  // _FREENOVE_POWER_MONITOR_H_
