#pragma once
#include "core/thread_pool.h"
#include <algorithm>

namespace ccbot {
// Producers can still submit order work while draining. Never destroy borrowed
// objects unless BOTH pools have drained; false requires retaining them or _Exit.
inline bool shutdown_pools(ThreadPool* producers, ThreadPool* orders, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&] {
        return std::max(0, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count()));
    };
    if (producers) {
        producers->stop_accepting();
        if (!producers->wait_idle(remaining())) return false;
    }
    if (orders) {
        orders->stop_accepting();
        if (!orders->wait_idle(remaining())) return false;
    }
    return true;
}
} // namespace ccbot
