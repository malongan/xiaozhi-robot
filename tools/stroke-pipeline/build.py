#!/usr/bin/env python3
"""生成 TF 卡笔顺数据包（index.bin / medians.bin / glyphs/gX.bin）。

输入（raw/，先跑 fetch.py）：
  hanzi-data/*.json      笔顺源数据（Make Me A Hanzi，1024 网格、y 轴朝上）
  《义务教育语文课程》（2022年版）识字写字教学基本字表.txt   300 字
  《义务教育语文课程》（2022年版）常用字表一.txt             2500 字
  pep-textbook-g*.ts      人教版 1-6 年级课本字（用于年级归属）

输出（output/，拷贝到 TF 卡根目录）：
  szver.txt  index.bin  medians.bin  glyphs/g1..g6.bin  NOTICE.txt  report.json

二进制格式见 .omo/plans/stroke-tool-tf-plan.md §2.3/§2.4。
"""
from __future__ import annotations

import json
import math
import re
import struct
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw
from pypinyin import lazy_pinyin, Style

ROOT = Path(__file__).resolve().parent
RAW = ROOT / "raw"
OUT = ROOT / "output"

SRC_W = 1024          # 源网格
GRID = 256            # medians 网格（u8 0..255）
GLYPH = 160           # 完整字位图边长
RDP_EPS = 1.0         # RDP 简化阈值（256 网格像素）
STROKE_TYPE_NAMES = []  # 见 build_STROKE_TYPES

# ---------- 笔画类型码（与固件 stroke_tool.h 保持一致）----------
# 1横 2竖 3撇 4捺 5点 6提 7横钩 8横折 9竖钩 10竖提 11撇折 12弯钩/其他 13竖折
STROKE_TYPES = [
    (1, "横"), (2, "竖"), (3, "撇"), (4, "捺"), (5, "点"), (6, "提"),
    (7, "横钩"), (8, "横折"), (9, "竖钩"), (10, "竖提"), (11, "撇折"),
    (12, "弯钩"), (13, "竖折"),
]
STROKE_CHEATTOP = {code: name for code, name in STROKE_TYPES}

# ---------- 字表与年级 ----------
HX_RE = re.compile(r"[一-鿿]")


def load_charlist(path: Path) -> list[str]:
    chars = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        ch = line[0]
        chars.append(ch)
    return chars


def load_pep_grades() -> dict[str, int]:
    """解析 PEP 课本 ts：按 `export const pepG<t>s<sem>` 块切分，
    块 ID（如 g3s1）给出年级，块内 characters 数组收集汉字。返回 汉字->最早年级。"""
    grades: dict[str, int] = {}
    blk_re = re.compile(r"export const\s+\w+:\s*TextbookGrade\s*=\s*\{", re.S)
    id_re = re.compile(r"id:\s*'g(\d)s(\d)'")
    char_re = re.compile(r"characters:\s*\[([^\]]*)\]")
    for name in ("pep-textbook-g1-g2.ts", "pep-textbook-g3-g4.ts", "pep-textbook-g5-g6.ts"):
        text = (RAW / name).read_text(encoding="utf-8")
        starts = [m.start() for m in blk_re.finditer(text)] + [len(text)]
        for i in range(len(starts) - 1):
            block = text[starts[i]:starts[i + 1]]
            im = id_re.search(block)
            if not im:
                continue
            g = int(im.group(1))
            if not 1 <= g <= 6:
                continue
            for cm in char_re.finditer(block):
                for ch in HX_RE.findall(cm.group(1)):
                    if ch not in grades or g < grades[ch]:
                        grades[ch] = g
    return grades


def build_charset(extend: bool = True) -> dict[str, dict]:
    basic = load_charlist(RAW / "《义务教育语文课程》（2022年版）识字写字教学基本字表.txt")
    common1 = load_charlist(RAW / "《义务教育语文课程》（2022年版）常用字表一.txt")
    pep = load_pep_grades()
    target = list(common1)
    for ch in pep:  # 人教课本超出课标字表一的字补入（先于字表进入 target）
        if ch not in target:
            target.append(ch)
    assert len(basic) == 300, f"基本字表应为 300，实际 {len(basic)}"

    out = {}
    for ch in target:
        if ch in pep:
            grade = pep[ch]
        elif ch in basic:
            grade = 2    # 基本字表：低段教学基础字
        else:
            grade = 3    # 课标字表一中未入课本字表的字：中段
        out[ch] = {"grade": grade, "basic": ch in basic}

    if extend:
        # Everything else that has stroke data: make it queryable too. These
        # are not part of the primary-school word lists, so group them as
        # grade 6 (the UI only shows the pinyin anyway).
        src = RAW / "hanzi-data"
        added = 0
        for f in src.glob("*.json"):
            ch = f.stem
            if len(ch) != 1 or not HX_RE.fullmatch(ch) or ch in out:
                continue
            out[ch] = {"grade": 6, "basic": False}
            added += 1
        print(f"扩展字库：额外加入 {added} 个常用/通用汉字")
    return out


# ---------- 几何 ----------
def _flatten_pts(median, slope):
    out = []
    for x, y in median:
        out.append((int(round(x * (GRID - 1) / SRC_W)),
                    int(round((SRC_W - y) * (GRID - 1) / SRC_W))))
    return out


def _rdp(points, eps):
    """标准 Ramer–Douglas–Peucker（迭代栈实现）。"""
    if len(points) < 3:
        return points
    n = len(points)
    keep = [False] * n
    keep[0] = keep[n - 1] = True
    stack = [(0, n - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi - lo < 2:
            continue
        ax, ay = points[lo]
        bx, by = points[hi]
        dx, dy = bx - ax, by - ay
        norm2 = dx * dx + dy * dy
        best_i, best_d = lo + 1, -1.0
        for i in range(lo + 1, hi):
            px, py = points[i]
            if norm2 == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / norm2
                t = max(0.0, min(1.0, t))
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if d > best_d:
                best_d, best_i = d, i
        if best_d > eps:
            keep[best_i] = True
            stack.append((lo, best_i))
            stack.append((best_i, hi))
    return [p for p, k in zip(points, keep) if k]


def _vec(p, q):
    dx, dy = q[0] - p[0], q[1] - p[1]
    n = math.hypot(dx, dy)
    return (dx / n, dy / n) if n else (0.0, 0.0)


def _at_frac(pts, frac):
    """按弧长分数取点（抗起笔/收笔微摆）。"""
    total = sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
                for i in range(len(pts) - 1)) or 1.0
    target = frac * total
    walk = 0.0
    for i in range(len(pts) - 1):
        seg = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
        if walk + seg >= target:
            t = (target - walk) / seg if seg else 0.0
            return (pts[i][0] + t * (pts[i + 1][0] - pts[i][0]),
                    pts[i][1] + t * (pts[i + 1][1] - pts[i][1]))
        walk += seg
    return pts[-1]


def classify_stroke(pts, area, length):
    """笔画类型码。pts 为 256 网格中点序（屏幕坐标，y 轴向下）。"""
    if length < GRID * 0.10:
        return 5
    p1, p2 = _at_frac(pts, 0.15), _at_frac(pts, 0.85)
    dxa, dya = _vec(p1, p2)

    def _main_dir():
        adx, ady = abs(dxa), abs(dya)
        if adx < 0.45 * ady:          # 近竖向：dy<0 向上写着极少见，归 竖
            return 2
        if ady < 0.45 * adx:          # 近横向（左右向均归横）
            return 1
        if dya > 0:                   # 斜向向下：右下=捺，左下=撇
            return 4 if dxa > 0 else 3
        return 6 if dxa > 0 else 3    # 斜向向上：右上=提，左上=撇（反向书写）

    bend_stats = []
    for i in range(1, len(pts) - 2):
        s1 = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
        s0 = math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
        if s0 < 4.0 or s1 < 4.0:
            continue
        v1, v2 = _vec(pts[i], pts[i + 1]), _vec(pts[i - 1], pts[i])
        dot = max(-1.0, min(1.0, v1[0] * v2[0] + v1[1] * v2[1]))
        th = math.degrees(math.acos(dot))
        if th > 50:
            bend_stats.append(th)
    sharp_bends = sum(1 for th in bend_stats if th > 75)
    d0 = _vec(_at_frac(pts, 0.08), _at_frac(pts, 0.35))
    main = _main_dir()
    main0 = _dir_of_vec(d0)
    p90 = _at_frac(pts, 0.90)
    tail_len = math.hypot(pts[-1][0] - p90[0], pts[-1][1] - p90[1])
    tail_dir = _vec(p90, pts[-1])

    if sharp_bends == 0:
        return main
    if tail_len < GRID * 0.10:
        if main0 == 1:
            return 7      # 横折钩/横钩（短尾均按横钩名显示）
        if main0 == 2:
            return 10 if tail_dir[0] > 0.2 else 9      # 竖提 / 竖钩
        if main0 == 4:
            return 8      # 斜钩类按横折组显示
        return 12
    if main0 == 1:
        return 8
    if main0 == 2:
        return 13
    if main0 == 3:
        return 11
    return 12


def _dir_of_vec(v):
    dxa, dya = v
    adx, ady = abs(dxa), abs(dya)
    if adx < 0.45 * ady:
        return 2
    if ady < 0.45 * adx:
        return 1
    if dya > 0:
        return 4 if dxa > 0 else 3
    return 6 if dxa > 0 else 3


def poly_area(pts):
    a = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % len(pts)]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2


def path_to_polys(path, scale=1.0, flip=True, margin=0.0):
    """SVG path（Make Me A Hanzi 填充轮廓）→ 多边形列表（屏幕坐标）。"""
    toks = re.findall(r"([MmQqCcLlZz])([^MmQqCcLlZz]*)", path)
    polys, cur, pts = [], [], []
    pos = (0.0, 0.0)
    sub = None
    for tok in toks:
        cmd = tok[0]
        nums = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?(?:e-?\d+)?", tok[1])]
        rel = cmd.islower()
        i = 0
        while i < len(nums):
            c = cmd.upper()
            if c == "M":
                x, y = nums[i], nums[i + 1]; i += 2
                if rel:
                    x += pos[0]; y += pos[1]
                if sub is not None and len(sub) > 2:
                    polys.append(sub)
                pos = (x, y)
                sub = [pos]; cur = pos
            elif c == "L":
                while i + 1 < len(nums):
                    x, y = nums[i], nums[i + 1]; i += 2
                    if rel:
                        x += pos[0]; y += pos[1]
                    pos = (x, y); sub.append(pos)
            elif c in "Qc":
                while i + 3 < len(nums):
                    c1x, c1y, x, y = nums[i], nums[i + 1], nums[i + 2], nums[i + 3]; i += 4
                    if rel:
                        c1x += pos[0]; c1y += pos[1]; x += pos[0]; y += pos[1]
                    for k in range(1, 9):
                        t = k / 8
                        mt = 1 - t
                        bx = mt * mt * pos[0] + 2 * mt * t * c1x + t * t * x
                        by = mt * mt * pos[1] + 2 * mt * t * c1y + t * t * y
                        sub.append((bx, by))
                    pos = (x, y)
            elif c == "C":
                while i + 5 < len(nums):
                    c1x, c1y, c2x, c2y, x, y = nums[i:i + 6]; i += 6
                    if rel:
                        c1x += pos[0]; c1y += pos[1]; c2x += pos[0]; c2y += pos[1]
                        x += pos[0]; y += pos[1]
                    for k in range(1, 13):
                        t = k / 12
                        mt = 1 - t
                        bx = mt**3 * pos[0] + 3 * mt**2 * t * c1x + 3 * mt * t**2 * c2x + t**3 * x
                        by = mt**3 * pos[1] + 3 * mt**2 * t * c1y + 3 * mt * t**2 * c2y + t**3 * y
                        sub.append((bx, by))
                    pos = (x, y)
            elif c == "Z":
                if sub and len(sub) > 2:
                    polys.append(sub)
                sub = None
                pos = sub[0] if sub else pos
                i += 0
                break
            else:
                break
    if sub and len(sub) > 2:
        polys.append(sub)

    out = []
    for poly in polys:
        out.append([(p[0] * scale + margin,
                     (SRC_W - p[1]) * scale + margin if flip else p[1] * scale + margin)
                    for p in poly])
    return out


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    src = RAW / "hanzi-data"

    charset = build_charset()
    print(f"目标字表 {len(charset)} 字")

    entries = []       # (ucs2, grade, stroke_count, medians_off, glyph_off, buf)
    medians_buf = bytearray()
    glyph_files = {g: bytearray() for g in range(1, 7)}
    glyph_recs = defaultdict(list)
    missing = []
    multi_note = 0

    for ch, meta in charset.items():
        f = src / f"{ch}.json"
        if not f.exists():
            missing.append(ch)
            continue
        data = json.loads(f.read_text(encoding="utf-8"))
        medians_raw = data["medians"]
        polys = [path_to_polys(p) for p in data["strokes"]]

        # Fit the real ink bounds (stroke outlines may reach outside the
        # 1024 em box, e.g. negative coordinates) into the glyph bitmap with a
        # uniform scale, keeping a small safe border so nothing is clipped.
        xs = [pt[0] for poly_list in polys for poly in poly_list for pt in poly]
        ys = [pt[1] for poly_list in polys for poly in poly_list for pt in poly]
        min_x, max_x = (min(xs), max(xs)) if xs else (0.0, SRC_W)
        min_y, max_y = (min(ys), max(ys)) if ys else (0.0, SRC_W)
        span_x = max(max_x - min_x, 1.0)
        span_y = max(max_y - min_y, 1.0)
        pad = 6
        fit_scale = min((GLYPH - 2 * pad) / span_x, (GLYPH - 2 * pad) / span_y)
        off_x = pad + ((GLYPH - 2 * pad) - span_x * fit_scale) / 2.0 - min_x * fit_scale
        off_y = pad + ((GLYPH - 2 * pad) - span_y * fit_scale) / 2.0 - min_y * fit_scale
        polys = [[[(x * fit_scale + off_x, y * fit_scale + off_y) for (x, y) in poly]
                  for poly in poly_list]
                 for poly_list in polys]

        # medians → 256 网格 + RDP
        strokes = []
        for mi, med in enumerate(medians_raw):
            pts = _flatten_pts(med, None)
            pts = _rdp(pts, RDP_EPS)
            length = sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
                         for i in range(len(pts) - 1)) or 1.0
            # 笔宽：以该笔填充面积/中长估算（256 网格）
            a256 = sum(poly_area([(p[0] * (GRID - 1) / GLYPH, p[1] * (GRID - 1) / GLYPH)
                                  for p in poly]) for poly in polys[mi])
            w = a256 / length * 1.6
            w = max(7, min(18, int(round(w))))
            tcode = classify_stroke(pts, a256, length)
            strokes.append({"pts": pts, "w": w, "type": tcode})

        ucs2 = ord(ch)
        med_off = len(medians_buf)
        medians_buf.append(len(strokes))
        for s in strokes:
            medians_buf.append(len(s["pts"]))
            fitted_w = int(round(s["w"] * (SRC_W * fit_scale) / GLYPH))
            medians_buf.append(max(7, min(18, fitted_w)))
            medians_buf.append(s["type"])
            medians_buf.append(0)
            for x, y in s["pts"]:
                # Same affine as the glyph bitmap (256 grid -> fitted pixels,
                # then back to the firmware's 0..255 median domain).
                px = (x / 255.0) * SRC_W * fit_scale + off_x
                py = (y / 255.0) * SRC_W * fit_scale + off_y
                mx = max(0, min(255, int(round(px * 255.0 / GLYPH))))
                my = max(0, min(255, int(round(py * 255.0 / GLYPH))))
                medians_buf.append(mx)
                medians_buf.append(my)

        # glyph 位图：全部笔画填充
        img = Image.new("1", (GLYPH, GLYPH), 0)
        dr = ImageDraw.Draw(img)
        for poly_list in polys:
            for poly in poly_list:
                if len(poly) > 2:
                    dr.polygon(poly, fill=1)
        bits_px = GLYPH * GLYPH
        bitmap = img.tobytes()
        rle = bytearray()
        # 位级游程（MSB first）
        vals = []
        for b in bitmap:
            for k in range(8):
                vals.append((b >> (7 - k)) & 1)
        i = 0
        while i < bits_px:
            v = vals[i]
            run = 1
            while i + run < bits_px and vals[i + run] == v and run < 127:
                run += 1
            rle.append((run << 1) | v)
            i += run
        gidx = meta["grade"]
        glyph_off = len(glyph_files[gidx])
        glyph_files[gidx] += struct.pack("<HBH", ucs2, 1, len(rle)) + rle
        glyph_recs[gidx].append((ucs2, glyph_off))

        entries.append((ucs2, meta["grade"], len(strokes), med_off, glyph_off, ch))

    # ---- 排序与文件 ----
    entries.sort(key=lambda e: (e[1], e[0]))
    total_m = len(medians_buf)
    header = b"SZBI" + struct.pack("<HHI", 1, len(entries), total_m)
    g_sizes = [len(glyph_files[g]) for g in range(1, 7)]
    header += struct.pack("<6I", *g_sizes)
    index_buf = bytearray(header)
    for ucs2, grade, n, mo, go, _ in entries:
        index_buf += struct.pack("<HBBII H", ucs2, grade, n, mo, go, 0)

    (OUT / "szver.txt").write_text("1\n", encoding="utf-8")
    (OUT / "index.bin").write_bytes(bytes(index_buf))
    (OUT / "medians.bin").write_bytes(bytes(medians_buf))
    gdir = OUT / "glyphs"
    gdir.mkdir(exist_ok=True)
    for g in range(1, 7):
        (gdir / f"g{g}.bin").write_bytes(bytes(glyph_files[g]))

    # pinyin 表（供固件顶栏显示 & MCP 回复）
    pinyin = {}
    for ch in charset:
        try:
            pinyin[ch] = lazy_pinyin(ch, style=Style.TONE)[0]
        except Exception:
            pinyin[ch] = ""
    (OUT / "pinyin.json").write_text(
        json.dumps(pinyin, ensure_ascii=False, sort_keys=True), encoding="utf-8")

    import shutil
    lic = src / "ARPHICPL.TXT"
    if lic.exists():
        shutil.copy(lic, OUT / "NOTICE.txt")
    else:
        (OUT / "NOTICE.txt").write_text(
            "Data source: hanzi-writer-data (Make Me A Hanzi derivative), Arphic Public License.\n"
            "Charlists: 《义务教育语文课程》(2022年版) 课标字表 + 人教版部编教材课本字。\n", encoding="utf-8")

    grade_counts = defaultdict(int)
    for *_, ch in entries:
        grade_counts[charset[ch]["grade"]] += 1
    report = {
        "target_chars": len(charset),
        "built": len(entries),
        "missing": missing,
        "grade_counts": dict(grade_counts),
        "sizes": {
            "index.bin": len(index_buf),
            "medians.bin": len(medians_buf),
            "glyphs": g_sizes,
        },
    }
    (OUT / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2)[:1200])


if __name__ == "__main__":
    main()
