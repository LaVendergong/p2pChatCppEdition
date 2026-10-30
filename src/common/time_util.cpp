#include "chat/time_util.h"

#include <chrono>
#include <ctime>

namespace chat {

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

namespace {
std::tm local_tm(uint64_t wall_ms) {
    std::time_t t = static_cast<std::time_t>(wall_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}
}  // namespace

std::string fmt_clock(uint64_t wall_ms) {
    std::tm tm = local_tm(wall_ms);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

std::string fmt_datetime(uint64_t wall_ms) {
    std::tm tm = local_tm(wall_ms);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace chat
