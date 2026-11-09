#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <chrono>
#include <stdexcept>

namespace ccbot {

// 简单高效的线程池：按 CPU 核心数开 worker，并发执行任务
class ThreadPool {
public:
    explicit ThreadPool(size_t n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 4;
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        stop_accepting();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // 提交任务，返回 future
    template <typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using Ret = decltype(f());
        auto task = std::make_shared<std::packaged_task<Ret()>>(std::forward<F>(f));
        std::future<Ret> fut = task->get_future();
        {
            std::unique_lock lock(mtx_);
            if (stop_) throw std::runtime_error("thread pool is shutting down");
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t size() const { return workers_.size(); }
    void stop_accepting() {
        { std::lock_guard lock(mtx_); stop_ = true; }
        cv_.notify_all();
    }

    // 阻塞到"队列空 且 没有在途任务"。
    //
    // 任务可能借用引擎和界面对象，调用方须在销毁它们前排空线程池，
    // 并在成交结果落地后保存断点。GUI/headless 统一通过 shutdown_pools 执行。
    //
    // timeout_ms 是兜底：一笔 HTTP 最长 15 秒，卡死的任务不该让关窗口变成假死。
    // 超时返回 false，调用方可以据此决定是否强退
    bool wait_idle(int timeout_ms = 20000) {
        std::unique_lock lock(mtx_);
        return idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] { return tasks_.empty() && active_ == 0; });
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++active_;                   // 出队即视为在途，直到执行完
            }
            task();
            {
                std::lock_guard lock(mtx_);
                --active_;
            }
            idle_cv_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    // 已出队但还没执行完的任务数。"队列空"不等于"没有在途任务"——
    // 一笔下单的 HTTP 可能已经出队、正在网络上跑，队列却是空的
    int  active_ = 0;
    bool stop_ = false;
};

} // namespace ccbot
