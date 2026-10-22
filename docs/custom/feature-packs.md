# 功能包与组合构建

定制版开始把可裁剪功能收进 `main/features/<id>/`，由 Kconfig 开关和 profile 决定是否编译。目标是同一套源码按需组合，而不是维护多份固件分支。

## 当前功能包

| 功能包 | Kconfig | 状态 |
|---|---|---|
| `english_dictation` | `CONFIG_FEATURE_ENGLISH_DICTATION` | MVP，默认开启 |
| `stroke_order` | `CONFIG_FEATURE_STROKE_ORDER` | MVP，默认开启 |

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
profiles/
├── base.json
├── english.json
├── stroke.json
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
```

## Profile

- `base`：关闭英语听写和笔顺学习，用于最小固件与体积基线。
- `english`：仅英语听写，用于验证功能包隔离。
- `stroke`：仅笔顺学习，用于验证功能包隔离。
- `full`：全部已登记功能包，等同当前 r3 行为。

```json
{
  "name": "base",
  "features": {
    "english_dictation": false,
    "stroke_order": false
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
| `base`（全关） | 3,388,016 B | 2,531,004 B | 709,960 B | 178,075 B |
| `english`（仅英语） | 3,462,720 B | 2,566,436 B | 749,016 B | 178,299 B |
| `stroke`（仅笔顺） | 3,439,472 B | 2,546,288 B | 746,024 B | 178,171 B |
| `full`（全开） | 3,490,288 B | 2,581,748 B | 761,176 B | 178,395 B |
| `full - base` | +102,272 B | +50,744 B | +51,216 B | +320 B |

符号检查：

| 配置 | Stroke 相关符号 | English 相关符号 |
|---|---:|---:|
| `base` | 0 | 0 |
| `english` | 0 | 54 |
| `stroke` | 23 | 0 |
| `full` | 23 | 54 |

主机测试 `python3 -m unittest discover -s scripts/tests` 通过。

2026-10-23 真机验证：`full` 固件（3,490,288 B）刷入 `0x20000` 后启动正常，TF 卡词库加载无警告，笔顺页、笔顺语音工具与英语听写回归均通过。

## 添加新功能包

1. 建 `main/features/<id>/`，放实现、`feature.json`、`README.md`。
2. 在 `main/Kconfig.projbuild` 增加 `FEATURE_<ID>` 开关。
3. 在 `main/CMakeLists.txt` 用该开关包住源文件。
4. 在 `profiles/*.json` 增加该功能包的布尔值。
5. 用 `feature_build.py` 构建开启和关闭两种配置，检查体积、符号和主机测试。
6. 如果功能涉及 LCD 页面，优先把页面注册入口收进功能包，避免核心类依赖具体功能。

## 限制

- 英语听写与笔顺页面仍嵌在 `main/display/lcd_display.*`。关闭功能后页面不创建、功能实现不编译，但抽屉里的入口保留并显示为不可用。
- profile 目前只切换功能包开关，不负责板型或分区差异；板型仍以根目录 `sdkconfig` 为准。
- 这是编译期组合，不是运行时插件；ELF 动态加载仍不采用。
