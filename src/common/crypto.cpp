// X25519 (RFC 7748) + ChaCha20-Poly1305 (RFC 8439) + HKDF-SHA256 (RFC 5869)
// 手写实现，正确性由 tests/test_crypto.cpp 的官方测试向量保证。
#include "chat/crypto.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "chat/sha256.h"

namespace chat::crypto {
// 内部实现放 detail，便于单元测试逐层验证域运算（密码学代码必须可测）
namespace detail {

using u128 = unsigned __int128;

uint32_t load32_le(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void store32_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

uint64_t load64_le(const uint8_t* p) {
    return uint64_t(load32_le(p)) | uint64_t(load32_le(p + 4)) << 32;
}

// ------------------------------------------------------------------ X25519
// 域元素：4 个 64 位 limb，小端。
// 内部约定：每个 fe 值恒 < 2^255（非规范，可能 ≥ p，p = 2^255 - 19）。
using fe = uint64_t[4];

constexpr uint64_t kTopMask = 0x7fffffffffffffffULL;  // 第 255 位

void fe_zero(fe h) { h[0] = h[1] = h[2] = h[3] = 0; }
void fe_one(fe h) {
    fe_zero(h);
    h[0] = 1;
}

void fe_copy(fe h, const fe f) {
    for (int i = 0; i < 4; ++i) h[i] = f[i];
}

// 把第 255 位折回：2^255 ≡ 19 (mod p)。最多两轮。
void fe_weak_reduce(fe h) {
    while (h[3] >> 63) {
        h[3] &= kTopMask;
        u128 c = 19;
        for (int i = 0; i < 4; ++i) {
            c += h[i];
            h[i] = static_cast<uint64_t>(c);
            c >>= 64;
        }
    }
}

void fe_add(fe h, const fe f, const fe g) {
    // 两个 < 2^255 的数相加 < 2^256，不会溢出
    uint64_t c = 0;
    for (int i = 0; i < 4; ++i) {
        const u128 cur = static_cast<u128>(f[i]) + g[i] + c;
        h[i] = static_cast<uint64_t>(cur);
        c = static_cast<uint64_t>(cur >> 64);
    }
    fe_weak_reduce(h);
}

void fe_sub(fe h, const fe f, const fe g) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t t = f[i] - g[i];
        const uint64_t b1 = (f[i] < g[i]) ? 1u : 0u;
        const uint64_t t2 = t - borrow;
        const uint64_t b2 = (t < borrow) ? 1u : 0u;
        h[i] = t2;
        borrow = b1 | b2;
    }
    if (borrow != 0) {
        // 回绕相当于多加了 2^256 ≡ 38，而我们要的是 f - g，所以减回去。
        // f,g < 2^255 ⇒ 回绕后的值 ≥ 2^255 > 38，不会二次借位。
        uint64_t b = 38;
        for (int i = 0; i < 4; ++i) {
            const uint64_t t = h[i] - b;
            b = (h[i] < b) ? 1u : 0u;
            h[i] = t;
        }
    }
    fe_weak_reduce(h);
}

void fe_mul(fe h, const fe f, const fe g) {
    uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        u128 carry = 0;
        for (int j = 0; j < 4; ++j) {
            const u128 cur = static_cast<u128>(f[i]) * g[j] + t[i + j] + carry;
            t[i + j] = static_cast<uint64_t>(cur);
            carry = cur >> 64;
        }
        t[i + 4] = static_cast<uint64_t>(carry);
    }

    // r = lo + 38 * hi   （2^256 ≡ 38 mod p）
    uint64_t r[4];
    u128 carry = 0;
    for (int i = 0; i < 4; ++i) {
        const u128 cur = static_cast<u128>(t[i]) + static_cast<u128>(t[i + 4]) * 38 + carry;
        r[i] = static_cast<uint64_t>(cur);
        carry = cur >> 64;
    }
    while (carry != 0) {
        u128 c2 = carry * 38;
        for (int i = 0; i < 4 && c2 != 0; ++i) {
            c2 += r[i];
            r[i] = static_cast<uint64_t>(c2);
            c2 >>= 64;
        }
        carry = c2;
    }

    fe_copy(h, r);
    fe_weak_reduce(h);  // 折叠第 255 位（2^255 ≡ 19）
}

void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

void fe_cswap(fe f, fe g, uint64_t b) {
    const uint64_t mask = 0 - b;
    for (int i = 0; i < 4; ++i) {
        const uint64_t x = (f[i] ^ g[i]) & mask;
        f[i] ^= x;
        g[i] ^= x;
    }
}

void fe_frombytes(fe h, const uint8_t s[32]) {
    for (int i = 0; i < 4; ++i) h[i] = load64_le(s + 8 * i);
    h[3] &= kTopMask;
}

void fe_tobytes(uint8_t s[32], const fe h) {
    // 条件减去 p，得到 [0, p) 的唯一编码
    static const uint64_t kP[4] = {0xffffffffffffffedULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
                                   kTopMask};
    uint64_t t[4];
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t t1 = h[i] - kP[i];
        const uint64_t b1 = (h[i] < kP[i]) ? 1u : 0u;
        const uint64_t t2 = t1 - borrow;
        const uint64_t b2 = (t1 < borrow) ? 1u : 0u;
        t[i] = t2;
        borrow = b1 | b2;
    }
    const uint64_t mask = borrow - 1;  // 未借位（h ≥ p）时全 1，选 t；否则选 h
    for (int i = 0; i < 4; ++i) {
        const uint64_t v = (t[i] & mask) | (h[i] & ~mask);
        for (int k = 0; k < 8; ++k) s[8 * i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
}

// z^(p-2)，即求逆
void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (int i = 1; i < 5; ++i) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 10; ++i) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 20; ++i) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 10; ++i) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 50; ++i) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 100; ++i) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 50; ++i) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 1; i < 5; ++i) fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

void fe_mul_small(fe h, const fe f, uint64_t v) {
    fe k;
    fe_zero(k);
    k[0] = v;
    fe_mul(h, f, k);
}

void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    std::memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_frombytes(x1, point);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        const uint64_t b = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul_small(z3, tmp1, 121666);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

// ------------------------------------------------------------------ ChaCha20
void chacha20_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b;
    d ^= a;
    d = (d << 16) | (d >> 16);
    c += d;
    b ^= c;
    b = (b << 12) | (b >> 20);
    a += b;
    d ^= a;
    d = (d << 8) | (d >> 24);
    c += d;
    b ^= c;
    b = (b << 7) | (b >> 25);
}

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]) {
    uint32_t x[16];
    x[0] = 0x61707865;
    x[1] = 0x3320646e;
    x[2] = 0x79622d32;
    x[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) x[4 + i] = load32_le(key + 4 * i);
    x[12] = counter;
    x[13] = load32_le(nonce);
    x[14] = load32_le(nonce + 4);
    x[15] = load32_le(nonce + 8);

    uint32_t in[16];
    std::memcpy(in, x, sizeof(in));
    for (int i = 0; i < 10; ++i) {
        chacha20_quarter_round(x[0], x[4], x[8], x[12]);
        chacha20_quarter_round(x[1], x[5], x[9], x[13]);
        chacha20_quarter_round(x[2], x[6], x[10], x[14]);
        chacha20_quarter_round(x[3], x[7], x[11], x[15]);
        chacha20_quarter_round(x[0], x[5], x[10], x[15]);
        chacha20_quarter_round(x[1], x[6], x[11], x[12]);
        chacha20_quarter_round(x[2], x[7], x[8], x[13]);
        chacha20_quarter_round(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) store32_le(out + 4 * i, x[i] + in[i]);
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, size_t len,
                  uint8_t* out) {
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, nonce, counter++, block);
        const size_t n = (len - off) < 64 ? (len - off) : 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = static_cast<uint8_t>(in[off + i] ^ block[i]);
        off += n;
    }
}

// ------------------------------------------------------------------ Poly1305
// 5 × 26-bit limb（donna 风格）
void poly1305_block(uint32_t h[5], const uint32_t r[5], const uint8_t block[16], uint32_t hibit) {
    uint32_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint32_t t0 = load32_le(block) & 0x3ffffff;
    uint32_t t1 = (load32_le(block + 3) >> 2) & 0x3ffffff;
    uint32_t t2 = (load32_le(block + 6) >> 4) & 0x3ffffff;
    uint32_t t3 = (load32_le(block + 9) >> 6) & 0x3ffffff;
    uint32_t t4 = (load32_le(block + 12) >> 8) | hibit;

    uint64_t d0 = uint64_t(h[0]) + t0;
    uint64_t d1 = uint64_t(h[1]) + t1;
    uint64_t d2 = uint64_t(h[2]) + t2;
    uint64_t d3 = uint64_t(h[3]) + t3;
    uint64_t d4 = uint64_t(h[4]) + t4;

    uint64_t m0 = d0 * r0 + d1 * s4 + d2 * s3 + d3 * s2 + d4 * s1;
    uint64_t m1 = d0 * r1 + d1 * r0 + d2 * s4 + d3 * s3 + d4 * s2;
    uint64_t m2 = d0 * r2 + d1 * r1 + d2 * r0 + d3 * s4 + d4 * s3;
    uint64_t m3 = d0 * r3 + d1 * r2 + d2 * r1 + d3 * r0 + d4 * s4;
    uint64_t m4 = d0 * r4 + d1 * r3 + d2 * r2 + d3 * r1 + d4 * r0;

    uint32_t c;
    c = static_cast<uint32_t>(m0 >> 26);
    h[0] = static_cast<uint32_t>(m0) & 0x3ffffff;
    m1 += c;
    c = static_cast<uint32_t>(m1 >> 26);
    h[1] = static_cast<uint32_t>(m1) & 0x3ffffff;
    m2 += c;
    c = static_cast<uint32_t>(m2 >> 26);
    h[2] = static_cast<uint32_t>(m2) & 0x3ffffff;
    m3 += c;
    c = static_cast<uint32_t>(m3 >> 26);
    h[3] = static_cast<uint32_t>(m3) & 0x3ffffff;
    m4 += c;
    c = static_cast<uint32_t>(m4 >> 26);
    h[4] = static_cast<uint32_t>(m4) & 0x3ffffff;
    h[0] += c * 5;
    c = h[0] >> 26;
    h[0] &= 0x3ffffff;
    h[1] += c;
}

void poly1305_auth(const uint8_t key[32], const uint8_t* aad, size_t aad_len, const uint8_t* cipher, size_t len,
                   uint8_t tag[16]) {
    uint8_t block[16];
    uint32_t r[5], h[5] = {0, 0, 0, 0, 0};
    r[0] = load32_le(key) & 0x3ffffff;
    r[1] = (load32_le(key + 3) >> 2) & 0x3ffff03;
    r[2] = (load32_le(key + 6) >> 4) & 0x3ffc0ff;
    r[3] = (load32_le(key + 9) >> 6) & 0x3f03fff;
    r[4] = (load32_le(key + 12) >> 8) & 0x00fffff;

    // AEAD 的 MAC 数据：aad || pad16 || cipher || pad16 || len(aad) || len(cipher)。
    // pad16 用零补满（不是原始 Poly1305 的「末尾补 0x01」），每个块都带 2^128 位。
    auto feed = [&](const uint8_t* p, size_t n) {
        size_t off = 0;
        while (off < n) {
            const size_t chunk = (n - off) < 16 ? (n - off) : 16;
            if (chunk == 16) {
                poly1305_block(h, r, p + off, 1u << 24);
            } else {
                std::memset(block, 0, 16);
                std::memcpy(block, p + off, chunk);
                poly1305_block(h, r, block, 1u << 24);
            }
            off += chunk;
        }
    };

    feed(aad, aad_len);
    feed(cipher, len);

    uint8_t lens[16];
    uint64_t a = aad_len, c = len;
    for (int i = 0; i < 8; ++i) lens[i] = static_cast<uint8_t>(a >> (8 * i));
    for (int i = 0; i < 8; ++i) lens[8 + i] = static_cast<uint8_t>(c >> (8 * i));
    poly1305_block(h, r, lens, 1u << 24);

    // 最终模约简 + 加 pad
    uint32_t cc;
    cc = h[1] >> 26;
    h[1] &= 0x3ffffff;
    h[2] += cc;
    cc = h[2] >> 26;
    h[2] &= 0x3ffffff;
    h[3] += cc;
    cc = h[3] >> 26;
    h[3] &= 0x3ffffff;
    h[4] += cc;
    cc = h[4] >> 26;
    h[4] &= 0x3ffffff;
    h[0] += cc * 5;
    cc = h[0] >> 26;
    h[0] &= 0x3ffffff;
    h[1] += cc;

    uint32_t g0 = h[0] + 5;
    cc = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h[1] + cc;
    cc = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h[2] + cc;
    cc = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h[3] + cc;
    cc = g3 >> 26;
    g3 &= 0x3ffffff;
    uint32_t g4 = h[4] + cc - (1u << 26);

    const uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    const uint32_t nmask = ~mask;
    h[0] = (h[0] & nmask) | g0;
    h[1] = (h[1] & nmask) | g1;
    h[2] = (h[2] & nmask) | g2;
    h[3] = (h[3] & nmask) | g3;
    h[4] = (h[4] & nmask) | g4;

    const uint32_t p0 = load32_le(key + 16);
    const uint32_t p1 = load32_le(key + 20);
    const uint32_t p2 = load32_le(key + 24);
    const uint32_t p3 = load32_le(key + 28);

    // 先把 5 个 26 位 limb 拼成 4 个 32 位字（必须截断到 32 位），再加 pad
    uint64_t f0 = ((uint64_t(h[0]) | uint64_t(h[1]) << 26) & 0xffffffffULL) + p0;
    uint64_t f1 = ((uint64_t(h[1] >> 6) | uint64_t(h[2]) << 20) & 0xffffffffULL) + p1 + (f0 >> 32);
    uint64_t f2 = ((uint64_t(h[2] >> 12) | uint64_t(h[3]) << 14) & 0xffffffffULL) + p2 + (f1 >> 32);
    uint64_t f3 = ((uint64_t(h[3] >> 18) | uint64_t(h[4]) << 8) & 0xffffffffULL) + p3 + (f2 >> 32);

    store32_le(tag, static_cast<uint32_t>(f0));
    store32_le(tag + 4, static_cast<uint32_t>(f1));
    store32_le(tag + 8, static_cast<uint32_t>(f2));
    store32_le(tag + 12, static_cast<uint32_t>(f3));
}

bool constant_time_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

void derive_poly_key(const uint8_t key[32], const uint8_t nonce[12], uint8_t poly_key[32]) {
    uint8_t block[64];
    chacha20_block(key, nonce, 0, block);
    std::memcpy(poly_key, block, 32);
}

}  // namespace detail

// ------------------------------------------------------------------ 对外接口
using namespace detail;  // 内部原语

void random_bytes(void* out, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(out);
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) {
        const size_t got = std::fread(p, 1, n, f);
        std::fclose(f);
        if (got == n) return;
    }
    // 退化路径（几乎不可能发生）：基于时间与地址混合，至少不会全零
    uint64_t seed = static_cast<uint64_t>(std::time(nullptr)) ^ reinterpret_cast<uintptr_t>(p);
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = static_cast<uint8_t>(seed >> 33);
    }
}

KeyPair x25519_generate() {
    KeyPair kp;
    random_bytes(kp.priv, kKeySize);
    x25519_public_from_priv(kp.priv, kp.pub);
    return kp;
}

bool x25519_public_from_priv(const uint8_t priv[kKeySize], uint8_t pub[kPubSize]) {
    static const uint8_t kBase[32] = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    x25519_scalarmult(pub, priv, kBase);
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= pub[i];
    return acc != 0;
}

bool x25519_shared(const uint8_t priv[kKeySize], const uint8_t peer_pub[kPubSize], uint8_t out[kKeySize]) {
    x25519_scalarmult(out, priv, peer_pub);
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out[i];
    return acc != 0;  // 全零 = 低阶点，必须拒绝
}

void aead_encrypt(const uint8_t key[kKeySize], const uint8_t nonce[kNonceSize], const uint8_t* aad, size_t aad_len,
                  const uint8_t* plain, size_t plain_len, uint8_t* cipher, uint8_t tag[kTagSize]) {
    uint8_t poly_key[32];
    derive_poly_key(key, nonce, poly_key);
    chacha20_xor(key, nonce, 1, plain, plain_len, cipher);
    poly1305_auth(poly_key, aad, aad_len, cipher, plain_len, tag);
}

bool aead_decrypt(const uint8_t key[kKeySize], const uint8_t nonce[kNonceSize], const uint8_t* aad, size_t aad_len,
                  const uint8_t* cipher, size_t cipher_len, const uint8_t tag[kTagSize], uint8_t* plain) {
    uint8_t poly_key[32];
    derive_poly_key(key, nonce, poly_key);
    uint8_t expect[16];
    poly1305_auth(poly_key, aad, aad_len, cipher, cipher_len, expect);
    if (!constant_time_eq(expect, tag, 16)) return false;
    chacha20_xor(key, nonce, 1, cipher, cipher_len, plain);
    return true;
}

void hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len, const uint8_t* info,
                 size_t info_len, uint8_t* out, size_t out_len) {
    uint8_t prk[32];
    if (salt == nullptr || salt_len == 0) {
        uint8_t zero[32] = {0};
        hmac_sha256(zero, 32, ikm, ikm_len, prk);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }

    uint8_t t[32];
    size_t t_len = 0;
    size_t done = 0;
    uint8_t counter = 1;
    while (done < out_len) {
        uint8_t buf[32 + 64 + 1];
        size_t n = 0;
        if (t_len > 0) {
            std::memcpy(buf, t, t_len);
            n += t_len;
        }
        if (info_len > 64) info_len = 64;  // 我们的 info 都很短，这里做防御
        if (info_len > 0 && info != nullptr) {
            std::memcpy(buf + n, info, info_len);
            n += info_len;
        }
        buf[n++] = counter++;
        hmac_sha256(prk, 32, buf, n, t);
        t_len = 32;
        const size_t take = (out_len - done) < 32 ? (out_len - done) : 32;
        std::memcpy(out + done, t, take);
        done += take;
    }
}

std::string fingerprint(const uint8_t pub[kPubSize]) {
    static const char* hex = "0123456789abcdef";
    uint8_t digest[32];
    sha256_digest(pub, kPubSize, digest);
    std::string out;
    out.reserve(39);
    for (int i = 0; i < 16; ++i) {
        if (i > 0 && i % 2 == 0) out.push_back(':');
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0xF]);
    }
    return out;
}

bool derive_session_keys(const KeyPair& me, const KeyPair& my_eph, const uint8_t peer_pub[kPubSize],
                         const uint8_t peer_eph_pub[kPubSize], SessionKeys& out) {
    // 同一份身份密钥的副本互连时，两个方向的密钥会重合（计数器相同即 nonce 重用），直接拒绝
    if (std::memcmp(me.pub, peer_pub, kPubSize) == 0) return false;

    const bool low = std::memcmp(me.pub, peer_pub, kPubSize) < 0;  // 自己的身份公钥较小

    // 四段 DH，顺序按身份公钥字节序规范（否则两端的串接顺序会不一致）：
    //   身份×身份 ‖ 临时(小)×身份(大) ‖ 身份(小)×临时(大) ‖ 临时×临时
    // 任何一段失败（低阶点 → 共享密钥全零）都放弃。
    uint8_t secret[4 * kKeySize];
    if (!x25519_shared(me.priv, peer_pub, secret)) return false;
    if (low) {
        if (!x25519_shared(my_eph.priv, peer_pub, secret + kKeySize)) return false;
        if (!x25519_shared(me.priv, peer_eph_pub, secret + 2 * kKeySize)) return false;
    } else {
        if (!x25519_shared(me.priv, peer_eph_pub, secret + kKeySize)) return false;
        if (!x25519_shared(my_eph.priv, peer_pub, secret + 2 * kKeySize)) return false;
    }
    if (!x25519_shared(my_eph.priv, peer_eph_pub, secret + 3 * kKeySize)) return false;

    uint8_t salt[4 * kKeySize];
    std::memcpy(salt + 0, low ? me.pub : peer_pub, kPubSize);
    std::memcpy(salt + 32, low ? peer_pub : me.pub, kPubSize);
    std::memcpy(salt + 64, low ? my_eph.pub : peer_eph_pub, kPubSize);
    std::memcpy(salt + 96, low ? peer_eph_pub : my_eph.pub, kPubSize);

    uint8_t okm[2 * kKeySize];
    static const char kInfo[] = "chatt-p2p/v2";
    hkdf_sha256(salt, sizeof(salt), secret, sizeof(secret), reinterpret_cast<const uint8_t*>(kInfo),
                sizeof(kInfo) - 1, okm, sizeof(okm));

    std::memcpy(out.send, low ? okm : okm + kKeySize, kKeySize);
    std::memcpy(out.recv, low ? okm + kKeySize : okm, kKeySize);
    std::memset(secret, 0, sizeof(secret));
    std::memset(okm, 0, sizeof(okm));
    return true;
}

}  // namespace chat::crypto
