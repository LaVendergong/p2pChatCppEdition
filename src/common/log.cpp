#include "chat/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "chat/time_util.h"

namespace chat {
namespace {

constexpr size_t kMaxQueue = 100000;
constexpr size_t kRotateBytes = 100u * 1024 * 1024;
constexpr int kKeepFiles = 5;

struct Logger {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> queue;
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<int> level{static_cast<int>(LogLevel::info)};
    bool to_stderr = true;
    std::string path;
    FILE* fp = nullptr;
    size_t written = 0;
    bool overflowed = false;

    void open_file() {
        if (path.empty()) return;
        fp = std::fopen(path.c_str(), "a");
        if (!fp) {
            std::fprintf(stderr, "[log] cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        } else {
            long sz = 0;
            if (std::fseek(fp, 0, SEEK_END) == 0) sz = std::ftell(fp);
            written = sz > 0 ? static_cast<size_t>(sz) : 0;
            std::fseek(fp, 0, SEEK_END);
        }
    }

    void rotate() {
        if (!fp) return;
        std::fclose(fp);
        fp = nullptr;
        for (int i = kKeepFiles - 1; i >= 1; --i) {
            std::string from = path + "." + std::to_string(i);
            std::string to = path + "." + std::to_string(i + 1);
            std::rename(from.c_str(), to.c_str());
        }
        std::rename(path.c_str(), (path + ".1").c_str());
        open_file();
    }

    void write_lines(const std::vector<std::string>& lines) {
        for (const auto& l : lines) {
            if (to_stderr) std::fwrite(l.data(), 1, l.size(), stderr);
            if (fp) {
                std::fwrite(l.data(), 1, l.size(), fp);
                written += l.size();
            }
        }
        if (to_stderr) std::fflush(stderr);
        if (fp) {
            std::fflush(fp);
            if (written > kRotateBytes) rotate();
        }
    }

    void run() {
        std::vector<std::string> batch;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(200), [this] { return stop || !queue.empty(); });
                if (queue.empty()) {
                    if (stop) break;
                    continue;
                }
                batch.swap(queue);
            }
            write_lines(batch);
            batch.clear();
        }
        // 收尾：把剩余日志写完
        std::vector<std::string> rest;
        {
            std::lock_guard<std::mutex> lk(mu);
            rest.swap(queue);
        }
        write_lines(rest);
        if (fp) {
            std::fclose(fp);
            fp = nullptr;
        }
    }

    ~Logger() {
        stop = true;
        cv.notify_all();
        if (th.joinable()) th.join();
        if (fp) std::fclose(fp);
    }
};

// 故意不析构：避免静态析构顺序导致的 use-after-free（退出前由 log_shutdown 收尾）
Logger& L() {
    static Logger* l = new Logger();
    return *l;
}

const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::trace: return "T";
        case LogLevel::debug: return "D";
        case LogLevel::info: return "I";
        case LogLevel::warn: return "W";
        case LogLevel::error: return "E";
        case LogLevel::off: return "-";
    }
    return "?";
}

}  // namespace

const char* log_level_name(LogLevel l) {
    switch (l) {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info";
        case LogLevel::warn: return "warn";
        case LogLevel::error: return "error";
        case LogLevel::off: return "off";
    }
    return "?";
}

bool parse_log_level(std::string_view s, LogLevel& out) {
    if (s == "trace") out = LogLevel::trace;
    else if (s == "debug") out = LogLevel::debug;
    else if (s == "info") out = LogLevel::info;
    else if (s == "warn") out = LogLevel::warn;
    else if (s == "error") out = LogLevel::error;
    else if (s == "off") out = LogLevel::off;
    else return false;
    return true;
}

void log_init(bool to_stderr, const std::string& path, LogLevel level) {
    Logger& l = L();
    std::lock_guard<std::mutex> lk(l.mu);
    l.to_stderr = to_stderr;
    l.path = path;
    l.level = static_cast<int>(level);
    if (!l.th.joinable()) {
        l.stop = false;
        l.open_file();
        l.th = std::thread([&l] { l.run(); });
    }
}

void log_set_level(LogLevel level) { L().level = static_cast<int>(level); }

LogLevel log_level() {
    const int v = L().level.load(std::memory_order_relaxed);
    return static_cast<LogLevel>(v);
}

void log_shutdown() {
    Logger& l = L();
    l.stop = true;
    l.cv.notify_all();
    if (l.th.joinable()) l.th.join();
}

void log_write(LogLevel lvl, const char* file, int line, std::string_view msg) {
    Logger& l = L();
    // 文件名只保留 basename
    const char* base = file;
    if (const char* p = std::strrchr(file, '/')) base = p + 1;

    std::string linebuf;
    linebuf.reserve(msg.size() + 96);
    linebuf.append(fmt_datetime(now_ms()));
    linebuf.append(" [");
    linebuf.append(level_tag(lvl));
    linebuf.append("] ");
    linebuf.append(std::to_string(static_cast<long long>(::gettid())));
    linebuf.append(" ");
    linebuf.append(base);
    linebuf.push_back(':');
    linebuf.append(std::to_string(line));
    linebuf.push_back(' ');
    linebuf.append(msg.data(), msg.size());
    linebuf.push_back('\n');

    {
        std::lock_guard<std::mutex> lk(l.mu);
        if (l.queue.size() >= kMaxQueue) {
            if (!l.overflowed) {
                l.overflowed = true;
                l.queue.emplace_back("*** log queue overflow, dropping messages ***\n");
            }
            return;
        }
        l.queue.push_back(std::move(linebuf));
    }
    l.cv.notify_one();
}

}  // namespace chat
