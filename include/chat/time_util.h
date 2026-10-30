// 时间工具：消息排序只依赖 msg_id，时间戳仅用于展示与超时判断。
#pragma once

#include <cstdint>
#include <string>

namespace chat {

// Unix 毫秒（墙钟，会受校时影响）
uint64_t now_ms();

// 单调毫秒（超时计算用它，不受校时影响）
uint64_t steady_ms();

// 本地时间 "14:32:05"
std::string fmt_clock(uint64_t wall_ms);

// 本地时间 "2026-10-28 14:32:05"
std::string fmt_datetime(uint64_t wall_ms);

}  // namespace chat
