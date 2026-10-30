// 密码学原语的官方测试向量（RFC 7748 / RFC 8439 / RFC 5869）。
// 这些用例是安全性的唯一保证——自己实现的密码学必须逐字节对得上标准。
#include <string>
#include <vector>

#include "chat/crypto.h"
#include "chat/sha256.h"
#include "test_framework.h"

using namespace chat;
using namespace chat::crypto;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(nib(s[i]) << 4 | nib(s[i + 1])));
    }
    return out;
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

template <size_t N>
std::string hex_of(const uint8_t (&a)[N]) {
    return to_hex(a, N);
}

}  // namespace

// 域运算逐层验证：X25519 对不上向量时，这里能定位到 mul / invert
TEST_CASE(fe_field_arithmetic) {
    using namespace chat::crypto::detail;
    uint8_t out[32];
    fe a, b, r;

    // 2 * 3 = 6（fe_tobytes 输出小端编码）
    fe_zero(a);
    a[0] = 2;
    fe_zero(b);
    b[0] = 3;
    fe_mul(r, a, b);
    fe_tobytes(out, r);
    CHECK_EQ(to_hex(out, 32), std::string("0600000000000000000000000000000000000000000000000000000000000000"));

    // 2^-1 = (p+1)/2 = 2^254 - 9
    fe_invert(r, a);
    fe_tobytes(out, r);
    CHECK_EQ(to_hex(out, 32),
             std::string("f7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff3f"));

    // z * z^-1 = 1（跨多个 limb 的任意值）
    fe z;
    fe_zero(z);
    z[0] = 0xfedcba9876543210ULL;
    z[1] = 0x0123456789abcdefULL;
    z[3] = 0x0000000012345678ULL;
    fe_invert(r, z);
    fe_mul(r, z, r);
    fe_tobytes(out, r);
    CHECK_EQ(to_hex(out, 32), std::string("0100000000000000000000000000000000000000000000000000000000000000"));
}

// RFC 7748 §5.2 向量 1
TEST_CASE(x25519_rfc7748_vector1) {
    const auto scalar = from_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    const auto point = from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    const std::string expect = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";

    uint8_t out[32];
    CHECK(x25519_shared(scalar.data(), point.data(), out));
    CHECK_EQ(to_hex(out, 32), expect);
}

// RFC 7748 §6.1：Alice/Bob 公钥与共享密钥
TEST_CASE(x25519_rfc7748_dh) {
    const auto alice_priv = from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob_priv = from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    uint8_t alice_pub[32], bob_pub[32];
    CHECK(x25519_public_from_priv(alice_priv.data(), alice_pub));
    CHECK(x25519_public_from_priv(bob_priv.data(), bob_pub));
    CHECK_EQ(to_hex(alice_pub, 32), std::string("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK_EQ(to_hex(bob_pub, 32), std::string("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));

    uint8_t s1[32], s2[32];
    CHECK(x25519_shared(alice_priv.data(), bob_pub, s1));
    CHECK(x25519_shared(bob_priv.data(), alice_pub, s2));
    const std::string shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
    CHECK_EQ(to_hex(s1, 32), shared);
    CHECK_EQ(to_hex(s2, 32), shared);
}

TEST_CASE(x25519_rejects_low_order_point) {
    // 全零点是非法的：共享密钥会变成全零
    const auto priv = from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    uint8_t zero[32] = {0};
    uint8_t out[32];
    CHECK(!x25519_shared(priv.data(), zero, out));
}

// RFC 8439 §2.8.2：完整 AEAD 向量
TEST_CASE(chacha20poly1305_rfc8439) {
    const auto key = from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto nonce = from_hex("070000004041424344454647");
    const auto aad = from_hex("50515253c0c1c2c3c4c5c6c7");
    const std::string plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen "
        "would be it.";
    const std::string expect_cipher =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
        "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7b"
        "c3ff4def08e4b7a9de576d26586cec64b6116";
    const std::string expect_tag = "1ae10b594f09e26a7e902ecbd0600691";

    std::vector<uint8_t> cipher(plain.size());
    uint8_t tag[16];
    aead_encrypt(key.data(), nonce.data(), aad.data(), aad.size(),
                 reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), cipher.data(), tag);

    CHECK_EQ(to_hex(cipher.data(), cipher.size()), expect_cipher);
    CHECK_EQ(to_hex(tag, 16), expect_tag);

    std::string back(plain.size(), '\0');
    CHECK(aead_decrypt(key.data(), nonce.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag,
                       reinterpret_cast<uint8_t*>(&back[0])));
    CHECK_EQ(back, plain);

    // 篡改密文必须被拒绝
    cipher[0] ^= 0x01;
    CHECK(!aead_decrypt(key.data(), nonce.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag,
                        reinterpret_cast<uint8_t*>(&back[0])));
    cipher[0] ^= 0x01;

    // 篡改 tag 必须被拒绝
    tag[0] ^= 0x01;
    CHECK(!aead_decrypt(key.data(), nonce.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag,
                        reinterpret_cast<uint8_t*>(&back[0])));
}

// RFC 8439 的向量里 AAD 都是 12 字节，覆盖不到 pad16 的「已对齐」和「空」分支。
// 下面的 tag 由独立的整数版 Poly1305 交叉计算得出（该实现已用上面的 RFC 向量验证过）。
TEST_CASE(aead_pad16_branches) {
    const auto key = from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto nonce = from_hex("070000004041424344454647");

    // 空 AAD + 3 字节明文
    {
        const std::string plain = "abc";
        const auto aad = from_hex("");
        std::vector<uint8_t> cipher(plain.size());
        uint8_t tag[16];
        aead_encrypt(key.data(), nonce.data(), aad.data(), aad.size(),
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), cipher.data(), tag);
        CHECK_EQ(to_hex(cipher.data(), cipher.size()), std::string("fe198a"));
        CHECK_EQ(to_hex(tag, 16), std::string("ee33b7c3cf0742d9c56155b194616080"));
    }

    // AAD 与明文都正好 16 字节（pad16 为空）
    {
        const std::string plain = "0123456789abcdef";
        const auto aad = from_hex("30313233343536373839616263646566");  // "0123456789abcdef"
        std::vector<uint8_t> cipher(plain.size());
        uint8_t tag[16];
        aead_encrypt(key.data(), nonce.data(), aad.data(), aad.size(),
                     reinterpret_cast<const uint8_t*>(plain.data()), plain.size(), cipher.data(), tag);
        CHECK_EQ(to_hex(cipher.data(), cipher.size()), std::string("af4adb6e35c8768d2ddbee9955e56fc8"));
        CHECK_EQ(to_hex(tag, 16), std::string("887241a725a7121902fcf5e2cd246062"));
    }

    // 明文为空
    {
        const auto aad = from_hex("");
        uint8_t tag[16];
        aead_encrypt(key.data(), nonce.data(), aad.data(), aad.size(), nullptr, 0, nullptr, tag);
        CHECK_EQ(to_hex(tag, 16), std::string("a0784d7a4716f3feb4f64e7f4b39bf04"));
    }
}

// RFC 5869 Test Case 1
TEST_CASE(hkdf_rfc5869_case1) {
    const auto ikm = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const auto salt = from_hex("000102030405060708090a0b0c");
    const auto info = from_hex("f0f1f2f3f4f5f6f7f8f9");
    uint8_t out[42];
    hkdf_sha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), out, sizeof(out));
    CHECK_EQ(to_hex(out, 42),
             std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
}

TEST_CASE(sha256_and_hmac_vectors) {
    uint8_t d[32];
    sha256_digest("abc", 3, d);
    CHECK_EQ(to_hex(d, 32), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // RFC 4231 Test Case 2
    const auto key = from_hex("4a656665");  // "Jefe"
    const std::string data = "what do ya want for nothing?";
    uint8_t mac[32];
    hmac_sha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(data.data()), data.size(), mac);
    CHECK_EQ(to_hex(mac, 32), std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

TEST_CASE(fingerprint_shape) {
    uint8_t pub[32] = {0};
    for (int i = 0; i < 32; ++i) pub[i] = static_cast<uint8_t>(i);
    const std::string fp = fingerprint(pub);
    // 32 hex + 7 个分隔符 = 39
    CHECK_EQ(fp.size(), 39u);
    CHECK(fp.find(':') != std::string::npos);

    uint8_t pub2[32] = {0};
    for (int i = 0; i < 32; ++i) pub2[i] = static_cast<uint8_t>(i + 1);
    CHECK(fingerprint(pub2) != fp);
}

TEST_CASE(x25519_generate_unique) {
    KeyPair a = x25519_generate();
    KeyPair b = x25519_generate();
    CHECK(to_hex(a.pub, 32) != to_hex(b.pub, 32));
    CHECK(to_hex(a.priv, 32) != to_hex(b.priv, 32));
}

// 会话密钥派生：双方独立计算必须得到互补的 send/recv
TEST_CASE(session_keys_agree) {
    KeyPair a = x25519_generate();   // 身份密钥
    KeyPair b = x25519_generate();
    KeyPair ea = x25519_generate();  // 本次连接的临时密钥
    KeyPair eb = x25519_generate();

    SessionKeys ka{}, kb{};
    CHECK(derive_session_keys(a, ea, b.pub, eb.pub, ka));
    CHECK(derive_session_keys(b, eb, a.pub, ea.pub, kb));
    CHECK_EQ(to_hex(ka.send, 32), to_hex(kb.recv, 32));
    CHECK_EQ(to_hex(ka.recv, 32), to_hex(kb.send, 32));
    CHECK(to_hex(ka.send, 32) != to_hex(ka.recv, 32));

    // 同样的输入必须可复现
    SessionKeys ka_again{};
    CHECK(derive_session_keys(a, ea, b.pub, eb.pub, ka_again));
    CHECK_EQ(to_hex(ka_again.send, 32), to_hex(ka.send, 32));

    // 换一把临时密钥（相当于下一次连接）→ 会话密钥必须不同（前向保密的前提）
    KeyPair ea2 = x25519_generate();
    SessionKeys ka2{};
    CHECK(derive_session_keys(a, ea2, b.pub, eb.pub, ka2));
    CHECK(to_hex(ka2.send, 32) != to_hex(ka.send, 32));

    // 低阶点（全零）必须拒绝：身份公钥、临时公钥都算
    uint8_t zero[32] = {0};
    SessionKeys bad{};
    CHECK(!derive_session_keys(a, ea, zero, eb.pub, bad));
    CHECK(!derive_session_keys(a, ea, b.pub, zero, bad));
    // 双方用同一份身份密钥（拷贝）必须拒绝
    CHECK(!derive_session_keys(a, ea, a.pub, eb.pub, bad));

    // 派生出来的密钥真的能互相解开：a 用 send 加密，b 用 recv 解密
    const auto key = from_hex(to_hex(ka.send, 32));
    const auto nonce = from_hex("000000000100000000000000");
    const std::string text = "端到端加密 hello";
    std::vector<uint8_t> cipher(text.size());
    uint8_t tag[16];
    aead_encrypt(key.data(), nonce.data(), nullptr, 0, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                 cipher.data(), tag);
    std::string back(text.size(), '\0');
    CHECK(aead_decrypt(kb.recv, nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag,
                       reinterpret_cast<uint8_t*>(&back[0])));
    CHECK_EQ(back, text);
}
