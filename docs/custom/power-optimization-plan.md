# 低功耗与刷新优化方案（借鉴 ESP32-S3-RLCD-4.2）

> 目标板型：Freenove ESP32-S3 Display 2.8"（16MB Flash / 8MB PSRAM / 彩屏 TFT 带背光）
> 参考项目：`wickenzh/ESP32-S3-RLCD-4.2`（Waveshare RLCD 4.2，ESP-IDF 5.5.3 + LVGL 8.4）
> 本文仅为方案说明，不改代码；评审通过后再按 P0→P3 落地。

---

## 0. 背景与目标

RLCD 项目是"常驻显示 + 按需功耗"的成熟范例：DFS 降频、Tickless Idle、Light Sleep、PM Lock、局部刷新、Wi-Fi 按需。本项目（XiaoZhi 定制版）与其同源（其小智页面基于 xiaozhi-esp32 二开），但功耗路径基本是"空白"。

目标：在不改变现有 UI/交互的前提下，补齐空闲省电与刷新策略，降低待机功耗；同时**不照搬** RLCD 的短连接/无背光假设，避免掉线、喊不醒。

---

## 1. 现状诊断（基于当前代码）

| # | 现状 | 证据 | 影响 |
|---|---|---|---|
| 1 | 动态电源管理未启用，CPU 恒定 240MHz | `sdkconfig`: `# CONFIG_PM_ENABLE is not set`、`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240` | 无 DFS、无 Tickless、无自动 Light Sleep |
| 2 | 省电配置空转 | `main/boards/common/power_save_timer.cc:92-97` 调 `esp_pm_configure(light_sleep_enable=true)`，但 PM 未开时不生效 | 看着做了省电，实际没有 |
| 3 | **Freenove 板未接入空闲省电定时器** | 板级 `.cc` 无 `PowerSaveTimer`/`SleepTimer`/`CanEnterSleepMode`；对比 `chd-esp32s3-box.cc:326`、`du-chatx-wifi.cc:44` 均有 `InitializePowerSaveTimer` | 平时零省电，仅手动关机 deep sleep |
| 4 | 背光常亮、无空闲调暗 | `freenove...cc:424` `PwmBacklight(DISPLAY_BACKLIGHT_PIN)`；`config.h:28` GPIO45 | 彩屏最大静态功耗项未管理 |
| 5 | 睡眠只有"手动关机 deep sleep" | `freenove...cc:324` `esp_deep_sleep_start()`（ext0 唤醒） | 无中间档 light sleep |
| 6 | 刷新无策略层 | 显示由 `LvglDisplay` + `esp_lvgl_port` 管理，LVGL 自动局部刷新 | 待机页面/隐藏页面仍在绘制，表情动画常刷 |
| 7 | 已有可复用的手动 light sleep 实现 | `main/boards/common/sleep_timer.cc:90-121`（30s 定时唤醒循环） | 可参考，但周期粗、会打断 |

---

## 2. 借鉴点 → 改造项映射

| RLCD 做法 | 本项目对应改造 | 优先级 |
|---|---|---|
| DFS 240→40MHz + Tickless + Light Sleep | 开 `CONFIG_PM_ENABLE` / `USE_TICKLESS_IDLE`，配 max240/min80 | **P0** |
| PM Lock 保护高负载 | 音频/AEC/播放/OTA/SD 加 `esp_pm_lock`，空闲释放 | **P0** |
| 按需功耗（页面不可见停刷新） | Freenove 接入 `PowerSaveTimer`，空闲降背光+停唤醒词+降频 | **P1** |
| 局部刷新优先 | 待机/隐藏页暂停绘制；时钟/状态仅按需 invalidate | **P2** |
| 定时唤醒对齐、深睡 | 分级：空闲降频 → light sleep（谨慎）→ 深睡 | **P2** |
| 背光（RLCD 无，彩屏必须管） | 待机调暗/关背光，交互恢复 | **P1** |
| Power Demo 裸帧缓冲 | 仅借鉴"停 LVGL 绘制"，不搬 1-bit 帧缓冲 | P3（可选） |

---

## 3. 改造方案

### P0 · 启用动态电源管理（收益最大，风险中）

**改动**
1. `sdkconfig.defaults.esp32s3`（或板级 profile）开启：
   - `CONFIG_PM_ENABLE=y`
   - `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
   - `CONFIG_PM_DFS_INIT_AUTO=y`（或运行期 `esp_pm_configure`）
2. 项目入口配置 `esp_pm_config_t`：`max_freq_mhz=240, min_freq_mhz=80, light_sleep_enable=true`。
3. 高负载路径加 **PM Lock**（参考 `nt26_board.cc:19` 的用法）：
   - 音频采集/播放、AEC、唤醒词检测、OTA 下载、SD 大块写入期间 `ESP_PM_CPU_FREQ_MAX`；
   - 结束后释放，让 DFS 回落到 80MHz。

**收益**：空闲自动降频 + 浅睡，不改业务逻辑即可见效。
**风险**：Light Sleep 与 I2S/ADC/唤醒词/VAD 时序；必须用 PM Lock 保护，否则音频卡顿、误唤醒。
**验证**：待机电流对比；连续对话 30 轮无卡顿；唤醒词灵敏度回归；AEC 模式回归。

### P1 · Freenove 接入空闲省电（对齐"按需功耗"）

**改动**
1. 在 `freenove-esp32s3-display-2.8-lcd.cc` 增加 `InitializePowerSaveTimer()`，参考 `chd/du-chatx`：
   - `power_save_timer_ = new PowerSaveTimer(-1, 60, -1)`（60s 空闲进省电；关机档按需）
2. 在 `OnEnterSleepMode` 回调里做三件事：
   - 背光降到低亮度（如 10%）或关；
   - 停唤醒词 / 关音频输入（现有 `power_save_timer.cc` 已做）；
   - 允许 DFS 降频。
3. `OnExitSleepMode` 恢复背光、唤醒词、满频。
4. 与现有"设置-声音与显示"里的亮度/休眠开关联动（`Settings("wifi")` 的 `sleep_mode`）。

**收益**：待机时背光 + CPU + 音频三项同时降，功耗下降最明显。
**风险**：低亮度下可读性；唤醒恢复时序（参考官方 #2336"空闲重复请求关机"教训，确保只触发一次）。

### P2 · 刷新策略（借鉴"能局部就不全屏"）

**改动**
1. 待机画面：时钟/状态只在数值变化时 invalidate，不做周期全刷。
2. 隐藏页面/二级页：暂停其专属绘制与动画（表情 GIF 常刷是重点）。
3. 检查是否存在 `lv_refr_now(nullptr)` 全屏刷新点，改为局部 `lv_obj_invalidate_area`。
4. 背光调暗与 P1 的空闲回调绑定。

**收益**：减少 SPI 刷屏与 CPU 占用，配合 P0 更省电。
**风险**：局部刷新残留/闪烁；需逐页回归。

### P3 · 睡眠分级（谨慎，勿照搬 RLCD）

**现状**：`sleep_timer.cc` 已有 30s 周期手动 light sleep 循环；Freenove 只在关机时深睡。

**建议**（仅对"确认无会话"场景）：
- 短空闲（60s）：P1 省电（降背光+降频），**不睡**——保证随时能喊醒；
- 长空闲（如 10min，可配置）：light sleep / deep sleep，但必须明确"会断 Wi-Fi、唤醒词失效"；
- 日常待机**不建议**自动 light sleep：XiaoZhi 是长连接语音助手，RLCD 是短连接拉天气，两者前提不同。

**收益**：极端待机省电；**风险**：掉线、喊不醒，需产品决策。

### P3.5 ·（可选）待机极简渲染

仅借鉴 Power Demo"停 LVGL 任务、裸刷最小区域"思想，不搬 1-bit 帧缓冲（与彩屏 UI 不兼容）。收益有限，优先级最低。

---

## 4. 明确不照搬的点（踩坑预防）

| RLCD 前提 | 本项目不同 | 结论 |
|---|---|---|
| 无背光、反射显示 | 彩屏 TFT 带背光（GPIO45 PWM） | 必须主动管背光；不能假设"不刷就省电" |
| 短连接拉天气，Wi-Fi 可随时关 | XiaoZhi 长连接（WebSocket/MQTT） | 不能随便关 Wi-Fi/自动 light sleep，否则掉线 |
| 可整分钟唤醒、深度睡眠 | 需要实时响应唤醒词 | 日常待机不睡，仅降频+降背光 |
| Power Demo 1-bit 帧缓冲、无 LVGL | 全 LVGL UI | 仅借"停绘制"思路，不搬实现 |
| 移植基线 `7b190b78`（老） | 本项目 v2.5.2 | 只借思路，不搬代码 |

---

## 5. 实施顺序与验证

| 阶段 | 内容 | 验证 |
|---|---|---|
| P0 | PM_ENABLE + PM Lock | 待机电流↓；对话/AEC/唤醒词回归；OTA 正常 |
| P1 | Freenove 接省电定时器 + 背光 | 空闲 60s 后亮度/频率/唤醒词状态正确；交互即恢复 |
| P2 | 刷新策略 | 待机刷新率/功耗；逐页无残影 |
| P3 | 睡眠分级 | 按产品决策；掉线/唤醒专项测试 |

**通用验证**：功耗仪测 mA；串口日志确认 PM 状态；`idf.py build` + 实机 smoke。

## 6. 风险与回滚

- 所有改动用 **Kconfig / feature 开关**包裹，默认关闭，可单开关回退。
- P0 影响面最大（音频时序），先独立分支验证，再合入。
- 改动集中：`sdkconfig.defaults.esp32s3`、`main/boards/freenove-.../...cc`、`main/boards/common/power_save_timer.cc`、`main/display/lvgl_display/*`。

## 7. 待确认

1. 是否接受"日常待机不自动 light sleep、只降频+降背光"（保唤醒/保连接）？
2. 空闲进省电的时长（60s？可配置？）与关机档是否保留？
3. 背光待机亮度目标（10%？可调？）。
