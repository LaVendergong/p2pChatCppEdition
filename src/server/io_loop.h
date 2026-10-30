// epoll 薄封装：注册 fd、等待事件、跨线程唤醒（eventfd）。
#pragma once

#include <cstdint>
#include <vector>

#include "chat/error.h"

namespace chat {

struct IoEvent {
    int fd = -1;
    uint32_t events = 0;
    uint64_t token = 0;  // 连接 id（不是 fd，避免 fd 复用歧义）
};

class IoLoop {
public:
    static constexpr uint64_t kWakeToken = ~0ull;

    Status init();
    void close();

    Status add(int fd, uint32_t events, uint64_t token);
    Status mod(int fd, uint32_t events, uint64_t token);
    void del(int fd);

    // 返回事件个数；超时返回 0；出错返回 -1
    int wait(std::vector<IoEvent>& out, int timeout_ms);

    void wakeup();
    void drain_wakeup();

    int raw_fd() const { return epfd_; }

private:
    int epfd_ = -1;
    int wakefd_ = -1;
};

}  // namespace chat
