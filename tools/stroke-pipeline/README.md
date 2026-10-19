# 笔顺数据管线（Stroke Pipeline）

从 Make Me A Hanzi 数据生成 TF 卡笔顺数据包：

```
/sdcard/szver.txt   "1\n"
/sdcard/index.bin   "SZBI" + u16 ver + u16 count + u32 medians_size + u32 glyph_size[6]
/sdcard/medians.bin 每字笔顺中位线
/sdcard/glyphs/g1..6.bin  160x160 1bpp 位级 RLE 字模
/sdcard/pinyin.json 拼音
```

## 用法

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install pypinyin pillow
python3 fetch.py            # 下载源数据到 raw/（约 68MB）
python3 build.py            # 生成 output/（约 10MB，9470 字）
python3 verify.py           # 校验 + 生成抽样图 output/samples/
./copy-to-sd.sh /Volumes/你的TF卡   # 写入 FAT32 卡（macOS 建议 dd conv=fsync）
```

## 许可

笔顺源数据来自 [Make Me A Hanzi](https://github.com/skishore/makemeahanzi)（Arphic Public License，见 `NOTICE.txt`）。生成的字模/中位线数据仅用于本项目的字库展示。
