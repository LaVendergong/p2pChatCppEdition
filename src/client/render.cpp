#include "render.h"

#include "chat/utf8.h"
#include "term.h"

namespace chat {

void Renderer::resize(int rows, int cols) {
    if (rows == rows_ && cols == cols_) return;
    rows_ = rows;
    cols_ = cols;
    prev_.clear();
    term_write("\x1b[2J");
}

void Renderer::draw(const std::vector<std::string>& lines, int cursor_row, int cursor_col) {
    if (rows_ <= 0 || cols_ <= 0) return;
    if (static_cast<int>(prev_.size()) != rows_) prev_.assign(static_cast<size_t>(rows_), "");

    std::string out;
    out.reserve(4096);
    for (int i = 0; i < rows_; ++i) {
        const std::string line = (i < static_cast<int>(lines.size())) ? lines[static_cast<size_t>(i)] : std::string();
        if (prev_[static_cast<size_t>(i)] == line) continue;
        out.append("\x1b[");
        out.append(std::to_string(i + 1));
        out.append(";1H");
        out.append(line);
        out.append("\x1b[K");
        prev_[static_cast<size_t>(i)] = line;
    }
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= rows_) cursor_row = rows_ - 1;
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col >= cols_) cursor_col = cols_ - 1;
    out.append("\x1b[");
    out.append(std::to_string(cursor_row + 1));
    out.append(";");
    out.append(std::to_string(cursor_col + 1));
    out.append("H\x1b[?25h");
    // 必须无条件输出：即便只有光标移动，也要让终端跟上内部状态。
    // （早先在这里按长度阈值跳过，导致 prev_ 已更新但屏幕没更新，之后再也不重绘该行）
    term_write(out);
}

std::vector<std::string> wrap_text(const std::string& first_prefix, const std::string& cont_indent,
                                   const std::string& text, int width) {
    std::vector<std::string> out;
    if (width < 8) width = 8;
    std::string cur = first_prefix;
    int used = utf8::string_width(cur);
    bool first = true;

    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t n = utf8::decode(text.data() + i, text.size() - i, cp);
        if (n == 0) break;
        if (cp == '\n') {
            out.push_back(cur);
            cur = cont_indent;
            used = utf8::string_width(cur);
            i += n;
            continue;
        }
        const int w = utf8::char_width(cp);
        if (used + w > width) {
            out.push_back(cur);
            cur = cont_indent;
            used = utf8::string_width(cur);
            if (used + w > width) {  // 缩进本身就超宽，放弃缩进
                cur.clear();
                used = 0;
            }
        }
        cur.append(text, i, n);
        used += w;
        i += n;
        first = false;
    }
    (void)first;
    if (!cur.empty()) out.push_back(cur);
    if (out.empty()) out.push_back(first_prefix);
    return out;
}

}  // namespace chat
