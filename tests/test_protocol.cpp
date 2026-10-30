#include "chat/crc32.h"
#include "chat/protocol.h"
#include "test_framework.h"

using namespace chat;

TEST_CASE(frame_roundtrip) {
    TlvWriter w;
    w.add_str(tag::username, "alice");
    w.add_u64(tag::msg_id, 1234567890123ull);
    w.add_u32(tag::limit, 50);
    w.add_u8(tag::msg_kind, 0);

    const std::string bytes = encode_frame(MsgType::msg_send, 42, w.data());
    CHECK_EQ(bytes.size(), kFrameHeaderSize + w.data().size());

    Frame f;
    size_t consumed = 0;
    CHECK(parse_frame(bytes, f, consumed) == ParseStatus::ok);
    CHECK_EQ(consumed, bytes.size());
    CHECK(f.header.type == MsgType::msg_send);
    CHECK_EQ(f.header.seq, 42u);
    CHECK_EQ(f.header.payload_len, w.data().size());

    TlvReader r(f.payload);
    CHECK(r.ok());
    CHECK_EQ(std::string(r.get_str(tag::username)), "alice");
    CHECK_EQ(r.u64_or(tag::msg_id, 0), 1234567890123ull);
    CHECK_EQ(r.u32_or(tag::limit, 0), 50u);
}

TEST_CASE(frame_truncated_needs_more) {
    TlvWriter w;
    w.add_str(tag::text, "hello world");
    const std::string bytes = encode_frame(MsgType::msg_recv, 7, w.data());

    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        Frame f;
        size_t consumed = 0;
        CHECK(parse_frame(std::string_view(bytes.data(), cut), f, consumed) == ParseStatus::need_more);
    }
}

TEST_CASE(frame_byte_by_byte) {
    TlvWriter w;
    w.add_str(tag::text, "逐字节到达");
    const std::string bytes = encode_frame(MsgType::msg_send, 1, w.data());

    std::string acc;
    size_t total_consumed = 0;
    int frames = 0;
    for (char c : bytes) {
        acc.push_back(c);
        for (;;) {
            Frame f;
            size_t consumed = 0;
            const ParseStatus ps = parse_frame(acc, f, consumed);
            if (ps == ParseStatus::need_more) break;
            CHECK(ps == ParseStatus::ok);
            CHECK_EQ(f.header.payload_len, w.data().size());
            ++frames;
            acc.erase(0, consumed);
            total_consumed += consumed;
        }
    }
    CHECK_EQ(frames, 1);
    CHECK_EQ(total_consumed, bytes.size());
}

TEST_CASE(frame_bad_magic) {
    std::string bytes = encode_frame(MsgType::ping, 1, "");
    bytes[0] = 'X';
    Frame f;
    size_t consumed = 0;
    CHECK(parse_frame(bytes, f, consumed) == ParseStatus::bad_magic);
}

TEST_CASE(frame_bad_version) {
    std::string bytes = encode_frame(MsgType::ping, 1, "");
    bytes[5] = 9;  // version 高字节
    Frame f;
    size_t consumed = 0;
    CHECK(parse_frame(bytes, f, consumed) == ParseStatus::bad_version);
}

TEST_CASE(frame_too_large) {
    std::string bytes = encode_frame(MsgType::msg_send, 1, "");
    // 把 payload_len 改成超过上限
    const uint32_t big = kMaxPayload + 1;
    bytes[20] = static_cast<char>(big >> 24);
    bytes[21] = static_cast<char>(big >> 16);
    bytes[22] = static_cast<char>(big >> 8);
    bytes[23] = static_cast<char>(big);
    Frame f;
    size_t consumed = 0;
    CHECK(parse_frame(bytes, f, consumed) == ParseStatus::too_large);
}

TEST_CASE(frame_crc_mismatch) {
    TlvWriter w;
    w.add_str(tag::text, "crc check");
    std::string bytes = encode_frame(MsgType::msg_recv, 3, w.data(), kFlagCrc);
    bytes[bytes.size() - 1] ^= 0x01;  // 破坏 crc
    Frame f;
    size_t consumed = 0;
    CHECK(parse_frame(bytes, f, consumed) == ParseStatus::bad_crc);

    // 未损坏时应能解析
    const std::string good = encode_frame(MsgType::msg_recv, 3, w.data(), kFlagCrc);
    CHECK(parse_frame(good, f, consumed) == ParseStatus::ok);
    CHECK_EQ(f.header.seq, 3u);
}

TEST_CASE(tlv_unknown_tag_skipped) {
    TlvWriter w;
    w.add_str(0x7777, "unknown");  // 未知 tag
    w.add_str(tag::username, "bob");
    w.add_u64(tag::msg_id, 9);

    TlvReader r(w.data());
    CHECK(r.ok());
    CHECK_EQ(std::string(r.get_str(tag::username)), "bob");
    CHECK_EQ(r.u64_or(tag::msg_id, 0), 9u);
    CHECK_EQ(r.field_count(), 3u);
}

TEST_CASE(tlv_malformed) {
    TlvWriter w;
    w.add_str(tag::text, "abcdef");
    std::string payload = w.data();
    // 截掉最后 3 字节 → 字段长度超过剩余数据
    TlvReader r(std::string_view(payload.data(), payload.size() - 3));
    CHECK(!r.ok());
}

TEST_CASE(tlv_array_roundtrip) {
    std::vector<std::string> items;
    for (int i = 0; i < 3; ++i) {
        TlvWriter iw;
        iw.add_str(tag::username, "user" + std::to_string(i));
        items.push_back(iw.data());
    }
    TlvWriter w;
    w.add_array(tag::users, items);

    TlvReader r(w.data());
    std::vector<std::string_view> out;
    CHECK(r.get_array(tag::users, out));
    CHECK_EQ(out.size(), 3u);
    TlvReader r0(out[0]);
    CHECK_EQ(std::string(r0.get_str(tag::username)), "user0");
}

TEST_CASE(message_roundtrip_cjk) {
    MessageView v;
    v.msg_id = 77;
    v.ts_ms = 1700000000000ull;
    v.from = "张三";
    v.text = "你好，终端聊天！";
    v.kind = 0;

    const std::string blob = encode_message_item(v);
    MessageView out;
    CHECK(decode_message(blob, out));
    CHECK_EQ(out.msg_id, 77u);
    CHECK_EQ(out.from, "张三");
    CHECK_EQ(out.text, "你好，终端聊天！");
}

TEST_CASE(names_validation) {
    CHECK(valid_username("alice"));
    CHECK(valid_username("a_b-c9"));
    CHECK(!valid_username("a"));
    CHECK(!valid_username("bad name"));
    CHECK(!valid_username("中文名"));

    CHECK(valid_room_name("public"));
    CHECK(valid_room_name("a.b-c_1"));
    CHECK(!valid_room_name("../etc"));
    CHECK(!valid_room_name(".hidden"));
    CHECK(!valid_room_name("a/b"));
    CHECK(!valid_room_name(""));
}

TEST_CASE(crc32_known_values) {
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32("", 0), 0u);
    // 链式调用等价于一次性计算
    const char* s = "hello world";
    const uint32_t a = crc32(s, 5);
    const uint32_t b = crc32(s + 5, 6, a);
    CHECK_EQ(b, crc32(s, 11));
}
