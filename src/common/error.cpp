#include "chat/error.h"

#include <cerrno>
#include <cstring>

namespace chat {

const char* err_name(Err e) {
    switch (e) {
        case Err::ok: return "ok";
        case Err::bad_frame: return "bad_frame";
        case Err::bad_version: return "bad_version";
        case Err::unauthenticated: return "unauthenticated";
        case Err::bad_credentials: return "bad_credentials";
        case Err::user_exists: return "user_exists";
        case Err::room_not_found: return "room_not_found";
        case Err::not_a_member: return "not_a_member";
        case Err::rate_limited: return "rate_limited";
        case Err::payload_too_large: return "payload_too_large";
        case Err::text_too_long: return "text_too_long";
        case Err::internal: return "internal";
        case Err::storage_error: return "storage_error";
        case Err::timeout: return "timeout";
        case Err::unsupported: return "unsupported";
        case Err::invalid_arg: return "invalid_arg";
        case Err::sys_errno: return "sys_errno";
        case Err::eof: return "eof";
        case Err::would_block: return "would_block";
    }
    return "unknown";
}

std::string err_text(Err e, const std::string& msg) {
    std::string s = err_name(e);
    if (!msg.empty()) {
        s += ": ";
        s += msg;
    }
    return s;
}

Status status_errno(const char* what) {
    const int e = errno;
    std::string m = what ? what : "";
    m += ": ";
    m += std::strerror(e);
    // EAGAIN/EWOULDBLOCK 单独归类，调用方通常要区别处理
    if (e == EAGAIN || e == EWOULDBLOCK) return Status(Err::would_block, std::move(m));
    return Status(Err::sys_errno, std::move(m));
}

}  // namespace chat
