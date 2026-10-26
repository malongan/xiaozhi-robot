# 性能优化方案分析

> 基于当前 `daily` profile（v2.5.2 定制版，commit `ee98fe5` + 本地改动）
> 分析时间：2026-10-26
> 本文档只做分析，不含代码改动。所有数据均来自源码与实机 INFO 日志实测。

---

## 一、实测基线

| 指标 | 当前值 | 来源 |
|---|---|---|
| 启动到可用（activation done） | **14.4 s** | 串口日志 `I (14430) Application: Activation done` |
| 拿到 IP → activation done | **6.5 s** | 7880 ms → 14430 ms |
| OTA 版本检查 HTTP 建连 | **4.86 s** | `HttpClient: ... cost=4860` |
| 内部 RAM free / 最低 | **120 KB / 33 KB** | `SystemInfo: free sram: 120555 minimal sram: 33412` |
| 固件体积 | 3,329,984 B（余量 46%） | 构建输出 |
| 显示 SPI 时钟 | 60 MHz | `DISPLAY_SPI_SCLK_HZ (60*1000*1000)` |
| LVGL buffer | 240×24 px ×2（双缓冲，内部 RAM） | `buffer_size = width_ * 24`, `double_buffer=true` |
| LVGL 任务优先级 | **1**（最低） | `port_cfg.task_priority = 1` |
| SD 卡 | 4-bit @ 20 MHz | `SDMMC_FREQ_DEFAULT` |
| FreeRTOS tick | 100 Hz | `CONFIG_FREERTOS_HZ=100` |

---

## 二、瓶颈清单（按收益排序）

### P0-1 · OTA 检查阻塞启动 —— 收益最大

**现象**

`Application::ActivationTask()` 是串行执行的：

```
ActivationTask()
  ├─ CheckAssetsVersion()      // 资源版本
  ├─ CheckNewVersion()         // 固件版本 ← 卡在这里
  ├─ InitializeProtocol()
  └─ xEventGroupSetBits(MAIN_EVENT_ACTIVATION_DONE)
```

实测时间线：7880 ms 已拿到 IP，但 14430 ms 才 activation done，**中间 6.5 秒几乎全耗在版本检查**。其中单次 HTTP 建连就花了 4.86 s，还伴随一次 SSL 读错误：

```
I (13470) HttpClient: Established new connection to api.tenclass.net:443 cost=4860
E (13600) Dynamic Impl: mbedtls_ssl_fetch_input error=76
E (13600) esp-tls-mbedtls: read error :-0x004C
I (13610) Ota: Current is the latest version
```

更值得注意的是 `CheckNewVersion()` 带 **10 次重试 + 递增延迟（起始 10 s）**。网络异常或服务器不可达时，最坏情况能把启动拖到数分钟，用户看到的就是"开机一直转圈"。

**方案（三选一，推荐 A+B 组合）**

| 方案 | 做法 | 收益 | 风险 |
|---|---|---|---|
| A. 后台化 | 把 `CheckNewVersion()` 从 `ActivationTask()` 拆出，activation done 后由低优先级任务执行 | 启动降到 ~8 s | 低，功能不变 |
| B. 节流 | NVS 记录 `last_ota_check_ts`，24 h 内跳过 | 日常开机再省 5 s | 低 |
| C. 仅手动 | 只保留设置页"检查更新"按钮 | 启动最快 | 中，失去自动升级 |

**推荐**：A + B。设备先进 idle 可用，版本检查在后台跑；一天只查一次。
**预期**：日常开机 **14.4 s → 约 8 s**（省 45%）。

---

### P1-1 · LVGL 任务优先级过低（1）

**现象**

任务优先级实际排布：

| 任务 | 优先级 | 栈 |
|---|---|---|
| `audio_input` | **8** | 6 KB（core 0） |
| `audio_output` | **4** | 4 KB |
| `opus_codec` | **2** | **24 KB** |
| `lvgl` | **1** | 默认 |

LVGL 优先级 1 意味着：**音频解码/播放一忙，UI 就抢不到 CPU**。表现是说话时动画掉帧、滚动发涩、点击响应迟钝。

**方案**：`port_cfg.task_priority` 1 → 3。

**为什么是 3 不是更高**：提到 4 以上会和 `audio_output` 同级，可能造成音频输出饿死、出现断续。3 高于 `opus_codec`(2) 但低于音频输出(4)，是较安全的折中。

**风险**：中。必须实机验证「播放长回复时是否卡顿/断开」。若出现音频断续，回退到 2。

---

### P1-2 · SD 卡频率只有 20 MHz

**现象**

```cpp
host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz
slot.width = 4;                          // 4-bit
```

4-bit 模式配合 20 MHz 是保守配置，理论带宽仅约 10 MB/s。影响面很广：

- 聊天记录每轮 `fopen/fwrite/fsync/fclose`
- 英语听写音频文件读取
- 网络传输功能读写 `/sdcard`

**方案**：改 `host.max_freq_khz = SDMMC_FREQ_HIGHSPEED`（40 MHz），保持 4-bit。

**预期**：顺序读写速度接近翻倍，聊天记录落盘阻塞时间减半。
**风险**：低。4-bit 40 MHz 是 SD 规范标准档位，绝大多数卡支持。若个别卡不稳，SD 驱动会自动降速。

---

### P1-3 · LVGL 绘制缓冲偏小（24 行）

**现象**

```cpp
.buffer_size = width_ * 24,   // 240 × 24 = 5,760 px = 11.5 KB
.double_buffer = true,        // 已开双缓冲
.buff_dma = 1, .buff_spiram = 0
```

双缓冲已启用（这点没问题），但每块只有 24 行 —— 240×320 的屏要 **14 次 flush** 才能画满一屏。每次 flush 都有中断、任务切换、SPI 配置开销。

注释里说明了为什么不敢开大：

> A full-frame transfer (~150KB) cannot allocate that bounce buffer, so keep a small DMA strip in internal SRAM and partial refresh.

**方案**：24 行 → **40 或 48 行**（双缓冲占用 23 KB → 38/46 KB，内部 RAM 当前余 120 KB 可承受）。

**预期**：flush 次数 14 → 7~8 次，连续滚动/表情切换更顺。
**风险**：低。但需注意内部 RAM 最低水位已到 33 KB，加大后要复测是否会触发分配失败。

---

### P2-1 · Wi-Fi 省电模式拖慢响应

**现象**

```
I (14470) WifiStation: Setting WiFi power save level: LOW_POWER (MAX_MODEM)
```

`MAX_MODEM` 是较激进的省电档，会让 MQTT 下行消息延迟增加（通常 100~300 ms），表现为"回复开头有停顿"。

**方案**：动态切换 —— 进入聆听/对话时切 `HIGH_THROUGHPUT`，回到待机 5 s 后切回 `LOW_POWER`。

**收益**：对话响应更跟手，待机功耗基本不变。
**风险**：中，需要和现有省电逻辑（`PowerSaveTimer`）协调，避免互相覆盖。

---

### P2-2 · ChatLog 的 CRC16 逐位计算

**现象**

```cpp
uint16_t crc = 0xFFFF;
for (size_t k = 0; k < item.text.size(); ++k) {
    crc ^= static_cast<uint8_t>(item.text[k]);
    for (int b = 0; b < 8; ++b) {          // 每字节 8 次循环
        crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
}
```

单条 512 B 正文 → 4,096 次循环。虽是微秒级，但属于纯浪费。

**方案**：改 256 项查表法（表可 const 静态生成），速度提升约 8 倍。

**收益**：很小（一轮对话一次）。**优先级低**，可在做其他 SD 优化时顺手改。
**风险**：极低，但必须保证 CRC 结果完全一致（有现成测试用例可回归）。

---

### P2-3 · SPI 时钟 60 → 80 MHz（超频尝试）

现状 60 MHz 已接近 ILI9341 官方上限（62.5 MHz）。提到 80 MHz 可获得约 30% 的刷屏带宽提升，属于超频。

**风险**：**中高**。会出现花屏、随机噪点、偶发白屏，且随温度变化。需要长时间烤机验证。
**建议**：仅作为实验项，不进主线。

---

## 三、已确认「不是瓶颈」的项

避免误判，这几项专门查过：

| 项 | 结论 |
|---|---|
| **LVGL 双缓冲未开** | ❌ 误判。Freenove 构造时传了 `true`，双缓冲已启用 |
| **省电 30 秒触发而非 60 秒** | ❌ 不是 bug。NVS `idle_seconds=30`（历史 50→40→30），是手动调的 |
| **full_refresh** | 已为 `0`（部分刷新），无需改 |
| **缓冲放 PSRAM** | 已是 `buff_spiram=0`（内部 RAM + DMA 直取），比放 PSRAM 快，正确 |
| **FREERTOS_HZ=100 偏低** | 不建议改。音频块 512 样本 @16 kHz = 32 ms，10 ms tick 足够；提到 1000 反而增加上下文切换开销 |
| **全屏 single buffer** | 不可行。150 KB 全帧传输拿不到 DMA bounce buffer，现有 strip 方案是合理折中 |

---

## 四、内存现状与风险提示

```
free sram: 120555   minimal sram: 33412
```

内部 RAM 只剩 120 KB，历史最低到 33 KB。主要占用：

| 占用方 | 大小 |
|---|---|
| `opus_codec` 任务栈 | 24 KB |
| `audio_input` 栈 | 6 KB |
| LVGL 双缓冲 | 23 KB |
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | 100 KB（保留给内部） |
| NimBLE host 栈 | 4 KB |

**NimBLE 协议栈说明**：`CONFIG_BT_ENABLED=y` + 四个角色全开（CENTRAL/PERIPHERAL/BROADCASTER/OBSERVER）+ GATT client/server + security 全编入，但 `ble_keyboard` 功能包是关闭的 —— 协议栈实际上没被业务使用。按你的要求"其它不关闭"，这里**只做提示不做改动**：如果将来确认蓝牙键盘彻底不用，关掉协议栈可回收约 40~60 KB RAM 和一个后台任务。

**做 P1-3（加大 LVGL buffer）前务必注意**：内部 RAM 最低水位 33 KB，加 23 KB 缓冲后余量会很紧，建议同步把 `opus_codec` 栈改用外部内存（`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` 已开）。

---

## 五、建议执行顺序

| 顺序 | 项目 | 预期收益 | 风险 | 构建/验证成本 |
|---|---|---|---|---|
| 1 | **P0-1 OTA 后台化 + 节流** | 启动省 6 s | 低 | 1 次构建 + 观察启动 |
| 2 | **P1-2 SD 40 MHz** | I/O 翻倍 | 低 | 1 次构建 + 读写验证 |
| 3 | **P1-1 LVGL 优先级 1→3** | UI 跟手 | 中 | 1 次构建 + 长回复播测试 |
| 4 | **P1-3 buffer 24→40 行** | 刷屏更顺 | 低 | 与 3 合并构建 |
| 5 | P2-1 Wi-Fi 动态省电 | 响应更快 | 中 | 需与省电逻辑协调 |
| 6 | P2-2 CRC 查表 | 可忽略 | 极低 | 顺手改 |

前 4 项可以合并成一次构建验证，总体预计启动从 14.4 s 降到 8 s 左右，UI 流畅度明显改善。

---

## 六、验证方法（有了 INFO 日志后可行）

本次已把日志提到 INFO，以下都有明确观测点：

| 优化项 | 验证日志 |
|---|---|
| OTA 后台化 | 启动日志中 `Activation done` 时间戳提前到 8000 ms 内 |
| SD 40 MHz | 无直接日志，用聊天记录写入耗时或大文件读测速 |
| LVGL 优先级 | 长回复播报时观察画面掉帧（可通过 `SystemInfo` 的 min sram 间接看） |
| buffer 加大 | 无日志，肉眼看滚动流畅度 |
| Wi-Fi 省电 | `WifiStation: Setting WiFi power save level:` 切换记录 |

---

**结论**：最值得做的是 **启动路径的 OTA 检查位置**（单独一项就占启动时间 45%），其次是 SD 频率和 LVGL 优先级。这三项风险都不高、收益可测。

以上为分析稿，是否落地、按什么顺序，你定。
