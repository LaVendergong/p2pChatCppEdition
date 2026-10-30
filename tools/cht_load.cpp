// cht-load：压测工具。每个连接一个线程，握手后用滑动窗口持续发消息，统计往返延迟分位。
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "chat/buffer.h"
#include "chat/log.h"
#include "chat/net.h"
#include "chat/protocol.h"
#include "chat/sha256.h"
#include "chat/time_util.h"
#include "chat/utf8.h"

using namespace chat;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 7700;
    int conns = 8;
    int msgs = 500;
    int window = 64;
    std::string room = "loadtest";
    std::string user_prefix = "load";
    std::string password = "loadpass";
};

struct Conn {
    int fd = -1;
    Buffer in;
    std::string out;
    size_t out_off = 0;

    bool flush_out() {
        while (out_off < out.size()) {
            size_t written = 0;
            Status st = net_write_some(fd, out.data() + out_off, out.size() - out_off, written);
            if (!st) {
                if (st.e == Err::would_block) return true;
                return false;
            }
            out_off += written;
        }
        out.clear();
        out_off = 0;
        return true;
    }

    void send(MsgType t, uint64_t seq, const std::string& payload) {
        out.append(encode_frame(t, seq, payload));
    }
};

// 同步等一个指定类型的帧（用于握手阶段）
bool wait_for(Conn& c, MsgType want, Frame& got, int timeout_ms) {
    const uint64_t deadline = steady_ms() + static_cast<uint64_t>(timeout_ms);
    while (steady_ms() < deadline) {
        pollfd p{c.fd, POLLIN, 0};
        const int n = ::poll(&p, 1, 50);
        if (n > 0 && (p.revents & POLLIN)) {
            char buf[65536];
            for (;;) {
                size_t got_n = 0;
                Status st = net_read_some(c.fd, buf, sizeof(buf), got_n);
                if (!st) break;
                c.in.append(buf, got_n);
            }
        }
        for (;;) {
            Frame f;
            size_t consumed = 0;
            const std::string_view view(reinterpret_cast<const char*>(c.in.data()), c.in.size());
            const ParseStatus ps = parse_frame(view, f, consumed);
            if (ps == ParseStatus::need_more) break;
            if (ps != ParseStatus::ok) return false;
            c.in.consume(consumed);
            if (f.header.type == want) {
                got = std::move(f);
                return true;
            }
        }
    }
    return false;
}

bool handshake(Conn& c, const Options& o, const std::string& user) {
    {
        TlvWriter w;
        w.add_str(tag::version, "cht-load/0.1.0");
        w.add_str(tag::client_name, "cht-load");
        c.send(MsgType::hello, 1, w.data());
        if (!c.flush_out()) return false;
        Frame f;
        if (!wait_for(c, MsgType::hello_ack, f, 5000)) return false;
    }
    // 第一次 LOGIN 取盐
    std::string salt;
    {
        TlvWriter w;
        w.add_str(tag::username, user);
        c.send(MsgType::login, 2, w.data());
        if (!c.flush_out()) return false;
        Frame f;
        if (!wait_for(c, MsgType::login_ack, f, 5000)) return false;
        TlvReader r(f.payload);
        r.get_str(tag::salt, salt);
    }
    {
        TlvWriter w;
        w.add_str(tag::username, user);
        if (!salt.empty()) w.add_str(tag::password_hash, password_digest(salt, o.password));
        c.send(MsgType::login, 3, w.data());
        if (!c.flush_out()) return false;
        Frame f;
        if (!wait_for(c, MsgType::login_ack, f, 5000)) return false;
        TlvReader r(f.payload);
        if (r.u16_or(tag::code, 999) != 0) return false;
    }
    {
        TlvWriter w;
        w.add_str(tag::room, o.room);
        c.send(MsgType::join, 4, w.data());
        if (!c.flush_out()) return false;
        Frame f;
        if (!wait_for(c, MsgType::join_ack, f, 10000)) return false;
    }
    return true;
}

bool connect_conn(Conn& c, const Options& o) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(o.port);
    if (::inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    (void)net_set_nonblock(fd);
    (void)net_set_nodelay(fd);
    c.fd = fd;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "cht-load: %s needs a value\n", a.c_str());
            return 2;
        }
        const std::string v = argv[++i];
        if (a == "--host") o.host = v;
        else if (a == "--port") o.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (a == "--conns") o.conns = std::atoi(v.c_str());
        else if (a == "--msgs") o.msgs = std::atoi(v.c_str());
        else if (a == "--window") o.window = std::atoi(v.c_str());
        else if (a == "--room") o.room = v;
        else if (a == "--user-prefix") o.user_prefix = v;
        else if (a == "--password") o.password = v;
        else {
            std::fprintf(stderr, "cht-load: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (o.conns < 1 || o.msgs < 1) {
        std::fprintf(stderr, "cht-load: invalid --conns/--msgs\n");
        return 2;
    }

    log_init(false, "", LogLevel::warn);

    std::atomic<uint64_t> acked_total{0};
    std::atomic<uint64_t> sent_total{0};
    std::atomic<int> failed_conns{0};
    std::vector<std::vector<uint64_t>> latencies(static_cast<size_t>(o.conns));

    const uint64_t t0 = steady_ms();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(o.conns));

    for (int i = 0; i < o.conns; ++i) {
        threads.emplace_back([&, i] {
            Conn c;
            if (!connect_conn(c, o)) {
                failed_conns.fetch_add(1);
                return;
            }
            const std::string user = o.user_prefix + std::to_string(i % 64);
            if (!handshake(c, o, user)) {
                failed_conns.fetch_add(1);
                ::close(c.fd);
                return;
            }

            std::unordered_map<uint64_t, uint64_t> inflight;
            inflight.reserve(static_cast<size_t>(o.window) * 2);
            std::vector<uint64_t>& lat = latencies[static_cast<size_t>(i)];
            lat.reserve(static_cast<size_t>(o.msgs));

            uint64_t cmid = 0;
            uint64_t sent = 0;
            uint64_t acked = 0;
            uint64_t errors = 0;
            const std::string text = "loadtest message payload 0123456789";
            const uint64_t deadline = steady_ms() + 120000;  // 防挂死

            while (acked < static_cast<uint64_t>(o.msgs)) {
                if (steady_ms() > deadline) {
                    std::fprintf(stderr, "cht-load: connection %d timed out (acked=%llu sent=%llu errors=%llu)\n", i,
                                 static_cast<unsigned long long>(acked), static_cast<unsigned long long>(sent),
                                 static_cast<unsigned long long>(errors));
                    break;
                }
                while (sent < static_cast<uint64_t>(o.msgs) && inflight.size() < static_cast<size_t>(o.window)) {
                    ++cmid;
                    TlvWriter w;
                    w.add_str(tag::room, o.room);
                    w.add_str(tag::text, text);
                    w.add_u64(tag::client_msg_id, cmid);
                    c.send(MsgType::msg_send, 100 + cmid, w.data());
                    inflight[cmid] = steady_ms();
                    ++sent;
                }
                if (!c.flush_out()) break;

                pollfd p{c.fd, POLLIN, 0};
                const int n = ::poll(&p, 1, 20);
                if (n <= 0) continue;
                char buf[65536];
                for (;;) {
                    size_t got = 0;
                    Status st = net_read_some(c.fd, buf, sizeof(buf), got);
                    if (!st) break;
                    c.in.append(buf, got);
                }
                for (;;) {
                    Frame f;
                    size_t consumed = 0;
                    const std::string_view view(reinterpret_cast<const char*>(c.in.data()), c.in.size());
                    const ParseStatus ps = parse_frame(view, f, consumed);
                    if (ps == ParseStatus::need_more) break;
                    if (ps != ParseStatus::ok) {
                        acked = static_cast<uint64_t>(o.msgs);
                        break;
                    }
                    c.in.consume(consumed);
                    if (f.header.type == MsgType::msg_send_ack) {
                        TlvReader r(f.payload);
                        const uint64_t id = r.u64_or(tag::client_msg_id, 0);
                        auto it = inflight.find(id);
                        if (it != inflight.end()) {
                            const uint64_t now = steady_ms();
                            lat.push_back(now >= it->second ? now - it->second : 0);
                            inflight.erase(it);
                            ++acked;
                        }
                    } else if (f.header.type == MsgType::error) {
                        // 被服务端拒绝（限流/校验失败）：从窗口里移除，避免永远等不到 ACK
                        ++errors;
                        if (f.header.seq > 100) {
                            auto it = inflight.find(f.header.seq - 100);
                            if (it != inflight.end()) {
                                inflight.erase(it);
                                ++acked;
                            }
                        }
                    }
                }
            }
            acked_total.fetch_add(acked, std::memory_order_relaxed);
            sent_total.fetch_add(sent, std::memory_order_relaxed);
            ::close(c.fd);
        });
    }
    for (auto& t : threads) t.join();
    const uint64_t elapsed = std::max<uint64_t>(steady_ms() - t0, 1);

    std::vector<uint64_t> all;
    for (auto& v : latencies) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    auto pct = [&all](double p) -> uint64_t {
        if (all.empty()) return 0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(all.size() - 1));
        return all[idx];
    };

    const uint64_t total = acked_total.load();
    std::printf("conns=%d failed=%d msgs_acked=%llu sent=%llu\n", o.conns, failed_conns.load(),
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(sent_total.load()));
    std::printf("elapsed=%llums qps=%.0f\n", static_cast<unsigned long long>(elapsed),
                static_cast<double>(total) * 1000.0 / static_cast<double>(elapsed));
    std::printf("latency_ms p50=%llu p90=%llu p99=%llu max=%llu\n", static_cast<unsigned long long>(pct(0.50)),
                static_cast<unsigned long long>(pct(0.90)), static_cast<unsigned long long>(pct(0.99)),
                static_cast<unsigned long long>(all.empty() ? 0 : all.back()));

    log_shutdown();
    return failed_conns.load() > 0 ? 1 : 0;
}
