// 极简格式化：只支持 {} 占位符，够日志用，不引入 fmt 库。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace chat {

inline void fmt_append(std::string& out, std::string_view v, int = 0) { out.append(v.data(), v.size()); }
inline void fmt_append(std::string& out, const std::string& v, int = 0) { out.append(v); }
inline void fmt_append(std::string& out, const char* v, int = 0) {
    if (v) out.append(v);
    else out.append("(null)");
}
inline void fmt_append(std::string& out, char v, int = 0) { out.push_back(v); }
inline void fmt_append(std::string& out, bool v, int = 0) { out.append(v ? "true" : "false"); }
inline void fmt_append(std::string& out, float v, int = 0) { out.append(std::to_string(v)); }
inline void fmt_append(std::string& out, double v, int = 0) { out.append(std::to_string(v)); }

template <typename T>
inline void fmt_append(std::string& out, T v, int = 0, std::enable_if_t<std::is_integral_v<T>>* = nullptr) {
    if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>) {
        out.append(std::to_string(static_cast<int>(v)));
    } else if constexpr (std::is_signed_v<T>) {
        out.append(std::to_string(static_cast<long long>(v)));
    } else {
        out.append(std::to_string(static_cast<unsigned long long>(v)));
    }
}

template <typename... Args>
std::string fmt(std::string_view f, const Args&... args) {
    std::string out;
    out.reserve(f.size() + 32);
    if constexpr (sizeof...(Args) == 0) {
        out.append(f);
    } else {
        size_t pos = 0;
        // 逐个替换；参数用完后剩余 {} 原样保留
        size_t n = 0;
        auto repl = [&](const auto& v) {
            size_t p = f.find("{}", pos);
            if (p == std::string_view::npos) return;
            out.append(f.substr(pos, p - pos));
            fmt_append(out, v);
            pos = p + 2;
            ++n;
        };
        (repl(args), ...);
        out.append(f.substr(pos));
    }
    return out;
}

}  // namespace chat
