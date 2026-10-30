#include <csignal>
#include <cstdio>
#include <string>

#include "chat/fs.h"
#include "chat/log.h"
#include "config.h"
#include "server.h"

int main(int argc, char** argv) {
    using namespace chat;

    // 必须在创建任何线程（日志线程、线程池、flusher）之前屏蔽信号。
    // 线程会继承创建时的掩码；只要有一个线程没屏蔽，SIGTERM 就可能落到它身上
    // 走默认动作直接杀掉进程，优雅退出的收尾（flush + meta）就全丢了。
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        std::fprintf(stderr, "chatd: cannot block signals\n");
        return 1;
    }

    Config cfg;
    std::string err;
    if (!parse_config(argc, argv, cfg, err)) {
        if (!err.empty()) std::fprintf(stderr, "chatd: %s\n", err.c_str());
        print_usage(argv[0]);
        return err.empty() ? 0 : 2;
    }

    LogLevel level = LogLevel::info;
    if (!parse_log_level(cfg.log_level, level)) {
        std::fprintf(stderr, "chatd: invalid log level '%s'\n", cfg.log_level.c_str());
        return 2;
    }

    Status st = fs::mkdirs(cfg.data_dir);
    if (!st) {
        std::fprintf(stderr, "chatd: cannot create data dir: %s\n", err_text(st.e, st.msg).c_str());
        return 1;
    }
    const std::string log_dir = fs::join(cfg.data_dir, "logs");
    (void)fs::mkdirs(log_dir);
    log_init(cfg.log_console, fs::join(log_dir, "chatd.log"), level);

    LOG_INFO("chatd: starting (version 0.1.0)");

    Server server(cfg);
    st = server.start();
    if (!st) {
        LOG_ERROR("chatd: start failed: {}", err_text(st.e, st.msg));
        std::fprintf(stderr, "chatd: start failed: %s\n", err_text(st.e, st.msg).c_str());
        log_shutdown();
        return 1;
    }

    server.run();
    LOG_INFO("chatd: stopped");
    log_shutdown();
    return 0;
}
