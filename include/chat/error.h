// 统一错误码与状态返回。业务代码不抛异常，跨层传递用 Status。
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace chat {

// 数值与协议错误码一一对应（见 docs/03 §6），便于直接放入 ERROR 帧。
enum class Err : uint16_t {
    ok = 0,
    bad_frame = 1,
    bad_version = 2,
    unauthenticated = 3,
    bad_credentials = 4,
    user_exists = 5,
    room_not_found = 6,
    not_a_member = 7,
    rate_limited = 8,
    payload_too_large = 9,
    text_too_long = 10,
    internal = 11,
    storage_error = 12,
    timeout = 13,
    unsupported = 14,
    // 以下仅本地使用，不上协议
    invalid_arg = 100,
    sys_errno = 101,
    eof = 102,
    would_block = 103,
};

const char* err_name(Err e);

struct Status {
    Err e = Err::ok;
    std::string msg;

    Status() = default;
    Status(Err e_, std::string m = {}) : e(e_), msg(std::move(m)) {}

    explicit operator bool() const { return e == Err::ok; }
    bool ok() const { return e == Err::ok; }

    static Status success() { return Status(); }
    static Status fail(Err e_, std::string m = {}) { return Status(e_, std::move(m)); }
};

// 错误日志用的可读串，含 errno 文本
std::string err_text(Err e, const std::string& msg);

// 便捷：从 errno 造 Status
Status status_errno(const char* what);

}  // namespace chat

// 传播错误：失败时立即返回该 Status
#define CHAT_TRY(expr)                    \
    do {                                  \
        ::chat::Status _chat_s = (expr);  \
        if (!_chat_s) return _chat_s;     \
    } while (0)
