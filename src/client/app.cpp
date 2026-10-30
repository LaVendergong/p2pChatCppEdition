#include "app.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "chat/fmt.h"
#include "chat/sha256.h"
#include "chat/time_util.h"
#include "chat/utf8.h"

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

constexpr size_t kMaxLocalMsgs = 2000;

std::string paint(const std::string& s, const char* code) {
    std::string out = "\x1b[";
    out += code;
    out += "m";
    out += s;
    out += "\x1b[0m";
    return out;
}

// 返回该首字节对应的 UTF-8 序列长度，非法返回 0
size_t utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

}  // namespace

App::App(ClientOptions opt) : opt_(std::move(opt)) {
    user_ = opt_.user;
    room_ = opt_.room;
}

bool App::connect_once() {
    Status st = net_.connect_to(opt_.host, opt_.port);
    if (!st) {
        add_system(fmt("connect failed: {}", err_text(st.e, st.msg)));
        return false;
    }
    peer_info_ = opt_.host + ":" + std::to_string(opt_.port);
    state_ = State::hello_wait;
    return true;
}

void App::send_hello() {
    TlvWriter w;
    w.add_str(tag::version, "chatt/0.1.0");
    w.add_str(tag::client_name, "chatt");
    net_.send(MsgType::hello, ++seq_, w.data());
    (void)net_.flush();
}

void App::send_login() {
    TlvWriter w;
    w.add_str(tag::username, opt_.user);
    if (!digest_.empty()) w.add_str(tag::password_hash, digest_);
    net_.send(MsgType::login, ++seq_, w.data());
    (void)net_.flush();
}

void App::send_join(const std::string& room) {
    TlvWriter w;
    w.add_str(tag::room, room);
    w.add_u8(tag::allow_create, 1);
    net_.send(MsgType::join, ++seq_, w.data());
    (void)net_.flush();
}

void App::send_message(const std::string& text) {
    if (text.size() > kMaxTextBytes) {
        add_system(fmt("message too long ({} > {} bytes)", text.size(), kMaxTextBytes));
        return;
    }
    if (!online_) {
        if (outbox_.size() >= 100) {
            add_system("offline queue full, message dropped");
            return;
        }
        outbox_.push_back(Outgoing{room_, text});
        add_system("queued until connected");
        return;
    }
    const uint64_t cmid = ++next_cmid_;
    TlvWriter w;
    w.add_str(tag::room, room_);
    w.add_str(tag::text, text);
    w.add_u64(tag::client_msg_id, cmid);
    net_.send(MsgType::msg_send, ++seq_, w.data());
    (void)net_.flush();
    append_local_pending(room_, text, cmid);
}

// 本地乐观插入：先显示消息（行尾带 …），收到 ACK 后转正
void App::append_local_pending(const std::string& room, const std::string& text, uint64_t cmid) {
    Msg m;
    m.ts = now_ms();
    m.from = user_;
    m.room = room;
    m.text = text;
    m.pending = true;
    m.cmid = cmid;
    msgs_.push_back(std::move(m));
    while (msgs_.size() > kMaxLocalMsgs) msgs_.pop_front();
    cache_dirty_ = true;
    dirty_ = true;
}

// Ctrl-R：重发最近一条失败消息。复用原 client_msg_id，服务端幂等表会去重，
// 因此"其实已经落盘但 ACK 丢了"的情况不会产生重复消息。
void App::retry_failed() {
    if (!online_) {
        add_system("not connected, cannot retry");
        return;
    }
    for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
        if (!it->failed) continue;
        if (it->room != room_) {
            add_system(fmt("that message was for #{}, /join {} first", it->room, it->room));
            return;
        }
        it->failed = false;
        it->pending = true;
        it->ts = now_ms();
        const std::string text = it->text;
        const uint64_t cmid = it->cmid;
        TlvWriter w;
        w.add_str(tag::room, it->room);
        w.add_str(tag::text, text);
        w.add_u64(tag::client_msg_id, cmid);
        net_.send(MsgType::msg_send, ++seq_, w.data());
        (void)net_.flush();
        cache_dirty_ = true;
        dirty_ = true;
        add_system("retrying last failed message");
        return;
    }
    add_system("no failed message to retry");
}

void App::flush_outbox() {
    if (outbox_.empty()) return;
    const size_t n = outbox_.size();
    for (const auto& o : outbox_) {
        if (o.room != room_) continue;
        const uint64_t cmid = ++next_cmid_;
        TlvWriter w;
        w.add_str(tag::room, o.room);
        w.add_str(tag::text, o.text);
        w.add_u64(tag::client_msg_id, cmid);
        net_.send(MsgType::msg_send, ++seq_, w.data());
        append_local_pending(o.room, o.text, cmid);
    }
    (void)net_.flush();
    outbox_.clear();
    add_system(fmt("sent {} queued message(s)", n));
}

void App::send_history(uint64_t before_id, uint32_t limit) {
    if (!online_) return;
    TlvWriter w;
    w.add_str(tag::room, room_);
    w.add_u64(tag::before_id, before_id);
    w.add_u32(tag::limit, limit);
    net_.send(MsgType::history_req, ++seq_, w.data());
    (void)net_.flush();
}

void App::add_system(const std::string& text) {
    Msg m;
    m.system = true;
    m.ts = now_ms();
    m.text = text;
    msgs_.push_back(std::move(m));
    while (msgs_.size() > kMaxLocalMsgs) msgs_.pop_front();
    cache_dirty_ = true;
    dirty_ = true;
}

void App::add_message(const MessageView& v) {
    const std::string text = utf8::sanitize(v.text);
    if (v.msg_id != 0) {
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
            if (it->id == v.msg_id) return;  // 去重（历史与实时推送同源）
            // 自己的回显可能先于 ACK 到达：就地转正，别再插一条
            if (it->pending && it->id == 0 && it->from == v.from && it->text == text) {
                it->id = v.msg_id;
                it->ts = v.ts_ms ? v.ts_ms : it->ts;
                it->pending = false;
                if (v.msg_id > last_id_) last_id_ = v.msg_id;
                cache_dirty_ = true;
                dirty_ = true;
                return;
            }
        }
    }
    Msg m;
    m.id = v.msg_id;
    m.from = v.from;
    m.room = v.room.empty() ? room_ : v.room;
    m.ts = v.ts_ms ? v.ts_ms : now_ms();
    m.text = text;
    m.system = (v.from == "*");
    if (v.msg_id > last_id_) last_id_ = v.msg_id;
    msgs_.push_back(std::move(m));
    while (msgs_.size() > kMaxLocalMsgs) msgs_.pop_front();
    if (scroll_ > 0) ++new_below_;  // 正在上翻：提示下方有新消息
    cache_dirty_ = true;
    dirty_ = true;
}

void App::merge_older(const std::vector<std::string_view>& items, bool keep_viewport) {
    size_t inserted = 0;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        MessageView v;
        if (!decode_message(*it, v)) continue;
        bool dup = false;
        for (auto mi = msgs_.rbegin(); mi != msgs_.rend(); ++mi) {
            if (mi->id != 0 && mi->id == v.msg_id) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        Msg m;
        m.id = v.msg_id;
        m.from = v.from;
        m.room = v.room.empty() ? room_ : v.room;
        m.ts = v.ts_ms;
        m.text = utf8::sanitize(v.text);
        m.system = (v.from == "*");
        msgs_.push_front(std::move(m));
        ++inserted;
    }
    while (msgs_.size() > kMaxLocalMsgs) msgs_.pop_back();
    if (inserted == 0) return;
    if (keep_viewport) scroll_ += inserted;  // 用户正在上翻：结果保持不动
    cache_dirty_ = true;
    dirty_ = true;
}

uint64_t App::oldest_msg_id() const {
    for (const auto& m : msgs_) {
        if (m.id != 0) return m.id;
    }
    return 0;
}

void App::request_older() {
    if (!online_ || history_pending_ || no_more_history_) return;
    const uint64_t oldest = oldest_msg_id();
    if (oldest == 0) return;  // 本地一条带 id 的都没有，无从翻页
    history_pending_ = true;
    send_history(oldest, 50);
}

void App::handle_disconnect(const std::string& why) {
    if (!online_ && state_ == State::disconnected) return;
    online_ = false;
    state_ = State::disconnected;
    net_.close();
    add_system("disconnected: " + why + " (reconnecting...)");
    // 未确认的消息标记为失败（M5 暂不自动重发，UI 上能看到是哪几条）
    for (auto& m : msgs_) {
        if (m.pending) {
            m.pending = false;
            m.failed = true;
        }
    }
    history_pending_ = false;
    cache_dirty_ = true;
    next_reconnect_ms_ = steady_ms() + static_cast<uint64_t>(backoff_ms_);
    backoff_ms_ = std::min(backoff_ms_ * 2, 5000);
}

void App::on_socket() {
    std::vector<Frame> frames;
    bool closed = false;
    Status st = net_.read_frames(frames, closed);
    for (auto& f : frames) {
        handle_frame(f);
        if (quit_) return;
    }
    if (closed) handle_disconnect(st.ok() ? "peer closed" : err_text(st.e, st.msg));
}

void App::handle_frame(const Frame& f) {
    switch (f.header.type) {
        case MsgType::hello_ack: {
            state_ = State::salt_wait;
            digest_.clear();  // 重连后重新取盐
            send_login();
            break;
        }
        case MsgType::login_ack: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 999);
            std::string salt;
            r.get_str(tag::salt, salt);
            if (code == static_cast<uint16_t>(Err::unauthenticated)) {
                if (!salt.empty()) {
                    salt_ = salt;
                    digest_ = password_digest(salt_, opt_.password);
                    send_login();
                } else {
                    add_system("login rejected");
                    quit_ = true;
                }
                break;
            }
            if (code == 0) {
                online_ = true;
                backoff_ms_ = 500;
                state_ = State::join_wait;
                add_system(fmt("logged in as {} (uid={})", opt_.user, r.u64_or(tag::user_id, 0)));
                send_join(room_);
            } else {
                std::string reason;
                r.get_str(tag::reason, reason);
                add_system(fmt("login failed (code {}): {}", code, reason));
                quit_ = true;
            }
            break;
        }
        case MsgType::join_ack: {
            TlvReader r(f.payload);
            std::string new_room = room_;
            r.get_str(tag::room, new_room);

            // 同一房间重连时不能清空本地消息：里面可能有未确认/失败的消息，要留给 Ctrl-R 重发
            bool has_local_pending = false;
            for (const auto& m : msgs_) {
                if (m.pending || m.failed) {
                    has_local_pending = true;
                    break;
                }
            }
            const bool same_room = (new_room == room_);
            if (!same_room || !has_local_pending) {
                msgs_.clear();
                cache_.clear();
                cache_cols_ = 0;
                last_id_ = 0;
                scroll_ = 0;
                new_below_ = 0;
                printed_plain_ = 0;
            }
            room_ = new_room;
            no_more_history_ = false;
            history_pending_ = false;
            cache_dirty_ = true;

            std::vector<std::string_view> items;
            if (r.get_array(tag::messages, items)) merge_older(items, false);

            state_ = State::ready;
            add_system(fmt("joined #{} ({} messages)", room_, msgs_.size()));
            flush_outbox();
            dirty_ = true;
            break;
        }
        case MsgType::msg_recv: {
            MessageView v;
            if (decode_message(f.payload, v)) add_message(v);
            break;
        }
        case MsgType::msg_send_ack: {
            TlvReader r(f.payload);
            const uint64_t mid = r.u64_or(tag::msg_id, 0);
            const uint64_t cmid = r.u64_or(tag::client_msg_id, 0);
            for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
                if (it->cmid == cmid && it->pending) {
                    it->id = mid;
                    it->pending = false;
                    it->failed = false;
                    cache_dirty_ = true;
                    break;
                }
            }
            if (mid > last_id_) last_id_ = mid;
            dirty_ = true;
            break;
        }
        case MsgType::history_ack: {
            TlvReader r(f.payload);
            history_pending_ = false;
            std::vector<std::string_view> items;
            if (!r.get_array(tag::messages, items)) break;
            const size_t before_size = msgs_.size();
            merge_older(items, true);
            if (msgs_.size() == before_size) {
                no_more_history_ = true;
                add_system("no more history");
            }
            break;
        }
        case MsgType::user_list_ack: {
            TlvReader r(f.payload);
            std::vector<std::string_view> items;
            std::string list = "users:";
            size_t count = 0;
            if (r.get_array(tag::users, items)) {
                count = items.size();
                for (const auto& it : items) {
                    TlvReader ir(it);
                    list += " ";
                    list += std::string(ir.get_str(tag::username));
                }
            }
            member_count_ = count;
            add_system(list);
            break;
        }
        case MsgType::room_list_ack: {
            TlvReader r(f.payload);
            std::vector<std::string_view> items;
            add_system("rooms:");
            if (r.get_array(tag::rooms, items)) {
                for (const auto& it : items) {
                    TlvReader ir(it);
                    add_system(fmt("  #{}  msgs={}  users={}", ir.get_str(tag::room), ir.u32_or(tag::count, 0),
                                   ir.u32_or(tag::limit, 0)));
                }
            }
            break;
        }
        case MsgType::ping: {
            TlvWriter w;
            w.add_u64(tag::server_time, now_ms());
            net_.send(MsgType::pong, f.header.seq, w.data());
            (void)net_.flush();
            break;
        }
        case MsgType::pong:
            break;
        case MsgType::error: {
            TlvReader r(f.payload);
            const uint16_t code = r.u16_or(tag::code, 0);
            std::string reason;
            r.get_str(tag::reason, reason);
            add_system(fmt("server error {}: {}", code, reason));
            if (code == static_cast<uint16_t>(Err::unauthenticated)) {
                // 服务端认为我们未认证（例如重启后 session 丢失），重新走一遍登录
                state_ = State::salt_wait;
                send_login();
            }
            break;
        }
        case MsgType::bye: {
            TlvReader r(f.payload);
            std::string reason;
            r.get_str(tag::reason, reason);
            handle_disconnect("server bye: " + reason);
            quit_ = true;
            break;
        }
        default:
            break;
    }
}

void App::tick() {
    const uint64_t now = steady_ms();
    // 每秒刷一次状态栏（时钟在走，用户能看出程序还活着）
    static uint64_t last_sec = 0;
    const uint64_t sec = now / 1000;
    if (sec != last_sec) {
        last_sec = sec;
        dirty_ = true;
    }
    if (state_ == State::disconnected && now >= next_reconnect_ms_) {
        if (connect_once()) {
            send_hello();
        } else {
            next_reconnect_ms_ = now + static_cast<uint64_t>(backoff_ms_);
            backoff_ms_ = std::min(backoff_ms_ * 2, 5000);
        }
        dirty_ = true;
    }
    if (online_ && now - last_ping_ms_ > 15000) {
        last_ping_ms_ = now;
        TlvWriter w;
        w.add_u64(tag::server_time, now_ms());
        net_.send(MsgType::ping, ++seq_, w.data());
        (void)net_.flush();
    }
    if (net_.has_pending()) (void)net_.flush();
}

void App::plain_print(const std::string& line) {
    std::string out = line;
    out.push_back('\n');
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

void App::on_stdin() {
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
        break;  // EAGAIN：暂无数据（stdin 是非阻塞的）
    }
    if (inbuf_.empty()) {
        // 管道输入结束（--plain 常见）：直接退出，不要空转
        if (eof && opt_.plain) quit_ = true;
        return;
    }
    if (opt_.plain) {
        handle_plain_line();
    } else {
        handle_bytes();
    }
}

void App::handle_plain_line() {
    size_t pos = 0;
    for (;;) {
        const size_t nl = inbuf_.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = inbuf_.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            editor_.insert(line);
            submit();
        }
        if (quit_) break;
    }
    inbuf_.erase(0, pos);
}

bool App::key_sequence(const std::string& s, size_t pos, size_t& consumed, std::string& action) {
    if (s.size() - pos < 2) return false;
    const char c1 = s[pos + 1];
    if (c1 != '[' && c1 != 'O') {
        consumed = 2;  // 其它 ESC + 单字节，忽略
        action = "ignore";
        return true;
    }
    if (s.size() - pos < 3) return false;
    const char c2 = s[pos + 2];
    if (c2 >= '0' && c2 <= '9') {
        if (s.size() - pos < 4) return false;
        const char c3 = s[pos + 3];
        consumed = 4;
        if (c2 == '3' && c3 == '~') action = "delete";
        else if (c2 == '5' && c3 == '~') action = "pgup";
        else if (c2 == '6' && c3 == '~') action = "pgdn";
        else if (c2 == '1' && c3 == '~') action = "home";
        else if (c2 == '4' && c3 == '~') action = "end";
        else action = "ignore";
        return true;
    }
    consumed = 3;
    switch (c2) {
        case 'A': action = "up"; break;
        case 'B': action = "down"; break;
        case 'C': action = "right"; break;
        case 'D': action = "left"; break;
        case 'H': action = "home"; break;
        case 'F': action = "end"; break;
        default: action = "ignore"; break;
    }
    return true;
}

void App::handle_bytes() {
    size_t i = 0;
    while (i < inbuf_.size()) {
        const unsigned char c = static_cast<unsigned char>(inbuf_[i]);

        if (c == 0x1B) {
            size_t consumed = 0;
            std::string action;
            if (!key_sequence(inbuf_, i, consumed, action)) break;  // 序列不完整，等下一批字节
            i += consumed;
            if (action == "left") editor_.move_left();
            else if (action == "right") editor_.move_right();
            else if (action == "home") editor_.move_home();
            else if (action == "end") editor_.move_end();
            else if (action == "delete") editor_.delete_forward();
            else if (action == "pgup") {
                const size_t total = cache_.empty() ? 0 : cache_.size();
                scroll_ = std::min(scroll_ + 10, total);
                // 已经翻到顶（视口第一条就是本地最早的行）：自动向前加载
                const size_t body = static_cast<size_t>(term_size().rows > 3 ? term_size().rows - 2 : 1);
                if (scroll_ + body >= total) request_older();
            } else if (action == "pgdn") {
                scroll_ = scroll_ > 10 ? scroll_ - 10 : 0;
                if (scroll_ == 0) new_below_ = 0;
            }
            else if (action == "up") {
                if (editor_.history_size() > 0) editor_.history_prev();
            } else if (action == "down") {
                editor_.history_next();
            }
            dirty_ = true;
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
        if (c == 0x03) {  // Ctrl-C：清空输入；空输入时退出
            ++i;
            if (editor_.text().empty()) {
                quit_ = true;
                return;
            }
            editor_.clear();
            dirty_ = true;
            continue;
        }
        if (c == 0x04) {  // Ctrl-D
            ++i;
            if (editor_.text().empty()) {
                quit_ = true;
                return;
            }
            editor_.delete_forward();
            dirty_ = true;
            continue;
        }
        if (c == 0x15) {  // Ctrl-U
            ++i;
            editor_.kill_to_start();
            dirty_ = true;
            continue;
        }
        if (c == 0x0B) {  // Ctrl-K
            ++i;
            editor_.kill_to_end();
            dirty_ = true;
            continue;
        }
        if (c == 0x17) {  // Ctrl-W
            ++i;
            editor_.kill_word();
            dirty_ = true;
            continue;
        }
        if (c == 0x01) {  // Ctrl-A
            ++i;
            editor_.move_home();
            dirty_ = true;
            continue;
        }
        if (c == 0x05) {  // Ctrl-E
            ++i;
            editor_.move_end();
            dirty_ = true;
            continue;
        }
        if (c == 0x0C) {  // Ctrl-L
            ++i;
            renderer_.invalidate();
            dirty_ = true;
            continue;
        }
        if (c == 0x10) {  // Ctrl-P
            ++i;
            if (editor_.history_size() > 0) editor_.history_prev();
            dirty_ = true;
            continue;
        }
        if (c == 0x0E) {  // Ctrl-N
            ++i;
            editor_.history_next();
            dirty_ = true;
            continue;
        }
        if (c == 0x12) {  // Ctrl-R：重发最近一条失败消息
            ++i;
            retry_failed();
            continue;
        }
        if (c < 0x20) {
            ++i;  // 其它控制字符忽略
            continue;
        }

        const size_t need = utf8_seq_len(c);
        if (need == 0) {
            ++i;  // 非法首字节
            continue;
        }
        if (i + need > inbuf_.size()) break;  // 半个字符，等待
        editor_.insert(std::string_view(inbuf_.data() + i, need));
        i += need;
        dirty_ = true;
    }
    inbuf_.erase(0, i);
}

void App::submit() {
    const std::string line = editor_.text();
    if (line.empty()) return;
    editor_.history_push(line);
    editor_.clear();
    scroll_ = 0;

    if (line[0] == '/') {
        exec_command(parse_command(line));
        dirty_ = true;  // 输入行被清空，必须重绘
        return;
    }
    send_message(line);
    dirty_ = true;
}

void App::exec_command(const ParsedCommand& pc) {
    if (!pc.ok) {
        add_system("unknown command: /" + pc.name + " (try /help)");
        return;
    }
    if (pc.name == "help") {
        add_system(commands_help());
    } else if (pc.name == "join") {
        if (pc.arg.empty()) {
            add_system("usage: /join <room>");
            return;
        }
        send_join(pc.arg);
    } else if (pc.name == "leave") {
        const std::string target = pc.arg.empty() ? room_ : pc.arg;
        if (!online_) return;
        TlvWriter w;
        w.add_str(tag::room, target);
        net_.send(MsgType::leave, ++seq_, w.data());
        (void)net_.flush();
        add_system("left #" + target);
    } else if (pc.name == "rooms") {
        if (!online_) return;
        net_.send(MsgType::room_list_req, ++seq_, std::string());
        (void)net_.flush();
    } else if (pc.name == "users") {
        if (!online_) return;
        TlvWriter w;
        w.add_str(tag::room, room_);
        net_.send(MsgType::user_list_req, ++seq_, w.data());
        (void)net_.flush();
    } else if (pc.name == "history") {
        uint32_t n = 50;
        if (!pc.arg.empty()) {
            const long v = std::strtol(pc.arg.c_str(), nullptr, 10);
            if (v > 0 && v <= 200) n = static_cast<uint32_t>(v);
        }
        uint64_t oldest = 0;
        for (const auto& m : msgs_) {
            if (m.id != 0) {
                oldest = m.id;
                break;
            }
        }
        if (oldest == 0) {
            add_system("no local message id to paginate from");
            return;
        }
        history_pending_ = true;
        send_history(oldest, n);
    } else if (pc.name == "clear") {
        msgs_.clear();
        cache_.clear();
        cache_cols_ = 0;
        cache_dirty_ = true;
        dirty_ = true;
    } else if (pc.name == "quit") {
        quit_ = true;
    }
}

void App::rebuild_cache(int cols) {
    cache_.clear();
    cache_cols_ = static_cast<size_t>(cols);
    cache_dirty_ = false;
    const int width = cols - 1 > 8 ? cols - 1 : 8;
    for (const auto& m : msgs_) {
        std::string prefix;
        std::string indent;
        if (m.system) {
            prefix = "* ";
            indent = "  ";
        } else {
            prefix = fmt_clock(m.ts) + " " + m.from + ": ";
            indent.assign(static_cast<size_t>(utf8::string_width(prefix)), ' ');
        }
        // 预留 2 列给状态后缀（… 未确认 / ! 发送失败）
        std::vector<std::string> lines = wrap_text(prefix, indent, m.text, width - 2);
        if (!lines.empty()) {
            if (m.failed) lines.back() += " !";
            else if (m.pending) lines.back() += " \xE2\x80\xA6";
        }
        for (auto& l : lines) {
            if (opt_.no_color) {
                cache_.push_back(l);
            } else if (m.system) {
                cache_.push_back(paint(l, "90"));
            } else if (m.from == user_) {
                cache_.push_back(paint(l, "36"));
            } else {
                cache_.push_back(l);
            }
        }
    }
}

void App::render() {
    if (opt_.plain) {
        for (; printed_plain_ < msgs_.size(); ++printed_plain_) {
            const Msg& m = msgs_[printed_plain_];
            if (m.system) {
                plain_print("* " + m.text);
            } else {
                plain_print(fmt_clock(m.ts) + " " + m.from + ": " + m.text);
            }
        }
        return;
    }

    const TermSize ts = term_size();
    renderer_.resize(ts.rows, ts.cols);
    if (cache_dirty_ || cache_cols_ != static_cast<size_t>(ts.cols)) rebuild_cache(ts.cols);

    // 输入区：按显示宽度折行，最多显示 4 行；其余行留给消息区
    const int wrap_width = ts.cols > 8 ? ts.cols - 1 : 8;
    const std::string& text = editor_.text();
    std::vector<std::string> input_lines = wrap_text("> ", "  ", text, wrap_width);
    constexpr int kMaxInputRows = 4;
    int shown_input = static_cast<int>(input_lines.size());
    if (shown_input < 1) shown_input = 1;
    if (shown_input > kMaxInputRows) shown_input = kMaxInputRows;

    int body_rows = ts.rows - 1 - shown_input;
    if (body_rows < 1) body_rows = 1;

    // 状态栏
    std::string status = "#" + room_;
    status += online_ ? "  \xE2\x97\x8F online" : "  \xE2\x97\x8B connecting";
    status += "  " + user_;
    if (member_count_ > 0) status += "  users:" + std::to_string(member_count_);
    status += "  msgs:" + std::to_string(msgs_.size());
    if (new_below_ > 0) status += "  \xE2\x86\x93 " + std::to_string(new_below_) + " new";
    status += "  " + fmt_clock(now_ms());
    const std::string status_plain = utf8::truncate_width(status, ts.cols - 1);
    const std::string status_line = opt_.no_color ? status_plain : paint(status_plain, "7");

    std::vector<std::string> screen;
    screen.reserve(static_cast<size_t>(ts.rows));
    screen.push_back(status_line);

    const size_t total = cache_.size();
    const size_t end = total > scroll_ ? total - scroll_ : 0;
    const size_t start = end > static_cast<size_t>(body_rows) ? end - static_cast<size_t>(body_rows) : 0;
    for (size_t i = start; i < end; ++i) screen.push_back(cache_[i]);
    while (static_cast<int>(screen.size()) < 1 + body_rows) screen.push_back("");

    // 输入区：显示最后 shown_input 行（长输入时向上滚动，光标始终可见）
    int first = static_cast<int>(input_lines.size()) - shown_input;
    if (first < 0) first = 0;
    for (int i = 0; i < shown_input; ++i) {
        const int idx = first + i;
        screen.push_back(idx < static_cast<int>(input_lines.size()) ? input_lines[idx] : std::string());
    }

    // 光标：按显示宽度折算到折行后的行列（宽字符按 2 列计，极端情况下差 1 列）
    const int cursor_w = 2 + utf8::string_width(std::string_view(text).substr(0, editor_.cursor()));
    int cursor_line = wrap_width > 0 ? cursor_w / wrap_width : 0;
    const int last_line = static_cast<int>(input_lines.size()) - 1;
    if (cursor_line > last_line) cursor_line = last_line;
    if (cursor_line < first) cursor_line = first;
    int cursor_col = cursor_w - cursor_line * wrap_width;
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col >= ts.cols) cursor_col = ts.cols - 1;
    int cursor_row = ts.rows - shown_input + (cursor_line - first);
    if (cursor_row >= ts.rows) cursor_row = ts.rows - 1;
    if (cursor_row < 0) cursor_row = 0;

    renderer_.draw(screen, cursor_row, cursor_col);
}

int App::run() {
    if (opt_.user.empty()) {
        std::fprintf(stderr, "error: --user is required\n");
        return 2;
    }
    if (opt_.password.empty()) {
        std::fprintf(stderr, "password for %s: ", opt_.user.c_str());
        std::fflush(stderr);
        read_secret_line(opt_.password);
    }

    std::signal(SIGWINCH, on_signal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!opt_.plain) {
        Status st = term_.enter();
        if (!st) {
            std::fprintf(stderr, "terminal error: %s\n", err_text(st.e, st.msg).c_str());
            opt_.plain = true;
        }
    }

    // stdin 必须非阻塞：否则 read 循环会阻塞等待更多输入，期间网络事件得不到处理。
    // 非阻塞后 EAGAIN 表示"暂无数据"，read 返回 0 才是真正的 EOF。
    const int stdin_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) (void)::fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    if (!connect_once()) {
        add_system("cannot reach server");
        render();
        term_.leave();
        if (!opt_.plain) {
            std::fprintf(stderr, "cannot connect to %s:%u\n", opt_.host.c_str(), opt_.port);
            return 1;
        }
        return 1;
    }
    send_hello();

    while (!quit_) {
        if (g_stop) {
            quit_ = true;
            break;
        }
        pollfd fds[2];
        fds[0] = pollfd{STDIN_FILENO, POLLIN, 0};
        fds[1] = pollfd{net_.fd(), POLLIN, 0};
        const int n = ::poll(fds, 2, 100);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_winch) {
                    g_winch = 0;
                    renderer_.invalidate();
                    dirty_ = true;
                }
                continue;
            }
            break;
        }
        if (n > 0) {
            if (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) on_socket();
            // 管道写端关闭时 revents 只有 POLLHUP（没有 POLLIN），必须一并处理，否则读不到 EOF
            if (!quit_ && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) on_stdin();
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

    if (net_.connected()) {
        TlvWriter w;
        w.add_str(tag::reason, "client quit");
        net_.send(MsgType::bye, ++seq_, w.data());
        (void)net_.flush();
    }
    net_.close();
    term_.leave();
    if (stdin_flags >= 0) (void)::fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    return 0;
}

}  // namespace chat
