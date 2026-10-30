// 手写密码学原语：X25519（RFC 7748）、ChaCha20-Poly1305（RFC 8439）、HKDF-SHA256（RFC 5869）。
// 只依赖 C++ 标准库与 /dev/urandom；正确性由 tests/test_crypto.cpp 里的官方测试向量保证。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat::crypto {

constexpr size_t kKeySize = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kPubSize = 32;

struct KeyPair {
    uint8_t priv[kKeySize];
    uint8_t pub[kPubSize];
};

void random_bytes(void* out, size_t n);

KeyPair x25519_generate();
bool x25519_public_from_priv(const uint8_t priv[kKeySize], uint8_t pub[kPubSize]);
// 返回 false 表示结果是全零（低阶点攻击），调用方必须拒绝
bool x25519_shared(const uint8_t priv[kKeySize], const uint8_t peer_pub[kPubSize], uint8_t out[kKeySize]);

void aead_encrypt(const uint8_t key[kKeySize], const uint8_t nonce[kNonceSize], const uint8_t* aad, size_t aad_len,
                  const uint8_t* plain, size_t plain_len, uint8_t* cipher, uint8_t tag[kTagSize]);

// 认证失败返回 false，plain 内容不可用
bool aead_decrypt(const uint8_t key[kKeySize], const uint8_t nonce[kNonceSize], const uint8_t* aad, size_t aad_len,
                  const uint8_t* cipher, size_t cipher_len, const uint8_t tag[kTagSize], uint8_t* plain);

void hkdf_sha256(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len, const uint8_t* info,
                 size_t info_len, uint8_t* out, size_t out_len);

// SHA-256(公钥) 前 16 字节，转成 32 个 hex 字符，每 4 个一组（XKCD-925 风格，便于人眼比对）
std::string fingerprint(const uint8_t pub[kPubSize]);

// ------------------------------------------------------------------ 会话密钥
// 双向会话密钥：send 用于自己发出的消息，recv 用于对方发来的消息
struct SessionKeys {
    uint8_t send[kKeySize];
    uint8_t recv[kKeySize];
};

// 由「长期身份密钥 + 本次连接的一次性临时密钥」派生会话密钥（四重 DH）：
//   secret = DH(me,peer) || DH(my_eph,peer) || DH(me,peer_eph) || DH(my_eph,peer_eph)
//   salt   = min(pub) || max(pub) || eph(公钥较小方) || eph(公钥较大方)
//   okm    = HKDF-SHA256(salt, secret, "chatt-p2p/v2", 64)
// 三点性质：
//   · 认证——没有对方身份私钥就算不出这四段（指纹对得上时不可能被冒充）；
//   · 前向保密——临时私钥用完即弃，事后再拿到身份私钥也解不开这次会话；
//   · 方向由公钥字节序决定（较小方用 okm 前半段发送），直连和中继都不需要协商角色。
// 返回 false：任一对端公钥是低阶点（共享密钥全零），或双方身份公钥相同。
bool derive_session_keys(const KeyPair& me, const KeyPair& my_eph, const uint8_t peer_pub[kPubSize],
                         const uint8_t peer_eph_pub[kPubSize], SessionKeys& out);

// 内部原语，仅供测试逐层验证域运算；业务代码不要用。
namespace detail {

// 4 × 64 位 limb，小端，值恒 < 2^255
using fe = uint64_t[4];

void fe_zero(fe h);
void fe_one(fe h);
void fe_mul(fe h, const fe f, const fe g);
void fe_invert(fe out, const fe z);
void fe_frombytes(fe h, const uint8_t s[32]);
void fe_tobytes(uint8_t s[32], const fe h);

}  // namespace detail

}  // namespace chat::crypto
