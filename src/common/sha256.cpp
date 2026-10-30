#include "chat/sha256.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace chat {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = uint32_t(blk[i * 4]) << 24 | uint32_t(blk[i * 4 + 1]) << 16 | uint32_t(blk[i * 4 + 2]) << 8 |
               uint32_t(blk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
}

void digest(std::string_view data, uint8_t out[32]) {
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size();
    size_t i = 0;
    for (; i + 64 <= n; i += 64) compress(st, p + i);

    uint8_t tail[128];
    size_t rem = n - i;
    std::memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    size_t pad_len = (rem + 1 + 8 <= 64) ? 64 : 128;
    std::memset(tail + rem + 1, 0, pad_len - rem - 1 - 8 + 8);
    uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; ++k) tail[pad_len - 1 - k] = uint8_t(bits >> (k * 8));
    compress(st, tail);
    if (pad_len == 128) compress(st, tail + 64);

    for (int k = 0; k < 8; ++k) {
        out[k * 4] = uint8_t(st[k] >> 24);
        out[k * 4 + 1] = uint8_t(st[k] >> 16);
        out[k * 4 + 2] = uint8_t(st[k] >> 8);
        out[k * 4 + 3] = uint8_t(st[k]);
    }
}

}  // namespace

void sha256_digest(const void* data, size_t len, uint8_t out[32]) {
    digest(std::string_view(static_cast<const char*>(data), len), out);
}

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]) {
    constexpr size_t kBlock = 64;
    uint8_t k[kBlock] = {0};
    if (key_len > kBlock) {
        sha256_digest(key, key_len, k);  // 超过块长先哈希，其余位置已为 0
        key_len = 32;
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[kBlock];
    uint8_t opad[kBlock];
    for (size_t i = 0; i < kBlock; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    std::string inner;
    inner.reserve(kBlock + data_len);
    inner.append(reinterpret_cast<const char*>(ipad), kBlock);
    inner.append(reinterpret_cast<const char*>(data), data_len);
    uint8_t inner_digest[32];
    sha256_digest(inner.data(), inner.size(), inner_digest);

    std::string outer;
    outer.reserve(kBlock + 32);
    outer.append(reinterpret_cast<const char*>(opad), kBlock);
    outer.append(reinterpret_cast<const char*>(inner_digest), 32);
    sha256_digest(outer.data(), outer.size(), out);
}

std::string sha256_hex(std::string_view data) {
    uint8_t out[32];
    digest(data, out);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.resize(64);
    for (int i = 0; i < 32; ++i) {
        s[i * 2] = hex[out[i] >> 4];
        s[i * 2 + 1] = hex[out[i] & 0x0F];
    }
    return s;
}

std::string random_salt_hex() {
    uint8_t buf[16];
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        if (f) std::fclose(f);
        // 退化路径：用时间与地址混合，仅在无法读取 urandom 时发生
        uint64_t t = static_cast<uint64_t>(::time(nullptr));
        for (int i = 0; i < 16; ++i) buf[i] = uint8_t((t >> (i % 8 * 8)) ^ (i * 37));
    } else {
        std::fclose(f);
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.resize(32);
    for (int i = 0; i < 16; ++i) {
        s[i * 2] = hex[buf[i] >> 4];
        s[i * 2 + 1] = hex[buf[i] & 0x0F];
    }
    return s;
}

std::string password_digest(std::string_view salt_hex, std::string_view password) {
    std::string buf;
    buf.reserve(salt_hex.size() + 1 + password.size());
    buf.append(salt_hex);
    buf.push_back(':');
    buf.append(password);
    return sha256_hex(buf);
}

}  // namespace chat
