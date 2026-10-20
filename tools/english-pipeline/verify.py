#!/usr/bin/env python3
"""校验 TF 卡英语词库：索引、音频、词表三者一致。

用法：
    python3 verify.py --out /Volumes/TF/english
"""

import argparse
import csv
import struct
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="词库目录（卡上的 english 目录）")
    args = ap.parse_args()

    root = Path(args.out)
    errors, warns = [], []

    # --- index.bin ---
    index_path = root / "index.bin"
    if not index_path.exists():
        print(f"[失败] 缺少 {index_path}")
        return 1
    data = index_path.read_bytes()
    if len(data) < 16:
        print(f"[失败] index.bin 太短：{len(data)}")
        return 1
    magic, version, count, _rsv = struct.unpack_from("<4sHHI", data, 0)
    if magic != b"ENGL":
        errors.append(f"magic 不匹配：{magic!r}")
    if version != 1:
        errors.append(f"版本不支持：{version}")
    expected = 16 + count * 12
    if len(data) != expected:
        errors.append(f"index.bin 长度 {len(data)}，应为 {expected}")
    print(f"index.bin: version={version} count={count} size={len(data)}")

    # --- 词表 ---
    words = []
    csv_path = root / "words.csv"
    if csv_path.exists():
        with csv_path.open(encoding="utf-8-sig", newline="") as f:
            reader = csv.reader(f)
            next(reader, None)
            for row in reader:
                if row and row[0].strip():
                    words.append(row)
        if len(words) != count:
            errors.append(f"words.csv 有 {len(words)} 条，index 声明 {count} 条")
    else:
        warns.append("缺少 words.csv（将无法显示释义/音标）")

    # --- 音频文件 ---
    audio_dir = root / "audio"
    if not audio_dir.exists():
        errors.append(f"缺少目录 {audio_dir}")
    else:
        total = 0
        for i in range(count):
            entry = data[16 + i * 12: 16 + (i + 1) * 12]
            _off, size, unit, flags, _r = struct.unpack("<IIBBH", entry)
            p = audio_dir / f"w{i + 1:04d}.ogg"
            if not p.exists():
                errors.append(f"缺音频：{p.name}")
                continue
            actual = p.stat().st_size
            if actual != size:
                errors.append(f"{p.name} 大小 {actual}，索引声明 {size}")
            blob = p.read_bytes()
            if blob[:4] != b"OggS":
                errors.append(f"{p.name} 不是 Ogg 文件")
                continue
            h = blob.find(b"OpusHead")
            if h < 0:
                errors.append(f"{p.name} 缺少 OpusHead")
                continue
            rate = struct.unpack_from("<I", blob, h + 12)[0]
            channels = blob[h + 9]
            if channels != 1:
                errors.append(f"{p.name} 声道 {channels}，固件只支持 mono")
            if rate not in (16000, 24000, 48000):
                errors.append(f"{p.name} 采样率 {rate}，固件支持 16k/24k/48k")
            total += actual
        print(f"音频: {count} 个文件，合计 {total / 1024:.1f} KB"
              f"（平均 {total / max(1, count) / 1024:.2f} KB/词）")

        # 多余文件
        extra = [p.name for p in audio_dir.glob("*.ogg")
                 if not (root / "audio" / p.name).exists() or
                 int(p.stem[1:]) > count]
        if extra:
            warns.append(f"有 {len(extra)} 个多余音频文件（索引外）")

    # --- meta.txt ---
    meta = root / "meta.txt"
    if meta.exists():
        info = dict(
            line.split("=", 1) for line in meta.read_text(encoding="utf-8").splitlines()
            if "=" in line
        )
        print(f"meta.txt: name={info.get('name')} count={info.get('count')}")
        if info.get("count") and int(info["count"]) != count:
            errors.append(f"meta.txt count={info['count']}，index 为 {count}")
    else:
        warns.append("缺少 meta.txt")

    # --- 结果 ---
    for w in warns:
        print(f"[提醒] {w}")
    for e in errors:
        print(f"[错误] {e}")
    if errors:
        print(f"\n校验失败：{len(errors)} 个问题")
        return 1
    print(f"\n校验通过：{count} 词")
    return 0


if __name__ == "__main__":
    sys.exit(main())
