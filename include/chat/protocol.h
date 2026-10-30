// CHTP/1 协议：帧编解码 + TLV 字段读写（见 docs/03-通信协议.md）
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chat/error.h"

namespace chat {

constexpr uint32_t kProtocolMagic = 0x43485431u;  // "CHT1"
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kFrameHeaderSize = 24;
constexpr uint32_t kMaxPayload = 1u << 20;   // 1 MiB
constexpr uint32_t kMaxTextBytes = 32768;    // 单条消息上限
constexpr size_t kMaxRoomLen = 64;
constexpr size_t kMaxUserLen = 32;

enum class MsgType : uint16_t {
    hello = 0x0001,
    hello_ack = 0x0002,
    login = 0x0010,
    login_ack = 0x0011,
    register_req = 0x0012,
    register_ack = 0x0013,
    join = 0x0020,
    join_ack = 0x0021,
    leave = 0x0022,
    room_list_req = 0x0023,
    room_list_ack = 0x0024,
    user_list_req = 0x0025,
    user_list_ack = 0x0026,
    msg_send = 0x0030,
    msg_send_ack = 0x0031,
    msg_recv = 0x0032,
    history_req = 0x0040,
    history_ack = 0x0041,
    ping = 0x0050,
    pong = 0x0051,
    // --- P2P 信令（chatsig）---
    peer_register = 0x0060,
    peer_register_ack = 0x0061,
    peer_lookup = 0x0062,
    peer_lookup_ack = 0x0063,
    peer_incoming = 0x0064,
    peer_offline = 0x0065,
    relay_open = 0x0070,
    relay_data = 0x0071,
    relay_ack = 0x0072,
    // --- P2P 端到端加密（媒体通道建立后双方互发一次）---
    crypto_kex = 0x0080,
    error = 0x00F0,
    bye = 0x00F1,
};

const char* msg_type_name(MsgType t);
const char* msg_type_name(uint16_t v);

constexpr uint32_t kFlagCrc = 1u << 0;
constexpr uint32_t kFlagCompressed = 1u << 1;
constexpr uint32_t kFlagPush = 1u << 2;

struct FrameHeader {
    uint16_t version = kProtocolVersion;
    MsgType type = MsgType::hello;
    uint32_t flags = 0;
    uint64_t seq = 0;
    uint32_t payload_len = 0;
};

struct Frame {
    FrameHeader header;
    std::string payload;

    bool is_push() const { return (header.flags & kFlagPush) != 0; }
};

// 编码一整帧（payload 可为空）
std::string encode_frame(MsgType type, uint64_t seq, std::string_view payload, uint32_t flags = 0);

enum class ParseStatus : uint16_t { ok, need_more, bad_magic, bad_version, too_large, bad_crc };
const char* parse_status_name(ParseStatus s);

// 从 in 尝试解析一帧：need_more 表示数据不足（consumed 不变）
ParseStatus parse_frame(std::string_view in, Frame& out, size_t& consumed);

// ------------------------------------------------------------------ TLV
namespace tag {
enum : uint16_t {
    version = 0x0001,
    client_name = 0x0002,
    username = 0x0003,
    password_hash = 0x0004,
    salt = 0x0005,
    room = 0x0006,
    text = 0x0007,
    client_msg_id = 0x0008,
    msg_id = 0x0009,
    from_user = 0x000A,
    ts_ms = 0x000B,
    code = 0x000C,
    reason = 0x000D,
    limit = 0x000E,
    before_id = 0x000F,
    messages = 0x0010,
    users = 0x0011,
    rooms = 0x0012,
    user_id = 0x0013,
    msg_kind = 0x0014,
    count = 0x0015,
    server_time = 0x0016,
    session_id = 0x0017,
    allow_create = 0x0018,
    // --- P2P ---
    lan_ip = 0x0019,
    tcp_port = 0x001A,
    candidates = 0x001B,
    peer_name = 0x001C,
    public_ip = 0x001D,
    public_port = 0x001E,
    cand_kind = 0x001F,
    relay_flag = 0x0020,  // u8: 1 表示"对方想通过中继连你"
    // --- 端到端加密 ---
    kex_pub = 0x0021,  // str(32): X25519 身份公钥（长期）
    kex_eph = 0x0022,  // str(32): 本次连接的一次性 X25519 公钥（前向保密）
    cipher = 0x0023,     // str: 密文 || 16 字节认证标签
    nonce = 0x0024,      // u64: 消息序号（用作 AEAD nonce，从 1 递增）
};
}  // namespace tag

// 候选地址类型
enum class CandKind : uint8_t { lan = 0, public_ = 1, relay = 2 };

// 只读 TLV 视图：字段以 string_view 指向原始 payload，生命周期由调用方保证
class TlvReader {
public:
    TlvReader() = default;
    explicit TlvReader(std::string_view payload);

    bool ok() const { return ok_; }
    bool has(uint16_t t) const;
    bool get_u8(uint16_t t, uint8_t& out) const;
    bool get_u16(uint16_t t, uint16_t& out) const;
    bool get_u32(uint16_t t, uint32_t& out) const;
    bool get_u64(uint16_t t, uint64_t& out) const;
    std::string_view get_str(uint16_t t, std::string_view def = {}) const;
    bool get_str(uint16_t t, std::string& out) const;
    bool get_array(uint16_t t, std::vector<std::string_view>& out) const;
    size_t field_count() const { return fields_.size(); }

    // 带默认值的整数读取
    uint64_t u64_or(uint16_t t, uint64_t def) const;
    uint32_t u32_or(uint16_t t, uint32_t def) const;
    uint16_t u16_or(uint16_t t, uint16_t def) const;
    uint8_t u8_or(uint16_t t, uint8_t def) const;

private:
    struct Field {
        uint16_t tag;
        std::string_view value;
    };
    std::vector<Field> fields_;
    bool ok_ = true;
};

class TlvWriter {
public:
    void add_u8(uint16_t t, uint8_t v);
    void add_u16(uint16_t t, uint16_t v);
    void add_u32(uint16_t t, uint32_t v);
    void add_u64(uint16_t t, uint64_t v);
    void add_str(uint16_t t, std::string_view v);
    void add_array(uint16_t t, const std::vector<std::string>& items);

    const std::string& data() const { return buf_; }
    std::string& data() { return buf_; }
    void reset() { buf_.clear(); }

private:
    void field(uint16_t t, std::string_view v);
    std::string buf_;
};

// ------------------------------------------------------------------ 消息体
struct MessageView {
    uint64_t msg_id = 0;
    uint64_t ts_ms = 0;
    uint64_t client_msg_id = 0;  // 仅 MSG_SEND / ACK 使用
    std::string room;
    std::string from;
    std::string text;
    uint8_t kind = 0;
};

void encode_message(TlvWriter& w, const MessageView& m);
bool decode_message(std::string_view blob, MessageView& m);
std::string encode_message_item(const MessageView& m);

// 输入校验
bool valid_username(std::string_view s);
bool valid_room_name(std::string_view s);

}  // namespace chat
