// 服务端主体：epoll 事件循环 + 连接生命周期 + 跨线程任务投递。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth.h"
#include "chat/error.h"
#include "chat/protocol.h"
#include "config.h"
#include "io_loop.h"
#include "router.h"
#include "session.h"
#include "storage.h"
#include "thread_pool.h"

namespace chat {

class Dispatcher;

class Server {
public:
    static constexpr uint64_t kSignalToken = ~0ull - 1;

    explicit Server(const Config& cfg);
    ~Server();

    Status start();
    void run();
    void stop();

    // ---- 供 Dispatcher 使用 ----
    const Config& config() const { return cfg_; }
    StorageManager& storage() { return storage_; }
    Router& router() { return router_; }
    Auth& auth() { return auth_; }
    ThreadPool& pool() { return pool_; }

    Session* find_session(uint64_t id);
    std::vector<Session*> sessions_of_user(const std::string& user);

    void send_frame(Session& s, MsgType type, uint64_t seq, const std::string& payload, bool push = false);
    void send_error(Session& s, Err code, const std::string& reason, uint64_t seq);
    // 广播：多个会话共享同一份帧缓冲（零拷贝）。string 重载会拷贝一次再共享。
    void broadcast_room(const std::string& room, std::shared_ptr<const std::string> frame, uint64_t except_session = 0);
    void broadcast_room(const std::string& room, const std::string& frame, uint64_t except_session = 0);
    void broadcast_system(const std::string& room, const std::string& text);

    // 从任意线程投递任务到 io 线程
    void post_to_io(std::function<void()> fn);
    void close_session(uint64_t id, const char* why);

    void register_user(Session& s);
    void unregister_user(Session& s);
    void enqueue(Session& s, std::string data);
    void enqueue_shared(Session& s, std::shared_ptr<const std::string> frame);

    uint64_t next_ack_tag() { return ++ack_tag_; }

    struct PendingAck {
        uint64_t session_id = 0;
        uint64_t client_msg_id = 0;
    };
    std::mutex ack_mu;
    std::unordered_map<uint64_t, PendingAck> ack_map;

    struct Stats {
        std::atomic<uint64_t> connections{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> messages{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> closed{0};
    };
    Stats stats;

private:
    void flush_pending();  // 本轮事件处理完后统一写，合并同一 session 的多次发送
    void handle_accept();
    void handle_read(Session& s);
    // closing_after=true 表示这是关闭前的最后一次发送，失败时不再递归关闭
    void flush_out(Session& s, bool closing_after = false);
    void process_timers();
    void run_posted();
    void handle_signal();
    void shutdown_all();
    void log_stats();

    Config cfg_;
    IoLoop loop_;
    ThreadPool pool_;
    StorageManager storage_;
    Router router_;
    Auth auth_;

    std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string, std::vector<uint64_t>> user_sessions_;
    uint64_t next_session_id_ = 0;
    int listen_fd_ = -1;
    int sig_fd_ = -1;
    uint64_t listen_token_ = 0;

    std::mutex post_mu_;
    std::vector<std::function<void()>> posted_;
    bool post_pending_ = false;

    std::vector<uint64_t> flush_list_;  // 本轮有数据待发的 session

    std::atomic<bool> stop_{false};
    std::unique_ptr<Dispatcher> dispatcher_;
    uint64_t last_timer_ms_ = 0;
    uint64_t last_stats_ms_ = 0;
    std::atomic<uint64_t> ack_tag_{0};
};

}  // namespace chat
