#include "commands.h"

#include <algorithm>
#include <cctype>

namespace chat {
namespace {

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool known(const std::string& name) {
    static const char* kNames[] = {"/help", "/join", "/leave", "/rooms", "/users", "/history", "/quit", "/clear"};
    for (const char* n : kNames) {
        if (name == n + 1) return true;
    }
    return false;
}

}  // namespace

ParsedCommand parse_command(std::string_view line) {
    ParsedCommand pc;
    if (line.empty() || line[0] != '/') return pc;
    pc.is_command = true;

    const size_t sp = line.find(' ');
    if (sp == std::string_view::npos) {
        pc.name = std::string(line.substr(1));
        pc.arg.clear();
    } else {
        pc.name = std::string(line.substr(1, sp - 1));
        pc.arg = trim(line.substr(sp + 1));
    }
    std::transform(pc.name.begin(), pc.name.end(), pc.name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    pc.ok = known(pc.name);
    return pc;
}

const char* commands_help() {
    return "/help            显示帮助\n"
           "/join <room>     加入房间（不存在则创建）\n"
           "/leave [room]    离开房间（默认当前）\n"
           "/rooms           列出服务端房间\n"
           "/users           列出当前房间在线用户\n"
           "/history [n]     拉取更早的 n 条（默认 50）\n"
           "/clear           清空本地消息显示\n"
           "/quit            退出\n"
           "快捷键: PgUp/PgDn 翻页, Ctrl-P/N 上下条, Ctrl-U/K 删行, Ctrl-L 重绘\n"
           "        Ctrl-R 重发最近一条失败消息（行尾带 ! 的）, Ctrl-C 清空/退出";
}

}  // namespace chat
