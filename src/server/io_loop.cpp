#include "io_loop.h"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace chat {

Status IoLoop::init() {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) return status_errno("epoll_create1");

    wakefd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakefd_ < 0) {
        Status s = status_errno("eventfd");
        ::close(epfd_);
        epfd_ = -1;
        return s;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeToken;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev) < 0) {
        Status s = status_errno("epoll_ctl(add wakefd)");
        ::close(wakefd_);
        ::close(epfd_);
        wakefd_ = epfd_ = -1;
        return s;
    }
    return Status::success();
}

void IoLoop::close() {
    if (wakefd_ >= 0) {
        ::close(wakefd_);
        wakefd_ = -1;
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
}

Status IoLoop::add(int fd, uint32_t events, uint64_t token) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) return status_errno("epoll_ctl(add)");
    return Status::success();
}

Status IoLoop::mod(int fd, uint32_t events, uint64_t token) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) return status_errno("epoll_ctl(mod)");
    return Status::success();
}

void IoLoop::del(int fd) { ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr); }

int IoLoop::wait(std::vector<IoEvent>& out, int timeout_ms) {
    constexpr int kMaxEvents = 256;
    epoll_event evs[kMaxEvents];
    const int n = ::epoll_wait(epfd_, evs, kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    out.clear();
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        IoEvent e;
        e.fd = evs[i].data.fd;
        e.events = evs[i].events;
        e.token = evs[i].data.u64;
        out.push_back(e);
    }
    return n;
}

void IoLoop::wakeup() {
    const uint64_t one = 1;
    ssize_t r = ::write(wakefd_, &one, sizeof(one));
    (void)r;
}

void IoLoop::drain_wakeup() {
    uint64_t v = 0;
    while (::read(wakefd_, &v, sizeof(v)) > 0) {
    }
}

}  // namespace chat
