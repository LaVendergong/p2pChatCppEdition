#!/usr/bin/env python3
"""验证 M4 新增交互：输入折行、未确认标记、PgUp 自动加载历史。

前置：testroom 里已有 > 50 条历史（先用 cht-load 灌）。
用法: python3 scripts/pty_test2.py [port]
"""
import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PROJ)
PORT = sys.argv[1] if len(sys.argv) > 1 else "7799"

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*\x07")


def clean(b: bytes) -> str:
    s = b.decode("utf-8", "replace")
    s = ANSI.sub("", s)
    return s.replace("\r", "")


pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(
        os.path.join(PROJ, "build/bin/chatt"),
        ["chatt", "--user", "alice", "--password", "pw", "--port", PORT, "--room", "testroom"],
    )

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

buf = b""


def pump(duration: float) -> None:
    global buf
    end = time.time() + duration
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if not r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        buf += data


def step(label: str, mark: int, show=12) -> int:
    chunk = buf[mark:]
    text = clean(chunk)
    lines = [ln.rstrip() for ln in text.split("\n") if ln.strip()]
    print(f"\n===== {label} (new bytes {len(chunk)}) =====")
    for ln in lines[-show:]:
        print(repr(ln))
    return len(buf)


pump(2.5)
m = step("1. 首屏（应有最近 50 条历史）", 0, 4)

# --- 输入折行：60 个中文（宽 120 列）应折成 2 行以上
long_text = "这是一条很长的消息" * 6
os.write(fd, long_text.encode())
pump(0.8)
m = step("2. 输入 60 个中文（应出现多行输入区，每行带 '> ' 或续行缩进）", m, 8)

# --- 未确认标记：回车后 100ms 内抓一帧，行尾应有 …
os.write(fd, b"\r")
pump(0.05)
m = step("3a. 回车后 50ms（行尾应有 … 表示未确认）", m, 6)
pump(0.8)
m = step("3b. ACK 到达后（… 应消失）", m, 6)

# --- PgUp 连续翻页，触发自动加载更早历史
for _ in range(12):
    os.write(fd, b"\x1b[5~")
    pump(0.12)
m = step("4. 连按 12 次 PgUp（应看到更早的消息 id，并触发 HISTORY_REQ）", m, 10)

os.write(fd, b"\x03")  # Ctrl-C
pump(0.3)
try:
    os.write(fd, b"\x04")
    pump(0.3)
except OSError:
    pass

with open("/tmp/pty_raw2.bin", "wb") as f:
    f.write(buf)
print(f"\n原始字节: /tmp/pty_raw2.bin ({len(buf)} bytes)")

try:
    os.close(fd)
except OSError:
    pass
os.waitpid(pid, 0)
