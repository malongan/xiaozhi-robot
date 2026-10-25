# Freenove ESP32-S3 ESP32 S3 Capacitive Touch Display CYD WiFi BT, 2.8 Inch 240x320 IPS Screen

[product](https://store.freenove.com/products/fnk0104)

Official github: [freenove-esp32s3-display-2.8-lcd](https://github.com/Freenove/Freenove_ESP32_S3_Display)

Likely the same hardware design as [LCD wiki ES3C28P/ES3N28P](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)

## 电池与充电图标

- 电池电压：`BAT+` 经 R14/R15（200K/200K）分压到 **GPIO9（ADC1_CH8）**。
- 充电 IC 为 TP4054，但它的 **CHRG 状态脚没有连到 ESP32**，板上也没有充电指示灯。
- 因此 `freenove_power_monitor.*` 用三级判定：
  1. `BATTERY_CHARGING_DETECT_PIN` 非 `NC` 时直接读 CHRG（开漏，低=充电中）；
  2. 检测到 USB 主机（USJ SOF）时视为外部供电，电池低于 4.19V 显示充电中；
  3. 否则比较电池电压的插拔阶跃（约 20s 内）和 5 分钟斜率。
- 电池接近满电时 TP4054 会截止，此时显示满格电池而不显示闪电，属正常现象。
- 想要更准确的充电状态，可把 TP4054 的 CHRG 飞线到空闲脚（IO2/IO3/IO14/IO21），
  加 10K 上拉到 3.3V，并把 `config.h` 里的 `BATTERY_CHARGING_DETECT_PIN` 改成该引脚。
