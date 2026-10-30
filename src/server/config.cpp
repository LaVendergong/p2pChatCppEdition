#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace chat {
namespace {

bool need_value(int i, int argc, const char* opt, std::string& err) {
    if (i + 1 >= argc) {
        err = std::string(opt) + " requires a value";
        return false;
    }
    return true;
}

}  // namespace

void print_usage(const char* prog) {
    std::printf(
        "usage: %s [options]\n"
        "  --bind ADDR           bind address (default 0.0.0.0)\n"
        "  --port N              listen port (default 7700)\n"
        "  --data DIR            data directory (default ./data)\n"
        "  --threads N           worker threads (default = cpu cores)\n"
        "  --sync MODE           batch|always|never (default batch)\n"
        "  --segment-mb N        segment file size limit (default 64)\n"
        "  --max-conns N         max concurrent connections (default 5000)\n"
        "  --rate-limit N        max frames/sec per connection, 0 = unlimited (default 20)\n"
        "  --max-room-mb N       per-room data limit in MB, 0 = unlimited (default 0)\n"
        "  --log-level L         trace|debug|info|warn|error (default info)\n"
        "  --no-log-console      do not print logs to stderr\n"
        "  --stats               print stats every 60s\n"
        "  --repair              rebuild index and repair tails on start\n"
        "  --strict-auth         disable auto register\n"
        "  -h, --help            show this help\n",
        prog);
}

bool parse_config(int argc, char** argv, Config& cfg, std::string& err) {
    const unsigned hw = std::thread::hardware_concurrency();
    cfg.threads = hw ? static_cast<int>(hw) : 4;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") return false;
        if (a == "--no-log-console") {
            cfg.log_console = false;
        } else if (a == "--stats") {
            cfg.stats = true;
        } else if (a == "--repair") {
            cfg.repair = true;
        } else if (a == "--strict-auth") {
            cfg.allow_auto_register = false;
        } else if (a == "--bind" || a == "--port" || a == "--data" || a == "--threads" || a == "--sync" ||
                   a == "--segment-mb" || a == "--max-conns" || a == "--log-level" || a == "--rate-limit" ||
                   a == "--max-room-mb") {
            if (!need_value(i, argc, a.c_str(), err)) return false;
            const std::string v = argv[++i];
            if (a == "--bind") cfg.bind_addr = v;
            else if (a == "--data") cfg.data_dir = v;
            else if (a == "--sync") {
                if (v != "batch" && v != "always" && v != "never") {
                    err = "--sync must be batch|always|never";
                    return false;
                }
                cfg.sync_mode = v;
            } else if (a == "--log-level") cfg.log_level = v;
            else if (a == "--port") {
                const int p = std::atoi(v.c_str());
                if (p <= 0 || p > 65535) {
                    err = "invalid --port";
                    return false;
                }
                cfg.port = static_cast<uint16_t>(p);
            } else if (a == "--threads") {
                const int n = std::atoi(v.c_str());
                if (n <= 0 || n > 64) {
                    err = "invalid --threads (1..64)";
                    return false;
                }
                cfg.threads = n;
            } else if (a == "--segment-mb") {
                const int n = std::atoi(v.c_str());
                if (n < 1 || n > 4096) {
                    err = "invalid --segment-mb";
                    return false;
                }
                cfg.segment_mb = static_cast<uint32_t>(n);
            } else if (a == "--max-conns") {
                const int n = std::atoi(v.c_str());
                if (n < 1) {
                    err = "invalid --max-conns";
                    return false;
                }
                cfg.max_conns = n;
            } else if (a == "--rate-limit") {
                const int n = std::atoi(v.c_str());
                if (n < 0) {
                    err = "invalid --rate-limit";
                    return false;
                }
                cfg.rate_limit = n;
            } else if (a == "--max-room-mb") {
                const int n = std::atoi(v.c_str());
                if (n < 0) {
                    err = "invalid --max-room-mb";
                    return false;
                }
                cfg.max_room_mb = static_cast<uint32_t>(n);
            }
        } else {
            err = "unknown option: " + a;
            return false;
        }
    }
    return true;
}

}  // namespace chat
