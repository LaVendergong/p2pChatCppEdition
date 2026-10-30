#!/usr/bin/env python3
"""用伪终端驱动 chatt 的 raw mode，验证按键、发送、渲染是否真的工作。

用法: python3 scripts/pty_test.py [port]
输出: 每轮交互后新产生的字节（已剥离 ANSI），以及原始字节存到 /tmp/pty_raw.bin
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
    return s.replace("\r\n", "\n").replace("\r", "\n")


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
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        buf += data


def step(label: str, mark_before: int) -> int:
    chunk = buf[mark_before:]
    print(f"\n===== {label} (new bytes: {len(chunk)}) =====")
    text = clean(chunk)
    # 只显示有内容的行，避免大量空白
    lines = [ln.rstrip() for ln in text.split("\n")]
    shown = [ln for ln in lines if ln.strip()]
    for ln in shown[-40:]:
        print(ln)
    return len(buf)


pump(2.5)
m = step("1. 登录并加入房间后的首屏", 0)

# 逐字符输入（模拟真人打字），应能实时看到输入行回显
for ch in "abc":
    os.write(fd, ch.encode())
    pump(0.25)
m = step("2a. 逐字符输入 'abc'（未回车，应看到输入行回显）", m)

os.write(fd, b"\x7f")  # Backspace
pump(0.3)
m = step("2b. 退格一次（应为 '> ab'）", m)

os.write(fd, " def\r".encode())
pump(1.5)
m = step("2c. 补完 'c def' 并回车后（应清空输入行并出现消息）", m)

os.write(fd, "/help\r".encode())
pump(1.5)
m = step("3. 输入 /help 后", m)

os.write(fd, "中文测试消息\r".encode())
pump(1.5)
m = step("4. 输入中文并回车后", m)

os.write(fd, b"\x03")  # Ctrl-C 清空
pump(0.5)
m = step("5. Ctrl-C 后", m)

with open("/tmp/pty_raw.bin", "wb") as f:
    f.write(buf)
print(f"\n原始字节已存 /tmp/pty_raw.bin ({len(buf)} bytes)")

try:
    os.write(fd, b"\x04")  # Ctrl-D 退出
    pump(0.5)
except OSError:
    pass
os.close(fd)
os.waitpid(pid, 0)
