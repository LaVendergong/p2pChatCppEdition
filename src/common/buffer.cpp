#include "chat/buffer.h"

namespace chat {

void Buffer::append(const void* p, size_t n) {
    if (n == 0) return;
    // 剩余空间不足时先压缩，避免 vector 无界增长
    if (buf_.size() - r_ + n > buf_.capacity()) compact();
    buf_.insert(buf_.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
}

void Buffer::clear() {
    buf_.clear();
    r_ = 0;
}

void Buffer::compact() {
    if (r_ == 0) return;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(r_));
    r_ = 0;
}

void Buffer::consume(size_t n) {
    if (n >= size()) {
        r_ = buf_.size();
    } else {
        r_ += n;
    }
}

bool Buffer::peek_u16(uint16_t& v) const {
    if (size() < 2) return false;
    v = be16(data());
    return true;
}

bool Buffer::read_u8(uint8_t& v) {
    if (size() < 1) return false;
    v = data()[0];
    consume(1);
    return true;
}

bool Buffer::read_u16(uint16_t& v) {
    if (size() < 2) return false;
    v = be16(data());
    consume(2);
    return true;
}

bool Buffer::read_u32(uint32_t& v) {
    if (size() < 4) return false;
    v = be32(data());
    consume(4);
    return true;
}

bool Buffer::read_u64(uint64_t& v) {
    if (size() < 8) return false;
    v = be64(data());
    consume(8);
    return true;
}

}  // namespace chat
