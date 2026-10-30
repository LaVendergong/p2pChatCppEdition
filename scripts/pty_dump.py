#!/usr/bin/env python3
"""打印 pty 原始输出里每个 ANSI 定位序列，便于分析渲染逻辑。"""
import re
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/pty_raw.bin", "rb").read()
s = data.decode("utf-8", "replace")
# 在每条光标定位前插入分隔，方便看"这一帧画了哪些行"
s = re.sub(r"\x1b\[(\d+);(\d+)H", r"\n  <row \1> ", s)
s = s.replace("\x1b[2J", "\n  <clear>")
s = s.replace("\x1b[K", "<eol>")
print(s)
