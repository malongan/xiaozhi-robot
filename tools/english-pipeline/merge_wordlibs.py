#!/usr/bin/env python3
"""合并课本词表与分类补充词表，生成最终词库。

用法：
    python3 merge_wordlibs.py --textbook data/wy4a_words.csv \
        --extra data/extra_words.csv --out data/merged_words.csv

单元编号约定：
    1..6  课本 Unit 1..6
    7     课本板块（Plays / 戏剧）
    8+    分类补充（见 extra_words.csv 的 unit 列）
    0     复习/词汇表
"""

import argparse
import csv
from pathlib import Path


def load(path, with_phonetic=True):
    rows = []
    with Path(path).open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            word = (r.get("word") or "").strip().lower()
            if not word:
                continue
            rows.append({
                "word": word,
                "phonetic": (r.get("phonetic") or "").strip(),
                "meaning": (r.get("meaning") or "").strip(),
                "unit": int(r.get("unit") or 0),
            })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--textbook", required=True, help="课本词表 CSV")
    ap.add_argument("--extra", default="", help="分类补充词表 CSV")
    ap.add_argument("--out", required=True, help="输出 CSV")
    ap.add_argument("--name", default="", help="词库名（写进 meta）")
    args = ap.parse_args()

    rows = load(args.textbook)
    textbook_words = {r["word"] for r in rows}
    added = 0
    if args.extra:
        for r in load(args.extra):
            if r["word"] in textbook_words:
                continue  # 课本已有的不重复加
            rows.append(r)
            textbook_words.add(r["word"])
            added += 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["word", "phonetic", "meaning", "unit"])
        w.writerows([[r["word"], r["phonetic"], r["meaning"], r["unit"]] for r in rows])

    from collections import Counter
    c = Counter(r["unit"] for r in rows)
    print(f"输出 {out}：{len(rows)} 词（课本 {len(rows) - added} + 补充 {added}）")
    for u, n in sorted(c.items()):
        print(f"  unit {u}: {n} 词")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
