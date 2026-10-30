// 终端控制：raw mode、备用屏幕、窗口尺寸、原样输出。
#pragma once

#include <string>
#include <string_view>
#include <termios.h>

#include "chat/error.h"

namespace chat {

struct TermSize {
    int rows = 24;
    int cols = 80;
};

TermSize term_size();

// 一次性写入 stdout（处理 EINTR 与部分写）
void term_write(std::string_view s);

// 进入 raw mode + 备用屏幕；析构/leave 一定还原，避免把用户终端留在坏状态
class TerminalGuard {
public:
    Status enter();
    void leave();
    bool active() const { return active_; }
    ~TerminalGuard() { leave(); }

private:
    termios orig_{};
    bool active_ = false;
};

// 关闭回显读取一行（用于输入密码），返回去掉换行的内容
Status read_secret_line(std::string& out);

}  // namespace chat
