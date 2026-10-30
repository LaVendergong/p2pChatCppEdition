#include "editor.h"

#include "chat/utf8.h"

namespace chat {

void Editor::insert(std::string_view utf8) {
    if (utf8.empty()) return;
    text_.insert(cursor_, utf8.data(), utf8.size());
    cursor_ += utf8.size();
}

bool Editor::backspace() {
    if (cursor_ == 0) return false;
    const size_t prev = utf8::prev_boundary(text_, cursor_);
    text_.erase(prev, cursor_ - prev);
    cursor_ = prev;
    return true;
}

bool Editor::delete_forward() {
    if (cursor_ >= text_.size()) return false;
    const size_t next = utf8::next_boundary(text_, cursor_);
    text_.erase(cursor_, next - cursor_);
    return true;
}

void Editor::move_left() {
    if (cursor_ == 0) return;
    cursor_ = utf8::prev_boundary(text_, cursor_);
}

void Editor::move_right() {
    if (cursor_ >= text_.size()) return;
    cursor_ = utf8::next_boundary(text_, cursor_);
}

void Editor::move_home() { cursor_ = 0; }
void Editor::move_end() { cursor_ = text_.size(); }

void Editor::kill_to_start() {
    text_.erase(0, cursor_);
    cursor_ = 0;
}

void Editor::kill_to_end() { text_.erase(cursor_); }

void Editor::kill_word() {
    size_t p = cursor_;
    while (p > 0 && text_[p - 1] == ' ') --p;
    while (p > 0 && text_[p - 1] != ' ') --p;
    text_.erase(p, cursor_ - p);
    cursor_ = p;
}

void Editor::clear() {
    text_.clear();
    cursor_ = 0;
    reset_nav();
}

void Editor::history_push(std::string line) {
    if (line.empty()) return;
    if (!history_.empty() && history_.back() == line) return;
    history_.push_back(std::move(line));
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());
    reset_nav();
}

void Editor::reset_nav() {
    hist_ = -1;
    stash_.clear();
}

bool Editor::history_prev() {
    if (history_.empty()) return false;
    if (hist_ == -1) {
        stash_ = text_;
        hist_ = static_cast<int>(history_.size()) - 1;
    } else if (hist_ > 0) {
        --hist_;
    } else {
        return false;
    }
    text_ = history_[static_cast<size_t>(hist_)];
    cursor_ = text_.size();
    return true;
}

bool Editor::history_next() {
    if (hist_ == -1) return false;
    if (hist_ + 1 >= static_cast<int>(history_.size())) {
        hist_ = -1;
        text_ = stash_;
        cursor_ = text_.size();
        return true;
    }
    ++hist_;
    text_ = history_[static_cast<size_t>(hist_)];
    cursor_ = text_.size();
    return true;
}

}  // namespace chat
