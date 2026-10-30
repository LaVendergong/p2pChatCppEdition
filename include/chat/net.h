// POSIX socket 薄封装：只管 fd 生命周期、非阻塞设置与全量读写。
#pragma once

#include <cstdint>
#include <string>
#include <sys/uio.h>

#include "chat/error.h"

namespace chat {

// RAII fd，move-only
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& o) noexcept;

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

// 监听；backlog 默认 1024。成功返回 listen fd
Status net_listen(const std::string& bind_addr, uint16_t port, UniqueFd& out);

// accept；非阻塞模式下无连接返回 would_block
Status net_accept(int listen_fd, UniqueFd& out, std::string& peer);

Status net_set_nonblock(int fd);
Status net_set_nodelay(int fd);
Status net_set_reuseaddr(int fd);

// 非阻塞全写：返回成功写入的字节数（可能 < n，此时不视为错误）
Status net_write_some(int fd, const char* data, size_t n, size_t& written);

// 非阻塞散布写（一次 syscall 写多段，用于合并待发队列）
Status net_writev_some(int fd, struct iovec* iov, int n, size_t& written);

// 非阻塞读：读到 0 字节且 errno==0 视为对端关闭
Status net_read_some(int fd, char* buf, size_t cap, size_t& got);

// 阻塞模式下写满（工具用）
Status net_write_all_blocking(int fd, const char* data, size_t n);

std::string net_peer_string(const std::string& ip, uint16_t port);

}  // namespace chat
