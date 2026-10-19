#!/usr/bin/env python3
"""下载 / 准备生字笔顺数据管线的原始输入。

产物（raw/ 目录，均不入 git，见 .gitignore）：
  hanzi-data/           hanzi-writer-data 2.0.0（每字 {strokes, medians, radStrokes}）
  hwd.tgz               上者的 npm 压缩包缓存
  《义务教育语文课程》（2022年版）识字写字教学基本字表.txt   300 字（课标）
  《义务教育语文课程》（2022年版）常用字表一.txt             2500 字（课标，需大文件下载）
  pep-textbook-g1-g2.ts / g3-g4.ts / g5-g6.ts               人教版课本字（年级归属）

用法：python3 fetch.py    # 幂等，已存在的文件跳过下载
"""
from __future__ import annotations

import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

RAW = Path(__file__).resolve().parent / "raw"

HWD_TGZ = "https://registry.npmjs.org/hanzi-writer-data/-/hanzi-writer-data-2.0.0.tgz"
TOGSCC_CHARS = (
    "https://raw.githubusercontent.com/jaywcjlove/"
    "table-of-general-standard-chinese-characters/main/data/characters.json"
)

PEP_FILES = {
    "https://raw.githubusercontent.com/cjp1016/hanzi-park/main/src/data/pep-textbook-g1-g2.ts":
        "pep-textbook-g1-g2.ts",
    "https://raw.githubusercontent.com/cjp1016/hanzi-park/main/src/data/pep-textbook-g3-g4.ts":
        "pep-textbook-g3-g4.ts",
    "https://raw.githubusercontent.com/cjp1016/hanzi-park/main/src/data/pep-textbook-g5-g6.ts":
        "pep-textbook-g5-g6.ts",
}

CHARLIST_FILES = {
    "https://raw.githubusercontent.com/zispace/hanzi-chars/main/"
    "data-charlist/%E3%80%8A%E4%B9%89%E5%8A%A1%E6%95%99%E8%82%B2%E8%AF%AD%E6%96%87%E8%AF%BE%E7%A8%8B%E3"
    "%80%8B%EF%BC%882022%E5%B9%B4%E7%89%88%EF%BC%89%E8%AF%86%E5%AD%97%E5%86%99%E5%AD%97%E6%95%99%E5"
    "%AD%A6%E5%9F%BA%E6%9C%AC%E5%AD%97%E8%A1%A8.txt":
        "《义务教育语文课程》（2022年版）识字写字教学基本字表.txt",
    "https://raw.githubusercontent.com/zispace/hanzi-chars/main/"
    "data-charlist/%E3%80%8A%E4%B9%89%E5%8A%A1%E6%95%99%E8%82%B2%E8%AF%AD%E6%96%87%E8%AF%BE%E7%A8%8B%E3"
    "%80%8B%EF%BC%882022%E5%B9%B4%E7%89%88%EF%BC%89%E5%B8%B8%E7%94%A8%E5%AD%97%E8%A1%A8%E4%B8%80.txt":
        "《义务教育语文课程》（2022年版）常用字表一.txt",
}


def _download(url: str, dest: Path) -> None:
    if dest.exists() and dest.stat().st_size > 0:
        print(f"= 已存在 {dest.name}")
        return
    print(f"> 下载 {dest.name} ...")
    tmp = dest.with_suffix(dest.suffix + ".part")
    urllib.request.urlretrieve(url, tmp)  # noqa: S310 管线固定可信域名
    tmp.replace(dest)
    print(f"  {dest.stat().st_size} bytes")


def main() -> None:
    RAW.mkdir(parents=True, exist_ok=True)
    _download(HWD_TGZ, RAW / "hwd.tgz")
    for url, name in {**CHARLIST_FILES, **PEP_FILES}.items():
        _download(url, RAW / name)

    # 解压 hanzi-writer-data
    target = RAW / "hanzi-data"
    if not (target / "index.js").exists():
        print("> 解压 hwd.tgz -> hanzi-data/")
        with tarfile.open(RAW / "hwd.tgz") as tf:
            tf.extractall(target, filter="data")  # noqa: S202 本地生成的确定包
        pkg = target / "package"
        for item in pkg.iterdir():
            shutil.move(str(item), str(target / item.name))
        pkg.rmdir()
        n = len(list(target.glob("*.json")))
        print(f"  hanzi-data 字数据 {n} 个")
    print("=?= raw 准备完成")


if __name__ == "__main__":
    main()
