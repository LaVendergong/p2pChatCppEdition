#!/usr/bin/env python3
"""验证断线重连 + Ctrl-R 重发失败消息。

流程：起服务端 → 客户端登录 → 杀服务端 → 客户端发消息（必然失败并被标记）→
重启服务端 → 等自动重连 + 重新 join → 按 Ctrl-R → 消息应落盘并转正。
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = "7821"
DATA = "/tmp/chat_ctrlr_data"
SRV_LOG = "/tmp/chat_ctrlr_srv.log"
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*\x07")


def clean(b: bytes) -> str:
    return ANSI.sub("", b.decode("utf-8", "replace")).replace("\r", "")


def port_ready(port: str, timeout=5.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", int(port)), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def start_server():
    subprocess.run(["pkill", "-f", f"chatd --port {PORT}"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    log = open(SRV_LOG, "ab")
    p = subprocess.Popen(
        [os.path.join(PROJ, "build/bin/chatd"), "--port", PORT, "--data", DATA,
         "--rate-limit", "0", "--log-level", "warn"],
        cwd=PROJ, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    port_ready(PORT)
    return p


subprocess.run(["pkill", "-f", f"chatd --port {PORT}"], check=False)
shutil.rmtree(DATA, ignore_errors=True)
os.makedirs(DATA, exist_ok=True)

srv = start_server()
print("== server started")

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(os.path.join(PROJ, "build/bin/chatt"),
             ["chatt", "--user", "alice", "--password", "pw", "--port", PORT, "--room", "ctrlr"])

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

buf = b""


def pump(duration):
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


pump(2.5)
print("== client joined:", "joined #ctrlr" in clean(buf))

# 冻结服务端：连接还在，客户端会以为在线，消息写进内核缓冲但永远不会被 ACK
print("== freeze server (SIGSTOP)")
os.kill(srv.pid, signal.SIGSTOP)
time.sleep(0.3)

print("== type a message (will never be acked)")
os.write(fd, "message sent while server frozen\r".encode())
pump(1.2)
print("   pending marker …:", "…" in clean(buf[-1500:]))

print("== kill -9 the frozen server")
os.kill(srv.pid, signal.SIGKILL)
srv.wait()
pump(1.5)
print("   failed marker !:", "!" in clean(buf[-1500:]))

print("== restart server, wait for auto reconnect")
srv = start_server()
pump(6.0)
reconnected = "joined #ctrlr" in clean(buf[-4000:])
print("   reconnected:", reconnected)

print("== press Ctrl-R")
mark = len(buf)
os.write(fd, b"\x12")
pump(2.5)
chunk = clean(buf[mark:])
print("   --- output after Ctrl-R ---")
for line in [l for l in chunk.split("\n") if l.strip()][-6:]:
    print("  ", repr(line))

try:
    os.write(fd, b"\x04")
    pump(0.5)
except OSError:
    pass
try:
    os.close(fd)
except OSError:
    pass
os.waitpid(pid, 0)

res = subprocess.run([os.path.join(PROJ, "build/bin/cht-dump")] + [
    os.path.join(DATA, "rooms/ctrlr", f) for f in os.listdir(os.path.join(DATA, "rooms/ctrlr"))
    if f.endswith(".seg")], capture_output=True, text=True)
records = [l for l in res.stdout.split("\n") if l.strip()]
print(f"== stored records: {len(records)}")
for l in records:
    print("  ", l)

srv.kill()
srv.wait()
subprocess.run(["pkill", "-f", f"chatd --port {PORT}"], check=False)

ok = reconnected and len(records) == 1 and "message sent while server frozen" in res.stdout
print("\nCTRL-R TEST", "PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
