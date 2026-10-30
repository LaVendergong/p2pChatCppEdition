// 一个 TCP 连接的全部状态。只在 io 线程访问，无需加锁。
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chat/buffer.h"
#include "chat/net.h"

namespace chat {

class Session {
public:
    static constexpr size_t kMaxOutBuffer = 8u << 20;  // 8 MiB，超出则断开（防慢速读）
    static constexpr size_t kIdemCapacity = 1024;

    // 待发送队列：单发时用 own（移动构造，零分配），广播时多个会话共享同一份缓冲
    struct OutChunk {
        std::string own;
        std::shared_ptr<const std::string> shared;
        size_t off = 0;

        const char* data() const { return shared ? shared->data() : own.data(); }
        size_t size() const { return shared ? shared->size() : own.size(); }
    };

    uint64_t id = 0;
    UniqueFd fd;
    Buffer in;
    std::deque<OutChunk> out;
    size_t out_bytes = 0;  // 尚未发出的字节数

    bool flush_pending = false;  // 已在待刷列表中，避免重复登记

    std::string peer;
    uint64_t connect_ms = 0;
    uint64_t last_active_ms = 0;
    uint64_t last_ping_ms = 0;

    // 简易令牌桶：每秒最多 20 帧，防止刷屏与放大器攻击
    uint64_t rate_window_ms = 0;
    int rate_count = 0;
    int login_fails = 0;

    bool authenticated = false;
    std::string user;
    uint64_t user_id = 0;
    std::vector<std::string> rooms;
    bool closing = false;  // 已决定断开，等发送缓冲排空

    bool out_empty() const { return out.empty(); }
    void reset_out() {
        out.clear();
        out_bytes = 0;
    }
    // 移动入队（单发场景，零分配）
    void enqueue(std::string data);
    // 共享入队（广播场景，所有成员共享同一个 string）
    void enqueue_shared(std::shared_ptr<const std::string> data);

    bool has_room(const std::string& room) const;
    void add_room(const std::string& room);
    void remove_room(const std::string& room);

    void touch(uint64_t now);
    bool idle_timeout(uint64_t now, uint64_t limit_ms) const {
        return now > last_active_ms && now - last_active_ms > limit_ms;
    }

    // 幂等表：client_msg_id -> msg_id（限制容量，FIFO 淘汰）
    void remember_ack(uint64_t client_msg_id, uint64_t msg_id);
    bool lookup_ack(uint64_t client_msg_id, uint64_t& msg_id) const;

private:
    std::deque<uint64_t> idem_order_;
    std::unordered_map<uint64_t, uint64_t> idem_;
};

}  // namespace chat
