# 功能包与组合构建

定制版开始把可裁剪功能收进 `main/features/<id>/`，由 Kconfig 开关和 profile 决定是否编译。目标是同一套源码按需组合，而不是维护多份固件分支。

> 2026-10-23 功能包拆分已合并到 `main`，合并点 tag 为 `custom-feature-packs-20261023`；`custom-final-20261022-r3` 保持原发布内容不变。

> 2026-10-23 新增 `music_player` 功能包（SD 卡本地音乐）。它晚于上面的合并点，未包含在 `custom-feature-packs-20261023` tag 中，需单独验证与发布。

> 新增或拆分功能前先读：[功能包开发指导](feature-development-guide.md)。

## 当前功能包

| 功能包 | Kconfig | 状态 |
|---|---|---|
| `english_dictation` | `CONFIG_FEATURE_ENGLISH_DICTATION` | MVP，默认开启 |
| `stroke_order` | `CONFIG_FEATURE_STROKE_ORDER` | MVP，默认开启 |
| `ble_keyboard` | `CONFIG_FEATURE_BLE_KEYBOARD` | MVP，默认开启，依赖 `BT_ENABLED` |
| `web_file_transfer` | `CONFIG_FEATURE_WEB_FILE_TRANSFER` | 目标板开启，`full` 启用，依赖 SD 卡与 Wi-Fi |
| `music_player` | `CONFIG_FEATURE_MUSIC_PLAYER` | 新增，默认关闭；目标板与 `full` 启用，依赖 SD 卡与本地解码器 |
| `chat_history` | `CONFIG_FEATURE_CHAT_HISTORY` | 新增，默认关闭；`daily`、`chat_history`、`full` 启用，依赖 SD 卡 |

英语功能包包含：

- `english_player.*`：听写/单词卡播放器
- `english_tool.*`：TF 卡词库与音频读取
- `english_dictation_tools.*`：`self.english.*` MCP 工具
- `display/font_english.c`：音标字体，仅在功能开启时链接

笔顺功能包包含：

- `stroke_tool.*`：TF 卡笔顺索引、字模与拼音读取
- `stroke_order_tools.*`：`self.stroke.*` MCP 工具
- 笔顺动画页的绘制、翻字与定时器逻辑
- `display/font_pinyin.c`：拼音字体，仅在功能开启时链接

蓝牙键盘功能包包含：

- `media_ble.*`：BLE HID 后端、配对/解绑、连接状态与按键发送
- 媒体控制面板、全键盘、数字键盘三个页面
- macOS 快捷键与设置页蓝牙开关
- 抽屉“键盘”入口；关闭时入口保留但显示为不可用

网络文件传输功能包包含：

- `web_file_server.*`：HTTP 服务、目录/上传/下载/删除 API、可选 PIN 校验
- `web_assets/index.html`：管理页面，构建时 gzip 后嵌入固件
- 网络页、上传进度、服务启停与 Wi-Fi 连接生命周期
- 抽屉“网络”入口；关闭时入口保留但显示为不可用

音乐播放器功能包包含：

- `music_library.*`：`/sdcard/music` 索引、ID3/FLAC/WAV/M4A 元数据与时长解析、
  索引缓存、检索、最近播放
- `media_player.*`：`esp_audio_codec` 本地解码（MP3/AAC/M4A/FLAC/WAV/OGG）、
  单声道下混、重采样到声卡输出率、分块推入播放队列、状态机与暂停/续播
- `AudioService` 的 PCM 直投通道（`PushPlaybackPcm` / `SetMediaGeneration`）
- iPod 风格音乐页、抽屉“音乐”入口、`self.music.*` MCP 工具
- 列表翻页：标题右侧 `‹ 1/12 ›`，两个 39 px 的点击区覆盖到中间页码（点页码左半=上一页、
  右半=下一页），首页/末页箭头置灰，只有一页时整组隐藏；列表右缘保留细滚动条
- 索引自动失效：整棵 `/sdcard/music` 树的签名（子目录、文件名、大小、mtime）变化时
  重新扫描；音乐页打开时、每 30 秒以及每次点播前都会检查，`self.music.rescan`
  可立即强制重建（索引缓存版本为 2，旧的 v1 缓存会自动作废一次）
- 关闭时入口保留但显示为不可用，且不链接本地解码器

音乐模式的语音流程：

- AI 点播成功后自动打开音乐页；回复（TTS）结束立刻开始播放，不再等设备回到 `Idle`
  才恢复，也不会因为连续聆听而一直暂停
- 音乐模式下唤醒词**不关闭**音乐页；唤醒后进入 5 秒指令窗口，窗口内音乐暂停
  （本板无硬件回采/AEC，播放会串进麦克风），指令的 TTS 结束后再给 5 秒
- 5 秒无指令自动退出聆听，音乐继续、页面保留；再次控制需要先说唤醒词
- 只有 `self.music.stop`（“退出听歌”）才关闭页面并停止播放
- 相关实现：`Application::EnterMusicMode/LeaveMusicMode/ExtendMusicCommandWindow`、
  `MediaPlayer::SetCommandWindowActive`、`Display::ShowMusicPlayer`

## 聊天记录（chat_history）

把每一轮对话追加写到 TF 卡，抽屉第 7 格"记录"进入只读回看页。

- **存储**：`/sdcard/chatlog/yyyymmdd.log`，24 字节定长头 + 变长 UTF-8 正文，
  追加写 + `fsync`，断电最多丢最后一批（≤10 条）。
- **容量**：单条正文 ≤512 B，单日 ≤2 MB，保留 7 天，攒批刷盘（10 条或 30 秒）。
- **落盘时机**：用户消息即时存；助手回复是流式下发的，攒到这一轮结束整段存，
  避免一句话被拆成多条碎片。
- **防打断（三层）**：回看页打开时关唤醒词、关语音处理，并在
  `OnIncomingAudio` 处静默丢弃服务器下行 TTS。只静音不丢弃会让音频排队，
  退出回看后一次性播出来。
- **体积**：相对 `base` 增加 text 10.5 KB、data 0.6 KB、bss 0。
- **符号**：关闭侧（`base`/`english`）`ChatLog::` 符号数为 0，开启侧（`chat_history`）15 个。

## 目录约定

```text
main/features/english_dictation/
├── feature.json
├── README.md
├── english_player.cc
├── english_player.h
├── english_tool.cc
├── english_tool.h
├── english_dictation_tools.cc
└── english_dictation_tools.h
main/features/stroke_order/
├── feature.json
├── README.md
├── stroke_tool.cc
├── stroke_tool.h
├── stroke_order_tools.cc
└── stroke_order_tools.h
main/features/ble_keyboard/
├── feature.json
├── README.md
├── media_ble.cc
└── media_ble.h
main/features/web_file_transfer/
├── feature.json
├── README.md
├── web_file_server.cc
├── web_file_server.h
└── web_assets/
    └── index.html
main/features/music_player/
├── feature.json
├── README.md
├── music_library.cc
├── music_library.h
├── media_player.cc
└── media_player.h
profiles/
├── base.json
├── english.json
├── stroke.json
├── keyboard.json
├── web_file_transfer.json
├── music.json
├── no_keyboard.json
└── full.json
scripts/feature_build.py
```

`feature.json` 记录功能包 ID、Kconfig、源文件和依赖，供构建脚本读取：

```json
{
  "id": "stroke_order",
  "config": "CONFIG_FEATURE_STROKE_ORDER",
  "default": true,
  "sources": [
    "features/stroke_order/stroke_tool.cc",
    "features/stroke_order/stroke_order_tools.cc"
  ]
}
```

CMake 只在对应开关打开时追加源文件和专属字体：

```cmake
if(CONFIG_FEATURE_ENGLISH_DICTATION)
    list(APPEND SOURCES
        "display/font_english.c"
        "features/english_dictation/english_tool.cc"
        "features/english_dictation/english_player.cc"
        "features/english_dictation/english_dictation_tools.cc"
    )
endif()

if(CONFIG_FEATURE_STROKE_ORDER)
    list(APPEND SOURCES
        "display/font_pinyin.c"
        "features/stroke_order/stroke_tool.cc"
        "features/stroke_order/stroke_order_tools.cc"
    )
endif()

if(CONFIG_FEATURE_BLE_KEYBOARD)
    list(APPEND SOURCES
        "features/ble_keyboard/media_ble.cc"
    )
endif()

if(CONFIG_FEATURE_WEB_FILE_TRANSFER)
    list(APPEND SOURCES "features/web_file_transfer/web_file_server.cc")
    set(WEB_INDEX_HTML
        "${CMAKE_CURRENT_SOURCE_DIR}/features/web_file_transfer/web_assets/index.html")
    set(WEB_INDEX_GZ
        "${CMAKE_CURRENT_BINARY_DIR}/features/web_file_transfer/index.html.gz")
    # gzip 后加入 WEB_ASSETS，生成 _binary_index_html_gz_* 符号
endif()

if(CONFIG_FEATURE_MUSIC_PLAYER)
    list(APPEND SOURCES
        "features/music_player/music_library.cc"
        "features/music_player/media_player.cc"
    )
endif()
```

## Profile

- `base`：关闭英语听写、笔顺学习、蓝牙键盘、网络文件传输和音乐播放，用于最小固件与体积基线。
- `english`：仅英语听写，用于验证功能包隔离。
- `stroke`：仅笔顺学习，用于验证功能包隔离。
- `keyboard`：仅蓝牙键盘，用于验证功能包隔离。
- `web_file_transfer`：仅网络文件传输，用于验证功能包隔离。
- `music`：仅 SD 卡音乐播放器，用于验证功能包隔离。
- `no_keyboard`：除蓝牙键盘外全部开启（保留 BT 协议栈），用于日常不带键盘遥控的固件。
- `full`：全部已登记功能包，保持当前完整功能。

```json
{
  "name": "base",
  "features": {
    "english_dictation": false,
    "stroke_order": false,
    "ble_keyboard": false,
    "web_file_transfer": false,
    "music_player": false
  }
}
```

## 配置迁移

2026-10-23 将网页传文件开关统一到功能包命名：

| 旧配置 | 新配置 |
|---|---|
| `CONFIG_WEB_FILE_SERVER` | `CONFIG_FEATURE_WEB_FILE_TRANSFER` |
| `CONFIG_WEB_FILE_SERVER_PORT` | `CONFIG_FEATURE_WEB_FILE_TRANSFER_PORT` |
| `CONFIG_WEB_FILE_SERVER_PIN` | `CONFIG_FEATURE_WEB_FILE_TRANSFER_PIN` |

目标板 `config.json` 已改为新开关。根目录 `sdkconfig` 是本地生成文件、不入库；拉取新代码后请用 `scripts/build.py` 重新生成配置，或用 `idf.py menuconfig` 打开新开关。不要只手工改旧 `sdkconfig` 的键名后继续使用旧构建目录。

## 主线与多处开发

- `github` 是定制仓，`origin` 是上游；功能开发在独立分支进行，验证后合入 `github/main`。
- `main` 是集成分支，`custom-final-*` tag 对应已发布固件；不要移动或覆盖旧 tag。
- 合并前至少构建 `base`、`english`、`stroke`、`keyboard`、`web_file_transfer`、`music`、`full` 七个 profile，并检查关闭侧无功能符号、开启侧有符号。
- 只改功能包且 `full` 行为不变时，可以用 fast-forward 合并；涉及核心行为、分区或 NVS 时按发布流程重新做真机验证。
- 本次功能包合并点使用 tag `custom-feature-packs-20261023`，后续固件发布另建 `custom-final-*` tag 和版本说明。

## 构建

先进入 ESP-IDF 环境：

```bash
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.12_env
source "$HOME/esp/esp-idf/export.sh"
```

列出功能包和 profile：

```bash
python3 scripts/feature_build.py --list
```

按 profile 构建：

```bash
python3 scripts/feature_build.py --profile base
python3 scripts/feature_build.py --profile english
python3 scripts/feature_build.py --profile stroke
python3 scripts/feature_build.py --profile keyboard
python3 scripts/feature_build.py --profile web_file_transfer
python3 scripts/feature_build.py --profile full
```

脚本会复制根目录 `sdkconfig` 到 `build/profiles/<name>/sdkconfig`，只改功能包开关，因此板型、分区、Wi-Fi 等既有配置不会被 profile 覆盖。

只生成配置、不构建：

```bash
python3 scripts/feature_build.py --profile base --no-build
```

## 验证结果

2026-10-23 在 Freenove ESP32-S3 Display 2.8" 配置上对比：

| 配置 | xiaozhi.bin | Flash Code | Flash Data | DIRAM |
|---|---:|---:|---:|---:|
| `base`（全关） | 3,115,584 B | 2,313,476 B | 677,680 B | 153,995 B |
| `english`（仅英语） | 3,191,360 B | 2,349,964 B | 716,752 B | 154,219 B |
| `stroke`（仅笔顺） | 3,166,704 B | 2,328,408 B | 713,760 B | 154,091 B |
| `keyboard`（仅键盘） | 3,351,984 B | 2,501,908 B | 703,016 B | 178,043 B |
| `web_file_transfer`（仅传输） | 3,151,152 B | 2,342,064 B | 684,656 B | 154,043 B |
| `full`（全开） | 3,490,304 B | 2,581,756 B | 761,192 B | 178,395 B |
| `full - base` | +374,720 B | +268,280 B | +83,512 B | +24,400 B |

符号检查：

| 配置 | Stroke 相关符号 | English 相关符号 | BLE 键盘符号 | Web 传输符号 |
|---|---:|---:|---:|---:|
| `base` | 0 | 0 | 0 | 0 |
| `english` | 0 | 54 | 0 | 0 |
| `stroke` | 23 | 0 | 0 | 0 |
| `keyboard` | 0 | 0 | 21 | 0 |
| `web_file_transfer` | 0 | 0 | 0 | 20 |
| `full` | 23 | 54 | 21 | 20 |

主机测试 `python3 -m unittest discover -s scripts/tests` 通过。

2026-10-23 蓝牙键盘拆分后，五种 profile 均编译通过，`base/english/stroke` 中无 `MediaBle` 符号，`keyboard/full` 中保留。

2026-10-23 真机验证：拆分后的 `full` 固件（3,490,304 B）刷入 `0x20000` 后启动正常，TF 卡 29,476 MB，`StrokeStore` 9,470 字、`EnglishStore` 1,220 词均就绪；macOS 自动重连 `Xiaozhi-Media`，识别为 Keyboard（Battery 100%），无 panic。三面板按键未逐项人工点按。

2026-10-23 网络文件传输拆分后，六种 profile 均编译通过；`base/english/stroke/keyboard` 无 `WebFileServer` 符号，`web_file_transfer/full` 各 20 个，嵌入资源符号 2 个。新 `full` 固件（3,490,304 B，SHA256 `b8f19d28…1faf`）刷入 `0x20000` 后启动正常，`/api/info` 返回 `running:true` 且 SD 已挂载；实测新建目录、上传 29 B 文件、列表、下载内容一致、递归删除均成功；网络页触摸操作与断线停止已人工验证正常。

### 2026-10-23 音乐播放器功能包

加入 `music_player` 后重新构建全部七种 profile（构建目录 `build/profiles/<name>`）：

| 配置 | xiaozhi.bin | Flash Code | Flash Data | DIRAM |
|---|---:|---:|---:|---:|
| `base`（全关） | 3,116,176 B | 2,313,964 B | 677,776 B | 153,995 B |
| `english`（仅英语） | 3,191,952 B | 2,350,468 B | 716,848 B | 154,219 B |
| `stroke`（仅笔顺） | 3,167,216 B | 2,328,852 B | 713,840 B | 154,091 B |
| `keyboard`（仅键盘） | 3,352,448 B | 2,502,276 B | 703,112 B | 178,043 B |
| `web_file_transfer`（仅传输） | 3,151,792 B | 2,342,592 B | 684,768 B | 154,043 B |
| `music`（仅音乐） | 3,685,936 B | 2,667,824 B | 889,952 B | 158,207 B |
| `full`（全开） | 4,064,720 B | 2,940,004 B | 973,624 B | 182,607 B |
| `no_keyboard`（关键盘） | 3,830,048 B | 2,753,284 B | 948,288 B | 158,559 B |
| `music - base` | +569,760 B | +353,860 B | +212,176 B | +4,212 B |
| `full - base` | +945,584 B | +623,240 B | +295,688 B | +28,612 B |

关闭音乐后 `base` 只比拆分前多 176 B（多出的抽屉入口、提示字符串和
`Display::ShowMusicPlayer` 虚函数表项），`DIRAM` 不变。音乐包增加的体积主要来自
本地解码器（MP3/AAC/FLAC/WAV/OGG）与元数据解析代码，`DIRAM` 只增加 4,212 B。

符号检查（`xtensa-esp32s3-elf-nm -C`）：

| 配置 | MediaPlayer / MusicLibrary | MediaBle | WebFileServer | EnglishPlayer | StrokeStore |
|---|---:|---:|---:|---:|---:|
| `base` | 0 | 0 | 0 | 0 | 0 |
| `english` | 0 | 0 | 0 | 27 | 0 |
| `stroke` | 0 | 0 | 0 | 0 | 19 |
| `keyboard` | 0 | 20 | 0 | 0 | 0 |
| `web_file_transfer` | 0 | 0 | 20 | 0 | 0 |
| `music` | 78 | 0 | 0 | 0 | 0 |
| `full` | 78 | 20 | 20 | 27 | 19 |

主机测试 `python -m unittest discover -s scripts/tests`：110 项中 97 项通过，
12 项为 Windows 环境缺少外部命令（`python3`/`cmake`）导致的 `FileNotFoundError`，
1 项跳过，与本次改动无关。

真机验证：`full` 固件（4,064,720 B）刷入 `0x20000` 后启动正常、无 panic，唤醒词模型
加载正常。列表翻页按钮的点击区域（含页码左右半区）与音乐语音流程尚未人工验证，
需要在 `/sdcard/music` 放歌后逐项确认。

### 2026-10-23 性能与占用实测（full）

用临时 INFO + 性能诊断固件（`build/profiles/full-perf`，测完已恢复源码并刷回发布版）
在 Freenove ESP32-S3 Display 2.8" 上实测。INFO 日志会让启动数字略高于发布版。

启动时间线（串口时间戳）：

| 阶段 | 时间 |
|---|---:|
| TF 卡挂载 | 0.30 s |
| StrokeStore 就绪（medians 懒加载） | 0.52 s |
| EnglishStore 词库就绪 | 0.67 s |
| LCD / LVGL 初始化 | 0.87 s |
| 首页布局完成（`Layout canvas`） | 0.96 s |
| BLE HID 开始广播 | 1.97 s |
| 板级/音频初始化完成 | 2.07 s |
| Wi-Fi 获得 IP、Web 服务启动 | 10.0 s（环境原因有一次信道重连，+5.4 s） |
| 资产校验、OTA 检查完成 | 12.2 s |
| 进入 idle、AFE/唤醒词就绪 | 13.3 s |
| 音乐索引（9 首） | 索引本身 < 0.1 s |

内存（内部 RAM / PSRAM，单位字节）：

| 状态 | 内部 free | 内部 largest | 内部 min（历史） | PSRAM free |
|---|---:|---:|---:|---:|
| 板级初始化后（Wi-Fi 前） | 114,731 | 69,632 | 41,884 | 4,133,152 |
| idle（Wi-Fi + AFE + 索引） | 54,803 | 25,600 | 25,272 | 3,622,904 |
| 播放 MP3 中 | 52,443 | 25,600 | 25,272 | 3,550,996 |

- 音乐播放占用：内部 RAM **−2.4 KB**，PSRAM **−72 KB**（10 KB 任务栈 + 解码器/重采样缓冲）
- 启动最紧的时刻是 AFE 创建（约 10–13 s），内部 RAM 最低 25.3 KB，之后回升
- PSRAM 大头：LVGL 2.5 MB 池 + 2 MB 图片缓存 + 64 KB 唤醒词缓存 + AFE ~490 KB

任务栈（`vTaskList` 剩余字节 / 配置值）：

| 任务 | 剩余 | 配置 | 说明 |
|---|---:|---:|---|
| `taskLVGL` | 560 | 6,144 | 最紧（约 9% 余量），与音乐包无关 |
| `main` | 4,676 | 8,192 | 实测时含诊断调用，生产路径更宽松 |
| `audio_input` | 4,824 | 6,144 | 播放时较空闲时少 576 B |
| `audio_output` | 2,880 | 4,096 | |
| `opus_codec` | 16,700 | 24,576 | 播放时不参与音乐解码 |
| `httpd` | 6,232 | 8,192 | |
| `music_player` | 7,772 | 10,240 | 峰值只用 2.5 KB |

CPU（`PrintTaskCpuUsage`，1 s 窗口，双核合计 100%）：

| 任务 | idle | 播放中 |
|---|---:|---:|
| `IDLE0` + `IDLE1` | 47% | 37% |
| `taskLVGL` | 41% | 40% |
| `music_player` | — | 10% |
| `audio_afe` | 5% | 5% |
| `audio_input` | 2% | 2% |

结论：音乐包解码 + 重采样约占用 **10% 总 CPU（≈ 单核 20%）**，内存余量充足；
当前最大的常驻 CPU 消耗是 idle 头像动画（`taskLVGL` 约 41%），与音乐无关。

头像动画的对比实验：把动画回调直接返回后，`taskLVGL` 从 **41% 降到 0%**
（IDLE0+IDLE1 从 47% 升到 91%），说明这 41% 几乎全部来自头像重绘。
据此做了不改变样式的降频处理：

- 空闲呼吸 25 fps → **15 fps**（66 ms/帧）；睡眠时 10 fps（100 ms/帧）
- 聆听/说话/思考/表情以及眨眼仍保持 25 fps，动效与原来一致
- 音乐、设置、网络、抽屉等整屏页面覆盖头像时不再运行动画（画面不可见）
- 内置 `avatar_period_ms_` 记录当前周期，只在档位切换时调用 `lv_timer_set_period`

按帧数比例估算，空闲 `taskLVGL` 从 41% 降到约 25%；后续若要继续降，
需要减少整张脸的每帧重绘（例如静态脸预渲染为图片，只动眼睛）。

CI 状态（2026-10-23）：`main` 的 Build Boards 共 20 个 job，19 个通过，目标板 `freenove-esp32s3-display-2.8-lcd` 通过；`waveshare-esp32-p4-nano-10.1-a-ci` 失败。该失败在 r3（run `39803223244`）已存在，原因是 WeChat 布局分支缺少 `ShowSettingsScreen`、`ApplyChatLayout` 等公共 LCD 方法定义，与本次功能包拆分无关。为避免扩大改动范围，本次合并没有修复它；需要单独整合 WeChat 布局与公共 LCD 方法。

## 添加新功能包

1. 建 `main/features/<id>/`，放实现、`feature.json`、`README.md`。
2. 在 `main/Kconfig.projbuild` 增加 `FEATURE_<ID>` 开关。
3. 在 `main/CMakeLists.txt` 用该开关包住源文件。
4. 在 `profiles/*.json` 增加该功能包的布尔值。
5. 用 `feature_build.py` 构建开启和关闭两种配置，检查体积、符号和主机测试。
6. 如果功能涉及 LCD 页面，优先把页面注册入口收进功能包，避免核心类依赖具体功能。

## 限制

- 英语听写、笔顺、蓝牙键盘、网络文件传输与音乐页面仍嵌在 `main/display/lcd_display.*`。关闭功能后页面不创建、功能实现不编译，但抽屉里的入口保留并显示为不可用。
- 关闭蓝牙键盘只移除功能包，不会关闭 ESP-IDF 的 BT 协议栈；`BT_ENABLED` 仍由根 `sdkconfig` 决定。
- 音乐包依赖 SD 卡与 `esp_audio_codec` 的本地解码器；关闭后不再链接这些解码器，但 `CONFIG_AUDIO_DECODER_*` 组件开关仍保持默认值，不随功能包变化。
- 音乐索引是 `/sdcard/music` 的编译期固定路径，最多 1000 首、3 层目录；不做拖动定位与单曲循环。
- profile 目前只切换功能包开关，不负责板型或分区差异；板型仍以根目录 `sdkconfig` 为准。
- 这是编译期组合，不是运行时插件；ELF 动态加载仍不采用。

## 版本记录

| 标签 | 内容 |
|---|---|
| `custom-feature-packs-20261023` | 功能包拆分合并到 `main`（英语/笔顺/蓝牙键盘/网络传输） |
| `custom-final-20261023` | 新增 `music_player` 功能包（第五个包） |
| `custom-final-20261023-r2` | 音乐浅色皮肤 + MENU 修复 |
| `custom-final-20261025` | 充电状态修复（板级 `freenove_power_monitor`）+ BOOT 长按关机；八个 profile 全量重编译 |
