// 固定大小工作线程池：用于 fsync、密码哈希等会阻塞 io 线程的活。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chat {

class ThreadPool {
public:
    void start(int n);
    void post(std::function<void()> fn);
    // 处理完队列中剩余任务后退出
    void stop();
    size_t pending() const;

private:
    void worker();

    std::vector<std::thread> workers_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

}  // namespace chat
