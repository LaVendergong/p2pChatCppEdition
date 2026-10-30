#include "chat/protocol.h"

#include <cstring>

#include "chat/buffer.h"
#include "chat/crc32.h"

namespace chat {
namespace {

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

}  // namespace

const char* msg_type_name(MsgType t) { return msg_type_name(static_cast<uint16_t>(t)); }

const char* msg_type_name(uint16_t v) {
    switch (static_cast<MsgType>(v)) {
        case MsgType::hello: return "HELLO";
        case MsgType::hello_ack: return "HELLO_ACK";
        case MsgType::login: return "LOGIN";
        case MsgType::login_ack: return "LOGIN_ACK";
        case MsgType::register_req: return "REGISTER";
        case MsgType::register_ack: return "REGISTER_ACK";
        case MsgType::join: return "JOIN";
        case MsgType::join_ack: return "JOIN_ACK";
        case MsgType::leave: return "LEAVE";
        case MsgType::room_list_req: return "ROOM_LIST_REQ";
        case MsgType::room_list_ack: return "ROOM_LIST_ACK";
        case MsgType::user_list_req: return "USER_LIST_REQ";
        case MsgType::user_list_ack: return "USER_LIST_ACK";
        case MsgType::msg_send: return "MSG_SEND";
        case MsgType::msg_send_ack: return "MSG_SEND_ACK";
        case MsgType::msg_recv: return "MSG_RECV";
        case MsgType::history_req: return "HISTORY_REQ";
        case MsgType::history_ack: return "HISTORY_ACK";
        case MsgType::ping: return "PING";
        case MsgType::pong: return "PONG";
        case MsgType::peer_register: return "PEER_REGISTER";
        case MsgType::peer_register_ack: return "PEER_REGISTER_ACK";
        case MsgType::peer_lookup: return "PEER_LOOKUP";
        case MsgType::peer_lookup_ack: return "PEER_LOOKUP_ACK";
        case MsgType::peer_incoming: return "PEER_INCOMING";
        case MsgType::peer_offline: return "PEER_OFFLINE";
        case MsgType::relay_open: return "RELAY_OPEN";
        case MsgType::relay_data: return "RELAY_DATA";
        case MsgType::relay_ack: return "RELAY_ACK";
        case MsgType::crypto_kex: return "CRYPTO_KEX";
        case MsgType::error: return "ERROR";
        case MsgType::bye: return "BYE";
    }
    return "UNKNOWN";
}

std::string encode_frame(MsgType type, uint64_t seq, std::string_view payload, uint32_t flags) {
    Writer w;
    w.u32(kProtocolMagic);
    w.u16(kProtocolVersion);
    w.u16(static_cast<uint16_t>(type));
    w.u32(flags);
    w.u64(seq);
    w.u32(static_cast<uint32_t>(payload.size()));

    std::string out;
    out.reserve(kFrameHeaderSize + payload.size() + 4);
    out.append(w.data());
    out.append(payload.data(), payload.size());
    if (flags & kFlagCrc) {
        Writer c;
        c.u32(crc32(out.data(), out.size()));
        out.append(c.data());
    }
    return out;
}

const char* parse_status_name(ParseStatus s) {
    switch (s) {
        case ParseStatus::ok: return "ok";
        case ParseStatus::need_more: return "need_more";
        case ParseStatus::bad_magic: return "bad_magic";
        case ParseStatus::bad_version: return "bad_version";
        case ParseStatus::too_large: return "too_large";
        case ParseStatus::bad_crc: return "bad_crc";
    }
    return "?";
}

ParseStatus parse_frame(std::string_view in, Frame& out, size_t& consumed) {
    if (in.size() < kFrameHeaderSize) return ParseStatus::need_more;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(in.data());
    if (be32(p) != kProtocolMagic) return ParseStatus::bad_magic;
    const uint16_t ver = be16(p + 4);
    if (ver != kProtocolVersion) return ParseStatus::bad_version;
    const uint16_t type = be16(p + 6);
    const uint32_t flags = be32(p + 8);
    const uint64_t seq = be64(p + 12);
    const uint32_t len = be32(p + 20);
    if (len > kMaxPayload) return ParseStatus::too_large;

    const size_t total = kFrameHeaderSize + size_t(len) + ((flags & kFlagCrc) ? 4 : 0);
    if (in.size() < total) return ParseStatus::need_more;

    if (flags & kFlagCrc) {
        const uint32_t want = be32(p + kFrameHeaderSize + len);
        const uint32_t got = crc32(in.data(), kFrameHeaderSize + len);
        if (want != got) return ParseStatus::bad_crc;
    }

    out.header.version = ver;
    out.header.type = static_cast<MsgType>(type);
    out.header.flags = flags;
    out.header.seq = seq;
    out.header.payload_len = len;
    out.payload.assign(in.data() + kFrameHeaderSize, len);
    consumed = total;
    return ParseStatus::ok;
}

// ------------------------------------------------------------------ TLV
TlvReader::TlvReader(std::string_view payload) {
    size_t i = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    while (i < payload.size()) {
        if (i + 6 > payload.size()) {
            ok_ = false;
            return;
        }
        const uint16_t t = be16(p + i);
        const uint32_t len = be32(p + i + 2);
        if (i + 6 + len > payload.size()) {
            ok_ = false;
            return;
        }
        fields_.push_back(Field{t, payload.substr(i + 6, len)});
        i += 6 + size_t(len);
    }
}

bool TlvReader::has(uint16_t t) const {
    for (const auto& f : fields_) {
        if (f.tag == t) return true;
    }
    return false;
}

// 所有 get_* 都是"首个匹配 tag 生效"，重复 tag 不会覆盖前面的值
bool TlvReader::get_u8(uint16_t t, uint8_t& out) const {
    for (const auto& f : fields_) {
        if (f.tag == t) {
            if (f.value.size() != 1) return false;
            out = static_cast<uint8_t>(f.value[0]);
            return true;
        }
    }
    return false;
}

bool TlvReader::get_u16(uint16_t t, uint16_t& out) const {
    for (const auto& f : fields_) {
        if (f.tag == t) {
            if (f.value.size() != 2) return false;
            out = be16(reinterpret_cast<const uint8_t*>(f.value.data()));
            return true;
        }
    }
    return false;
}

bool TlvReader::get_u32(uint16_t t, uint32_t& out) const {
    for (const auto& f : fields_) {
        if (f.tag == t) {
            if (f.value.size() != 4) return false;
            out = be32(reinterpret_cast<const uint8_t*>(f.value.data()));
            return true;
        }
    }
    return false;
}

bool TlvReader::get_u64(uint16_t t, uint64_t& out) const {
    for (const auto& f : fields_) {
        if (f.tag == t) {
            if (f.value.size() != 8) return false;
            out = be64(reinterpret_cast<const uint8_t*>(f.value.data()));
            return true;
        }
    }
    return false;
}

std::string_view TlvReader::get_str(uint16_t t, std::string_view def) const {
    for (const auto& f : fields_) {
        if (f.tag == t) return f.value;
    }
    return def;
}

bool TlvReader::get_str(uint16_t t, std::string& out) const {
    for (const auto& f : fields_) {
        if (f.tag == t) {
            out.assign(f.value.data(), f.value.size());
            return true;
        }
    }
    return false;
}

bool TlvReader::get_array(uint16_t t, std::vector<std::string_view>& out) const {
    for (const auto& f : fields_) {
        if (f.tag != t) continue;
        if (f.value.size() < 4) return false;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(f.value.data());
        const uint32_t count = be32(p);
        size_t i = 4;
        out.clear();
        out.reserve(count);
        for (uint32_t k = 0; k < count; ++k) {
            if (i + 4 > f.value.size()) return false;
            const uint32_t len = be32(p + i);
            i += 4;
            if (i + len > f.value.size()) return false;
            out.push_back(f.value.substr(i, len));
            i += len;
        }
        return true;
    }
    return false;
}

uint64_t TlvReader::u64_or(uint16_t t, uint64_t def) const {
    uint64_t v = 0;
    return get_u64(t, v) ? v : def;
}
uint32_t TlvReader::u32_or(uint16_t t, uint32_t def) const {
    uint32_t v = 0;
    return get_u32(t, v) ? v : def;
}
uint16_t TlvReader::u16_or(uint16_t t, uint16_t def) const {
    uint16_t v = 0;
    return get_u16(t, v) ? v : def;
}
uint8_t TlvReader::u8_or(uint16_t t, uint8_t def) const {
    uint8_t v = 0;
    return get_u8(t, v) ? v : def;
}

void TlvWriter::field(uint16_t t, std::string_view v) {
    Writer w;
    w.u16(t);
    w.u32(static_cast<uint32_t>(v.size()));
    buf_.append(w.data());
    buf_.append(v.data(), v.size());
}

void TlvWriter::add_u8(uint16_t t, uint8_t v) {
    char c = static_cast<char>(v);
    field(t, std::string_view(&c, 1));
}

void TlvWriter::add_u16(uint16_t t, uint16_t v) {
    Writer w;
    w.u16(v);
    field(t, w.data());
}

void TlvWriter::add_u32(uint16_t t, uint32_t v) {
    Writer w;
    w.u32(v);
    field(t, w.data());
}

void TlvWriter::add_u64(uint16_t t, uint64_t v) {
    Writer w;
    w.u64(v);
    field(t, w.data());
}

void TlvWriter::add_str(uint16_t t, std::string_view v) { field(t, v); }

void TlvWriter::add_array(uint16_t t, const std::vector<std::string>& items) {
    Writer w;
    w.u32(static_cast<uint32_t>(items.size()));
    for (const auto& it : items) {
        w.u32(static_cast<uint32_t>(it.size()));
        w.str(it);
    }
    field(t, w.data());
}

// ------------------------------------------------------------------ 消息体
void encode_message(TlvWriter& w, const MessageView& m) {
    if (m.msg_id) w.add_u64(tag::msg_id, m.msg_id);
    if (!m.room.empty()) w.add_str(tag::room, m.room);
    if (!m.from.empty()) w.add_str(tag::from_user, m.from);
    if (m.ts_ms) w.add_u64(tag::ts_ms, m.ts_ms);
    w.add_str(tag::text, m.text);
    w.add_u8(tag::msg_kind, m.kind);
    if (m.client_msg_id) w.add_u64(tag::client_msg_id, m.client_msg_id);
}

bool decode_message(std::string_view blob, MessageView& m) {
    TlvReader r(blob);
    if (!r.ok()) return false;
    m.msg_id = r.u64_or(tag::msg_id, 0);
    m.ts_ms = r.u64_or(tag::ts_ms, 0);
    m.client_msg_id = r.u64_or(tag::client_msg_id, 0);
    m.kind = r.u8_or(tag::msg_kind, 0);
    r.get_str(tag::room, m.room);
    r.get_str(tag::from_user, m.from);
    r.get_str(tag::text, m.text);
    return true;
}

std::string encode_message_item(const MessageView& m) {
    TlvWriter w;
    encode_message(w, m);
    return w.data();
}

bool valid_username(std::string_view s) {
    if (s.size() < 2 || s.size() > kMaxUserLen) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

bool valid_room_name(std::string_view s) {
    if (s.empty() || s.size() > kMaxRoomLen) return false;
    if (s[0] == '.' || s[0] == '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) return false;
        if (c == '.' && i + 1 < s.size() && s[i + 1] == '.') return false;  // 防路径穿越
    }
    return true;
}

}  // namespace chat
