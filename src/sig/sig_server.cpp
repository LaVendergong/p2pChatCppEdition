#include "sig_server.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "chat/fs.h"
#include "chat/log.h"
#include "chat/time_util.h"

namespace chat {
namespace {

constexpr uint64_t kListenToken = ~0ull - 2;
constexpr uint64_t kSignalToken = ~0ull - 1;
constexpr size_t kMaxInBuffer = 64u << 10;  // 信令流量很小

// 从 "1.2.3.4:5678" 里取地址部分
std::string peer_ip(const std::string& peer) {
    const size_t colon = peer.rfind(':');
    return colon == std::string::npos ? peer : peer.substr(0, colon);
}

}  // namespace

SigServer::SigServer(const SigConfig& cfg) : cfg_(cfg) {}

SigServer::~SigServer() {
    stop();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (sig_fd_ >= 0) ::close(sig_fd_);
    loop_.close();
}

Status SigServer::start() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) return status_errno("pthread_sigmask");

    CHAT_TRY(loop_.init());

    const std::string dir = cfg_.data_dir;
    CHAT_TRY(fs::mkdirs(dir));
    CHAT_TRY(auth_.load(fs::join(dir, "sig_accounts.db"), true));

    UniqueFd lf;
    CHAT_TRY(net_listen(cfg_.bind_addr, cfg_.port, lf));
    listen_fd_ = lf.release();
    CHAT_TRY(loop_.add(listen_fd_, EPOLLIN, kListenToken));

    sig_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd_ < 0) return status_errno("signalfd");
    CHAT_TRY(loop_.add(sig_fd_, EPOLLIN, kSignalToken));

    last_timer_ms_ = steady_ms();
    LOG_INFO("chatsig: listening on {}:{} (data={})", cfg_.bind_addr, cfg_.port, dir);
    return Status::success();
}

void SigServer::stop() {
    if (stop_.exchange(true)) return;
    loop_.wakeup();
}

void SigServer::send_frame(Session& s, MsgType t, uint64_t seq, const std::string& payload) {
    if (s.closing) return;
    std::string frame = encode_frame(t, seq, payload);
    if (s.out_bytes + frame.size() > Session::kMaxOutBuffer) {
        close_session(s.id, "slow reader");
        return;
    }
    s.enqueue(std::move(frame));
    flush_out(s);
}

void SigServer::send_error(Session& s, Err code, const std::string& reason, uint64_t seq) {
    TlvWriter w;
    w.add_u16(tag::code, static_cast<uint16_t>(code));
    w.add_str(tag::reason, reason);
    send_frame(s, MsgType::error, seq, w.data());
}

void SigServer::flush_out(Session& s, bool closing_after) {
    constexpr int kMaxIov = 16;
    while (!s.out.empty()) {
        iovec iov[kMaxIov];
        int n = 0;
        for (auto& c : s.out) {
            if (n >= kMaxIov) break;
            iov[n].iov_base = const_cast<char*>(c.data() + c.off);
            iov[n].iov_len = c.size() - c.off;
            ++n;
        }
        size_t written = 0;
        Status st = net_writev_some(s.fd.get(), iov, n, written);
        if (!st) {
            if (st.e == Err::would_block) {
                if (!closing_after) loop_.mod(s.fd.get(), EPOLLIN | EPOLLOUT, s.id);
                return;
            }
            if (!closing_after) close_session(s.id, "write error");
            return;
        }
        size_t left = written;
        while (left > 0 && !s.out.empty()) {
            auto& c = s.out.front();
            const size_t avail = c.size() - c.off;
            if (left >= avail) {
                left -= avail;
                s.out_bytes -= avail;
                s.out.pop_front();
            } else {
                c.off += left;
                s.out_bytes -= left;
                left = 0;
            }
        }
    }
    s.reset_out();
    if (s.closing) {
        if (!closing_after) close_session(s.id, "closing");
        return;
    }
    if (!closing_after) loop_.mod(s.fd.get(), EPOLLIN, s.id);
}

void SigServer::close_session(uint64_t id, const char* why) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    LOG_DEBUG("chatsig: session {} closed: {} ({})", id, why, s.peer);
    if (!s.out.empty()) flush_out(s, true);
    loop_.del(s.fd.get());

    // 注销该会话注册的端点
    if (!s.user.empty()) {
        auto pit = peers_.find(s.user);
        if (pit != peers_.end() && pit->second.session_id == id) {
            peers_.erase(pit);
            LOG_INFO("chatsig: peer {} offline", s.user);
        }
        // 清理中继状态，并通知对端
        relay_waiting_.erase(s.user);
        auto pair = relay_paired_.find(s.user);
        if (pair != relay_paired_.end()) {
            const std::string other = pair->second;
            relay_paired_.erase(pair);
            relay_paired_.erase(other);
            auto op = peers_.find(other);
            if (op != peers_.end()) {
                auto os = sessions_.find(op->second.session_id);
                if (os != sessions_.end()) {
                    TlvWriter w;
                    w.add_str(tag::peer_name, s.user);
                    send_frame(*os->second, MsgType::peer_offline, 0, w.data());
                }
            }
            LOG_INFO("chatsig: relay unpaired {} <-> {}", s.user, other);
        }
    }
    sessions_.erase(it);
}

void SigServer::handle_accept() {
    for (;;) {
        UniqueFd fd;
        std::string peer;
        Status st = net_accept(listen_fd_, fd, peer);
        if (!st) {
            if (st.e == Err::would_block) break;
            LOG_ERROR("chatsig: accept failed: {}", err_text(st.e, st.msg));
            break;
        }
        auto s = std::make_unique<Session>();
        s->id = ++next_session_id_;
        s->fd = std::move(fd);
        s->peer = peer;
        s->connect_ms = steady_ms();
        s->last_active_ms = s->connect_ms;
        const uint64_t id = s->id;
        if (!loop_.add(s->fd.get(), EPOLLIN, id)) continue;
        sessions_[id] = std::move(s);
        LOG_DEBUG("chatsig: session {} from {}", id, peer);
    }
}

void SigServer::handle_read(Session& s) {
    const uint64_t sid = s.id;
    char buf[16384];
    for (;;) {
        size_t got = 0;
        Status st = net_read_some(s.fd.get(), buf, sizeof(buf), got);
        if (!st) {
            if (st.e == Err::would_block) break;
            close_session(sid, st.e == Err::eof ? "peer closed" : "read error");
            return;
        }
        if (got == 0) break;
        if (s.in.size() + got > kMaxInBuffer) {
            close_session(sid, "input overflow");
            return;
        }
        s.in.append(buf, got);
        s.touch(steady_ms());
    }

    for (;;) {
        Frame f;
        size_t consumed = 0;
        const std::string_view view(reinterpret_cast<const char*>(s.in.data()), s.in.size());
        const ParseStatus ps = parse_frame(view, f, consumed);
        if (ps == ParseStatus::need_more) break;
        if (ps != ParseStatus::ok) {
            send_error(s, Err::bad_frame, parse_status_name(ps), 0);
            close_session(sid, "bad frame");
            return;
        }
        s.in.consume(consumed);
        dispatch(s, f);
        if (!sessions_.count(sid)) return;
    }
}

void SigServer::dispatch(Session& s, const Frame& f) {
    switch (f.header.type) {
        case MsgType::hello: on_hello(s, f); break;
        case MsgType::login: on_login(s, f); break;
        case MsgType::ping: on_ping(s, f); break;
        case MsgType::peer_register:
        case MsgType::peer_lookup: {
            if (!s.authenticated) {
                send_error(s, Err::unauthenticated, "login required", f.header.seq);
                return;
            }
            if (f.header.type == MsgType::peer_register) on_peer_register(s, f);
            else on_peer_lookup(s, f);
            break;
        }
        case MsgType::relay_open:
        case MsgType::relay_data: {
            if (!s.authenticated) {
                send_error(s, Err::unauthenticated, "login required", f.header.seq);
                return;
            }
            if (f.header.type == MsgType::relay_open) on_relay_open(s, f);
            else on_relay_data(s, f);
            break;
        }
        case MsgType::bye:
            close_session(s.id, "client bye");
            break;
        default:
            send_error(s, Err::unsupported, msg_type_name(f.header.type), f.header.seq);
            break;
    }
}

void SigServer::on_hello(Session& s, const Frame& f) {
    TlvWriter w;
    w.add_str(tag::version, "chatsig/0.1.0");
    w.add_u64(tag::server_time, now_ms());
    w.add_u64(tag::session_id, s.id);
    send_frame(s, MsgType::hello_ack, f.header.seq, w.data());
}

void SigServer::on_login(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    std::string user;
    r.get_str(tag::username, user);
    const std::string_view digest = r.get_str(tag::password_hash);

    if (!valid_username(user)) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::bad_credentials));
        w.add_str(tag::reason, "invalid username");
        send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        return;
    }

    LoginResult res = auth_.login(user, digest, true);
    if (res.kind == LoginKind::need_digest) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::unauthenticated));
        w.add_str(tag::salt, res.salt);
        send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        return;
    }
    if (res.kind != LoginKind::ok) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::bad_credentials));
        w.add_str(tag::reason, "bad username or password");
        send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        return;
    }

    s.authenticated = true;
    s.user = user;
    s.user_id = res.user_id;

    TlvWriter w;
    w.add_u16(tag::code, static_cast<uint16_t>(Err::ok));
    w.add_u64(tag::user_id, res.user_id);
    w.add_str(tag::username, user);
    send_frame(s, MsgType::login_ack, f.header.seq, w.data());
    LOG_INFO("chatsig: {} logged in (session {}, uid {})", user, s.id, res.user_id);
}

void SigServer::on_peer_register(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    uint16_t tcp_port = 0;
    r.get_u16(tag::tcp_port, tcp_port);
    std::string lan_ip;
    r.get_str(tag::lan_ip, lan_ip);
    if (tcp_port == 0) {
        send_error(s, Err::invalid_arg, "tcp_port required", f.header.seq);
        return;
    }
    if (lan_ip.empty()) lan_ip = peer_ip(s.peer);

    // 公网地址就是这条 TCP 连接的对端地址：NAT 后的源端口即映射端口（全锥形 NAT 可直接连）
    const std::string ip = peer_ip(s.peer);
    uint16_t public_port = 0;
    const size_t colon = s.peer.rfind(':');
    if (colon != std::string::npos) public_port = static_cast<uint16_t>(std::atoi(s.peer.c_str() + colon + 1));

    // 同一用户重复注册：踢掉旧会话
    auto old = peers_.find(s.user);
    if (old != peers_.end() && old->second.session_id != s.id) {
        LOG_INFO("chatsig: {} re-registered, dropping old session {}", s.user, old->second.session_id);
        peers_.erase(old);
    }

    Peer p;
    p.user = s.user;
    p.lan_ip = lan_ip;
    p.public_ip = ip;
    p.tcp_port = tcp_port;
    p.public_port = public_port;
    p.session_id = s.id;
    p.last_seen_ms = steady_ms();
    peers_[s.user] = p;

    TlvWriter w;
    w.add_u16(tag::code, static_cast<uint16_t>(Err::ok));
    w.add_str(tag::public_ip, ip);
    w.add_u16(tag::public_port, public_port);
    w.add_str(tag::lan_ip, lan_ip);
    send_frame(s, MsgType::peer_register_ack, f.header.seq, w.data());
    LOG_INFO("chatsig: peer {} registered lan={}:{} public={}:{}", s.user, lan_ip, tcp_port, ip, public_port);
}

std::vector<std::string> SigServer::candidates_of(const Peer& p) const {
    std::vector<std::string> out;
    {
        TlvWriter w;
        w.add_u8(tag::cand_kind, static_cast<uint8_t>(CandKind::lan));
        w.add_str(tag::lan_ip, p.lan_ip);
        w.add_u16(tag::tcp_port, p.tcp_port);
        out.push_back(w.data());
    }
    if (!p.public_ip.empty() && p.public_port != 0) {
        TlvWriter w;
        w.add_u8(tag::cand_kind, static_cast<uint8_t>(CandKind::public_));
        w.add_str(tag::lan_ip, p.public_ip);
        w.add_u16(tag::tcp_port, p.public_port);
        out.push_back(w.data());
    }
    return out;
}

void SigServer::on_peer_lookup(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string target;
    r.get_str(tag::peer_name, target);
    if (!valid_username(target)) {
        send_error(s, Err::invalid_arg, "invalid peer name", f.header.seq);
        return;
    }

    std::vector<std::string> cands;
    auto it = peers_.find(target);
    if (it != peers_.end() && sessions_.count(it->second.session_id)) {
        cands = candidates_of(it->second);
    }

    TlvWriter w;
    w.add_str(tag::peer_name, target);
    w.add_array(tag::candidates, cands);
    send_frame(s, MsgType::peer_lookup_ack, f.header.seq, w.data());

    // 通知被叫方：有人要连你（附带主叫的候选地址）
    if (!cands.empty()) {
        auto self = peers_.find(s.user);
        Session* target_session = nullptr;
        auto ts = sessions_.find(it->second.session_id);
        if (ts != sessions_.end()) target_session = ts->second.get();
        if (target_session && self != peers_.end()) {
            TlvWriter iw;
            iw.add_str(tag::peer_name, s.user);
            iw.add_array(tag::candidates, candidates_of(self->second));
            send_frame(*target_session, MsgType::peer_incoming, 0, iw.data());
        }
    }
    LOG_INFO("chatsig: {} looked up {} -> {} candidate(s)", s.user, target, cands.size());
}

void SigServer::on_relay_open(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string target;
    r.get_str(tag::peer_name, target);
    if (!valid_username(target) || target == s.user) {
        send_error(s, Err::invalid_arg, "invalid relay target", f.header.seq);
        return;
    }
    auto pit = peers_.find(target);
    if (pit == peers_.end() || !sessions_.count(pit->second.session_id)) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::room_not_found));
        w.add_str(tag::peer_name, target);
        w.add_str(tag::reason, "peer offline");
        send_frame(s, MsgType::relay_ack, f.header.seq, w.data());
        return;
    }

    relay_waiting_[s.user] = target;

    // 双边都申请了就配对
    auto it = relay_waiting_.find(target);
    if (it != relay_waiting_.end() && it->second == s.user) {
        relay_paired_[s.user] = target;
        relay_paired_[target] = s.user;
        relay_waiting_.erase(s.user);
        relay_waiting_.erase(target);

        Session* other = sessions_[pit->second.session_id].get();
        {
            TlvWriter w;
            w.add_u16(tag::code, 0);
            w.add_str(tag::peer_name, target);
            send_frame(s, MsgType::relay_ack, f.header.seq, w.data());
        }
        if (other) {
            TlvWriter w;
            w.add_u16(tag::code, 0);
            w.add_str(tag::peer_name, s.user);
            send_frame(*other, MsgType::relay_ack, 0, w.data());
        }
        LOG_INFO("chatsig: relay paired {} <-> {}", s.user, target);
        return;
    }

    // 只登记，通知对端"有人想通过中继连你"
    if (Session* other = sessions_[pit->second.session_id].get()) {
        TlvWriter w;
        w.add_str(tag::peer_name, s.user);
        w.add_u8(tag::relay_flag, 1);
        send_frame(*other, MsgType::peer_incoming, 0, w.data());
    }
    LOG_INFO("chatsig: relay requested by {} -> {}, waiting", s.user, target);
}

void SigServer::on_relay_data(Session& s, const Frame& f) {
    auto it = relay_paired_.find(s.user);
    if (it == relay_paired_.end()) {
        send_error(s, Err::not_a_member, "relay not paired", f.header.seq);
        return;
    }
    auto pit = peers_.find(it->second);
    if (pit == peers_.end()) {
        send_error(s, Err::room_not_found, "relay peer offline", f.header.seq);
        return;
    }
    auto ts = sessions_.find(pit->second.session_id);
    if (ts == sessions_.end()) {
        send_error(s, Err::room_not_found, "relay peer offline", f.header.seq);
        return;
    }
    // 原样转发内层帧（chatsig 不解包内容）
    send_frame(*ts->second, MsgType::relay_data, 0, f.payload);
}

void SigServer::on_ping(Session& s, const Frame& f) {
    TlvWriter w;
    w.add_u64(tag::server_time, now_ms());
    send_frame(s, MsgType::pong, f.header.seq, w.data());
}

void SigServer::process_timers() {
    const uint64_t now = steady_ms();
    if (now - last_timer_ms_ < 5000) return;
    last_timer_ms_ = now;

    std::vector<uint64_t> kill;
    for (auto& kv : sessions_) {
        Session& s = *kv.second;
        if (now - s.last_active_ms > 120000) kill.push_back(s.id);
    }
    for (uint64_t id : kill) {
        LOG_INFO("chatsig: session {} timed out", id);
        close_session(id, "timeout");
    }
}

void SigServer::run() {
    std::vector<IoEvent> evs;
    while (!stop_.load()) {
        const int n = loop_.wait(evs, 200);
        if (n < 0) break;
        for (const auto& e : evs) {
            if (e.token == kListenToken) {
                handle_accept();
                continue;
            }
            if (e.token == kSignalToken) {
                signalfd_siginfo info{};
                while (::read(sig_fd_, &info, sizeof(info)) == sizeof(info)) {
                    LOG_INFO("chatsig: signal {}, shutting down", info.ssi_signo);
                    stop_.store(true);
                }
                continue;
            }
            auto it = sessions_.find(e.token);
            if (it == sessions_.end()) continue;
            Session* s = it->second.get();
            if (e.events & (EPOLLERR | EPOLLHUP)) {
                close_session(e.token, "hup");
                continue;
            }
            if (e.events & EPOLLIN) {
                handle_read(*s);
                if (!sessions_.count(e.token)) continue;
                s = sessions_[e.token].get();
            }
            if (e.events & EPOLLOUT) flush_out(*s);
        }
        process_timers();
    }
    LOG_INFO("chatsig: stopped, {} peer(s) registered", peers_.size());
}

}  // namespace chat
