#include "server.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "chat/fs.h"
#include "chat/log.h"
#include "chat/time_util.h"
#include "dispatcher.h"

namespace chat {
namespace {
constexpr uint64_t kListenToken = ~0ull - 2;
constexpr size_t kMaxInBuffer = 1u << 20;  // 单连接读缓冲上限
}  // namespace

Server::Server(const Config& cfg) : cfg_(cfg) {}

Server::~Server() {
    stop();
    pool_.stop();
    storage_.stop();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (sig_fd_ >= 0) ::close(sig_fd_);
    loop_.close();
}

Status Server::start() {
    // 必须在线程池/flusher 启动之前屏蔽信号：线程会继承创建时的信号掩码，
    // 否则 SIGTERM 可能落到工作线程上走默认动作，直接杀掉进程（优雅退出就失效了）。
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) return status_errno("pthread_sigmask");

    CHAT_TRY(loop_.init());

    CHAT_TRY(storage_.init(cfg_.data_dir, cfg_.segment_mb * 1024u * 1024u, cfg_.sync_mode, cfg_.repair,
                           static_cast<uint64_t>(cfg_.max_room_mb) * 1024u * 1024u));

    // 提交回调：flusher 线程 → 投递到 io 线程发送 ACK 与广播
    storage_.set_commit_callback([this](const std::string& room, const StoredMessage& m, uint64_t tag) {
        post_to_io([this, room, m, tag]() {
            PendingAck pa;
            bool has = false;
            {
                std::lock_guard<std::mutex> lk(ack_mu);
                auto it = ack_map.find(tag);
                if (it != ack_map.end()) {
                    pa = it->second;
                    ack_map.erase(it);
                    has = true;
                }
            }
            if (has) {
                Session* s = find_session(pa.session_id);
                if (s) {
                    TlvWriter w;
                    w.add_u64(tag::msg_id, m.msg_id);
                    w.add_u64(tag::client_msg_id, pa.client_msg_id);
                    send_frame(*s, MsgType::msg_send_ack, 0, w.data());
                    s->remember_ack(pa.client_msg_id, m.msg_id);
                }
            }
            MessageView v;
            v.msg_id = m.msg_id;
            v.room = room;
            v.from = m.from;
            v.ts_ms = m.ts_ms;
            v.text = m.text;
            v.kind = m.kind;
            TlvWriter w;
            encode_message(w, v);
            broadcast_room(room, encode_frame(MsgType::msg_recv, 0, w.data(), kFlagPush));
            stats.messages.fetch_add(1, std::memory_order_relaxed);
        });
    });

    CHAT_TRY(storage_.open_room("public", nullptr));
    storage_.start_flusher();
    pool_.start(cfg_.threads);

    std::string auth_path = fs::join(cfg_.data_dir, "meta.db");
    CHAT_TRY(auth_.load(auth_path, true));

    UniqueFd lf;
    CHAT_TRY(net_listen(cfg_.bind_addr, cfg_.port, lf));
    listen_fd_ = lf.release();
    CHAT_TRY(loop_.add(listen_fd_, EPOLLIN, kListenToken));

    // 信号统一走 signalfd，不写异步信号处理函数（掩码已在函数开头设置）
    sig_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd_ < 0) return status_errno("signalfd");
    CHAT_TRY(loop_.add(sig_fd_, EPOLLIN, kSignalToken));

    dispatcher_ = std::make_unique<Dispatcher>(*this);
    last_timer_ms_ = steady_ms();
    last_stats_ms_ = last_timer_ms_;
    LOG_INFO("chatd: listening on {}:{} (threads={}, data={})", cfg_.bind_addr, cfg_.port, cfg_.threads,
             cfg_.data_dir);
    return Status::success();
}

void Server::stop() {
    if (stop_.exchange(true)) return;
    loop_.wakeup();
}

Session* Server::find_session(uint64_t id) {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

std::vector<Session*> Server::sessions_of_user(const std::string& user) {
    std::vector<Session*> out;
    auto it = user_sessions_.find(user);
    if (it == user_sessions_.end()) return out;
    for (uint64_t id : it->second) {
        Session* s = find_session(id);
        if (s) out.push_back(s);
    }
    return out;
}

void Server::send_frame(Session& s, MsgType type, uint64_t seq, const std::string& payload, bool push) {
    if (s.closing) return;
    std::string frame = encode_frame(type, seq, payload, push ? kFlagPush : 0);
    if (s.out_bytes + frame.size() > Session::kMaxOutBuffer) {
        LOG_WARN("session {} out buffer overflow ({} bytes pending), closing", s.id, s.out_bytes);
        close_session(s.id, "slow reader");
        return;
    }
    s.enqueue(std::move(frame));  // 移动，无额外分配
    if (!s.flush_pending) {
        s.flush_pending = true;
        flush_list_.push_back(s.id);
    }
}

void Server::send_error(Session& s, Err code, const std::string& reason, uint64_t seq) {
    TlvWriter w;
    w.add_u16(tag::code, static_cast<uint16_t>(code));
    w.add_str(tag::reason, reason);
    send_frame(s, MsgType::error, seq, w.data());
}

void Server::broadcast_room(const std::string& room, std::shared_ptr<const std::string> frame,
                            uint64_t except_session) {
    const std::vector<std::string> members = router_.members(room);
    for (const auto& user : members) {
        auto it = user_sessions_.find(user);
        if (it == user_sessions_.end()) continue;
        const std::vector<uint64_t> ids = it->second;  // 拷贝：enqueue 可能关闭连接
        for (uint64_t sid : ids) {
            if (sid == except_session) continue;
            Session* s = find_session(sid);
            if (!s) continue;
            enqueue_shared(*s, frame);  // 同一个 shared_ptr，所有成员零拷贝共享
        }
    }
}

void Server::broadcast_room(const std::string& room, const std::string& frame, uint64_t except_session) {
    broadcast_room(room, std::make_shared<const std::string>(frame), except_session);
}

void Server::broadcast_system(const std::string& room, const std::string& text) {
    MessageView v;
    v.from = "*";
    v.ts_ms = now_ms();
    v.text = text;
    TlvWriter w;
    encode_message(w, v);
    broadcast_room(room, encode_frame(MsgType::msg_recv, 0, w.data(), kFlagPush));
}

void Server::enqueue(Session& s, std::string data) {
    enqueue_shared(s, std::make_shared<const std::string>(std::move(data)));
}

void Server::enqueue_shared(Session& s, std::shared_ptr<const std::string> frame) {
    if (s.closing || !frame || frame->empty()) return;
    if (s.out_bytes + frame->size() > Session::kMaxOutBuffer) {
        LOG_WARN("session {} out buffer overflow ({} bytes pending), closing", s.id, s.out_bytes);
        close_session(s.id, "slow reader");
        return;
    }
    s.enqueue_shared(std::move(frame));
    if (!s.flush_pending) {
        s.flush_pending = true;
        flush_list_.push_back(s.id);
    }
}

// 一轮 epoll 事件处理完再统一写：同一 session 的多条消息合并成一到几次 sendmsg
void Server::flush_pending() {
    if (flush_list_.empty()) return;
    std::vector<uint64_t> ids;
    ids.swap(flush_list_);
    for (uint64_t id : ids) {
        Session* s = find_session(id);
        if (!s) continue;
        s->flush_pending = false;
        flush_out(*s);
    }
}

void Server::post_to_io(std::function<void()> fn) {
    bool need_wake = false;
    {
        std::lock_guard<std::mutex> lk(post_mu_);
        posted_.push_back(std::move(fn));
        if (!post_pending_) {
            post_pending_ = true;
            need_wake = true;
        }
    }
    if (need_wake) loop_.wakeup();
}

void Server::run_posted() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lk(post_mu_);
        tasks.swap(posted_);
        post_pending_ = false;
    }
    for (auto& t : tasks) t();
}

void Server::register_user(Session& s) {
    s.authenticated = true;
    auto& v = user_sessions_[s.user];
    if (std::find(v.begin(), v.end(), s.id) == v.end()) v.push_back(s.id);
    stats.connections.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("session {} logged in as {} (uid={})", s.id, s.user, s.user_id);
}

void Server::unregister_user(Session& s) {
    if (!s.authenticated) return;
    auto it = user_sessions_.find(s.user);
    if (it != user_sessions_.end()) {
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), s.id), v.end());
        if (v.empty()) {
            user_sessions_.erase(it);
            router_.leave_all(s.user);
        }
    }
    stats.connections.fetch_sub(1, std::memory_order_relaxed);
    s.authenticated = false;
}

void Server::close_session(uint64_t id, const char* why) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    LOG_DEBUG("session {} closed: {} ({})", id, why, s.peer);
    // 关闭前尽量把已入队的 ERROR/BYE 发出去（不会递归关闭）
    if (!s.out.empty()) flush_out(s, true);
    loop_.del(s.fd.get());
    unregister_user(s);
    sessions_.erase(it);
    stats.closed.fetch_add(1, std::memory_order_relaxed);
}

void Server::handle_accept() {
    for (;;) {
        UniqueFd fd;
        std::string peer;
        Status st = net_accept(listen_fd_, fd, peer);
        if (!st) {
            if (st.e == Err::would_block) break;
            LOG_ERROR("accept failed: {}", err_text(st.e, st.msg));
            break;
        }
        if (static_cast<int>(sessions_.size()) >= cfg_.max_conns) {
            LOG_WARN("too many connections, rejecting {}", peer);
            continue;  // fd 析构即关闭
        }
        auto s = std::make_unique<Session>();
        s->id = ++next_session_id_;
        s->fd = std::move(fd);
        s->peer = peer;
        s->connect_ms = steady_ms();
        s->last_active_ms = s->connect_ms;
        const uint64_t id = s->id;
        const int raw = s->fd.get();
        if (!loop_.add(raw, EPOLLIN, id)) {
            LOG_ERROR("epoll add failed for {}", peer);
            continue;
        }
        sessions_[id] = std::move(s);
        stats.accepted.fetch_add(1, std::memory_order_relaxed);
        LOG_DEBUG("session {} accepted from {}", id, peer);
    }
}

void Server::handle_read(Session& s) {
    const uint64_t sid = s.id;
    char buf[65536];
    for (;;) {
        size_t got = 0;
        Status st = net_read_some(s.fd.get(), buf, sizeof(buf), got);
        if (!st) {
            if (st.e == Err::would_block) break;
            if (st.e == Err::eof) {
                close_session(sid, "peer closed");
            } else {
                LOG_WARN("session {} read error: {}", sid, err_text(st.e, st.msg));
                close_session(sid, "read error");
            }
            return;
        }
        if (got == 0) break;
        if (s.in.size() + got > kMaxInBuffer) {
            send_error(s, Err::payload_too_large, "input buffer overflow", 0);
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
            LOG_WARN("session {} bad frame: {}", sid, parse_status_name(ps));
            stats.errors.fetch_add(1, std::memory_order_relaxed);
            send_error(s, Err::bad_frame, parse_status_name(ps), 0);
            close_session(sid, "bad frame");
            return;
        }
        s.in.consume(consumed);

        // 速率限制（默认每秒 20 帧，--rate-limit 0 关闭）
        const uint64_t now = steady_ms();
        if (now - s.rate_window_ms >= 1000) {
            s.rate_window_ms = now;
            s.rate_count = 0;
        }
        ++s.rate_count;
        if (cfg_.rate_limit > 0 && s.rate_count > cfg_.rate_limit) {
            stats.errors.fetch_add(1, std::memory_order_relaxed);
            send_error(s, Err::rate_limited, "too many frames per second", f.header.seq);
            if (!find_session(sid)) return;
            continue;
        }

        LOG_TRACE("session {} <- {} seq={} len={}", sid, msg_type_name(f.header.type), f.header.seq,
                  f.header.payload_len);
        dispatcher_->on_frame(s, f);
        if (!find_session(sid)) return;  // handler 可能关闭了连接
    }
}

void Server::flush_out(Session& s, bool closing_after) {
    constexpr int kMaxIov = 64;  // 一次 writev 最多合并 64 段（约 6 KB），减少 syscall
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
            LOG_WARN("session {} write error: {}", s.id, err_text(st.e, st.msg));
            if (!closing_after) close_session(s.id, "write error");
            return;
        }
        // 按实际写出的字节数推进队列
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

void Server::process_timers() {
    const uint64_t now = steady_ms();
    if (now - last_timer_ms_ < 1000) return;
    last_timer_ms_ = now;

    std::vector<uint64_t> kill;
    std::vector<uint64_t> ping;
    for (auto& kv : sessions_) {
        Session& s = *kv.second;
        const uint64_t idle = now - s.last_active_ms;
        const uint64_t age = now - s.connect_ms;
        if (!s.authenticated && age > 60000) {
            kill.push_back(s.id);
        } else if (idle > 90000) {
            kill.push_back(s.id);
        } else if (idle > 45000 && now - s.last_ping_ms > 40000) {
            s.last_ping_ms = now;
            ping.push_back(s.id);
        }
    }
    for (uint64_t id : ping) {
        Session* s = find_session(id);
        if (!s) continue;
        TlvWriter w;
        w.add_u64(tag::server_time, now_ms());
        send_frame(*s, MsgType::ping, 0, w.data(), true);
    }
    for (uint64_t id : kill) {
        LOG_INFO("session {} timed out", id);
        close_session(id, "timeout");
    }

    if (cfg_.stats && now - last_stats_ms_ > 60000) log_stats();
}

void Server::log_stats() {
    last_stats_ms_ = steady_ms();
    LOG_INFO("stats: conns={} accepted={} msgs={} errors={} rooms={}", stats.connections.load(),
             stats.accepted.load(), stats.messages.load(), stats.errors.load(), router_.room_count());
}

void Server::handle_signal() {
    signalfd_siginfo info{};
    while (::read(sig_fd_, &info, sizeof(info)) == sizeof(info)) {
        if (info.ssi_signo == SIGUSR1) {
            log_stats();
            continue;
        }
        LOG_INFO("chatd: received signal {}, shutting down", info.ssi_signo);
        stop_.store(true);
    }
}

void Server::shutdown_all() {
    LOG_INFO("chatd: shutting down, {} session(s) to close", sessions_.size());
    std::vector<uint64_t> ids;
    ids.reserve(sessions_.size());
    for (const auto& kv : sessions_) ids.push_back(kv.first);

    for (uint64_t id : ids) {
        Session* s = find_session(id);
        if (!s) continue;
        TlvWriter w;
        w.add_str(tag::reason, "server shutdown");
        send_frame(*s, MsgType::bye, 0, w.data(), true);
    }
    // 给内核一点时间把 BYE 发出去
    const uint64_t deadline = steady_ms() + 300;
    while (steady_ms() < deadline) {
        std::vector<IoEvent> evs;
        if (loop_.wait(evs, 50) <= 0) continue;
        for (const auto& e : evs) {
            if (e.token == IoLoop::kWakeToken) {
                loop_.drain_wakeup();
                continue;
            }
            Session* s = find_session(e.token);
            if (!s) continue;
            if (e.events & EPOLLOUT) flush_out(*s);
            if (find_session(e.token) && (e.events & (EPOLLERR | EPOLLHUP))) close_session(e.token, "hup");
        }
    }
    ids.clear();
    for (const auto& kv : sessions_) ids.push_back(kv.first);
    for (uint64_t id : ids) close_session(id, "shutdown");
    log_stats();
}

void Server::run() {
    std::vector<IoEvent> evs;
    while (!stop_.load()) {
        const int n = loop_.wait(evs, 100);
        if (n < 0) {
            LOG_ERROR("epoll_wait failed: {}", std::strerror(errno));
            break;
        }
        for (const auto& e : evs) {
            if (e.token == IoLoop::kWakeToken) {
                loop_.drain_wakeup();
                run_posted();
                continue;
            }
            if (e.token == kSignalToken) {
                handle_signal();
                continue;
            }
            if (e.token == kListenToken) {
                handle_accept();
                continue;
            }
            Session* s = find_session(e.token);
            if (!s) continue;

            if (e.events & (EPOLLERR | EPOLLHUP)) {
                close_session(e.token, "hup");
                continue;
            }
            if (e.events & EPOLLIN) {
                handle_read(*s);
                if (!find_session(e.token)) continue;
                s = find_session(e.token);
            }
            if (e.events & EPOLLOUT) {
                flush_out(*s);
            }
        }
        run_posted();
        process_timers();
        flush_pending();  // 本轮产生的所有待发数据一次性写出
    }
    shutdown_all();
}

}  // namespace chat
