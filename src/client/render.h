// 屏幕渲染：行级 diff，只重绘变化的行。
#pragma once

#include <string>
#include <vector>

namespace chat {

class Renderer {
public:
    void resize(int rows, int cols);
    // lines 已经是"不超过 cols 显示宽度"的最终内容（可含 ANSI 颜色）
    void draw(const std::vector<std::string>& lines, int cursor_row, int cursor_col);
    void invalidate() { prev_.clear(); }
    int rows() const { return rows_; }
    int cols() const { return cols_; }

private:
    int rows_ = 0;
    int cols_ = 0;
    std::vector<std::string> prev_;
};

// 把一段文本按显示宽度切成多行；首行前缀 prefix，续行用 indent
std::vector<std::string> wrap_text(const std::string& first_prefix, const std::string& cont_indent,
                                   const std::string& text, int width);

}  // namespace chat
