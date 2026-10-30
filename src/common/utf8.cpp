#include "chat/utf8.h"

namespace chat::utf8 {

size_t decode(const char* s, size_t n, uint32_t& cp) {
    if (n == 0) {
        cp = 0xFFFD;
        return 0;
    }
    const uint8_t c0 = static_cast<uint8_t>(s[0]);
    if (c0 < 0x80) {
        cp = c0;
        return 1;
    }
    int need = 0;
    uint32_t v = 0;
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        v = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        v = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        v = c0 & 0x07;
    } else {
        cp = 0xFFFD;
        return 1;
    }
    if (n < size_t(need + 1)) {
        cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i <= need; ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if ((c & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (c & 0x3F);
    }
    cp = v;
    return size_t(need + 1);
}

void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

size_t prev_boundary(std::string_view s, size_t pos) {
    if (pos == 0) return 0;
    size_t p = pos - 1;
    // 一个字符最多 4 字节，最多回退 3 次
    int guard = 4;
    while (p > 0 && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80 && --guard > 0) --p;
    return p;
}

size_t next_boundary(std::string_view s, size_t pos) {
    if (pos >= s.size()) return s.size();
    size_t p = pos + 1;
    int guard = 4;
    while (p < s.size() && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80 && --guard > 0) ++p;
    return p;
}

int char_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (cp < 0x7F) return 1;
    // 组合字符
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    // 宽字符（东亚宽 + 常见 emoji）
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

int string_width(std::string_view s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t used = decode(s.data() + i, s.size() - i, cp);
        if (used == 0) break;
        i += used;
        w += char_width(cp);
    }
    return w;
}

bool valid(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t used = decode(s.data() + i, s.size() - i, cp);
        if (used == 0) return false;
        if (cp == 0xFFFD && used == 1 && static_cast<uint8_t>(s[i]) >= 0x80) return false;
        i += used;
    }
    return true;
}

std::string sanitize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B) {  // ESC: CSI / OSC / 其它两字节
            ++i;
            if (i < s.size() && s[i] == '[') {
                ++i;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
                if (i < s.size()) ++i;
            } else if (i < s.size() && s[i] == ']') {
                ++i;
                while (i < s.size() && s[i] != '\x07') {
                    if (s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    ++i;
                }
                if (i < s.size() && s[i] == '\x07') ++i;
            } else {
                if (i < s.size()) ++i;
            }
            continue;
        }
        if (c < 0x20 && c != '\t') {  // 其它 C0 控制字符（含 \r \n）丢弃
            ++i;
            continue;
        }
        if (c == 0x7F) {
            ++i;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

std::string truncate_width(std::string_view s, int max_width) {
    if (max_width <= 0) return {};
    std::string out;
    out.reserve(s.size());
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t used = decode(s.data() + i, s.size() - i, cp);
        if (used == 0) break;
        int cw = char_width(cp);
        if (w + cw > max_width) break;
        out.append(s.data() + i, used);
        w += cw;
        i += used;
    }
    return out;
}

}  // namespace chat::utf8
