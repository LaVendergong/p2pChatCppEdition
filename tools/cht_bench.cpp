// cht-bench：绕过网络直接打存储引擎，验证千万级写入与随机读（N1/N4）。
// 用法: cht-bench --dir /tmp/chat_bench --count 10000000 --reads 100000 --segment-mb 64
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "chat/log.h"
#include "storage.h"

namespace {

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t pct(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "/tmp/chat_bench";
    uint64_t count = 1000000;
    uint64_t reads = 100000;
    uint32_t seg_mb = 64;
    uint64_t sync_every = 1000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "cht-bench: %s needs a value\n", a.c_str());
            return 2;
        }
        const std::string v = argv[++i];
        if (a == "--dir") dir = v;
        else if (a == "--count") count = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--reads") reads = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--segment-mb") seg_mb = static_cast<uint32_t>(std::atoi(v.c_str()));
        else if (a == "--sync-every") sync_every = std::strtoull(v.c_str(), nullptr, 10);
        else {
            std::fprintf(stderr, "cht-bench: unknown option %s\n", a.c_str());
            return 2;
        }
    }

    chat::log_init(false, "", chat::LogLevel::warn);

    chat::RoomStore rs;
    chat::Status st = rs.open("bench", dir, seg_mb << 20, false);
    if (!st) {
        std::fprintf(stderr, "cht-bench: open failed: %s\n", chat::err_text(st.e, st.msg).c_str());
        return 1;
    }

    std::printf("== write %llu messages to %s (segment %u MB, sync every %llu)\n",
                static_cast<unsigned long long>(count), dir.c_str(), seg_mb,
                static_cast<unsigned long long>(sync_every));

    const uint64_t t0 = now_us();
    uint64_t written = 0;
    for (uint64_t i = 0; i < count; ++i) {
        chat::StoredMessage m;
        m.from = "bench";
        m.ts_ms = 1700000000000ull + i;
        m.text = "benchmark payload line " + std::to_string(i % 1000);
        st = rs.append(m);
        if (!st) {
            std::fprintf(stderr, "cht-bench: append failed at %llu: %s\n", static_cast<unsigned long long>(i),
                         chat::err_text(st.e, st.msg).c_str());
            break;
        }
        ++written;
        if (sync_every > 0 && (i + 1) % sync_every == 0) {
            st = rs.sync();
            if (!st) {
                std::fprintf(stderr, "cht-bench: sync failed: %s\n", chat::err_text(st.e, st.msg).c_str());
                break;
            }
        }
    }
    (void)rs.sync();
    const uint64_t t1 = now_us();

    const double secs = static_cast<double>(t1 - t0) / 1e6;
    std::printf("write: %llu msgs in %.2fs = %.0f msg/s, %.1f MB/s\n", static_cast<unsigned long long>(written), secs,
                static_cast<double>(written) / (secs > 0 ? secs : 1),
                static_cast<double>(written) * 120.0 / 1024.0 / 1024.0 / (secs > 0 ? secs : 1));

    std::printf("== random read %llu times (50 msgs each, before random id)\n",
                static_cast<unsigned long long>(reads));
    std::mt19937_64 rng(42);
    std::vector<uint64_t> lat_us;
    lat_us.reserve(static_cast<size_t>(reads));
    uint64_t total_returned = 0;
    // 用房间当前最大 id 作为随机范围（--count 0 时 written 是 0，不能拿它当范围）
    const uint64_t max_id = rs.last_msg_id();
    for (uint64_t k = 0; k < reads; ++k) {
        const uint64_t before = 1 + rng() % (max_id ? max_id : 1);
        std::vector<chat::StoredMessage> out;
        const uint64_t a = now_us();
        st = rs.read_before(before, 50, out);
        lat_us.push_back(now_us() - a);
        if (!st) {
            std::fprintf(stderr, "cht-bench: read failed: %s\n", chat::err_text(st.e, st.msg).c_str());
            break;
        }
        total_returned += out.size();
    }
    std::sort(lat_us.begin(), lat_us.end());
    std::printf("read: %llu queries, avg %.0f msgs/query\n", static_cast<unsigned long long>(lat_us.size()),
                lat_us.empty() ? 0.0 : static_cast<double>(total_returned) / static_cast<double>(lat_us.size()));
    std::printf("read latency(us): p50=%llu p90=%llu p99=%llu max=%llu\n",
                static_cast<unsigned long long>(pct(lat_us, 0.50)), static_cast<unsigned long long>(pct(lat_us, 0.90)),
                static_cast<unsigned long long>(pct(lat_us, 0.99)),
                static_cast<unsigned long long>(lat_us.empty() ? 0 : lat_us.back()));

    std::printf("stored: last_msg_id=%llu count=%llu\n", static_cast<unsigned long long>(rs.last_msg_id()),
                static_cast<unsigned long long>(rs.msg_count()));
    chat::log_shutdown();
    return 0;
}
