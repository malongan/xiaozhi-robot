# 功能包与组合构建

定制版开始把可裁剪功能收进 `main/features/<id>/`，由 Kconfig 开关和 profile 决定是否编译。目标是同一套源码按需组合，而不是维护多份固件分支。

> 新增或拆分功能前先读：[功能包开发指导](feature-development-guide.md)。

## 当前功能包

| 功能包 | Kconfig | 状态 |
|---|---|---|
| `english_dictation` | `CONFIG_FEATURE_ENGLISH_DICTATION` | MVP，默认开启 |
| `stroke_order` | `CONFIG_FEATURE_STROKE_ORDER` | MVP，默认开启 |
| `ble_keyboard` | `CONFIG_FEATURE_BLE_KEYBOARD` | MVP，默认开启，依赖 `BT_ENABLED` |
| `web_file_transfer` | `CONFIG_FEATURE_WEB_FILE_TRANSFER` | 目标板开启，`full` 启用，依赖 SD 卡与 Wi-Fi |

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
profiles/
├── base.json
├── english.json
├── stroke.json
├── keyboard.json
├── web_file_transfer.json
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
```

## Profile

- `base`：关闭英语听写、笔顺学习、蓝牙键盘和网络文件传输，用于最小固件与体积基线。
- `english`：仅英语听写，用于验证功能包隔离。
- `stroke`：仅笔顺学习，用于验证功能包隔离。
- `keyboard`：仅蓝牙键盘，用于验证功能包隔离。
- `web_file_transfer`：仅网络文件传输，用于验证功能包隔离。
- `full`：全部已登记功能包，保持当前完整功能。

```json
{
  "name": "base",
  "features": {
    "english_dictation": false,
    "stroke_order": false,
    "ble_keyboard": false,
    "web_file_transfer": false
  }
}
```

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

## 添加新功能包

1. 建 `main/features/<id>/`，放实现、`feature.json`、`README.md`。
2. 在 `main/Kconfig.projbuild` 增加 `FEATURE_<ID>` 开关。
3. 在 `main/CMakeLists.txt` 用该开关包住源文件。
4. 在 `profiles/*.json` 增加该功能包的布尔值。
5. 用 `feature_build.py` 构建开启和关闭两种配置，检查体积、符号和主机测试。
6. 如果功能涉及 LCD 页面，优先把页面注册入口收进功能包，避免核心类依赖具体功能。

## 限制

- 英语听写、笔顺、蓝牙键盘与网络文件传输页面仍嵌在 `main/display/lcd_display.*`。关闭功能后页面不创建、功能实现不编译，但抽屉里的入口保留并显示为不可用。
- 关闭蓝牙键盘只移除功能包，不会关闭 ESP-IDF 的 BT 协议栈；`BT_ENABLED` 仍由根 `sdkconfig` 决定。
- profile 目前只切换功能包开关，不负责板型或分区差异；板型仍以根目录 `sdkconfig` 为准。
- 这是编译期组合，不是运行时插件；ELF 动态加载仍不采用。
