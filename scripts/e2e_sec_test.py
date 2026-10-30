#!/usr/bin/env python3
"""端到端加密互操作测试（独立实现，不调用项目代码）。

用一个纯 Python 的 CHTP 对端连上真实的 `chatt --p2p`，验证：
  1) 握手派生出的会话密钥和 C++ 实现一致（双向都能解开对方的密文）；
  2) 双方算出的身份指纹一致（公钥绑定正确）；
  3) 明文 MSG_SEND 被丢弃；
  4) 重放同一序号被丢弃；
  5) 篡改密文被丢弃（认证失败）。

用法: python3 scripts/e2e_sec_test.py
"""
import fcntl
import hashlib
import hmac
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
A_PORT = 7800
DIR_A = "/tmp/sec_alice"
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07]*\x07")

MAGIC = b"CHT1"
T_HELLO, T_HELLO_ACK = 0x0001, 0x0002
T_MSG_SEND = 0x0030
T_CRYPTO_KEX = 0x0080
TAG_USER = 0x0003
TAG_TS = 0x000B
TAG_KEX_PUB, TAG_KEX_EPH, TAG_CIPHER, TAG_NONCE = 0x0021, 0x0022, 0x0023, 0x0024

AAD_MSG = b"chatt-p2p/msg-v1"
INFO = b"chatt-p2p/v2"

# ------------------------------------------------------------------ 密码学
P25519 = 2 ** 255 - 19


def x25519(priv, peer_pub):
    k = bytearray(priv)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    n = int.from_bytes(k, "little")
    x1 = int.from_bytes(peer_pub, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in reversed(range(255)):
        kt = (n >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = (x2 + z2) % P25519
        aa = a * a % P25519
        b = (x2 - z2) % P25519
        bb = b * b % P25519
        e = (aa - bb) % P25519
        c = (x3 + z3) % P25519
        d = (x3 - z3) % P25519
        da = d * a % P25519
        cb = c * b % P25519
        x3 = (da + cb) ** 2 % P25519
        z3 = x1 * ((da - cb) ** 2 % P25519) % P25519
        x2 = aa * bb % P25519
        z2 = e * ((aa + 121665 * e) % P25519) % P25519
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, P25519 - 2, P25519) % P25519).to_bytes(32, "little")


def hkdf_sha256(salt, ikm, info, n):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, t, i = b"", b"", 1
    while len(out) < n:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:n]


def rotl32(v, n):
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def chacha_block(key, nonce, counter):
    x = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    x += [int.from_bytes(key[4 * i:4 * i + 4], "little") for i in range(8)]
    x += [counter] + [int.from_bytes(nonce[4 * i:4 * i + 4], "little") for i in range(3)]
    w = list(x)

    def qr(a, b, c, d):
        w[a] = (w[a] + w[b]) & 0xFFFFFFFF
        w[d] = rotl32(w[d] ^ w[a], 16)
        w[c] = (w[c] + w[d]) & 0xFFFFFFFF
        w[b] = rotl32(w[b] ^ w[c], 12)
        w[a] = (w[a] + w[b]) & 0xFFFFFFFF
        w[d] = rotl32(w[d] ^ w[a], 8)
        w[c] = (w[c] + w[d]) & 0xFFFFFFFF
        w[b] = rotl32(w[b] ^ w[c], 7)

    for _ in range(10):
        qr(0, 4, 8, 12)
        qr(1, 5, 9, 13)
        qr(2, 6, 10, 14)
        qr(3, 7, 11, 15)
        qr(0, 5, 10, 15)
        qr(1, 6, 11, 12)
        qr(2, 7, 8, 13)
        qr(3, 4, 9, 14)
    return b"".join(((w[i] + x[i]) & 0xFFFFFFFF).to_bytes(4, "little") for i in range(16))


def poly1305(key, data):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    acc = 0
    for i in range(0, len(data), 16):
        acc = ((acc + int.from_bytes(data[i:i + 16], "little") + (1 << 128)) * r) % ((1 << 130) - 5)
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def aead_seal(key, nonce, aad, plain):
    ks = b"".join(chacha_block(key, nonce, 1 + i // 64) for i in range(0, max(len(plain), 1), 64))
    ct = bytes(a ^ b for a, b in zip(plain, ks))
    otk = chacha_block(key, nonce, 0)[:32]
    mac_data = (aad + b"\0" * ((16 - len(aad) % 16) % 16) + ct + b"\0" * ((16 - len(ct) % 16) % 16)
                + len(aad).to_bytes(8, "little") + len(ct).to_bytes(8, "little"))
    return ct + poly1305(otk, mac_data)


def aead_open(key, nonce, aad, blob):
    ct, tag = blob[:-16], blob[-16:]
    otk = chacha_block(key, nonce, 0)[:32]
    mac_data = (aad + b"\0" * ((16 - len(aad) % 16) % 16) + ct + b"\0" * ((16 - len(ct) % 16) % 16)
                + len(aad).to_bytes(8, "little") + len(ct).to_bytes(8, "little"))
    if poly1305(otk, mac_data) != tag:
        return None
    ks = b"".join(chacha_block(key, nonce, 1 + i // 64) for i in range(0, max(len(ct), 1), 64))
    return bytes(a ^ b for a, b in zip(ct, ks))


def fingerprint(pub):
    h = hashlib.sha256(pub).hexdigest()[:32]
    return ":".join(h[i:i + 4] for i in range(0, 32, 4))


def msg_nonce(ctr):
    return ctr.to_bytes(8, "little") + b"\0\0\0\0"


# ------------------------------------------------------------------ 协议
def tlv(tag, value):
    if isinstance(value, int):
        value = struct.pack(">Q", value) if tag in (TAG_NONCE, TAG_TS) else struct.pack(">H", value)
    elif isinstance(value, str):
        value = value.encode()
    return struct.pack(">HI", tag, len(value)) + value


def parse_tlv(data):
    out, i = {}, 0
    while i + 6 <= len(data):
        tag, ln = struct.unpack(">HI", data[i:i + 6])
        i += 6
        if i + ln > len(data):
            break
        out[tag] = data[i:i + ln]
        i += ln
    return out


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(2)
        self.buf = b""
        self.seq = 0

    def send(self, mtype, payload=b""):
        self.seq += 1
        self.sock.sendall(MAGIC + struct.pack(">HHIQI", 1, mtype, 0, self.seq, len(payload)) + payload)

    def recv(self, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            while len(self.buf) >= 24:
                ver, mtype, flags, seq, ln = struct.unpack(">HHIQI", self.buf[4:24])
                if len(self.buf) < 24 + ln:
                    break
                payload = self.buf[24:24 + ln]
                self.buf = self.buf[24 + ln:]
                return mtype, parse_tlv(payload)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                raise RuntimeError("peer closed")
            self.buf += chunk
        return None

    def wait(self, want, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            f = self.recv(end - time.time())
            if f is None:
                break
            if f[0] == want:
                return f[1]
        return None


class Ui:
    """真实 chatt 的伪终端，用于读它屏幕上的提示。"""

    def __init__(self, args):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(os.path.join(PROJ, "build/bin/chatt"), ["chatt"] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
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
        return ANSI.sub("", self.buf.decode("utf-8", "replace")).replace("\r", "")

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


ok = True


def check(cond, msg):
    global ok
    print(("  OK   " if cond else "  FAIL ") + msg)
    if not cond:
        ok = False


# ------------------------------------------------------------------ 场景
shutil.rmtree(DIR_A, ignore_errors=True)
subprocess.run(["pkill", "-f", "chatsig --port"], check=False)
subprocess.run(["pkill", "-f", "chatt --p2p"], check=False)
time.sleep(0.4)

sig = subprocess.Popen([os.path.join(PROJ, "build/bin/chatsig"), "--port", SIG_PORT,
                        "--data", "/tmp/sec_sigdata", "--log-level", "warn"],
                       cwd=PROJ, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
time.sleep(0.8)

alice = Ui(["--p2p", "--user", "alice", "--password", "pw", "--sig", f"127.0.0.1:{SIG_PORT}",
            "--listen", str(A_PORT), "--data", DIR_A])
alice.pump(3.0)
check("ready:" in alice.text(), "alice is listening")

# 我们冒充 bob 直连 alice
me_priv = hashlib.sha256(b"e2e-sec-test-identity").digest()
me_pub = x25519(me_priv, bytes([9] + [0] * 31))
eph_priv = hashlib.sha256(b"e2e-sec-test-ephemeral").digest()
eph_pub = x25519(eph_priv, bytes([9] + [0] * 31))
my_fp = fingerprint(me_pub)

peer = Peer(A_PORT)
peer.send(T_HELLO, tlv(TAG_USER, "bob") + tlv(0x0001, "python/1"))
hello_ack = peer.wait(T_HELLO_ACK)
check(hello_ack is not None, "handshake: hello_ack")
peer_kex = peer.wait(T_CRYPTO_KEX)
check(peer_kex is not None and len(peer_kex.get(TAG_KEX_PUB, b"")) == 32, "handshake: got alice's public key")

alice_pub = peer_kex[TAG_KEX_PUB]
alice_eph = peer_kex[TAG_KEX_EPH]
peer.send(T_CRYPTO_KEX, tlv(TAG_KEX_PUB, me_pub) + tlv(TAG_KEX_EPH, eph_pub))

# 与 C++ 完全相同的派生规则（四重 DH，顺序按身份公钥字节序规范）
low = me_pub < alice_pub
s1 = x25519(me_priv, alice_pub)
if low:
    s2 = x25519(eph_priv, alice_pub)   # 临时(小) × 身份(大)
    s3 = x25519(me_priv, alice_eph)    # 身份(小) × 临时(大)
    salt = me_pub + alice_pub + eph_pub + alice_eph
else:
    s2 = x25519(me_priv, alice_eph)
    s3 = x25519(eph_priv, alice_pub)
    salt = alice_pub + me_pub + alice_eph + eph_pub
s4 = x25519(eph_priv, alice_eph)
okm = hkdf_sha256(salt, s1 + s2 + s3 + s4, INFO, 64)
send_key, recv_key = (okm[:32], okm[32:]) if low else (okm[32:], okm[:32])

alice.pump(1.0)
check("e2e ready with bob" in alice.text(), "alice reports e2e ready")
check(my_fp in alice.text(), f"alice shows our fingerprint {my_fp}")

# 1) alice -> python：alice 在界面上发一条，我们用派生出的 recv_key 解开
alice.send("alice-to-python interop\r")
alice.pump(0.4)
frame = peer.wait(T_MSG_SEND, 3.0)
plain = None
if frame and TAG_CIPHER in frame and TAG_NONCE in frame:
    ctr = struct.unpack(">Q", frame[TAG_NONCE])[0]
    plain = aead_open(recv_key, msg_nonce(ctr), AAD_MSG, frame[TAG_CIPHER])
check(plain == b"alice-to-python interop", f"decrypted alice's ciphertext: {plain!r}")

# 2) python -> alice：用 send_key 加密，alice 应该正常显示
ctr = 1
blob = aead_seal(send_key, msg_nonce(ctr), AAD_MSG, b"python-to-alice interop")
peer.send(T_MSG_SEND, tlv(TAG_NONCE, ctr) + tlv(TAG_TS, int(time.time() * 1000)) + tlv(TAG_CIPHER, blob))
alice.pump(1.2)
check("python-to-alice interop" in alice.text(), "alice decrypted our message")

# 3) 重放同一序号
peer.send(T_MSG_SEND, tlv(TAG_NONCE, ctr) + tlv(TAG_TS, int(time.time() * 1000)) + tlv(TAG_CIPHER, blob))
alice.pump(1.0)
check("dropped replayed message" in alice.text(), "replay rejected")

# 4) 篡改密文（认证失败）
blob2 = bytearray(aead_seal(send_key, msg_nonce(ctr + 1), AAD_MSG, b"tampered payload"))
blob2[-1] ^= 0x01
peer.send(T_MSG_SEND, tlv(TAG_NONCE, ctr + 1) + tlv(TAG_TS, int(time.time() * 1000)) + tlv(TAG_CIPHER, bytes(blob2)))
alice.pump(1.0)
check("failed authentication" in alice.text(), "tampered ciphertext rejected")
check("tampered payload" not in alice.text(), "tampered plaintext never displayed")

# 5) 明文消息（旧版/伪造客户端）
peer.send(T_MSG_SEND, tlv(0x0007, "PLAINTEXT-LEAK") + tlv(TAG_TS, int(time.time() * 1000)))
alice.pump(1.0)
check("dropped a plaintext/invalid message" in alice.text(), "plaintext message rejected")
check("PLAINTEXT-LEAK" not in alice.text(), "plaintext never displayed")

print("\n--- alice screen ---")
for line in [l for l in alice.text().split("\n") if l.strip()][-14:]:
    print("  ", line)

peer.sock.close()
alice.close()
sig.kill()
sig.wait()
subprocess.run(["pkill", "-f", "chatsig --port"], check=False)

print("\nE2E SEC TEST", "PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
