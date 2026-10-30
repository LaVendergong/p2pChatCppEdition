#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "chat/fs.h"
#include "chat/log.h"
#include "sig_server.h"

namespace {

void usage(const char* prog) {
    std::printf(
        "usage: %s [options]\n"
        "  --bind ADDR        bind address (default 0.0.0.0)\n"
        "  --port N           listen port (default 7900)\n"
        "  --data DIR         data dir for accounts (default ./sigdata)\n"
        "  --log-level L      trace|debug|info|warn|error (default info)\n"
        "  --no-log-console   do not print logs to stderr\n"
        "  -h, --help         show this help\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace chat;

    // 必须在任何线程（日志线程）创建之前屏蔽信号
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        std::fprintf(stderr, "chatsig: cannot block signals\n");
        return 1;
    }

    SigConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--no-log-console") {
            cfg.log_console = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "chatsig: %s needs a value\n", a.c_str());
            return 2;
        }
        const std::string v = argv[++i];
        if (a == "--bind") cfg.bind_addr = v;
        else if (a == "--port") cfg.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (a == "--data") cfg.data_dir = v;
        else if (a == "--log-level") cfg.log_level = v;
        else {
            std::fprintf(stderr, "chatsig: unknown option %s\n", a.c_str());
            return 2;
        }
    }

    LogLevel level = LogLevel::info;
    if (!parse_log_level(cfg.log_level, level)) level = LogLevel::info;

    Status st = fs::mkdirs(cfg.data_dir);
    if (!st) {
        std::fprintf(stderr, "chatsig: cannot create data dir: %s\n", err_text(st.e, st.msg).c_str());
        return 1;
    }
    (void)fs::mkdirs(fs::join(cfg.data_dir, "logs"));
    log_init(cfg.log_console, fs::join(cfg.data_dir, "logs/chatsig.log"), level);

    SigServer srv(cfg);
    st = srv.start();
    if (!st) {
        LOG_ERROR("chatsig: start failed: {}", err_text(st.e, st.msg));
        std::fprintf(stderr, "chatsig: start failed: %s\n", err_text(st.e, st.msg).c_str());
        log_shutdown();
        return 1;
    }
    srv.run();
    log_shutdown();
    return 0;
}
