#include "term.h"

#include <cerrno>
#include <cstdio>
#include <sys/ioctl.h>
#include <unistd.h>

namespace chat {

TermSize term_size() {
    TermSize ts;
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) ts.rows = ws.ws_row;
        if (ws.ws_col > 0) ts.cols = ws.ws_col;
    }
    return ts;
}

void term_write(std::string_view s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;  // 终端已断开，无法再输出
    }
}

Status TerminalGuard::enter() {
    if (active_) return Status::success();
    if (::tcgetattr(STDIN_FILENO, &orig_) < 0) return status_errno("tcgetattr");

    termios raw = orig_;
    raw.c_iflag &= ~static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB);
    raw.c_cflag |= static_cast<tcflag_t>(CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return status_errno("tcsetattr");
    active_ = true;
    term_write("\x1b[?1049h\x1b[?25l\x1b[2J");
    return Status::success();
}

void TerminalGuard::leave() {
    if (!active_) return;
    active_ = false;
    term_write("\x1b[?25h\x1b[0m\x1b[?1049l");
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_);
}

Status read_secret_line(std::string& out) {
    termios orig{};
    if (::tcgetattr(STDIN_FILENO, &orig) < 0) return status_errno("tcgetattr");
    termios quiet = orig;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    quiet.c_lflag |= static_cast<tcflag_t>(ICANON);
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) < 0) return status_errno("tcsetattr");

    out.clear();
    char c = 0;
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (c == '\n' || c == '\r') break;
        if (c == 127 || c == 8) {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(c);
    }
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    std::fputc('\n', stdout);
    return Status::success();
}

}  // namespace chat
