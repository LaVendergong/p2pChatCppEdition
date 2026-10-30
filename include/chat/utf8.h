// UTF-8 与终端宽度工具。客户端编辑器与渲染器共用。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace chat::utf8 {

// 解码一个码点，返回消耗字节数（非法序列返回 1，cp = 0xFFFD）
size_t decode(const char* s, size_t n, uint32_t& cp);

// 编码一个码点到 out
void encode(uint32_t cp, std::string& out);

inline std::string encode(uint32_t cp) {
    std::string s;
    encode(cp, s);
    return s;
}

// 从 pos 向前/向后找到字符边界
size_t prev_boundary(std::string_view s, size_t pos);
size_t next_boundary(std::string_view s, size_t pos);

// 终端显示宽度：0/1/2，组合字符返回 0
int char_width(uint32_t cp);

// 字符串显示宽度（不含 ANSI）
int string_width(std::string_view s);

bool valid(std::string_view s);

// 剥离 ANSI 转义序列与控制字符（\r \n \t 之外的 C0），防终端注入
std::string sanitize(std::string_view s);

// 按显示宽度截断，不切断多字节序列
std::string truncate_width(std::string_view s, int max_width);

}  // namespace chat::utf8
