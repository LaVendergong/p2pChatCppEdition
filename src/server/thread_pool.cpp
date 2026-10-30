#include "thread_pool.h"

namespace chat {

void ThreadPool::start(int n) {
    if (n < 1) n = 1;
    stop_ = false;
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { worker(); });
}

void ThreadPool::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

size_t ThreadPool::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

void ThreadPool::worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) {
                if (stop_) return;
                continue;
            }
            job = std::move(q_.front());
            q_.pop_front();
        }
        job();
    }
}

}  // namespace chat
