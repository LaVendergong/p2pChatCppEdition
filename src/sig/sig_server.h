// chatsig：P2P 信令服务。只做端点注册、地址查询、在线状态，不转发聊天内容
// （中继是端到端转发，见 docs/11-P2P架构.md）。
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth.h"
#include "chat/error.h"
#include "chat/protocol.h"
#include "io_loop.h"
#include "session.h"

namespace chat {

struct SigConfig {
    std::string bind_addr = "0.0.0.0";
    uint16_t port = 7900;
    std::string data_dir = "./sigdata";
    std::string log_level = "info";
    bool log_console = true;
};

class SigServer {
public:
    explicit SigServer(const SigConfig& cfg);
    ~SigServer();

    Status start();
    void run();
    void stop();

private:
    // 一个已登录用户的端点信息
    struct Peer {
        std::string user;
        std::string lan_ip;
        std::string public_ip;
        uint16_t tcp_port = 0;
        uint16_t public_port = 0;
        uint64_t session_id = 0;
        uint64_t last_seen_ms = 0;
    };

    void handle_accept();
    void handle_read(Session& s);
    void flush_out(Session& s, bool closing_after = false);
    void close_session(uint64_t id, const char* why);
    void process_timers();

    void dispatch(Session& s, const Frame& f);
    void on_hello(Session& s, const Frame& f);
    void on_login(Session& s, const Frame& f);
    void on_peer_register(Session& s, const Frame& f);
    void on_peer_lookup(Session& s, const Frame& f);
    void on_relay_open(Session& s, const Frame& f);
    void on_relay_data(Session& s, const Frame& f);
    void on_ping(Session& s, const Frame& f);

    void send_frame(Session& s, MsgType t, uint64_t seq, const std::string& payload);
    void send_error(Session& s, Err code, const std::string& reason, uint64_t seq);

    // 把 peer 的候选地址编码成 CANDIDATES 数组元素
    std::vector<std::string> candidates_of(const Peer& p) const;

    SigConfig cfg_;
    IoLoop loop_;
    Auth auth_;

    std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string, Peer> peers_;

    // 中继配对：user -> 想连的对方；配对成功后 peer（对端用户名）即可转发
    std::unordered_map<std::string, std::string> relay_waiting_;
    std::unordered_map<std::string, std::string> relay_paired_;  // user -> peer

    int listen_fd_ = -1;
    int sig_fd_ = -1;
    uint64_t next_session_id_ = 0;
    uint64_t last_timer_ms_ = 0;

    std::atomic<bool> stop_{false};
};

}  // namespace chat
