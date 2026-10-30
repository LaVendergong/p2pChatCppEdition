// 字节缓冲：读缓冲（含边界检查）与写缓冲（大端整数 + 长度前缀字符串）。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

// 只读缓冲：从读游标消费，支持半包（数据不够时返回 false，不移动游标）
class Buffer {
public:
    void append(const void* p, size_t n);
    void clear();

    const uint8_t* data() const { return buf_.data() + r_; }
    uint8_t* mutable_data() { return buf_.data() + r_; }
    size_t size() const { return buf_.size() - r_; }
    bool empty() const { return size() == 0; }

    // 保留尾部未消费内容并压缩
    void compact();
    void consume(size_t n);

    bool peek_u16(uint16_t& v) const;
    bool read_u8(uint8_t& v);
    bool read_u16(uint16_t& v);
    bool read_u32(uint32_t& v);
    bool read_u64(uint64_t& v);

    // 大端读取，无越界
    static uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
    static uint32_t be32(const uint8_t* p) {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }
    static uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

private:
    std::vector<uint8_t> buf_;
    size_t r_ = 0;
};

// 写缓冲：全部大端
class Writer {
public:
    void u8(uint8_t v) { s_.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) {
        s_.push_back(static_cast<char>(v >> 8));
        s_.push_back(static_cast<char>(v));
    }
    void u32(uint32_t v) {
        s_.push_back(static_cast<char>(v >> 24));
        s_.push_back(static_cast<char>(v >> 16));
        s_.push_back(static_cast<char>(v >> 8));
        s_.push_back(static_cast<char>(v));
    }
    void u64(uint64_t v) {
        u32(uint32_t(v >> 32));
        u32(uint32_t(v));
    }
    void raw(const void* p, size_t n) { s_.append(static_cast<const char*>(p), n); }
    void str(std::string_view v) { s_.append(v.data(), v.size()); }

    void reset() { s_.clear(); }
    const std::string& data() const { return s_; }
    std::string& data() { return s_; }
    size_t size() const { return s_.size(); }

private:
    std::string s_;
};

}  // namespace chat
