# 性能优化清单

> 配套分析：`docs/custom/performance-analysis.md`
> 基线版本：v2.5.2 定制版（`ee98fe5` + 本地改动），启动 14.4 s，内部 RAM free 120 KB
> 使用方式：按批次执行，每批次一次构建 + 一次实机验证

---

## 批次 A · 低风险，可一次构建（建议先做）

### [ ] A-1 OTA 检查移出启动关键路径

| 项 | 内容 |
|---|---|
| 位置 | `main/application.cc:460-479`（`ActivationTask`）、`:542`（`CheckNewVersion`） |
| 现状 | `CheckAssetsVersion()` → `CheckNewVersion()` → `InitializeProtocol()` 串行，版本检查没完成就不发 `MAIN_EVENT_ACTIVATION_DONE` |
| 改法 | ① 把 `CheckNewVersion()` 从 `ActivationTask()` 移出，activation done 之后由独立低优先级任务执行<br>② NVS 新增 `last_ota_check_ts`，24 h 内跳过；设置页"检查更新"可强制触发 |
| 收益 | **启动 14.4 s → 约 8 s**（实测 OTA 段占 6.5 s，单次 HTTP 建连 cost=4860 ms） |
| 风险 | 低。功能不变，只是检查时机后移 |
| 验证 | 串口时间戳 `Application: Activation done` 提前到 8000 ms 内；OTA 日志在 idle 之后出现 |
| 回滚 | 删除后台任务启动代码，恢复 `ActivationTask` 内原调用顺序 |

> 附带价值：`CheckNewVersion` 有 10 次重试 + 递增延迟（10 s 起），网络异常时能把启动拖到分钟级。后台化后，这种情况用户完全无感。

---

### [ ] A-2 SD 卡频率 20 → 40 MHz

| 项 | 内容 |
|---|---|
| 位置 | `main/boards/freenove-esp32s3-display-2.8-lcd/freenove-esp32s3-display-2.8-lcd.cc:78`（SDMMC）、`:132`（SDSPI） |
| 现状 | `host.max_freq_khz = SDMMC_FREQ_DEFAULT`（20 MHz），4-bit |
| 改法 | 两处都改成 `SDMMC_FREQ_HIGHSPEED`（40 MHz），保持 `slot.width = 4` |
| 收益 | 顺序读写接近翻倍；聊天记录落盘阻塞时间减半；听写音频加载更快 |
| 风险 | 低。4-bit 40 MHz 是 SD 规范标准档，驱动会自动降速兜底 |
| 验证 | 开机确认 `TF 卡 SDMMC 挂载成功`；连续对话 10 轮看聊天记录是否完整；读一个大文件测速 |
| 回滚 | 改回 `SDMMC_FREQ_DEFAULT` |

> 注意：**两处都要改**（SDMMC 与 SDSPI 两条挂载路径）。

---

### [ ] A-3 LVGL 绘制缓冲 24 → 40 行

| 项 | 内容 |
|---|---|
| 位置 | `main/display/lcd_display.cc:255`（SPI LCD 路径，**不是** 314 的 RGB 路径） |
| 现状 | `.buffer_size = width_ * 24` = 5,760 px = 11.5 KB ×2（双缓冲已开） |
| 改法 | 改成 `width_ * 40`（19,200 px = 38.4 KB ×2），缓冲仍在内部 RAM + DMA |
| 收益 | flush 次数 14 → 8 次，滚动/表情切换更顺 |
| 风险 | 低~中。内部 RAM 会多占约 23 KB（当前最低水位 33 KB，需复测） |
| 验证 | 连续滑动设置页、切换表情观察流畅度；看 `SystemInfo` 的 `minimal sram` 是否跌破 20 KB |
| 回滚 | 改回 `width_ * 24` |

---

## 批次 B · 需要实测权衡（与 A 分开验证，便于定位问题）

### [ ] B-1 LVGL 任务优先级 1 → 3

| 项 | 内容 |
|---|---|
| 位置 | `main/display/lcd_display.cc:241` |
| 现状 | `port_cfg.task_priority = 1`；同系统内 `audio_input=8`、`audio_output=4`、`opus_codec=2` |
| 改法 | 改成 `3`（高于 opus 2，低于 audio_output 4） |
| 收益 | 音频忙时 UI 不再被饿死，点击/滚动更跟手 |
| 风险 | **中**。抢 opus 时间片可能导致音频断续 |
| 验证 | **播一段长回复（10 s 以上），听是否有卡顿/爆音**；同时观察画面是否流畅。若有断续 → 降到 2 |
| 回滚 | 改回 `1` |

> 这是本清单里唯一"必须听声音验证"的一项，不要和 A 批次一起上，否则出问题分不清是哪个。

---

### [ ] B-2 Wi-Fi 省电模式动态切换

| 项 | 内容 |
|---|---|
| 位置 | `main/boards/common/wifi_board.cc:302`（`SetPowerSaveLevel`） |
| 现状 | 固定 `LOW_POWER (MAX_MODEM)`，MQTT 下行延迟增加约 100~300 ms |
| 改法 | 进入聆听/对话时切 `HIGH_THROUGHPUT`，回待机 5 s 后切回 `LOW_POWER` |
| 收益 | 对话响应更跟手 |
| 风险 | 中。需与现有 `PowerSaveTimer` 逻辑协调，避免状态互相覆盖 |
| 验证 | `WifiStation: Setting WiFi power save level:` 日志是否按预期切换；体感回复开头停顿是否变短 |
| 回滚 | 恢复固定 LOW_POWER |

---

## 批次 C · 顺手改（可与任意批次合并）

### [ ] C-1 ChatLog CRC16 逐位 → 查表

| 项 | 内容 |
|---|---|
| 位置 | `main/features/chat_history/chat_log.cc:240` |
| 现状 | 每字节 8 次循环，512 B 正文 = 4,096 次 |
| 改法 | 改用 256 项 const 查表（CRC16/MODBUS 表） |
| 收益 | 约 8 倍提速，绝对值很小（一轮一次） |
| 风险 | 极低，但**必须保证 CRC 结果一致** |
| 验证 | 跑现有存储格式测试（含断电截断用例） |
| 回滚 | 还原原循环 |

---

## 明确不做（附理由，避免反复讨论）

| 项 | 理由 |
|---|---|
| SPI 60 → 80 MHz | 超频，会花屏/噪点且随温度漂移，不适合长期运行 |
| FREERTOS_HZ 100 → 1000 | 音频块 32 ms，10 ms tick 足够；提高只增加上下文切换开销 |
| 全屏单缓冲 + full_refresh | 150 KB 全帧传输拿不到 DMA bounce buffer，现有 strip 方案是正确折中 |
| LVGL 缓冲放 PSRAM | 当前 `buff_spiram=0`（内部 RAM + DMA 直取）比放 PSRAM 快，不要改 |
| 关闭 NimBLE 协议栈 | 你已明确要求"其它不关闭"。补充信息：`ble_keyboard` 功能包本就关闭，协议栈实际未被业务使用，如将来确认彻底不用，关掉可回收 40~60 KB RAM |

---

## 执行记录

| 批次 | 构建 | 烧录 | 验证结果 | 日期 |
|---|---|---|---|---|
| A | ⬜ | ⬜ | | |
| B-1 | ⬜ | ⬜ | | |
| B-2 | ⬜ | ⬜ | | |
| C-1 | ⬜ | ⬜ | | |

---

## 通用注意事项

1. **构建命令**：`python scripts\feature_build.py --profile daily`（PowerShell，勿用 `>` 重定向，曾导致 gcc 探测进程僵死卡死）
2. **卡死处理**：ninja CPU 不涨且 10 分钟无新 obj → 杀 `ninja`/`feature_build`/`idf.py`/所有 `xtensa` 进程，删 `build/profiles/daily/.ninja_lock` 后重启
3. **烧录**：只写 app 分区，保留 NVS
   ```
   python -m esptool --chip esp32s3 -p COM4 -b 460800 --before default-reset --after hard-reset write-flash 0x20000 build/profiles/daily/xiaozhi.bin
   ```
4. **每批只改一类**，便于出问题时定位归因
