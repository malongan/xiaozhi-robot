# 英语听写功能包

依赖 TF 卡中的 `/sdcard/english` 词库与音频，提供：

- 英语听写、单词卡播放（`EnglishPlayer`）
- 词库索引与音频读取（`EnglishStore`）
- `self.english.*` MCP 语音工具

## 开关

```text
CONFIG_FEATURE_ENGLISH_DICTATION=y
```

关闭后不编译本目录的播放器、词库与 MCP 注册代码，也不链接 `font_english.c`。LCD 中的英语入口保留为不可用状态。

## 构建

```bash
python3 scripts/feature_build.py --profile english
```
