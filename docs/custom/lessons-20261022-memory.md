# 2026-10-22 复盘：内部 RAM 优化与启动提速（含蓝牙运行时开关）

本轮目标：解决"蓝牙常开时内部 RAM 只剩 15.6 KB，导致语音/TLS/新特性随时可能 OOM"的
问题，并顺手把启动时间压下来。以下数据全部来自本机 Freenove ESP32-S3 Display 2.8"
实机串口实测，不是估算。

## 一、结论速览

| 指标 | 优化前 | 优化后 |
| --- | --- | --- |
| 内部 RAM 空闲（蓝牙开） | 15,639 B | **~57–60 KB** |
| 内部 RAM 空闲（蓝牙关） | 78–82 KB | **~94 KB** |
| 蓝牙自身开销 | ~66 KB | **~38 KB**（另 27.5 KB 在 PSRAM） |
| 启动到 UI 可交互 | 3,788 ms | **1,795 ms** |
| 应用初始化完成 | 4,192 ms | **2,190 ms** |
| 静态 DIRAM | 178,379 B / 341,760 B（52.19%） | 不变 |
| app 分区 | 3.32 MB / 5.94 MB | 3.32 MB / 5.94 MB |

净收益：蓝牙开着也比原来"关蓝牙"多 12 KB；关掉蓝牙多 41 KB。

## 二、做了什么

### 1. 蓝牙运行时开关（设置页新增 switch）

- `MediaBle::Stop()`：断开链路 → `nimble_port_stop()` → `nimble_port_deinit()`
  （内部会 disable + deinit controller），清空句柄，回调通知 UI。
- `MediaBle::Start()` 失败时把 `started_` 复位，允许重试。
- 状态持久化：NVS 命名空间 `bluetooth`，键 `enabled`，默认开；启动时按它决定是否 Start。
- UI：设置页连接卡片内的 switch，三种状态 已连接/未连接/已关闭，另有 正在开启…/正在关闭…。
  关闭时列表与「清空」置灰；原「断开」按钮移除（关 = 断开，开 = 重新广播）。

**关键结论**：`esp_bt_mem_release()` 在本芯片上只能回收 ~1.3 KB 链接期 .bss，
真正省内存的是 `nimble_port_deinit()`。但 `libbtdm_app.a` 的 **21,313 B 静态 DRAM
（控制器 BSS）运行时无法回收**，想省掉只能编译期 `CONFIG_BT_ENABLED=n`。

### 2. 蓝牙内存搬到 PSRAM（配置层）

`main/boards/freenove-esp32s3-display-2.8-lcd/config.json` 的 `sdkconfig_append`：

```json
"CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1",
"CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y",
"CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256"
```

- `MEM_ALLOC_MODE_EXTERNAL` 让 NimBLE host 池（msys/ACL/EVT/GATT cache，约 20–28 KB）
  走 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`；controller 缓冲仍在内网。
- `MAX_CONNECTIONS 3→1`：同时只连一个遥控器，省 5–8 KB。
- `ALWAYSINTERNAL 1024→256`：小分配也允许落到 PSRAM，空载多出 13–16 KB。
  之前 MP3 分支做过 A/B（5/5 干净启动），本轮复测同样稳定。

实测拆分：开蓝牙 = 内部 37.8 KB + PSRAM 27.5 KB。

### 3. 启动提速

启动阶段实测（优化前）：

| 阶段 | 耗时 |
| --- | --- |
| SD 挂载 + 列目录 | 0.14 s |
| StrokeStore 加载 medians（1.61 MB） | 0.50 s |
| **EnglishStore 音频自检（20 个文件）** | **1.71 s** |
| LCD 初始化 | 0.27 s |
| UI 布局 / 字体修复 | 0.84 s |
| BLE 启动 | 0.09 s |

两个改动：

1. **笔顺 medians 懒加载**：`StrokeStore::Init()` 只读 index + pinyin；
   新增 `EnsureMedians()`，在 `LoadMedians()` 首次调用时读 1.61 MB。
   代价：第一次进笔顺页略慢；收益：启动省 0.28 s，且顺手省下 1.6 MB PSRAM 常驻。
2. **英语音频自检后台化**：SD 上每次 `fopen` 约 85 ms，20 个文件必然 1.7 s。
   改为启动后 8 s 由 `eng_chk` 任务跑；同时 `CheckAudio()` 改成只读 4 字节 Ogg 头，
   不再把整文件读进内存。

结果：UI 3,788 → **1,795 ms**，应用就绪 4,192 → **2,190 ms**。

## 三、踩坑与教训（最重要）

### 1. 栈高水位只能证明"空闲够用"，不能代表峰值

把 `opus_codec` 栈从 24 KB 砍到 18 KB（依据：空闲时 high-water mark 显示还剩 16.6 KB），
结果**一对话就 `A stack overflow in task opus_codec` → rst:0xc 重启**，
表现像"WiFi 掉线重连"。

教训：**音频 / TLS / 编解码这类突发路径，必须在本帧真实负载下测高水位**，
待机数据会严重低估。已全部回滚（opus 24 KB、audio_input 6 KB、httpd 8 KB）。

### 2. 任务栈不能放 PSRAM（对特定任务）

蓝牙开关任务最初用 PSRAM 栈，切换瞬间 BT 初始化会短暂关 flash cache，
触发 `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu ... esp_task_stack_is_sane_cache_disabled()` 重启。
凡是可能碰 flash / NVS / BT init 的任务必须用内部 RAM 栈。

### 3. 日志会骗人

`assets: The storage free size is 20672 KB` 是 **MMU 映射窗口剩余页数 × 64 KB**，
不是 SD 卡剩余空间。

### 4. 既有无害日志，不要误判

`mbedtls_ssl_fetch_input error=76` / `esp-tls-mbedtls: read error :-0x004C`
在本项目长期存在，与本轮改动无关；`MediaBle: adv set fields failed: 30`
出现在关闭蓝牙瞬间（host 正在下去），也无害。

## 四、当前占用画像（v4）

静态 DIRAM 178,379 B：

| 子系统 | 占用 |
| --- | --- |
| 系统 / 内核 | 79.5 KB（spi_flash 18.5、esp_hw_support 13.2、freertos 10.1、heap 6.6） |
| WiFi 协议栈（含 wpa） | 36.8 KB |
| 蓝牙控制器 | 23.8 KB（运行时不可回收） |
| 应用与 C++ 运行时 | 27.7 KB |
| 音频（esp_audio_processor） | 5.0 KB |
| lwIP | 4.3 KB |
| LVGL 静态 | 0.6 KB |

动态内部 RAM（蓝牙开约 106 KB）：

| 项 | 占用 |
| --- | --- |
| 任务栈合计 | ~80 KB（opus 24 KB 最大，其次 httpd/main 各 8 KB、wifi 6.7 KB、LVGL 6.1 KB、audio_input 6.1 KB） |
| LVGL 双缓冲（DMA，内部） | 23 KB（240×24 行 ×2） |
| WiFi 动态 TX 缓冲 | 最高 ~51 KB（32×~1.6 KB，按需） |
| esp-sr AFE / 唤醒词 | ~20–30 KB |
| TLS 会话 | ~16–30 KB（仅连接时） |
| TCP 窗口 | 11.5 KB / 连接 |

PSRAM：LVGL 池 1 MB + 扩展 2.56 MB + 图片缓存 2 MB ≈ 5.56 MB；
笔顺 medians 1.6 MB 已改懒加载。

## 五、可复用的优化清单

| 手段 | 收益 | 风险 |
| --- | --- | --- |
| 蓝牙运行时 deinit（本轮已做） | 62–66 KB | 低，可逆 |
| NimBLE 池进 PSRAM + 单连接（已做） | 28 KB 内部 | 低 |
| `SPIRAM_MALLOC_ALWAYSINTERNAL=256`（已做） | 13–16 KB | 低（需 A/B 启动验证） |
| 启动期大块数据懒加载（已做） | 1.8 s | 低，首次进入变慢 |
| 自检类 IO 后台化（已做） | 1.7 s | 低 |
| WiFi TX 缓冲 32→16 | ~25 KB 峰值 | 低 |
| LVGL 双缓冲改单缓冲 | 11.5 KB | 中（可能撕裂） |
| httpd 首次访问才启动 | 8 KB + 缓冲 | 中（网页服务不再自动可用） |
| 任务栈瘦身 | 视任务 | **高：必须按真实负载测** |
| 编译期关蓝牙 | 21 KB 静态 | 高（失去遥控器） |

## 六、测量方法（可复用）

1. 串口抓包：`python readlog.py COM4 <秒> <输出> [reset]`（第 4 参数要写 `reset` 字面量）。
2. 堆：`heap_caps_get_free_size/largest_free_block/minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_SPIRAM)`。
3. 任务栈与 CPU：`uxTaskGetSystemState()` + `ulRunTimeCounter`（本项目已开
   `CONFIG_FREERTOS_USE_TRACE_FACILITY` / `GENERATE_RUN_TIME_STATS`）。
   `usStackHighWaterMark` 单位是字节。
4. 分阶段启动耗时：临时把 `CONFIG_LOG_DEFAULT_LEVEL_WARN` 改 INFO 拿到各子系统时间戳，
   再在 `app_main` / `Application::Initialize()` 里插几条 mark（诊断代码用完即删）。
5. 只烧 app 到 `0x20000` 可保留 NVS / 蓝牙配对：

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 460800 write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB 0x20000 build\xiaozhi.bin
```

## 七、回退点

| tag | 内容 |
| --- | --- |
| `bt-switch-v1` | 蓝牙开关（优化前基线） |
| `bt-switch-v2` | + NimBLE 池进 PSRAM、ALWAYSINTERNAL、单连接 |
| `bt-switch-v3` | + 启动优化（含已被回滚的栈瘦身，**不建议使用**） |
| `bt-switch-v4` | 回滚栈瘦身后的稳定版（当前） |

## 八、下次可以做

1. WiFi 动态 TX 缓冲 32→16（省 ~25 KB 峰值），需跑一次上传/语音并发验证。
2. 一次完整对话结束后的任务栈高水位，再决定 opus 能否安全降到 20 KB。
3. 内部 RAM 低于 15 KB 时打 WARN（提前预警，而不是等崩）。
4. LVGL 单缓冲 / 更小条带的流畅度对比。
