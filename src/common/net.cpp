#include "chat/net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace chat {

UniqueFd::~UniqueFd() {
    if (fd_ >= 0) ::close(fd_);
}

UniqueFd& UniqueFd::operator=(UniqueFd&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

void UniqueFd::reset(int fd) {
    if (fd_ >= 0 && fd_ != fd) ::close(fd_);
    fd_ = fd;
}

Status net_set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return status_errno("fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return status_errno("fcntl(F_SETFL)");
    return Status::success();
}

Status net_set_nodelay(int fd) {
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) return status_errno("setsockopt(TCP_NODELAY)");
    return Status::success();
}

Status net_set_reuseaddr(int fd) {
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) return status_errno("setsockopt(SO_REUSEADDR)");
    return Status::success();
}

Status net_listen(const std::string& bind_addr, uint16_t port, UniqueFd& out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return status_errno("socket");

    Status st = net_set_reuseaddr(fd);
    if (!st) {
        ::close(fd);
        return st;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_addr.empty() || bind_addr == "0.0.0.0" || bind_addr == "*") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return Status::fail(Err::invalid_arg, "invalid bind address: " + bind_addr);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Status e = status_errno("bind");
        ::close(fd);
        return e;
    }
    if (::listen(fd, 1024) < 0) {
        Status e = status_errno("listen");
        ::close(fd);
        return e;
    }
    out.reset(fd);
    return Status::success();
}

Status net_accept(int listen_fd, UniqueFd& out, std::string& peer) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::fail(Err::would_block);
        if (errno == EINTR) return Status::fail(Err::would_block);
        return status_errno("accept4");
    }
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    peer = net_peer_string(ip, ntohs(addr.sin_port));
    out.reset(fd);
    (void)net_set_nodelay(fd);
    return Status::success();
}

Status net_write_some(int fd, const char* data, size_t n, size_t& written) {
    written = 0;
    // MSG_NOSIGNAL：对端已关闭时返回 EPIPE 而不是直接 SIGPIPE 杀进程
    const ssize_t r = ::send(fd, data, n, MSG_NOSIGNAL);
    if (r >= 0) {
        written = static_cast<size_t>(r);
        return Status::success();
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::fail(Err::would_block);
    if (errno == EINTR) return Status::fail(Err::would_block);
    return status_errno("send");
}

Status net_writev_some(int fd, struct iovec* iov, int n, size_t& written) {
    written = 0;
    if (n <= 0) return Status::success();
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<size_t>(n);
    const ssize_t r = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (r >= 0) {
        written = static_cast<size_t>(r);
        return Status::success();
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::fail(Err::would_block);
    if (errno == EINTR) return Status::fail(Err::would_block);
    return status_errno("sendmsg");
}

Status net_read_some(int fd, char* buf, size_t cap, size_t& got) {
    got = 0;
    const ssize_t r = ::recv(fd, buf, cap, 0);
    if (r > 0) {
        got = static_cast<size_t>(r);
        return Status::success();
    }
    if (r == 0) return Status::fail(Err::eof, "peer closed");
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::fail(Err::would_block);
    if (errno == EINTR) return Status::fail(Err::would_block);
    return status_errno("recv");
}

Status net_write_all_blocking(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t r = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
        if (r > 0) {
            off += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return status_errno("send");
    }
    return Status::success();
}

std::string net_peer_string(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

}  // namespace chat
