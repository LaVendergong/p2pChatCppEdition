#include "net_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace chat {

Status NetClient::connect_to(const std::string& host, uint16_t port, int timeout_ms) {
    close();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return Status::fail(Err::invalid_arg, "invalid host: " + host);
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return status_errno("socket");

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        Status st = status_errno("connect");
        ::close(fd);
        return st;
    }
    if (rc < 0) {
        pollfd p{fd, POLLOUT, 0};
        const int n = ::poll(&p, 1, timeout_ms);
        if (n <= 0) {
            ::close(fd);
            return Status::fail(Err::timeout, "connect timeout");
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            ::close(fd);
            return Status::fail(Err::sys_errno, std::string("connect: ") + std::strerror(err));
        }
    }
    (void)net_set_nodelay(fd);
    fd_.reset(fd);
    in_.clear();
    out_.clear();
    out_off_ = 0;
    return Status::success();
}

void NetClient::adopt(UniqueFd fd) {
    close();
    fd_ = std::move(fd);
    (void)net_set_nodelay(fd_.get());
}

void NetClient::close() {
    fd_.reset();
    in_.clear();
    out_.clear();
    out_off_ = 0;
}

void NetClient::send(MsgType type, uint64_t seq, const std::string& payload, bool push) {
    if (!fd_.valid()) return;
    out_.append(encode_frame(type, seq, payload, push ? kFlagPush : 0));
}

Status NetClient::flush() {
    if (!fd_.valid()) return Status::fail(Err::eof, "not connected");
    while (out_off_ < out_.size()) {
        size_t written = 0;
        Status st = net_write_some(fd_.get(), out_.data() + out_off_, out_.size() - out_off_, written);
        if (!st) {
            if (st.e == Err::would_block) return st;
            return st;
        }
        out_off_ += written;
    }
    out_.clear();
    out_off_ = 0;
    return Status::success();
}

Status NetClient::read_frames(std::vector<Frame>& out, bool& closed) {
    closed = false;
    out.clear();
    if (!fd_.valid()) {
        closed = true;
        return Status::fail(Err::eof, "not connected");
    }

    char buf[65536];
    for (;;) {
        size_t got = 0;
        Status st = net_read_some(fd_.get(), buf, sizeof(buf), got);
        if (!st) {
            if (st.e == Err::would_block) break;
            closed = true;
            return st;
        }
        if (got == 0) {
            closed = true;
            return Status::fail(Err::eof, "peer closed");
        }
        in_.append(buf, got);
    }

    for (;;) {
        Frame f;
        size_t consumed = 0;
        const std::string_view view(reinterpret_cast<const char*>(in_.data()), in_.size());
        const ParseStatus ps = parse_frame(view, f, consumed);
        if (ps == ParseStatus::need_more) break;
        if (ps != ParseStatus::ok) {
            closed = true;
            return Status::fail(Err::bad_frame, parse_status_name(ps));
        }
        in_.consume(consumed);
        out.push_back(std::move(f));
    }
    return Status::success();
}

}  // namespace chat
