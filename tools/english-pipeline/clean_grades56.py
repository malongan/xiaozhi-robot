#!/usr/bin/env python3
"""清洗五/六年级词表：把单元标题从释义里拆出来，并去掉混入的词性标记。

输入 data/grades56_words.csv（释义形如 "盘子（What's on your plate Unit 1）"）
输出 data/grades56_clean.csv（多一列 unit_title）

用法：
    python3 clean_grades56.py
"""

import csv
import re
from pathlib import Path

ROOT = Path(__file__).parent
SRC = ROOT / "data" / "grades56_words.csv"
DST = ROOT / "data" / "grades56_clean.csv"

BRACKET = re.compile(r"^(.*?)（([^）]*)）\s*$")
POS = re.compile(r"^(n|v|vt|vi|adj|adv|prep|conj|pron|num|int|aux|abbr)\.\s*")


def main():
    rows = list(csv.DictReader(SRC.open(encoding="utf-8")))
    out = []
    for r in rows:
        meaning = r["meaning"].strip()
        title = ""
        m = BRACKET.match(meaning)
        if m:
            meaning = m.group(1).strip()
            title = m.group(2).strip()
        meaning = POS.sub("", meaning).strip()
        out.append([r["word"].strip().lower(), r["phonetic"].strip(), meaning,
                    r["unit"].strip(), title])
    with DST.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["word", "phonetic", "meaning", "unit", "unit_title"])
        w.writerows(out)
    titles = sorted({o[4] for o in out if o[4]})
    print(f"写入 {DST}：{len(out)} 词")
    print(f"单元标题 {len(titles)} 个：")
    for t in titles:
        print("  " + t)


if __name__ == "__main__":
    main()
