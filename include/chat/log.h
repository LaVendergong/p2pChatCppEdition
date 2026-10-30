// 分级日志：前台只做格式化，落盘与滚动交给后台线程。
#pragma once

#include <string>
#include <string_view>

#include "chat/fmt.h"

namespace chat {

enum class LogLevel : int { trace = 0, debug = 1, info = 2, warn = 3, error = 4, off = 5 };

const char* log_level_name(LogLevel l);
bool parse_log_level(std::string_view s, LogLevel& out);

// path 为空表示只输出到 stderr。创建失败不致命，自动降级为 stderr。
void log_init(bool to_stderr, const std::string& path, LogLevel level);
void log_set_level(LogLevel level);
LogLevel log_level();
void log_shutdown();

void log_write(LogLevel l, const char* file, int line, std::string_view msg);

// 全部日志线程安全；参数只在低于当前级别时才会被求值（宏里先判断）
#define CHAT_LOG(lvl, ...)                                                        \
    do {                                                                          \
        if (::chat::log_level() <= (lvl))                                          \
            ::chat::log_write((lvl), __FILE__, __LINE__, ::chat::fmt(__VA_ARGS__)); \
    } while (0)

#define LOG_TRACE(...) CHAT_LOG(::chat::LogLevel::trace, __VA_ARGS__)
#define LOG_DEBUG(...) CHAT_LOG(::chat::LogLevel::debug, __VA_ARGS__)
#define LOG_INFO(...) CHAT_LOG(::chat::LogLevel::info, __VA_ARGS__)
#define LOG_WARN(...) CHAT_LOG(::chat::LogLevel::warn, __VA_ARGS__)
#define LOG_ERROR(...) CHAT_LOG(::chat::LogLevel::error, __VA_ARGS__)

}  // namespace chat
