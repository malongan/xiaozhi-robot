#!/usr/bin/env python3
"""把 JSON 词表转成 english-pipeline 需要的 CSV。

输入 JSON：数组，元素形如
    {"word": "classroom", "phonetic": "/ˈklæsruːm/", "meaning": "教室", "unit": 1}
也接受 {"words": [...]} 包装形式。

用法：
    python3 json2csv.py data/pep4a_words.json data/pep4a_words.csv
"""

import argparse
import csv
import json
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", help="输入 JSON")
    ap.add_argument("dst", help="输出 CSV")
    ap.add_argument("--name", default="", help="词库名（写入 build.py 的 --name）")
    args = ap.parse_args()

    data = json.loads(Path(args.src).read_text(encoding="utf-8"))
    if isinstance(data, dict):
        for key in ("words", "data", "list", "items"):
            if key in data and isinstance(data[key], list):
                data = data[key]
                break
    if not isinstance(data, list):
        print("无法识别的 JSON 结构", file=sys.stderr)
        return 1

    rows, seen = [], set()
    for item in data:
        word = str(item.get("word", "")).strip().lower()
        if not word or word in seen:
            continue
        seen.add(word)
        unit = item.get("unit", 0)
        try:
            unit = int(unit)
        except (TypeError, ValueError):
            unit = 0
        rows.append([
            word,
            str(item.get("phonetic", "")).strip(),
            str(item.get("meaning", "")).strip(),
            unit,
        ])

    if not rows:
        print("没有解析到词条", file=sys.stderr)
        return 1

    out = Path(args.dst)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["word", "phonetic", "meaning", "unit"])
        w.writerows(rows)

    units = {}
    for r in rows:
        units[r[3]] = units.get(r[3], 0) + 1
    print(f"写入 {out}：{len(rows)} 词")
    print("单元分布：" + ", ".join(f"U{u}={c}" for u, c in sorted(units.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
