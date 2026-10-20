# 英语词库生成工具

把 CSV 词表编译成 TF 卡上的词库（单词发音用 macOS 内置 `say` 离线合成）。

## 词表来源

| 文件 | 说明 |
|---|---|
| `data/wy4a_textbook.csv` | 外研版四年级上册课本词（从用户的 vocabulary.md 解析） |
| `data/wy4a_words.csv` | 课本词 + 手工标注音标（`add_phonetics.py` 生成） |
| `data/extra_words.csv` | 分类补充词（食物水果/数字颜色时间/动物场所物品/日常动作形容） |
| `data/merged_words.csv` | 最终合并词表（课本优先，补充去重后追加） |
| `data/pep4a_words.csv` | 人教 PEP 四年级上册词表（备选） |
| `data/extra_grades.csv` | 五年级 / 六年级拓展词汇（`build_extra_grades.py` 生成） |

单元编号：

```
1..6  课本 Unit 1..6
7     课本板块（Plays 戏剧）
8     食物水果        9   数字颜色时间
10    动物场所物品    11  日常动作形容
12    五年级          13  六年级
```

合计 721 词（184 课本 + 537 补充）。

## 用法

```bash
# 1) 合并课本词与分类补充词
python3 merge_wordlibs.py --textbook data/wy4a_words.csv \
    --extra data/extra_words.csv --out data/merged_words.csv
# 再叠加五/六年级拓展词（合并脚本只支持一个 --extra，可分两次后自行拼接）

# 2) 生成到 TF 卡
python3 build.py --csv data/merged_words.csv --out /Volumes/TF/english \
    --voice Samantha --name "外研版四年级上册+分类补充" --jobs 8

# 3) 校验
python3 verify.py --out /Volumes/TF/english
```

`add_phonetics.py` 用于给课本词表补音标（新词表需手工维护 PHONETICS 字典）。

## 产物

```
english/
├── words.csv      词表源文件（UTF-8）
├── index.bin      索引（固件读取）
├── meta.txt       元信息
└── audio/w0001.ogg ...  16kHz 单声道 Opus，每词约 1.6KB
```

格式细节见 `docs/english-wordlib-format.md`。

## 依赖

- macOS：`say`（内置）、`afconvert`（内置）
- `ffmpeg`（需带 libopus）：`brew install ffmpeg`

## 音色

| 音色 | 口音 |
|---|---|
| `Samantha` | 美音 en_US（默认） |
| `Daniel` | 英音 en_GB |
| `Karen` | 澳音 en_AU |
