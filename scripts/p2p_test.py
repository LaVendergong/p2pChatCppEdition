#!/usr/bin/env python3
"""P2P 端到端：chatsig + 两个客户端，alice 输入 /chat bob 后直连互发消息。

用法: python3 scripts/p2p_test.py
"""
import fcntl
import os
import pty
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import termios
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIG_PORT = "7900"
A_PORT = "7800"
B_PORT = "7801"
DIR_A = "/tmp/p2p_alice"
DIR_B = "/tmp/p2p_bob"
FORCE_RELAY = len(sys.argv) > 1 and sys.argv[1] == "relay"
MODE_ARGS = ["--force-relay"] if FORCE_RELAY else []
print("== mode:", "relay (forced)" if FORCE_RELAY else "direct")
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*\x07")


def clean(b):
    return ANSI.sub("", b.decode("utf-8", "replace")).replace("\r", "")


def port_ready(port, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", int(port)), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class Ui:
    def __init__(self, args):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(os.path.join(PROJ, "build/bin/chatt"), ["chatt"] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 90, 0, 0))
        self.buf = b""

    def pump(self, dur):
        end = time.time() + dur
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                d = os.read(self.fd, 65536)
            except OSError:
                return
            if not d:
                return
            self.buf += d

    def send(self, s):
        os.write(self.fd, s.encode())

    def text(self):
        return clean(self.buf)

    def close(self):
        try:
            self.send("\x04")
            self.pump(0.3)
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except OSError:
            pass


for d in (DIR_A, DIR_B):
    shutil.rmtree(d, ignore_errors=True)

subprocess.run(["pkill", "-f", "chatsig --port"], check=False)
subprocess.run(["pkill", "-f", f"chatt --p2p"], check=False)
time.sleep(0.4)

sig_log = open("/tmp/p2p_sig.log", "ab")
sig = subprocess.Popen([os.path.join(PROJ, "build/bin/chatsig"), "--port", SIG_PORT,
                        "--data", "/tmp/p2p_sigdata", "--log-level", "debug"],
                       cwd=PROJ, stdout=sig_log, stderr=subprocess.STDOUT, start_new_session=True)
if not port_ready(SIG_PORT):
    print("chatsig failed to start")
    sys.exit(1)
print("== chatsig up")

alice = Ui(["--p2p", "--user", "alice", "--password", "pw", "--sig", f"127.0.0.1:{SIG_PORT}",
            "--listen", A_PORT, "--data", DIR_A] + MODE_ARGS)
bob = Ui(["--p2p", "--user", "bob", "--password", "pw", "--sig", f"127.0.0.1:{SIG_PORT}",
          "--listen", B_PORT, "--data", DIR_B] + MODE_ARGS)
alice.pump(3.0)
bob.pump(2.0)
print("== alice registered:", "ready:" in alice.text())
print("== bob registered:", "ready:" in bob.text())

print("== alice: /chat bob")
alice.send("/chat bob\r")
alice.pump(2.5)
bob.pump(0.6)
print("== bob sees session:", "session with alice" in bob.text())

print("== alice sends a message")
alice.send("hello from alice\r")
alice.pump(1.5)
bob.pump(1.0)
print("== bob received:", "hello from alice" in bob.text())

print("== bob replies")
bob.send("hi alice, got it\r")
bob.pump(1.5)
alice.pump(1.0)
print("== alice received reply:", "hi alice, got it" in alice.text())

FP = r"([0-9a-f]{4}(?::[0-9a-f]{4}){7})"


def own_fp(text):
    m = re.search(r"identity fingerprint " + FP, text)
    return m.group(1) if m else ""


def peer_fp(text, who):
    m = re.search(r"e2e ready with " + who + r"\s+fp " + FP, text)
    return m.group(1) if m else ""


a_own, b_own = own_fp(alice.text()), own_fp(bob.text())
a_view, b_view = peer_fp(alice.text(), "bob"), peer_fp(bob.text(), "alice")
print("== alice fp:", a_own, " sees bob:", a_view)
print("== bob   fp:", b_own, " sees alice:", b_view)
crypto_ok = (a_own != "" and a_view == b_own and b_view == a_own)
print("== e2e fingerprint cross-check:", crypto_ok)

print("\n--- alice screen ---")
for line in [l for l in alice.text().split("\n") if l.strip()][-8:]:
    print("  ", line)
print("--- bob screen ---")
for line in [l for l in bob.text().split("\n") if l.strip()][-8:]:
    print("  ", line)

alice.close()
bob.close()
time.sleep(0.5)

res = subprocess.run([os.path.join(PROJ, "build/bin/cht-dump")], capture_output=True, text=True)
dump_ok = True
for who, d, peer in (("alice", DIR_A, "bob"), ("bob", DIR_B, "alice")):
    segs_dir = os.path.join(d, "peers", peer)
    segs = [os.path.join(segs_dir, f) for f in os.listdir(segs_dir) if f.endswith(".seg")] if os.path.isdir(segs_dir) else []
    if not segs:
        print(f"== {who}: no local store for {peer}")
        dump_ok = False
        continue
    out = subprocess.run([os.path.join(PROJ, "build/bin/cht-dump")] + segs, capture_output=True, text=True).stdout
    lines = [l for l in out.split("\n") if l.strip()]
    print(f"== {who} local store ({peer}): {len(lines)} record(s)")
    for l in lines:
        print("  ", l)

sig.kill()
sig.wait()
subprocess.run(["pkill", "-f", "chatsig --port"], check=False)

ok = (("hello from alice" in bob.text()) and ("hi alice, got it" in alice.text()) and dump_ok and crypto_ok)
print("\nP2P TEST", "PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
