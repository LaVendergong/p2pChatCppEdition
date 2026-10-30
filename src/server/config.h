// 服务端配置：命令行优先，缺省用默认值。
#pragma once

#include <cstdint>
#include <string>

namespace chat {

struct Config {
    std::string bind_addr = "0.0.0.0";
    uint16_t port = 7700;
    std::string data_dir = "./data";
    int threads = 0;  // 0 = CPU 核数
    std::string log_level = "info";
    bool log_console = true;
    bool stats = false;
    bool repair = false;
    std::string sync_mode = "batch";  // batch | always | never
    uint32_t segment_mb = 64;
    bool allow_auto_register = true;
    int max_conns = 5000;
    int rate_limit = 20;       // 每连接每秒最大帧数，0 = 不限（压测用）
    uint32_t max_room_mb = 0;  // 单房间数据上限（MB），0 = 不限；超限拒绝写入而不是静默删
};

// 返回 false 表示参数非法或要求退出（--help 时 err 为空）
bool parse_config(int argc, char** argv, Config& cfg, std::string& err);
void print_usage(const char* prog);

}  // namespace chat
