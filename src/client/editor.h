// 行编辑器：按 UTF-8 字符边界移动，支持历史。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

class Editor {
public:
    void insert(std::string_view utf8);
    bool backspace();
    bool delete_forward();
    void move_left();
    void move_right();
    void move_home();
    void move_end();
    void kill_to_start();
    void kill_to_end();
    void kill_word();
    void clear();

    const std::string& text() const { return text_; }
    size_t cursor() const { return cursor_; }

    // 历史：push 时去重连续相同项
    void history_push(std::string line);
    bool history_prev();
    bool history_next();
    void reset_nav();
    size_t history_size() const { return history_.size(); }

private:
    std::string text_;
    size_t cursor_ = 0;
    std::vector<std::string> history_;
    int hist_ = -1;
    std::string stash_;
    static constexpr size_t kMaxHistory = 200;
};

}  // namespace chat
