# 笔顺学习功能包

依赖 TF 卡中的笔顺数据包，提供：

- 汉字笔顺动画页（`StrokeStore` + LCD 页面）
- 按年级翻字、逐笔播放、拼音与笔画名称
- `self.stroke.*` MCP 语音工具

## 开关

```text
CONFIG_FEATURE_STROKE_ORDER=y
```

关闭后不编译本目录的数据层与 MCP 注册代码，也不链接 `font_pinyin.c`。LCD 中的“笔画”入口保留为不可用状态。

## 构建

```bash
python3 scripts/feature_build.py --profile full
```
