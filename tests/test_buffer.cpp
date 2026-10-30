#include "chat/buffer.h"
#include "chat/utf8.h"
#include "test_framework.h"

using namespace chat;

TEST_CASE(buffer_read_integers) {
    Writer w;
    w.u8(0x12);
    w.u16(0x3456);
    w.u32(0x789ABCDE);
    w.u64(0x0102030405060708ull);

    Buffer b;
    b.append(w.data().data(), w.data().size());

    uint8_t a = 0;
    uint16_t c = 0;
    uint32_t d = 0;
    uint64_t e = 0;
    CHECK(b.read_u8(a));
    CHECK_EQ(a, 0x12);
    CHECK(b.read_u16(c));
    CHECK_EQ(c, 0x3456);
    CHECK(b.read_u32(d));
    CHECK_EQ(d, 0x789ABCDEu);
    CHECK(b.read_u64(e));
    CHECK_EQ(e, 0x0102030405060708ull);
    CHECK(b.empty());
}

TEST_CASE(buffer_partial_read_keeps_data) {
    Buffer b;
    const char data[] = {1, 2, 3};
    b.append(data, 3);
    uint32_t v = 0;
    uint64_t v8 = 0;
    CHECK(!b.read_u32(v));
    CHECK(!b.read_u64(v8));
    CHECK_EQ(b.size(), 3u);
    uint16_t h = 0;
    CHECK(b.read_u16(h));
    CHECK_EQ(h, 0x0102);
    CHECK_EQ(b.size(), 1u);
}

TEST_CASE(buffer_compact_after_consume) {
    Buffer b;
    std::string chunk(1000, 'x');
    for (int i = 0; i < 100; ++i) {
        b.append(chunk.data(), chunk.size());
        b.consume(1000);
    }
    CHECK(b.empty());
    b.append("tail", 4);
    CHECK_EQ(b.size(), 4u);
}

TEST_CASE(utf8_decode_encode) {
    const std::string s = "a中🙂";
    size_t i = 0;
    std::vector<uint32_t> cps;
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t n = utf8::decode(s.data() + i, s.size() - i, cp);
        CHECK(n > 0);
        cps.push_back(cp);
        i += n;
    }
    CHECK_EQ(cps.size(), 3u);
    CHECK_EQ(cps[0], 0x61u);
    CHECK_EQ(cps[1], 0x4E2Du);
    CHECK_EQ(cps[2], 0x1F642u);

    std::string back;
    for (uint32_t cp : cps) utf8::encode(cp, back);
    CHECK_EQ(back, s);
}

TEST_CASE(utf8_width_and_truncate) {
    CHECK_EQ(utf8::string_width("abc"), 3);
    CHECK_EQ(utf8::string_width("中文"), 4);
    CHECK_EQ(utf8::string_width("a中b"), 4);
    CHECK_EQ(utf8::truncate_width("中文abc", 3), "中");
    CHECK_EQ(utf8::truncate_width("中文abc", 4), "中文");
}

TEST_CASE(utf8_sanitize_ansi) {
    CHECK_EQ(utf8::sanitize("hello"), "hello");
    CHECK_EQ(utf8::sanitize("\x1b[31mred\x1b[0m"), "red");
    CHECK_EQ(utf8::sanitize("a\rb"), "ab");
    CHECK_EQ(utf8::sanitize("\x1b]0;title\x07x"), "x");
    CHECK_EQ(utf8::sanitize("中文\x1b[2J!"), "中文!");
}

TEST_CASE(utf8_boundaries) {
    // "a中b" = a E4 B8 AD b  (5 字节)
    const std::string s = "a中b";
    CHECK_EQ(utf8::prev_boundary(s, 5), 4);
    CHECK_EQ(utf8::prev_boundary(s, 4), 1);
    CHECK_EQ(utf8::prev_boundary(s, 1), 0);
    CHECK_EQ(utf8::next_boundary(s, 0), 1);
    CHECK_EQ(utf8::next_boundary(s, 1), 4);
    CHECK_EQ(utf8::next_boundary(s, 4), 5);
}
