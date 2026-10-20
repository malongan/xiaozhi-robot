#!/usr/bin/env python3
"""合并全部词源，生成最终词库 CSV（统一单元编号）。

单元编号方案：
    1..7    外研四上课本（6 个 Unit + Plays 戏剧板块）
    8       四年级分类补充：食物水果
    9       四年级分类补充：数字颜色时间
    10      四年级分类补充：动物场所物品
    11      四年级分类补充：日常动作形容
    12..17  外研五年级上册 Unit 1..6
    18..23  外研五年级下册 Unit 1..6
    24..29  外研六年级上册 Unit 1..6
    30..33  外研六年级下册 Unit 1..4
    34      通用拓展词（不在任何教材里的常用词）

用法：
    python3 build_final_words.py --out data/final_words.csv
"""

import argparse
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).parent
DATA = ROOT / "data"

# 册别 -> 起始单元号
BOOK_BASE = {"5a": 12, "5b": 18, "6a": 24, "6b": 30}

# 少数词在词表里只有词性没有释义，这里补上
MEANING_FIX = {
    "er": "和；跟；与",
    "golden": "金色的；金制的",
    "happily": "快乐地",
}


def read_csv(path, extra_col=None):
    with Path(path).open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            word = (r.get("word") or "").strip().lower()
            if not word:
                continue
            yield {
                "word": word,
                "phonetic": (r.get("phonetic") or "").strip(),
                "meaning": (r.get("meaning") or "").strip(),
                "unit": int(r.get("unit") or 0),
                "unit_title": (r.get(extra_col) if extra_col else r.get("unit_title") or "").strip(),
            }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DATA / "final_words.csv"))
    args = ap.parse_args()

    rows = []
    seen = set()

    def add(word, phonetic, meaning, unit, title=""):
        if word in seen:
            return False
        seen.add(word)
        rows.append([word, phonetic, meaning, unit, title])
        return True

    # 1) 四上课本（用户词表，unit 1-7）
    n_textbook = 0
    for r in read_csv(DATA / "wy4a_words.csv"):
        if add(r["word"], r["phonetic"], r["meaning"], r["unit"]):
            n_textbook += 1

    # 2) 四年级分类补充（unit 8-11）
    n_extra4 = 0
    for r in read_csv(DATA / "extra_words.csv"):
        if add(r["word"], r["phonetic"], r["meaning"], r["unit"]):
            n_extra4 += 1

    # 3) 五/六年级课本（从 JSON 读，保留册别与单元标题）
    n_g56 = 0
    json_path = DATA / "grades56_words.json"
    if json_path.exists():
        data = json.loads(json_path.read_text(encoding="utf-8"))
        for item in data:
            book = item.get("book", "")
            base = BOOK_BASE.get(book)
            if base is None:
                continue
            local_unit = int(item.get("unit", 0))
            # 册内单元号：5a 的 1..6 → 1..6；5b 的 7..12 → 1..6
            offset = {"5a": 0, "5b": 6, "6a": 12, "6b": 18}[book]
            unit = base + (local_unit - offset - 1)
            meaning = (item.get("meaning") or "").strip()
            unit_title = (item.get("unit_title") or "").strip()
            if not unit_title:
                # 兜底：释义里可能还带着 "（单元标题 Unit N）"
                m = re.match(r"^(.*?)（([^）]*)）\s*$", meaning)
                if m:
                    meaning = m.group(1).strip()
                    unit_title = m.group(2).strip()
            meaning = re.sub(
                r"^(n|v|vt|vi|adj|adv|prep|conj|pron|num|int|aux|abbr)\.\s*", "", meaning).strip()
            if not meaning:
                meaning = MEANING_FIX.get(item.get("word", "").lower(), "")
            if add(item.get("word", "").lower(), item.get("phonetic", "").strip(),
                   meaning, unit, unit_title):
                n_g56 += 1

    # 4) 通用拓展词（unit 34）——放在最后，重复的自动跳过
    n_extra_gen = 0
    for r in read_csv(DATA / "extra_grades.csv"):
        if add(r["word"], r["phonetic"], r["meaning"], 34):
            n_extra_gen += 1

    out = Path(args.out)
    with out.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["word", "phonetic", "meaning", "unit", "unit_title"])
        w.writerows(rows)

    from collections import Counter
    c = Counter(r[3] for r in rows)
    print(f"写入 {out}：{len(rows)} 词")
    print(f"  四上课本 {n_textbook} / 四上分类 {n_extra4} / 五六年级课本 {n_g56} / 通用拓展 {n_extra_gen}")
    for u, n in sorted(c.items()):
        print(f"  unit {u:2d}: {n} 词")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
