#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class SingleLed : public Led {
public:
    SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

    // 关机前主动熄灭指示灯（WS2812 点亮时十几 mA，关机流程需要关掉）
    void TurnOff();

    /* 直接指定颜色并立即刷新，用于省电提示（蓝色呼吸、极暗闪一下）。
       绕开基类按设备状态驱动的那套颜色，调用后要记得恢复。 */
    void SetRgb(uint8_t r, uint8_t g, uint8_t b);

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void TurnOn();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif // _SINGLE_LED_H_
