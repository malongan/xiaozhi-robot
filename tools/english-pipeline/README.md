# 英语词库生成工具

把 CSV 词表编译成 TF 卡上的词库（单词发音用 macOS 内置 `say` 离线合成）。

## 用法

```bash
# 生成四年级上册词库到 TF 卡
python3 build.py --csv data/pep4a_words.csv --out /Volumes/TF/english \
    --voice Samantha --name "人教版PEP四年级上册" --jobs 8

# 校验
python3 verify.py --out /Volumes/TF/english
```

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
