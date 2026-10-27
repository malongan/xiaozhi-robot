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
//   2) 开机时就已经插着电，没有阶跃可看，用几分钟的电压斜率兜底；
//   3) 开机就插着电且电池已充满 —— 电压贴在恒压点、既不上升也不下降，
//      前两条都失效。这一种用"满电平坦"判定：TP4054 恒压会把电池顶在
//      4.20V，而拔电静置的满电电池只有 4.15~4.17V 且会缓慢下滑，所以
//      电压长期停在 4.18V 以上又不下滑，只可能是还接在外接电源上。
// 如果以后把 TP4054 的 CHRG 焊到空闲脚（IO2/IO3/IO14/IO21），把这个引脚
// 传给构造函数即可切换成硬件直读（CHRG 开漏：低=充电中）。
class FreenovePowerMonitor {
public:
    explicit FreenovePowerMonitor(gpio_num_t charging_pin = GPIO_NUM_NC);
    ~FreenovePowerMonitor();

    bool IsCharging() const { return charging_; }
    bool IsDischarging() const { return !charging_; }
    bool IsBatteryPresent() const { return battery_present_; }
    // 是否接在外接电源上（USB 主机、纯充电头、或者没装电池）。
    // 与 IsCharging() 分开：满了以后不再"充电"，但仍然是在插着电的，
    // 省电逻辑要的是后者。
    bool IsExternalPower() const { return external_power_; }
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
    // 满电判据只能靠趋势，不能只靠电压绝对值：实测这颗电池接在 USB 上充满时
    // 读数是 4170mV，而拔线后的开路电压是 4160mV 上下，两者只差 10mV，落在
    // ADC 噪声里。所以取"电压够高"作为前置门槛，再用长时间是否下滑来定性。
    static constexpr int kHighVoltageMv = 4100;                     // 前置门槛
    static constexpr int kDeclineThresholdMv = 10;                  // 长时间下滑超过此值算在放电
    static constexpr int64_t kSlowSampleIntervalUs = 60 * 1000 * 1000;        // 慢采样 1 分钟
    static constexpr int64_t kSlowHistoryUs = 90LL * 60 * 1000 * 1000;         // 慢历史 90 分钟

    gpio_num_t charging_pin_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool adc_ready_ = false;

    std::deque<Sample> history_;
    std::deque<Sample> slow_history_;  // 每分钟一个点，用来观察小时级趋势
    int64_t last_sample_us_ = 0;
    int battery_mv_ = 0;
    uint8_t level_ = 0;
    bool battery_present_ = false;
    bool charging_ = false;
    bool external_power_ = false;
    bool state_known_ = false;
    std::function<void(bool)> on_charging_status_changed_;

    void InitAdc();
    void Sample();
    int ReadBatteryMillivolts() const;
    void UpdateLevel();
    void UpdateChargingState();
    void UpdateExternalPower();
    bool IsBatteryDeclining() const;
    int MedianMillivolts(int64_t from_us, int64_t to_us) const;
    bool HasSampleIn(int64_t from_us, int64_t to_us) const;
    void SetCharging(bool charging);
};

#endif  // _FREENOVE_POWER_MONITOR_H_
