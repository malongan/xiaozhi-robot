#!/usr/bin/env python3
"""生成 TF 卡英语词库。

输入 CSV（UTF-8）：word,phonetic,meaning,unit
输出目录：words.csv / index.bin / meta.txt / audio/w0001.ogg ...

单词发音用 macOS 内置 say 合成（离线），再转成 16kHz 单声道 Opus
（与服务器 TTS 同采样率，避免设备端解码器反复重配置）。

用法：
    python3 build.py --csv data/pep4a_words.csv --out /Volumes/TF/english
    python3 build.py --csv data/pep4a_words.csv --out ./out --limit 20 --jobs 8
"""

import argparse
import csv
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# 设备端解码器用 16kHz（与服务器 TTS 一致）
OPUS_SAMPLE_RATE = 16000
OPUS_BITRATE = "24k"

VOICES = {
    "Samantha": "美音 (en_US)",
    "Daniel": "英音 (en_GB)",
    "Karen": "澳音 (en_AU)",
}


def synthesize_ogg(word: str, voice: str, out_path: Path) -> bool:
    """say -> aiff -> wav(16k mono) -> ogg(Opus)。失败返回 False。"""
    with tempfile.TemporaryDirectory() as tmp:
        aiff = Path(tmp) / "a.aiff"
        wav = Path(tmp) / "a.wav"
        try:
            subprocess.run(
                ["say", "-v", voice, "-o", str(aiff), word],
                check=True, capture_output=True, timeout=60,
            )
            subprocess.run(
                ["afconvert", "-f", "WAVE", "-d", f"LEI16@{OPUS_SAMPLE_RATE}", "-c", "1",
                 str(aiff), str(wav)],
                check=True, capture_output=True, timeout=60,
            )
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            print(f"  [合成失败] {word}: {e}", file=sys.stderr)
            return False

        out_path.parent.mkdir(parents=True, exist_ok=True)
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(wav),
            "-c:a", "libopus", "-b:a", OPUS_BITRATE,
            "-ar", str(OPUS_SAMPLE_RATE), "-ac", "1",
            "-application", "voip", "-frame_duration", "60",
            str(out_path),
        ]
        try:
            subprocess.run(cmd, check=True, capture_output=True, timeout=120)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            print(f"  [编码失败] {word}: {e}", file=sys.stderr)
            return False
    return out_path.exists() and out_path.stat().st_size > 0


def read_words(csv_path: Path, limit: int | None):
    """读 CSV，返回 [(word, phonetic, meaning, unit)]，跳过表头。"""
    rows = []
    with csv_path.open(encoding="utf-8-sig", newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            raise SystemExit(f"{csv_path} 是空文件")
        header = [h.strip().lower() for h in header]
        if "word" not in header:
            raise SystemExit(f"{csv_path} 缺少 word 列，实际表头：{header}")
        w_i = header.index("word")
        p_i = header.index("phonetic") if "phonetic" in header else -1
        m_i = header.index("meaning") if "meaning" in header else -1
        u_i = header.index("unit") if "unit" in header else -1

        for row in reader:
            if not row or not row[w_i].strip():
                continue
            def get(i):
                return row[i].strip() if 0 <= i < len(row) else ""
            unit = 0
            if u_i >= 0:
                try:
                    unit = int(get(u_i) or 0)
                except ValueError:
                    unit = 0
            rows.append((get(w_i).lower(), get(p_i), get(m_i), unit))
            if limit and len(rows) >= limit:
                break
    return rows


def main():
    ap = argparse.ArgumentParser(description="生成 TF 卡英语词库")
    ap.add_argument("--csv", required=True, help="词库 CSV 路径")
    ap.add_argument("--out", required=True, help="输出目录（卡上的 english 目录）")
    ap.add_argument("--voice", default="Samantha", choices=sorted(VOICES),
                    help=f"发音音色，默认 Samantha（{VOICES['Samantha']}）")
    ap.add_argument("--name", default="", help="词库名（默认取 CSV 文件名）")
    ap.add_argument("--jobs", type=int, default=8, help="并发合成数")
    ap.add_argument("--limit", type=int, default=0, help="只取前 N 个词（调试用）")
    ap.add_argument("--keep-existing", action="store_true",
                    help="保留已有 ogg，只补缺失的（增量更新）")
    args = ap.parse_args()

    csv_path = Path(args.csv)
    out_dir = Path(args.out)
    audio_dir = out_dir / "audio"
    if not args.keep_existing and audio_dir.exists():
        shutil.rmtree(audio_dir)
    audio_dir.mkdir(parents=True, exist_ok=True)

    rows = read_words(csv_path, args.limit or None)
    if not rows:
        raise SystemExit("词库为空")
    name = args.name or csv_path.stem
    print(f"词库：{name}（{len(rows)} 词，音色 {args.voice} {VOICES[args.voice]}）")

    # 1) 合成音频
    ok_words, sizes = [], []
    failed = []
    done = 0

    def make(i_word):
        i, (word, *_rest) = i_word
        target = audio_dir / f"w{i + 1:04d}.ogg"
        if args.keep_existing and target.exists() and target.stat().st_size > 0:
            return (i, word, target.stat().st_size, True)
        if synthesize_ogg(word, args.voice, target):
            return (i, word, target.stat().st_size, True)
        return (i, word, 0, False)

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for i, word, size, success in pool.map(make, enumerate(rows)):
            done += 1
            if success:
                ok_words.append((i, word, size))
                sizes.append(size)
            else:
                failed.append(word)
            if done % 20 == 0 or done == len(rows):
                print(f"  合成 {done}/{len(rows)}")

    if failed:
        print(f"[警告] {len(failed)} 个词合成失败：{failed[:10]}", file=sys.stderr)

    # 2) 写 index.bin（只写成功合成的词条，保持索引连续）
    #    注意：词条序号必须与文件名 w{i+1} 对齐，所以失败词会导致错位。
    #    为保证对齐，这里直接失败即中止，避免产出损坏词库。
    if failed:
        raise SystemExit(
            f"有 {len(failed)} 个词没有音频，为避免索引错位已中止。"
            f"失败词：{failed[:20]}\n"
            f"修好后可加 --keep-existing 增量补齐。"
        )

    index = bytearray()
    index += b"ENGL"
    index += struct.pack("<HHI", 1, len(ok_words), 0)  # magic/ver/count/rsv = 12 字节
    index += b"\x00" * 4  # 保留字段，头部对齐到 16 字节
    for i, word, size in ok_words:
        unit = rows[i][3]
        flags = 0
        if " " in word:
            flags |= 0x01
        if rows[i][1]:
            flags |= 0x02
        index += struct.pack("<IIBBH", 0, size, unit, flags, 0)

    (out_dir / "index.bin").write_bytes(bytes(index))

    # 3) 写 words.csv（与 index 对齐，只含成功词）
    with (out_dir / "words.csv").open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["word", "phonetic", "meaning", "unit"])
        for i, word, _size in ok_words:
            w.writerow([word, rows[i][1], rows[i][2], rows[i][3]])

    # 4) 写 meta.txt
    units = sorted({rows[i][3] for i, _w, _s in ok_words})
    (out_dir / "meta.txt").write_text(
        f"name={name}\nversion=1\ncount={len(ok_words)}\nunits={','.join(str(u) for u in units)}\n",
        encoding="utf-8",
    )

    total = sum(sizes)
    print(f"\n完成：{len(ok_words)} 词，音频 {total / 1024:.1f} KB"
          f"（平均 {total / max(1, len(sizes)) / 1024:.2f} KB/词）")
    print(f"输出目录：{out_dir}")


if __name__ == "__main__":
    main()
