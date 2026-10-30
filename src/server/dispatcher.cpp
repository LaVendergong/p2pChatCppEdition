#include "dispatcher.h"

#include <algorithm>

#include "chat/log.h"
#include "chat/time_util.h"
#include "chat/utf8.h"
#include "server.h"

namespace chat {
namespace {

bool needs_auth(MsgType t) {
    switch (t) {
        case MsgType::hello:
        case MsgType::login:
        case MsgType::register_req:
        case MsgType::ping:
        case MsgType::bye:
            return false;
        default:
            return true;
    }
}

std::string to_item(const MessageView& v) { return encode_message_item(v); }

MessageView to_view(const std::string& room, const StoredMessage& m) {
    MessageView v;
    v.msg_id = m.msg_id;
    v.room = room;
    v.from = m.from;
    v.ts_ms = m.ts_ms;
    v.text = m.text;
    v.kind = m.kind;
    return v;
}

}  // namespace

void Dispatcher::on_frame(Session& s, const Frame& f) {
    if (needs_auth(f.header.type) && !s.authenticated) {
        server_.stats.errors.fetch_add(1, std::memory_order_relaxed);
        server_.send_error(s, Err::unauthenticated, "login required", f.header.seq);
        return;
    }
    switch (f.header.type) {
        case MsgType::hello: on_hello(s, f); break;
        case MsgType::login: on_login(s, f); break;
        case MsgType::register_req: on_register(s, f); break;
        case MsgType::join: on_join(s, f); break;
        case MsgType::leave: on_leave(s, f); break;
        case MsgType::msg_send: on_msg_send(s, f); break;
        case MsgType::history_req: on_history(s, f); break;
        case MsgType::user_list_req: on_user_list(s, f); break;
        case MsgType::room_list_req: on_room_list(s, f); break;
        case MsgType::ping: on_ping(s, f); break;
        case MsgType::bye: on_bye(s, f); break;
        default:
            server_.stats.errors.fetch_add(1, std::memory_order_relaxed);
            server_.send_error(s, Err::unsupported, msg_type_name(f.header.type), f.header.seq);
            break;
    }
}

void Dispatcher::on_hello(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        server_.send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    const std::string_view ver = r.get_str(tag::version);
    const std::string_view client = r.get_str(tag::client_name);
    LOG_DEBUG("session {} hello: version={} client={}", s.id, ver, client);

    TlvWriter w;
    w.add_str(tag::version, "chatd/0.1.0");
    w.add_u64(tag::server_time, now_ms());
    w.add_u64(tag::session_id, s.id);
    server_.send_frame(s, MsgType::hello_ack, f.header.seq, w.data());
}

void Dispatcher::on_login(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        server_.send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    std::string user;
    r.get_str(tag::username, user);
    const std::string_view digest = r.get_str(tag::password_hash);

    if (!valid_username(user)) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::bad_credentials));
        w.add_str(tag::reason, "invalid username (2-32 chars, [A-Za-z0-9_-])");
        server_.send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        return;
    }

    LoginResult res = server_.auth().login(user, digest, server_.config().allow_auto_register);

    if (res.kind == LoginKind::need_digest) {
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::unauthenticated));
        w.add_str(tag::salt, res.salt);
        w.add_str(tag::reason, "need password digest");
        server_.send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        return;
    }

    if (res.kind != LoginKind::ok) {
        s.login_fails += 1;
        TlvWriter w;
        w.add_u16(tag::code, static_cast<uint16_t>(Err::bad_credentials));
        w.add_str(tag::reason, "bad username or password");
        server_.send_frame(s, MsgType::login_ack, f.header.seq, w.data());
        LOG_WARN("session {} login failed for {} ({} attempts)", s.id, user, s.login_fails);
        if (s.login_fails >= 3) server_.close_session(s.id, "too many login failures");
        return;
    }

    if (s.authenticated && s.user != user) server_.unregister_user(s);
    s.user = user;
    s.user_id = res.user_id;
    if (!s.authenticated) server_.register_user(s);

    TlvWriter w;
    w.add_u16(tag::code, static_cast<uint16_t>(Err::ok));
    w.add_u64(tag::user_id, res.user_id);
    w.add_str(tag::username, user);
    if (res.created) w.add_str(tag::reason, "account created");
    server_.send_frame(s, MsgType::login_ack, f.header.seq, w.data());
    LOG_INFO("session {} login ok: {} (uid={}, created={})", s.id, user, res.user_id, res.created);
}

void Dispatcher::on_register(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        server_.send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    std::string user;
    r.get_str(tag::username, user);
    const std::string_view digest = r.get_str(tag::password_hash);

    TlvWriter w;
    if (!valid_username(user)) {
        w.add_u16(tag::code, static_cast<uint16_t>(Err::bad_credentials));
        w.add_str(tag::reason, "invalid username");
        server_.send_frame(s, MsgType::register_ack, f.header.seq, w.data());
        return;
    }
    if (server_.auth().exists(user)) {
        w.add_u16(tag::code, static_cast<uint16_t>(Err::user_exists));
        w.add_str(tag::reason, "user already exists");
        server_.send_frame(s, MsgType::register_ack, f.header.seq, w.data());
        return;
    }
    LoginResult res = server_.auth().login(user, digest, true);
    if (res.kind == LoginKind::need_digest) {
        w.add_u16(tag::code, static_cast<uint16_t>(Err::unauthenticated));
        w.add_str(tag::salt, res.salt);
        server_.send_frame(s, MsgType::register_ack, f.header.seq, w.data());
        return;
    }
    if (res.kind != LoginKind::ok) {
        w.add_u16(tag::code, static_cast<uint16_t>(Err::internal));
        w.add_str(tag::reason, "register failed");
        server_.send_frame(s, MsgType::register_ack, f.header.seq, w.data());
        return;
    }
    w.add_u16(tag::code, static_cast<uint16_t>(Err::ok));
    w.add_u64(tag::user_id, res.user_id);
    server_.send_frame(s, MsgType::register_ack, f.header.seq, w.data());
}

void Dispatcher::on_join(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string room;
    r.get_str(tag::room, room);
    if (!valid_room_name(room)) {
        server_.send_error(s, Err::invalid_arg, "invalid room name", f.header.seq);
        return;
    }
    RoomStore* rs = nullptr;
    Status st = server_.storage().open_room(room, &rs);
    if (!st) {
        LOG_ERROR("open room {} failed: {}", room, err_text(st.e, st.msg));
        server_.send_error(s, Err::storage_error, st.msg, f.header.seq);
        return;
    }

    server_.router().join(room, s.user);
    s.add_room(room);

    std::vector<StoredMessage> msgs;
    (void)rs->read_latest(50, msgs);

    TlvWriter w;
    w.add_str(tag::room, room);
    std::vector<std::string> items;
    items.reserve(msgs.size());
    for (const auto& m : msgs) items.push_back(to_item(to_view(room, m)));
    w.add_array(tag::messages, items);
    server_.send_frame(s, MsgType::join_ack, f.header.seq, w.data());

    server_.broadcast_system(room, s.user + " joined the room");
    LOG_INFO("session {} ({}) joined {}", s.id, s.user, room);
}

void Dispatcher::on_leave(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string room;
    r.get_str(tag::room, room);
    if (!s.has_room(room)) {
        server_.send_error(s, Err::not_a_member, "not in room", f.header.seq);
        return;
    }
    server_.router().leave(room, s.user);
    s.remove_room(room);
    server_.broadcast_system(room, s.user + " left the room");
    LOG_INFO("session {} ({}) left {}", s.id, s.user, room);
}

void Dispatcher::on_msg_send(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    if (!r.ok()) {
        server_.send_error(s, Err::bad_frame, "malformed TLV", f.header.seq);
        return;
    }
    std::string room;
    r.get_str(tag::room, room);
    const std::string_view text = r.get_str(tag::text);
    const uint64_t cmid = r.u64_or(tag::client_msg_id, 0);

    if (!s.has_room(room)) {
        server_.send_error(s, Err::not_a_member, "join the room first", f.header.seq);
        return;
    }
    if (text.size() > kMaxTextBytes) {
        server_.send_error(s, Err::text_too_long, "message too long", f.header.seq);
        return;
    }
    if (!utf8::valid(text)) {
        server_.send_error(s, Err::invalid_arg, "text is not valid utf-8", f.header.seq);
        return;
    }

    // 幂等：重复的 client_msg_id 直接回原来的 msg_id，不重复落盘
    uint64_t existing = 0;
    if (cmid != 0 && s.lookup_ack(cmid, existing)) {
        TlvWriter w;
        w.add_u64(tag::msg_id, existing);
        w.add_u64(tag::client_msg_id, cmid);
        server_.send_frame(s, MsgType::msg_send_ack, f.header.seq, w.data());
        return;
    }

    const uint64_t tag_id = server_.next_ack_tag();
    {
        std::lock_guard<std::mutex> lk(server_.ack_mu);
        server_.ack_map[tag_id] = Server::PendingAck{s.id, cmid};
    }

    StoredMessage m;
    m.from = s.user;
    m.ts_ms = now_ms();
    m.text.assign(text.data(), text.size());

    // append 只是入队（内存操作），真正的 pwrite 由存储的 flusher 线程按顺序执行：
    // io 线程不会为落盘停下来，同时保证「同一条连接的消息按接收顺序编号」。
    Status st = server_.storage().append(room, m, tag_id);
    if (!st) {
        std::lock_guard<std::mutex> lk(server_.ack_mu);
        server_.ack_map.erase(tag_id);
        LOG_ERROR("enqueue to {} failed: {}", room, err_text(st.e, st.msg));
        server_.send_error(s, Err::storage_error, st.msg, f.header.seq);
        return;
    }
    LOG_TRACE("session {} queued message for {}", s.id, room);
}

void Dispatcher::on_history(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string room;
    r.get_str(tag::room, room);
    uint64_t before_id = r.u64_or(tag::before_id, 0);
    uint32_t limit = r.u32_or(tag::limit, 50);
    if (limit == 0) limit = 50;
    if (limit > 200) limit = 200;

    if (!s.has_room(room)) {
        server_.send_error(s, Err::not_a_member, "join the room first", f.header.seq);
        return;
    }
    RoomStore* rs = server_.storage().room(room);
    if (!rs) {
        server_.send_error(s, Err::room_not_found, room, f.header.seq);
        return;
    }

    const uint64_t sid = s.id;
    const uint64_t seq = f.header.seq;
    // 读盘可能较慢，放工作线程，结果回投 io 线程
    server_.pool().post([this, sid, seq, room, before_id, limit, rs]() {
        std::vector<StoredMessage> msgs;
        Status st = rs->read_before(before_id, limit, msgs);
        server_.post_to_io([this, sid, seq, room, msgs = std::move(msgs), st]() {
            Session* target = server_.find_session(sid);
            if (!target) return;
            if (!st) {
                server_.send_error(*target, st.e, st.msg, seq);
                return;
            }
            TlvWriter w;
            w.add_str(tag::room, room);
            w.add_u64(tag::before_id, 0);
            std::vector<std::string> items;
            items.reserve(msgs.size());
            for (const auto& m : msgs) items.push_back(to_item(to_view(room, m)));
            w.add_array(tag::messages, items);
            server_.send_frame(*target, MsgType::history_ack, seq, w.data());
        });
    });
}

void Dispatcher::on_user_list(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    std::string room;
    r.get_str(tag::room, room);
    std::vector<std::string> members = server_.router().members(room);
    std::sort(members.begin(), members.end());

    std::vector<std::string> items;
    items.reserve(members.size());
    for (const auto& u : members) {
        TlvWriter iw;
        iw.add_str(tag::username, u);
        items.push_back(iw.data());
    }
    TlvWriter w;
    w.add_str(tag::room, room);
    w.add_array(tag::users, items);
    server_.send_frame(s, MsgType::user_list_ack, f.header.seq, w.data());
}

void Dispatcher::on_room_list(Session& s, const Frame& f) {
    std::vector<std::string> rooms = server_.storage().list_rooms();
    std::vector<std::string> items;
    items.reserve(rooms.size());
    for (const auto& name : rooms) {
        RoomStore* rs = server_.storage().room(name);
        TlvWriter iw;
        iw.add_str(tag::room, name);
        iw.add_u32(tag::count, rs ? static_cast<uint32_t>(rs->msg_count()) : 0);
        iw.add_u32(tag::limit, static_cast<uint32_t>(server_.router().member_count(name)));
        items.push_back(iw.data());
    }
    TlvWriter w;
    w.add_array(tag::rooms, items);
    server_.send_frame(s, MsgType::room_list_ack, f.header.seq, w.data());
}

void Dispatcher::on_ping(Session& s, const Frame& f) {
    TlvWriter w;
    w.add_u64(tag::server_time, now_ms());
    server_.send_frame(s, MsgType::pong, f.header.seq, w.data());
}

void Dispatcher::on_bye(Session& s, const Frame& f) {
    TlvReader r(f.payload);
    const std::string_view reason = r.get_str(tag::reason);
    LOG_INFO("session {} bye: {}", s.id, reason);
    server_.close_session(s.id, "client bye");
}

}  // namespace chat
