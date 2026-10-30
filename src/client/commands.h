// 客户端斜杠命令解析（只做解析，具体动作由 App 执行）。
#pragma once

#include <string>
#include <string_view>

namespace chat {

struct ParsedCommand {
    bool is_command = false;
    std::string name;   // 不含 '/'
    std::string arg;    // 第一个空格之后的全部内容（已 trim）
    bool ok = true;     // 命令名是否合法
};

ParsedCommand parse_command(std::string_view line);

const char* commands_help();

}  // namespace chat
