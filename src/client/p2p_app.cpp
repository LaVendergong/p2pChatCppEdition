#include "p2p_app.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "chat/fs.h"
#include "chat/log.h"
#include "chat/sha256.h"
#include "chat/time_util.h"
#include "chat/utf8.h"
#include "storage.h"

namespace chat {
namespace {

volatile sig_atomic_t g_winch = 0;
volatile sig_atomic_t g_stop = 0;

void on_signal(int sig) {
    if (sig == SIGWINCH) {
        g_winch = 1;
    } else {
        g_stop = 1;
    }
}

std::string paint(const std::string& s, const char* code) {
    std::string out = "\x1b[";
    out += code;
    out += "m";
    out += s;
    out += "\x1b[0m";
    return out;
}

constexpr size_t kMaxMsgs = 2000;

// 聊天消息的 AEAD 附加认证数据：固定字符串，把"聊天内容"和别的帧类型隔开
constexpr char kAadMsg[] = "chatt-p2p/msg-v1";

// 消息序号既做重放检查也做 AEAD nonce（12 字节，前 8 字节小端存序号）
void msg_nonce(uint8_t nonce[12], uint64_t ctr) {
    std::memset(nonce, 0, 12);
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(ctr >> (8 * i));
}

std::string hex_encode(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

std::vector<uint8_t> hex_decode(std::string_view s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]);
        const int lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        v.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return v;
}

}  // namespace

P2PApp::P2PApp(P2POptions opt) : opt_(std::move(opt)) {}
P2PApp::~P2PApp() { term_.leave(); }

// ---------------------------------------------------------------- 信令
bool P2PApp::sig_connect() {
    sig_close();
    Status st = sig_.connect_to(opt_.sig_host, opt_.sig_port, 3000);
    if (!st) {
        next_retry_ms_ = steady_ms() + static_cast<uint64_t>(backoff_ms_);
        backoff_ms_ = std::min(backoff_ms_ * 2, 8000);
        return false;
    }
    sig_online_ = true;
    sig_logged_in_ = false;
    sig_registered_ = false;
    sig_salt_.clear();
    sig_digest_.clear();
    sig_seq_ = 0;
    sig_send_hello();
    add_system(fmt("signaling connected to {}:{}", opt_.sig_host, opt_.sig_port));
    dirty_ = true;
    return true;
}

void P2PApp::sig_close() {
    sig_.close();
    sig_online_ = false;
    sig_logged_in_ = false;
    sig_registered_ = false;
}

void P2PApp::sig_send(MsgType t, const std::string& payload) {
    if (!sig_.connected()) return;
    sig_.send(t, ++sig_seq_, payload);
    (void)sig_.flush();
}

void P2PApp::sig_send_hello() {
    TlvWriter w;
    w.add_str(tag::version, "chatt-p2p/0.1.0");
    w.add_str(tag::client_name, "chatt-p2p");
    sig_send(MsgType::hello, w.data());
}

void P2PApp::sig_send_login() {
    TlvWriter w;
    w.add_str(tag::username, opt_.user);
    if (!sig_digest_.empty()) w.add_str(tag::password_hash, sig_digest_);
    sig_send(MsgType::login, w.data());
}

std::string P2PApp::listen_lan_ip() const {
    // 用信令连接的本端地址判断自己在局域网里的 IP
    if (!sig_.connected()) return "127.0.0.1";
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(sig_.fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "127.0.0.1";
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return ip;
}

void P2PApp::sig_send_register() {
    TlvWriter w;
    w.add_u16(tag::tcp_port, listen_port_);
    w.add_str(tag::lan_ip, listen_lan_ip());
    sig_send(MsgType::peer_register, w.data());
}

void P2PApp::sig_on_read() {
    std::vector<Frame> frames;
    bool closed = false;
    Status st = sig_.read_frames(frames, closed);
    for (auto& f : frames) {
        sig_dispatch(f);
        if (quit_) return;
    }
    if (closed) {
        add_system("signaling disconnected, retrying...");
        sig_close();
        next_retry_ms_ = steady_ms() + static_cast<uint64_t>(backoff_ms_);
        backoff_ms_ = std::min(backoff_ms_ * 2, 8000);
        dirty_ = true;
    }
}

void P2PApp::sig_dispatch(const Frame& f) {
    switch (f.header.type) {
        case MsgType::hello_ack:
            sig_send_login();
            break;
        case MsgType::login_ack: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 999);
            if (code == static_cast<uint16_t>(Err::unauthenticated)) {
                std::string salt;
                r.get_str(tag::salt, salt);
                if (salt.empty()) {
                    add_system("signaling rejected login");
                    quit_ = true;
                    break;
                }
                sig_salt_ = salt;
                sig_digest_ = password_digest(sig_salt_, opt_.password);
                sig_send_login();
                break;
            }
            if (code == 0) {
                sig_logged_in_ = true;
                add_system(fmt("signed in to signaling as {}", opt_.user));
                sig_send_register();
                dirty_ = true;
            } else {
                add_system(fmt("signaling login failed (code {})", code));
                quit_ = true;
            }
            break;
        }
        case MsgType::peer_register_ack: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 999);
            if (code != 0) {
                std::string reason;
                r.get_str(tag::reason, reason);
                add_system("registration failed: " + reason);
                break;
            }
            sig_registered_ = true;
            backoff_ms_ = 500;
            std::string pub_ip;
            r.get_str(tag::public_ip, pub_ip);
            const uint16_t pub_port = r.u16_or(tag::public_port, 0);
            add_system(fmt("ready: {} listening on :{}, public {}:{}", opt_.user, listen_port_, pub_ip, pub_port));
            add_system("type /chat <name> to start talking, /help for help");
            dirty_ = true;
            break;
        }
        case MsgType::peer_lookup_ack: {
            TlvReader r(f.payload);
            std::string name;
            r.get_str(tag::peer_name, name);
            std::vector<std::string_view> items;
            std::vector<Candidate> cands;
            if (r.get_array(tag::candidates, items)) {
                for (const auto& it : items) {
                    TlvReader ir(it);
                    Candidate c;
                    c.kind = ir.u8_or(tag::cand_kind, 0);
                    ir.get_str(tag::lan_ip, c.ip);
                    c.port = ir.u16_or(tag::tcp_port, 0);
                    if (!c.ip.empty() && c.port != 0) cands.push_back(std::move(c));
                }
            }
            if (cands.empty()) {
                add_system(fmt("{} is not online (or has not registered yet)", name));
                pending_lookup_.clear();
            } else if (!pending_lookup_.empty()) {
                const std::string target = pending_lookup_;
                pending_lookup_.clear();
                connect_peer(target, cands);
            }
            break;
        }
        case MsgType::peer_incoming: {
            TlvReader r(f.payload);
            std::string name;
            r.get_str(tag::peer_name, name);
            const uint8_t relay = r.u8_or(tag::relay_flag, 0);
            if (relay != 0) {
                add_system(name + " wants to reach you via relay, accepting...");
                TlvWriter w;
                w.add_str(tag::peer_name, name);
                sig_send(MsgType::relay_open, w.data());
            } else {
                add_system(fmt("{} wants to connect; waiting for it...", name));
            }
            dirty_ = true;
            break;
        }
        case MsgType::relay_ack: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 999);
            std::string name;
            r.get_str(tag::peer_name, name);
            if (code != 0) {
                std::string reason;
                r.get_str(tag::reason, reason);
                add_system(fmt("relay to {} failed: {}", name, reason.empty() ? "unknown" : reason));
                dirty_ = true;
                break;
            }
            relay_peer_ = name;
            auto pc = std::make_unique<PeerConn>();
            pc->name = name;
            pc->relay = true;
            pc->last_active = steady_ms();
            pc->last_ping = pc->last_active;
            TlvWriter hw;
            hw.add_str(tag::username, opt_.user);
            hw.add_str(tag::version, "chatt-p2p/0.1.0");
            peer_send(*pc, MsgType::hello, 1, hw.data());  // 走中继
            peers_.push_back(std::move(pc));
            current_ = name;
            chat_of(name);
            add_system("relay established with " + name);
            cache_dirty_ = true;
            dirty_ = true;
            break;
        }
        case MsgType::relay_data: {
            // payload 是对方发来的完整内层帧
            Frame inner;
            size_t consumed = 0;
            if (parse_frame(f.payload, inner, consumed) != ParseStatus::ok) break;
            for (auto& p : peers_) {
                if (p->relay) {
                    on_peer_frame(*p, inner);
                    break;
                }
            }
            break;
        }
        case MsgType::peer_offline: {
            TlvReader r(f.payload);
            std::string name;
            r.get_str(tag::peer_name, name);
            add_system(name + " went offline");
            if (name == relay_peer_) {
                relay_peer_.clear();
                peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                            [](const std::unique_ptr<PeerConn>& p) { return p->relay; }),
                             peers_.end());
            }
            dirty_ = true;
            break;
        }
        case MsgType::error: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 0);
            std::string reason;
            r.get_str(tag::reason, reason);
            add_system(fmt("signaling error {}: {}", code, reason));
            dirty_ = true;
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- 对端
P2PApp::PeerConn* P2PApp::find_peer(const std::string& name) {
    for (auto& p : peers_) {
        if (p->name == name && (p->relay || p->net.connected())) return p.get();
    }
    return nullptr;
}

void P2PApp::start_chat(const std::string& peer) {
    if (!valid_username(peer)) {
        add_system("invalid name (2-32 chars, [A-Za-z0-9_-])");
        dirty_ = true;
        return;
    }
    if (peer == opt_.user) {
        add_system("that's you");
        dirty_ = true;
        return;
    }
    if (PeerConn* c = find_peer(peer); c && c->ready) {
        current_ = peer;
        add_system("switched to " + peer);
        cache_dirty_ = true;
        dirty_ = true;
        return;
    }
    if (!sig_registered_) {
        add_system("signaling not ready yet, cannot look up peers");
        dirty_ = true;
        return;
    }
    pending_lookup_ = peer;
    TlvWriter w;
    w.add_str(tag::peer_name, peer);
    sig_send(MsgType::peer_lookup, w.data());
    add_system("looking up " + peer + "...");
    dirty_ = true;
}

void P2PApp::connect_peer(const std::string& peer, const std::vector<Candidate>& cands) {
    if (!opt_.force_relay) {
        for (const auto& c : cands) {
            auto pc = std::make_unique<PeerConn>();
            pc->name = peer;
            Status st = pc->net.connect_to(c.ip, c.port, 800);
            if (!st) continue;
            TlvWriter w;
            w.add_str(tag::username, opt_.user);
            w.add_str(tag::version, "chatt-p2p/0.1.0");
            pc->net.send(MsgType::hello, 1, w.data());
            (void)pc->net.flush();
            pc->last_active = steady_ms();
            pc->last_ping = pc->last_active;
            peers_.push_back(std::move(pc));
            current_ = peer;
            chat_of(peer);
            add_system(fmt("connected to {} via {}:{} ({})", peer, c.ip, c.port, c.kind == 0 ? "lan" : "public"));
            cache_dirty_ = true;
            dirty_ = true;
            return;
        }
        add_system(fmt("direct connection to {} failed ({} candidate(s)), requesting relay...", peer,
                       cands.size()));
    } else {
        add_system(fmt("force-relay mode: skipping direct connect to {}", peer));
    }

    // 直连不行就走中继（对方收到 peer_incoming 后也会申请，chatsig 配对后双方收到 relay_ack）
    TlvWriter w;
    w.add_str(tag::peer_name, peer);
    sig_send(MsgType::relay_open, w.data());
    pending_lookup_.clear();
    dirty_ = true;
}

void P2PApp::accept_inbound() {
    for (;;) {
        UniqueFd fd;
        std::string peer;
        Status st = net_accept(listen_fd_.get(), fd, peer);
        if (!st) break;
        auto pc = std::make_unique<PeerConn>();
        pc->net.adopt(std::move(fd));
        pc->inbound = true;
        pc->last_active = steady_ms();
        pc->last_ping = pc->last_active;
        TlvWriter w;
        w.add_str(tag::username, opt_.user);
        w.add_str(tag::version, "chatt-p2p/0.1.0");
        pc->net.send(MsgType::hello, 1, w.data());
        (void)pc->net.flush();
        peers_.push_back(std::move(pc));
        LOG_INFO("p2p: inbound connection from {}", peer);
        add_system("inbound connection from " + peer);
        dirty_ = true;
    }
}

void P2PApp::peer_send(PeerConn& c, MsgType t, uint64_t seq, const std::string& payload) {
    if (c.relay) {
        // 中继：内层帧原样塞进 RELAY_DATA 的 payload，由 chatsig 转发（它不解包）
        if (!sig_.connected()) return;
        std::string inner = encode_frame(t, seq, payload);
        sig_.send(MsgType::relay_data, ++sig_seq_, inner);
        (void)sig_.flush();
        return;
    }
    if (!c.net.connected()) return;
    c.net.send(t, seq, payload);
}

void P2PApp::on_peer_read(PeerConn& c) {
    std::vector<Frame> frames;
    bool closed = false;
    Status st = c.net.read_frames(frames, closed);
    for (auto& f : frames) {
        on_peer_frame(c, f);
        if (quit_) return;
        if (!c.net.connected()) return;
    }
    if (closed) {
        const std::string name = c.name;
        c.net.close();
        add_system((name.empty() ? std::string("peer") : name) + " disconnected");
        dirty_ = true;
    }
}

void P2PApp::on_peer_frame(PeerConn& c, const Frame& f) {
    switch (f.header.type) {
        case MsgType::hello: {
            TlvReader r(f.payload);
            std::string name;
            r.get_str(tag::username, name);
            if (!valid_username(name)) {
                c.net.close();
                add_system("peer sent invalid name, dropped");
                return;
            }
            const bool was_ready = c.ready;
            c.name = name;
            c.ready = true;
            TlvWriter w;
            w.add_str(tag::username, opt_.user);
            w.add_str(tag::version, "chatt-p2p/0.1.0");
            c.net.send(MsgType::hello_ack, f.header.seq, w.data());
            (void)c.net.flush();
            // 只有还没开对话时才抢占焦点，别把正在聊的人挤掉
            if (current_.empty() || current_ == "__info__") current_ = name;
            chat_of(name);
            if (!was_ready) add_system("session with " + name + " is up");
            kex_send(c);  // 连接可用 → 立刻开始密钥交换
            cache_dirty_ = true;
            dirty_ = true;
            break;
        }
        case MsgType::hello_ack: {
            const bool was_ready = c.ready;
            c.ready = true;
            TlvReader r(f.payload);
            std::string name;
            r.get_str(tag::username, name);
            if (!name.empty()) c.name = name;
            if (current_.empty() || current_ == "__info__") current_ = c.name;
            chat_of(c.name);
            if (!was_ready) add_system("session with " + c.name + " is up");
            kex_send(c);  // 连接可用 → 立刻开始密钥交换
            cache_dirty_ = true;
            dirty_ = true;
            break;
        }
        case MsgType::crypto_kex:
            kex_on(c, TlvReader(f.payload));
            break;
        case MsgType::msg_send: {
            recv_text(c, TlvReader(f.payload));
            break;
        }
        case MsgType::ping: {
            TlvWriter w;
            w.add_u64(tag::server_time, now_ms());
            c.net.send(MsgType::pong, f.header.seq, w.data());
            (void)c.net.flush();
            break;
        }
        case MsgType::pong:
            break;
        case MsgType::bye:
            c.net.close();
            add_system((c.name.empty() ? std::string("peer") : c.name) + " left");
            dirty_ = true;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- 端到端加密
// 身份密钥是长期密钥（data/identity.key），握手时双方交换身份公钥 + 本次连接的临时公钥，
// 派生出的会话密钥只用于本次连接；聊天内容只以密文出现在传输层。
void P2PApp::load_identity() {
    const std::string path = fs::join(opt_.data_dir, "identity.key");
    std::string text;
    if (fs::read_file(path, text)) {
        const auto raw = hex_decode(text);
        if (raw.size() == crypto::kKeySize) {
            std::memcpy(identity_.priv, raw.data(), crypto::kKeySize);
            identity_ok_ = crypto::x25519_public_from_priv(identity_.priv, identity_.pub);
        }
    }
    if (!identity_ok_) {
        identity_ = crypto::x25519_generate();
        identity_ok_ = true;
        (void)fs::atomic_write_file(path, hex_encode(identity_.priv, crypto::kKeySize) + "\n");
        add_system("generated a new identity key, fingerprint " + crypto::fingerprint(identity_.pub));
    }

    // 已知对端指纹（TOFU，类似 SSH known_hosts）：用户名 → 指纹
    std::string lines;
    if (fs::read_file(fs::join(opt_.data_dir, "known_peers.txt"), lines)) {
        size_t pos = 0;
        while (pos < lines.size()) {
            const size_t eol = lines.find('\n', pos);
            const std::string line = lines.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) known_[line.substr(0, tab)] = line.substr(tab + 1);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
}

void P2PApp::save_known(const std::string& peer, const std::string& fp) {
    known_[peer] = fp;
    std::string text;
    for (const auto& kv : known_) text += kv.first + "\t" + kv.second + "\n";
    (void)fs::atomic_write_file(fs::join(opt_.data_dir, "known_peers.txt"), text);
}

void P2PApp::kex_send(PeerConn& c) {
    if (c.sec.started || !identity_ok_) return;
    c.sec.started = true;
    c.sec.eph = crypto::x25519_generate();  // 每次连接一把临时密钥
    TlvWriter w;
    w.add_str(tag::kex_pub, std::string_view(reinterpret_cast<const char*>(identity_.pub), crypto::kPubSize));
    w.add_str(tag::kex_eph, std::string_view(reinterpret_cast<const char*>(c.sec.eph.pub), crypto::kPubSize));
    peer_send(c, MsgType::crypto_kex, 0, w.data());
    (void)c.net.flush();  // 中继路径 peer_send 内部已 flush
}

void P2PApp::kex_on(PeerConn& c, const TlvReader& r) {
    if (c.sec.ready || !c.ready) return;  // 握手已完成，或还没互相通报身份
    const std::string_view pub = r.get_str(tag::kex_pub);
    const std::string_view eph = r.get_str(tag::kex_eph);
    if (pub.size() != crypto::kPubSize || eph.size() != crypto::kPubSize) {
        add_system("peer sent a malformed key exchange, encryption stays off");
        return;
    }
    std::memcpy(c.sec.peer_pub, pub.data(), crypto::kPubSize);
    std::memcpy(c.sec.peer_eph, eph.data(), crypto::kPubSize);
    if (!c.sec.started) kex_send(c);  // 对方先发时在这里补发自己的（正常流程双方都会主动发）
    if (!crypto::derive_session_keys(identity_, c.sec.eph, c.sec.peer_pub, c.sec.peer_eph, c.sec.keys)) {
        add_system("key exchange with " + c.name + " failed (weak or duplicate identity key)");
        return;
    }
    std::memset(c.sec.eph.priv, 0, crypto::kKeySize);  // 临时私钥用完即弃（前向保密的前提）
    c.sec.ready = true;
    c.sec.send_ctr = 0;
    c.sec.recv_ctr = 0;
    c.sec.peer_fp = crypto::fingerprint(c.sec.peer_pub);

    const auto it = known_.find(c.name);
    if (it == known_.end()) {
        c.sec.trust = 1;
        save_known(c.name, c.sec.peer_fp);
        add_system(fmt("e2e ready with {}  fp {}\n(first contact: fingerprint recorded)", c.name, c.sec.peer_fp));
    } else if (it->second == c.sec.peer_fp) {
        c.sec.trust = 2;
        add_system(fmt("e2e ready with {}  fp {}", c.name, c.sec.peer_fp));
    } else {
        c.sec.trust = 3;
        add_system(fmt("WARNING: fingerprint of {} changed!\n  recorded {}\n  now      {}\n"
                       "could be an impersonator; run /trust {} to accept the new key",
                       c.name, it->second, c.sec.peer_fp, c.name));
    }
    cache_dirty_ = true;
    dirty_ = true;
}

bool P2PApp::send_text(PeerConn& c, const std::string& text) {
    if (!c.sec.ready) {
        add_system("session with " + c.name + " is not encrypted yet, nothing was sent");
        return false;
    }
    const uint64_t ctr = ++c.sec.send_ctr;
    uint8_t nonce[crypto::kNonceSize];
    msg_nonce(nonce, ctr);
    std::vector<uint8_t> out(text.size() + crypto::kTagSize);
    crypto::aead_encrypt(c.sec.keys.send, nonce, reinterpret_cast<const uint8_t*>(kAadMsg), sizeof(kAadMsg) - 1,
                         reinterpret_cast<const uint8_t*>(text.data()), text.size(), out.data(),
                         out.data() + text.size());
    TlvWriter w;
    w.add_u64(tag::nonce, ctr);
    w.add_u64(tag::ts_ms, now_ms());
    w.add_str(tag::cipher, std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
    peer_send(c, MsgType::msg_send, 0, w.data());
    return true;
}

void P2PApp::recv_text(PeerConn& c, const TlvReader& r) {
    const std::string_view blob = r.get_str(tag::cipher);
    const uint64_t ctr = r.u64_or(tag::nonce, 0);
    if (!c.sec.ready || ctr == 0 || blob.size() < crypto::kTagSize) {
        add_system("dropped a plaintext/invalid message from " + (c.name.empty() ? std::string("peer") : c.name));
        dirty_ = true;
        return;
    }
    if (ctr <= c.sec.recv_ctr) {  // 重放或乱序回退
        add_system(fmt("dropped replayed message #{} from {}", ctr, c.name));
        dirty_ = true;
        return;
    }
    const size_t n = blob.size() - crypto::kTagSize;
    uint8_t nonce[crypto::kNonceSize];
    msg_nonce(nonce, ctr);
    std::string text(n, '\0');
    if (!crypto::aead_decrypt(c.sec.keys.recv, nonce, reinterpret_cast<const uint8_t*>(kAadMsg), sizeof(kAadMsg) - 1,
                              reinterpret_cast<const uint8_t*>(blob.data()), n,
                              reinterpret_cast<const uint8_t*>(blob.data() + n),
                              reinterpret_cast<uint8_t*>(text.data()))) {
        add_system(fmt("message from {} failed authentication (dropped)", c.name));
        dirty_ = true;
        return;
    }
    c.sec.recv_ctr = ctr;

    uint64_t ts = r.u64_or(tag::ts_ms, 0);
    if (ts == 0) ts = now_ms();
    Chat& ch = chat_of(c.name);
    Msg m;
    m.from = c.name;
    m.ts = ts;
    m.text = utf8::sanitize(text);
    add_msg(ch, m);
    persist(ch, m);
    if (current_ != c.name) add_system("new message from " + c.name);
    dirty_ = true;
}

std::string P2PApp::e2e_state(const PeerConn* c) const {
    if (!c) return "e2e:-";
    if (!c->sec.ready) return c->sec.started ? "e2e:handshake" : "e2e:wait";
    if (c->sec.trust == 3) return "e2e:KEY-CHANGED";
    return c->sec.trust == 1 ? "e2e:new" : "e2e:ok";
}

// ---------------------------------------------------------------- 会话与存储
P2PApp::Chat& P2PApp::chat_of(const std::string& peer) {
    for (auto& c : chats_) {
        if (c->peer == peer) return *c;
    }
    auto c = std::make_unique<Chat>();
    c->peer = peer;

    auto store = std::make_unique<RoomStore>();
    const std::string dir = fs::join(fs::join(opt_.data_dir, "peers"), peer);
    Status st = store->open(peer, dir, 8u << 20, false);
    if (st) {
        std::vector<StoredMessage> hist;
        if (store->read_latest(500, hist)) {
            for (auto& m : hist) {
                Msg msg;
                msg.from = m.from;
                msg.ts = m.ts_ms;
                msg.text = m.text;
                c->msgs.push_back(std::move(msg));
            }
        }
        c->store = std::move(store);
    } else {
        LOG_ERROR("p2p: cannot open local store for {}: {}", peer, err_text(st.e, st.msg));
    }
    chats_.push_back(std::move(c));
    cache_dirty_ = true;
    return *chats_.back();
}

void P2PApp::add_msg(Chat& ch, Msg m) {
    ch.msgs.push_back(std::move(m));
    if (ch.msgs.size() > kMaxMsgs) ch.msgs.erase(ch.msgs.begin(), ch.msgs.begin() + (ch.msgs.size() - kMaxMsgs));
    cache_dirty_ = true;
    dirty_ = true;
}

void P2PApp::add_system(const std::string& text) {
    if (current_.empty()) {
        // 还没打开会话：放进一个本地提示流，用 "__info__" 伪会话承载
        Chat& ch = chat_of("__info__");
        Msg m;
        m.from = "*";
        m.ts = now_ms();
        m.text = text;
        m.system = true;
        ch.msgs.push_back(std::move(m));
        if (ch.msgs.size() > kMaxMsgs) ch.msgs.erase(ch.msgs.begin());
        cache_dirty_ = true;
        dirty_ = true;
        return;
    }
    Chat& ch = chat_of(current_);
    Msg m;
    m.from = "*";
    m.ts = now_ms();
    m.text = text;
    m.system = true;
    add_msg(ch, m);
}

void P2PApp::persist(Chat& ch, const Msg& m) {
    if (!ch.store || m.system) return;
    StoredMessage sm;
    sm.from = m.from;
    sm.ts_ms = m.ts;
    sm.text = m.text;
    if (ch.store->append(sm)) ch.store_dirty = true;
}

// ---------------------------------------------------------------- 输入与命令
void P2PApp::on_stdin() {
    char buf[4096];
    bool eof = false;
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            inbuf_.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR) continue;
        break;
    }
    if (inbuf_.empty()) {
        if (eof) quit_ = true;
        return;
    }

    size_t i = 0;
    while (i < inbuf_.size()) {
        const unsigned char c = static_cast<unsigned char>(inbuf_[i]);
        if (c == 0x1B) {
            // ESC 序列（方向键等）简单忽略，避免插入乱码
            if (i + 2 < inbuf_.size() && (inbuf_[i + 1] == '[' || inbuf_[i + 1] == 'O')) {
                i += 3;
                if (i <= inbuf_.size() && inbuf_[i - 1] >= '0' && inbuf_[i - 1] <= '9') ++i;
            } else {
                ++i;
            }
            continue;
        }
        if (c == 0x0D || c == 0x0A) {
            ++i;
            submit();
            if (quit_) return;
            continue;
        }
        if (c == 0x7F || c == 0x08) {
            ++i;
            editor_.backspace();
            dirty_ = true;
            continue;
        }
        if (c == 0x03) {
            ++i;
            if (editor_.text().empty()) {
                quit_ = true;
                return;
            }
            editor_.clear();
            dirty_ = true;
            continue;
        }
        if (c == 0x04) {
            ++i;
            if (editor_.text().empty()) {
                quit_ = true;
                return;
            }
            continue;
        }
        if (c == 0x0C) {
            ++i;
            renderer_.invalidate();
            dirty_ = true;
            continue;
        }
        if (c < 0x20) {
            ++i;
            continue;
        }
        // UTF-8：按首字节长度整体插入
        size_t need = 1;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        if (i + need > inbuf_.size()) break;
        editor_.insert(std::string_view(inbuf_.data() + i, need));
        i += need;
        dirty_ = true;
    }
    inbuf_.erase(0, i);
}

void P2PApp::submit() {
    const std::string line = editor_.text();
    if (line.empty()) return;
    editor_.history_push(line);
    editor_.clear();
    scroll_ = 0;
    dirty_ = true;

    if (line[0] == '/') {
        exec_command(line);
        return;
    }
    if (current_.empty() || current_ == "__info__") {
        add_system("no chat open, use /chat <name>");
        return;
    }
    PeerConn* c = find_peer(current_);
    if (!c || !c->ready) {
        add_system("not connected to " + current_);
        return;
    }
    if (!send_text(*c, line)) {
        editor_.insert(line);  // 没发出去：把内容还给输入行，别让用户白打一遍
        dirty_ = true;
        return;
    }
    (void)c->net.flush();

    Chat& ch = chat_of(current_);
    Msg m;
    m.from = opt_.user;
    m.ts = now_ms();
    m.text = line;
    add_msg(ch, m);
    persist(ch, m);
}

void P2PApp::exec_command(const std::string& line) {
    const size_t sp = line.find(' ');
    std::string cmd = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    std::string arg = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\r')) arg.pop_back();

    if (cmd == "chat" || cmd == "add") {
        if (arg.empty()) {
            add_system("usage: /chat <name>");
            return;
        }
        start_chat(arg);
    } else if (cmd == "peers") {
        std::string s = "sessions:";
        for (auto& p : peers_) {
            s += " " + (p->name.empty() ? std::string("?") : p->name) + (p->ready ? "(ok)" : "(...)");
        }
        add_system(s);
    } else if (cmd == "whoami") {
        add_system(fmt("{}  listen=:{}  signaling {}:{}  registered={}", opt_.user, listen_port_, opt_.sig_host,
                       opt_.sig_port, sig_registered_ ? "yes" : "no"));
    } else if (cmd == "fingerprint" || cmd == "fp") {
        std::string s = identity_ok_ ? "my fingerprint  " + crypto::fingerprint(identity_.pub)
                                     : std::string("my fingerprint  (none)");
        for (auto& p : peers_) {
            if (!p->sec.ready) continue;
            const char* tag_txt = p->sec.trust == 3 ? "CHANGED" : (p->sec.trust == 1 ? "new" : "known");
            s += fmt("\npeer {}  {}  ({})", p->name, p->sec.peer_fp, tag_txt);
        }
        add_system(s);
    } else if (cmd == "trust") {
        // 指纹变更后的人工确认（TOFU 的"覆盖"操作）
        std::string target = arg.empty() ? current_ : arg;
        const PeerConn* c = find_peer(target);
        if (!c || !c->sec.ready) {
            add_system("no encrypted session with " + (target.empty() ? std::string("<none>") : target));
        } else {
            save_known(target, c->sec.peer_fp);
            for (auto& p : peers_) {
                if (p->name == target) p->sec.trust = 2;
            }
            add_system("trusted " + target + " " + c->sec.peer_fp);
            dirty_ = true;
        }
    } else if (cmd == "close") {
        if (!current_.empty()) {
            const std::string target = current_;
            for (auto& p : peers_) {
                if (p->name == target) p->net.close();
            }
            if (target == relay_peer_) relay_peer_.clear();
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                        [&](const std::unique_ptr<PeerConn>& p) { return p->name == target; }),
                         peers_.end());
            add_system("closed session with " + target);
            dirty_ = true;
        }
    } else if (cmd == "clear") {
        if (!current_.empty()) {
            Chat& ch = chat_of(current_);
            ch.msgs.clear();
            cache_dirty_ = true;
        }
    } else if (cmd == "help") {
        add_system(
            "/chat <name>   和某人开始/切换会话\n"
            "/peers         列出当前会话\n"
            "/fingerprint   显示自己和各对端的公钥指纹\n"
            "/trust <name>  指纹变了之后确认接受新指纹\n"
            "/close         关闭当前会话\n"
            "/whoami        显示自己的信息\n"
            "/clear         清空当前会话的本地显示\n"
            "/quit          退出\n"
            "Ctrl-C 清空输入/退出");
    } else if (cmd == "quit") {
        quit_ = true;
    } else {
        add_system("unknown command: /" + cmd + " (try /help)");
    }
}

// ---------------------------------------------------------------- 渲染
void P2PApp::render() {
    const TermSize ts = term_size();
    renderer_.resize(ts.rows, ts.cols);

    Chat& ch = chat_of(current_.empty() ? "__info__" : current_);
    if (cache_dirty_ || cache_cols_ != static_cast<size_t>(ts.cols)) {
        cache_.clear();
        cache_cols_ = static_cast<size_t>(ts.cols);
        cache_dirty_ = false;
        const int width = ts.cols > 10 ? ts.cols - 2 : 8;
        for (const auto& m : ch.msgs) {
            std::string prefix;
            std::string indent;
            if (m.system) {
                prefix = "* ";
                indent = "  ";
            } else {
                prefix = fmt_clock(m.ts) + " " + m.from + ": ";
                indent.assign(static_cast<size_t>(utf8::string_width(prefix)), ' ');
            }
            auto lines = wrap_text(prefix, indent, m.text, width);
            for (auto& l : lines) {
                if (opt_.no_color) cache_.push_back(l);
                else if (m.system) cache_.push_back(paint(l, "90"));
                else if (m.from == opt_.user) cache_.push_back(paint(l, "36"));
                else cache_.push_back(l);
            }
        }
    }

    const int body_rows = ts.rows > 3 ? ts.rows - 2 : 1;

    std::string status;
    if (current_.empty() || current_ == "__info__") {
        status = fmt("@{}  (no chat)  sig:{}  :{}", opt_.user, sig_registered_ ? "ok" : "…", listen_port_);
    } else {
        PeerConn* c = find_peer(current_);
        status = fmt("@{} → {}  {}  {}  msgs:{}", opt_.user, current_,
                      (c && c->ready) ? "● online" : "○ offline", e2e_state(c), ch.msgs.size());
    }
    status += "  " + fmt_clock(now_ms());
    const std::string status_plain = utf8::truncate_width(status, ts.cols - 1);
    const std::string status_line = opt_.no_color ? status_plain : paint(status_plain, "7");

    std::vector<std::string> screen;
    screen.push_back(status_line);

    const size_t total = cache_.size();
    const size_t end = total > scroll_ ? total - scroll_ : 0;
    const size_t start = end > static_cast<size_t>(body_rows) ? end - static_cast<size_t>(body_rows) : 0;
    for (size_t i = start; i < end; ++i) screen.push_back(cache_[i]);
    while (static_cast<int>(screen.size()) < ts.rows - 1) screen.push_back("");

    const std::string& text = editor_.text();
    std::string visible = text;
    const int avail = ts.cols - 3 > 4 ? ts.cols - 3 : 4;
    while (utf8::string_width(visible) > avail && !visible.empty()) {
        visible.erase(0, utf8::next_boundary(visible, 0));
    }
    const size_t cut = text.size() - visible.size();
    const size_t cur = editor_.cursor() >= cut ? editor_.cursor() - cut : 0;
    const int cursor_col = 2 + utf8::string_width(std::string_view(visible).substr(0, cur));
    screen.push_back("> " + visible);

    renderer_.draw(screen, ts.rows - 1, cursor_col);
}

void P2PApp::tick() {
    const uint64_t now = steady_ms();

    // 信令重连
    if (!sig_.connected() && now >= next_retry_ms_) {
        sig_connect();
    }

    // 清理已断开的直连对端（中继会话的 net 本来就是空的，跳过）
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const std::unique_ptr<PeerConn>& p) {
                                    return !p->relay && !p->net.connected();
                                }),
                 peers_.end());

    // 心跳
    for (auto& p : peers_) {
        if (now - p->last_ping > 30000) {
            p->last_ping = now;
            TlvWriter w;
            w.add_u64(tag::server_time, now_ms());
            peer_send(*p, MsgType::ping, 0, w.data());
            (void)p->net.flush();
        }
    }

    // 本地存储批量刷盘
    if (now - last_sync_ms_ > 2000) {
        last_sync_ms_ = now;
        for (auto& c : chats_) {
            if (c->store && c->store_dirty) {
                (void)c->store->sync();
                c->store_dirty = false;
            }
        }
    }

    // 每秒刷状态栏
    if (now - last_tick_ms_ >= 1000) {
        last_tick_ms_ = now;
        dirty_ = true;
    }
}

// ---------------------------------------------------------------- 入口
int P2PApp::run() {
    Status st = fs::mkdirs(opt_.data_dir);
    if (!st) {
        std::fprintf(stderr, "cannot create data dir: %s\n", err_text(st.e, st.msg).c_str());
        return 1;
    }
    st = net_listen("0.0.0.0", opt_.listen_port, listen_fd_);
    if (!st) {
        std::fprintf(stderr, "cannot listen on port %u: %s\n", opt_.listen_port, err_text(st.e, st.msg).c_str());
        return 1;
    }
    listen_port_ = opt_.listen_port;

    std::signal(SIGWINCH, on_signal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    st = term_.enter();
    if (!st) {
        std::fprintf(stderr, "terminal error: %s\n", err_text(st.e, st.msg).c_str());
        return 1;
    }
    const int stdin_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) (void)::fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    current_ = "__info__";
    chat_of("__info__");
    load_identity();
    add_system(fmt("P2P mode: listening on :{} , signaling {}:{}", listen_port_, opt_.sig_host, opt_.sig_port));
    add_system("identity fingerprint " + crypto::fingerprint(identity_.pub) + "  (/fingerprint)");
    if (!sig_connect()) add_system("signaling unreachable, will retry in background");

    while (!quit_) {
        if (g_stop) {
            quit_ = true;
            break;
        }

        std::vector<pollfd> pfds;
        std::vector<int> kinds;
        pfds.push_back(pollfd{STDIN_FILENO, POLLIN, 0});
        kinds.push_back(0);
        pfds.push_back(pollfd{listen_fd_.get(), POLLIN, 0});
        kinds.push_back(1);
        if (sig_.connected()) {
            pfds.push_back(pollfd{sig_.fd(), POLLIN, 0});
            kinds.push_back(2);
        }
        for (size_t i = 0; i < peers_.size(); ++i) {
            if (!peers_[i]->net.connected()) continue;
            pfds.push_back(pollfd{peers_[i]->net.fd(), POLLIN, 0});
            kinds.push_back(static_cast<int>(3 + i));
        }

        const int n = ::poll(pfds.data(), pfds.size(), 100);
        if (n < 0 && errno != EINTR) break;

        if (n > 0) {
            for (size_t k = 0; k < pfds.size(); ++k) {
                if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
                const int kind = kinds[k];
                if (kind == 0) {
                    on_stdin();
                } else if (kind == 1) {
                    accept_inbound();
                } else if (kind == 2) {
                    sig_on_read();
                } else {
                    const size_t idx = static_cast<size_t>(kind - 3);
                    if (idx < peers_.size()) on_peer_read(*peers_[idx]);
                }
                if (quit_) break;
            }
        }

        tick();
        if (g_winch) {
            g_winch = 0;
            renderer_.invalidate();
            dirty_ = true;
        }
        if (dirty_) {
            render();
            dirty_ = false;
        }
    }

    for (auto& c : chats_) {
        if (c->store) (void)c->store->sync();
    }
    sig_close();
    term_.leave();
    if (stdin_flags >= 0) (void)::fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    return 0;
}

}  // namespace chat
