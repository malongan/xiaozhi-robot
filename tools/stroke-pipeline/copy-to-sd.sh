#!/bin/bash
# 把笔画数据包拷到 TF 卡根目录（卡需先插到 Mac）
set -e
SRC="$(cd "$(dirname "$0")" && pwd)/sdcard"
DEST="${1:?用法: ./copy-to-sd.sh /Volumes/你的TF卡}"
[ -d "$DEST" ] || { echo "目录不存在: $DEST"; exit 1; }
cp -R "$SRC"/. "$DEST"/
sync
echo "已拷贝到: $DEST"
ls -la "$DEST"
