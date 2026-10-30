// CRC32 (IEEE 802.3)。用于帧与存储记录校验。
#pragma once

#include <cstddef>
#include <cstdint>

namespace chat {

uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);

}  // namespace chat
