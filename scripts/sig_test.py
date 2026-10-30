#!/usr/bin/env python3
"""chatsig 信令联调：两个客户端注册 -> 查询 -> 收到 peer_incoming。

用法: python3 scripts/sig_test.py [port]
"""
import hashlib
import socket
import struct
import sys
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7900

MAGIC = b"CHT1"
T_HELLO, T_HELLO_ACK = 0x0001, 0x0002
T_LOGIN, T_LOGIN_ACK = 0x0010, 0x0011
T_PEER_REG, T_PEER_REG_ACK = 0x0060, 0x0061
T_PEER_LOOKUP, T_PEER_LOOKUP_ACK = 0x0062, 0x0063
T_PEER_INCOMING = 0x0064

TAG_USER, TAG_DIGEST, TAG_SALT, TAG_CODE = 0x0003, 0x0004, 0x0005, 0x000C
TAG_LAN_IP, TAG_TCP_PORT, TAG_CANDIDATES = 0x0019, 0x001A, 0x001B
TAG_PEER_NAME, TAG_PUBLIC_IP, TAG_PUBLIC_PORT = 0x001C, 0x001D, 0x001E
TAG_CAND_KIND = 0x001F


def tlv(tag, value):
    if isinstance(value, int):
        value = struct.pack(">H", value)
    elif isinstance(value, str):
        value = value.encode()
    return struct.pack(">HI", tag, len(value)) + value


def tlv_array(tag, items):
    body = struct.pack(">I", len(items))
    for it in items:
        body += struct.pack(">I", len(it)) + it
    return struct.pack(">HI", tag, len(body)) + body


def parse_tlv(data):
    out = {}
    i = 0
    while i + 6 <= len(data):
        tag, ln = struct.unpack(">HI", data[i:i + 6])
        i += 6
        if i + ln > len(data):
            break
        out[tag] = data[i:i + ln]
        i += ln
    return out


def parse_array(raw):
    items = []
    if len(raw) < 4:
        return items
    count = struct.unpack(">I", raw[:4])[0]
    i = 4
    for _ in range(count):
        if i + 4 > len(raw):
            break
        ln = struct.unpack(">I", raw[i:i + 4])[0]
        i += 4
        items.append(raw[i:i + ln])
        i += ln
    return items


class Client:
    def __init__(self, user, password="pw"):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.sock.settimeout(5)
        self.buf = b""
        self.seq = 0
        self.user = user
        self.password = password

    def send(self, mtype, payload=b""):
        self.seq += 1
        hdr = MAGIC + struct.pack(">HHIQI", 1, mtype, 0, self.seq, len(payload))
        self.sock.sendall(hdr + payload)

    def recv_frame(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            while len(self.buf) >= 24:
                if self.buf[:4] != MAGIC:
                    raise RuntimeError("bad magic")
                ver, mtype, flags, seq, ln = struct.unpack(">HHIQI", self.buf[4:24])
                if len(self.buf) < 24 + ln:
                    break
                payload = self.buf[24:24 + ln]
                self.buf = self.buf[24 + ln:]
                return mtype, seq, parse_tlv(payload)
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                raise RuntimeError("closed")
            self.buf += chunk
        return None

    def wait(self, want, timeout=5.0):
        while True:
            f = self.recv_frame(timeout)
            if f is None:
                raise RuntimeError(f"timeout waiting for {want:#x}")
            if f[0] == want:
                return f[2]

    def login(self):
        self.send(T_HELLO, tlv(0x0001, "sig_test/1") + tlv(0x0002, "sig_test"))
        self.wait(T_HELLO_ACK)
        self.send(T_LOGIN, tlv(TAG_USER, self.user))
        r = self.wait(T_LOGIN_ACK)
        code = struct.unpack(">H", r[TAG_CODE])[0]
        assert code == 3, f"expected need_digest(3), got {code}"
        salt = r[TAG_SALT].decode()
        digest = hashlib.sha256(f"{salt}:{self.password}".encode()).hexdigest()
        self.send(T_LOGIN, tlv(TAG_USER, self.user) + tlv(TAG_DIGEST, digest))
        r = self.wait(T_LOGIN_ACK)
        code = struct.unpack(">H", r[TAG_CODE])[0]
        assert code == 0, f"login failed code={code}"

    def register(self, tcp_port, lan_ip):
        self.send(T_PEER_REG, tlv(TAG_TCP_PORT, tcp_port) + tlv(TAG_LAN_IP, lan_ip))
        r = self.wait(T_PEER_REG_ACK)
        return r

    def lookup(self, peer):
        self.send(T_PEER_LOOKUP, tlv(TAG_PEER_NAME, peer))
        return self.wait(T_PEER_LOOKUP_ACK)


ok = True


def check(cond, msg):
    global ok
    print(("  OK   " if cond else "  FAIL ") + msg)
    if not cond:
        ok = False


a = Client("alice")
a.login()
ra = a.register(7800, "192.168.1.10")
print("== alice registered")
check(TAG_PUBLIC_IP in ra, "got public_ip: " + ra.get(TAG_PUBLIC_IP, b"").decode())
check(TAG_PUBLIC_PORT in ra, "got public_port")

b = Client("bob")
b.login()
b.register(7801, "192.168.1.11")
print("== bob registered")

time.sleep(0.2)
r = a.lookup("bob")
cands = parse_array(r.get(TAG_CANDIDATES, b""))
print(f"== alice looked up bob: {len(cands)} candidate(s)")
for c in cands:
    t = parse_tlv(c)
    kind = t.get(TAG_CAND_KIND, b"\xff")[0]
    ip = t.get(TAG_LAN_IP, b"").decode()
    port = struct.unpack(">H", t.get(TAG_TCP_PORT, b"\0\0"))[0]
    print(f"   kind={kind} {ip}:{port}")
check(len(cands) >= 1, "at least one candidate")
check(any(parse_tlv(c).get(TAG_LAN_IP, b"").decode() == "192.168.1.11" for c in cands),
      "lan candidate points to bob")

inc = b.recv_frame(2.0)
check(inc is not None and inc[0] == T_PEER_INCOMING, "bob received peer_incoming")
if inc and inc[0] == T_PEER_INCOMING:
    name = inc[2].get(TAG_PEER_NAME, b"").decode()
    check(name == "alice", f"incoming from {name}")

r = a.lookup("nobody")
check(len(parse_array(r.get(TAG_CANDIDATES, b""))) == 0, "unknown peer -> empty candidates")

a.sock.close()
time.sleep(0.3)
r = b.lookup("alice")
check(len(parse_array(r.get(TAG_CANDIDATES, b""))) == 0, "offline peer -> empty candidates")
b.sock.close()

print("\nSIG TEST", "PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
