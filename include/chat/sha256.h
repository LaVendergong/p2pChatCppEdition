// SHA-256 与 HMAC-SHA256：用于口令摘要、HKDF 与指纹。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace chat {

// 原始 32 字节摘要
void sha256_digest(const void* data, size_t len, uint8_t out[32]);

std::string sha256_hex(std::string_view data);

// 16 字节随机盐的十六进制（来自 /dev/urandom）
std::string random_salt_hex();

// 计算 sha256(salt + ":" + password) 的十六进制
std::string password_digest(std::string_view salt_hex, std::string_view password);

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]);

}  // namespace chat
