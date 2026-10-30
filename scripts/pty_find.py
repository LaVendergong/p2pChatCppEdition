#!/usr/bin/env python3
"""在 pty 原始输出里找出包含某关键词的每次重绘，观察该行内容如何变化。"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pty_raw2.bin"
keyword = sys.argv[2] if len(sys.argv) > 2 else "很长的消息"

s = open(path, "rb").read().decode("utf-8", "replace")
parts = re.split(r"(\x1b\[\d+;\d+H)", s)
row = None
count = 0
for i, part in enumerate(parts):
    m = re.match(r"\x1b\[(\d+);\d+H", part)
    if m:
        row = int(m.group(1))
        continue
    if keyword in part:
        count += 1
        text = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", part)
        print(f"[{count}] row={row} len={len(text)} pending={'…' in text}")
        print(f"     head={text[:40]!r}")
        print(f"     tail={text[-40:]!r}")
print(f"\n共 {count} 次重绘包含关键词 '{keyword}'")
