#!/usr/bin/env python3
"""Gzip a text asset for embedding into the firmware.

Deterministic (no timestamp, no original name) so rebuilds stay reproducible.
"""

import gzip
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <input> <output.gz>", file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    data = src.read_bytes()
    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(dst, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
            gz.write(data)
    print(f"gzip: {src.name} {len(data)} -> {dst.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
