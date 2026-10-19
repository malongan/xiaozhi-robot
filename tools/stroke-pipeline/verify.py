#!/usr/bin/env python3
"""校验 output/ 二进制自洽，并抽样渲染 PNG 供人工核对笔顺/笔名。

用法：python3 verify.py
"""
from __future__ import annotations

import json
import struct
from collections import Counter
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "output"
GRID = 256
CANVAS = 400  # 放大 2 倍渲染

TYPE_NAMES = {
    1: "横", 2: "竖", 3: "撇", 4: "捺", 5: "点", 6: "提",
    7: "横钩", 8: "横折", 9: "竖钩", 10: "竖提", 11: "撇折", 12: "弯钩", 13: "竖折",
}

SAMPLES = ["一", "人", "火", "女", "出", "万", "此", "九", "贝", "方", "母", "及",
           "我", "张", "写", "好", "字", "笔", "顺", "学", "校", "是", "的", "在"]


def load_index():
    idx = (OUT / "index.bin").read_bytes()
    assert idx[:4] == b"SZBI", "magic 错误"
    ver, n, m_size = struct.unpack_from("<HHI", idx, 4)
    g_sizes = struct.unpack_from("<6I", idx, 12)
    entries = []
    off = 12 + 24
    rec = struct.calcsize("<HBBIIH")
    for i in range(n):
        ucs2, grade, ns, mo, go, _ = struct.unpack_from("<HBBIIH", idx, off + i * rec)
        entries.append((ucs2, grade, ns, mo, go))
    assert off + n * rec == len(idx), "index 尺寸不自洽"
    # glyph 包按总大小校验
    (OUT / "szver.txt").read_text  # 触发存在性
    glyphs = {}
    for g in range(1, 7):
        glyphs[g] = (OUT / f"glyphs/g{g}.bin").read_bytes()
        assert len(glyphs[g]) == g_sizes[g - 1], f"g{g} 尺寸与 index 头不符"
    mmap_off = {}
    for g in range(2, 7):
        mmap_off[g] = mmap_off.get(g - 1, 0) + g_sizes[g - 2]
    for g in range(1, 7):
        mmap_off[g] = mmap_off.get(g, 0)
    return ver, n, m_size, entries, glyphs, mmap_off


def decode_glyph(buf, go, m_off_base, want_ucs2):
    ucs2, mode, rle_len = struct.unpack_from("<HBH", buf, go)
    assert ucs2 == want_ucs2, f"glyph 题头字不符 {ucs2:#x} vs {want_ucs2:#x}"
    assert mode == 1
    raw = buf[go + 5: go + 5 + rle_len]
    bitmap = bytearray(160 * 160 // 8)
    bit_i = 0
    for b in raw:
        count = b >> 1
        color = b & 1
        assert count in range(1, 128), f"RLE 计数非法 {count}"
        if bit_i + count > len(bitmap) * 8:
            raise ValueError("RLE 越界")
        if color:
            for k in range(count):
                byte_i, bit_o = (bit_i + k) >> 3, (bit_i + k) & 7
                bitmap[byte_i] |= 0x80 >> bit_o
        bit_i += count
    assert bit_i == len(bitmap) * 8, f"RLE 位数不齐 {bit_i}"
    return bitmap


def medians_for(med_buf, off):
    ns = med_buf[off]
    strokes = []
    p = off + 1
    for _ in range(ns):
        n, w, t, _r = med_buf[p:p + 4]
        pts = []
        for k in range(n):
            pts.append((med_buf[p + 4 + 2 * k], med_buf[p + 4 + 2 * k + 1]))
        strokes.append((pts, w, t))
        p += 4 + 2 * n
    return strokes, p - off


def render(strokes, path: Path, glyph_bits=None):
    img = Image.new("RGB", (CANVAS, CANVAS), (20, 20, 20))
    dr = ImageDraw.Draw(img)
    s = CANVAS / GRID
    if glyph_bits is not None:
        gimg = Image.new("1", (160, 160), 0)
        for i, v in enumerate(glyph_bits):
            if v:
                for b in range(8):
                    if v & (0x80 >> b):
                        gimg.putpixel(((i * 8 + b) % 160, (i * 8 + b) // 160), 1)
        gimg = gimg.resize((CANVAS, 0 + CANVAS), Image.NEAREST)
        gimg = gimg.point(lambda v: 80 if v else 0)
        img.paste(gimg, (0, 0), gimg)
    for si, (pts, w, t) in enumerate(strokes):
        color = (255, 255, 0) if si == len(strokes) - 1 else (255, 255, 255)
        pw = max(3, int(w * CANVAS / GRID))
        for i in range(len(pts) - 1):
            dr.line([pts[i][0] * s, pts[i][1] * s, pts[i + 1][0] * s, pts[i + 1][1] * s],
                    fill=color, width=pw)
    path.parent.mkdir(exist_ok=True, parents=True)
    img.save(path)


def main() -> None:
    ver, n, m_size, entries, glyphs, gbase = load_index()
    med_buf = (OUT / "medians.bin").read_bytes()
    assert len(med_buf) == m_size, "medians 尺寸与 index 头不符"
    print(f"index v{ver} 字数 {n} medians {m_size}B")

    type_counter: Counter[int] = Counter()
    grade_counter: Counter[int] = Counter()
    max_m_end = 0
    for ucs2, grade, ns, mo, go in entries:
        strokes, seg_len = medians_for(med_buf, mo)
        max_m_end = max(max_m_end, mo + seg_len)
        assert len(strokes) == ns
        for pts, w, t in strokes:
            assert 1 <= t <= 13, f"type 越界 {t} @ {ucs2:#x}"
            assert 7 <= w <= 18, f"width 越界 {w} @ {ucs2:#x}"
            assert all(0 <= x < GRID and 0 <= y < GRID for x, y in pts)
            type_counter[t] += 1
        grade_counter[grade] += 1
        buf = glyphs[grade]
        decode_glyph(buf, go, 0, ucs2)
    assert max_m_end == m_size, f"medians 尾部 {max_m_end} != {m_size}（有空洞或越界）"

    report = {
        "chars": n,
        "stroke_type_dist": {TYPE_NAMES[k]: v for k, v in sorted(type_counter.items())},
        "grade_dist": dict(sorted(grade_counter.items())),
    }
    (OUT / "verify.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                     encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))

    # 抽样渲染：medians 动画路径 + 完整字位图灰底
    sdir = OUT / "samples"
    by_ucs = {e[0]: e for e in entries}
    for ch in SAMPLES:
        e = by_ucs.get(ord(ch))
        if e is None:
            print(f"! 样字不在库：{ch}")
            continue
        ucs2, grade, ns, mo, go = e
        strokes, _ = medians_for(med_buf, mo)
        bits = None
        try:
            bits = decode_glyph(glyphs[grade], go, 0, ord(ch))
        except Exception as ex:
            print(f"! {ch} glyph 解码失败: {ex}")
        names = " ".join(TYPE_NAMES[t] for _p, _w, t in strokes)
        render(strokes, sdir / f"{ch}_g{grade}.png", bits)
        print(f"{ch} g{grade} 共{ns}笔: {names}")
    print(f"抽样图已存 {sdir}")


if __name__ == "__main__":
    main()
