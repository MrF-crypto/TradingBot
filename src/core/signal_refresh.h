#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ccbot {

// Runtime-only scheduling and batch notices, shared by GUI and headless.
// No strategy counters, positions or durable monitoring cursors live here.
class SignalRefresh {
public:
    using Clock = std::chrono::steady_clock;
    enum class Result { Ready, Backfill, Waiting, Failed };
    struct Notice { std::string text; bool warning = false; };

    bool claim(const std::string& id, const std::string& symbol, Clock::time_point now, bool force = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = entries_[id];
        if (e.in_flight || (now < e.next && !(force && !e.problem))) return false;
        e.symbol = symbol;
        e.in_flight = true;
        return true;
    }

    // Returns true only while a missing current candle is in bounded fast retry.
    bool finish(const std::string& id, Result result, const std::string& reason,
                int refresh_sec, int64_t period_ms, int64_t market_ms, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return false; // bot was removed
        auto& e = it->second;
        e.in_flight = false;
        int64_t delay = static_cast<int64_t>(std::max(1, refresh_sec)) * 1000;
        // A successful refresh just before the hour must not postpone the new
        // candle for another full minute. The application's 3s tick drives this.
        if (period_ms > 0 && market_ms >= 0)
            delay = std::min(delay, period_ms - market_ms % period_ms + 1000);
        bool waiting = false;
        if (result == Result::Ready) {
            if (e.wait_noted || e.fail_noted) recovered_[id] = e.symbol;
            e.problem = e.wait_noted = e.fail_noted = false;
            e.fast_retries = e.failures = 0;
        } else if (result == Result::Backfill) {
            // Valid history is progress, but not a restored live signal.
            e.failures = 0;
        } else {
            e.problem = true;
            if (result == Result::Waiting) {
                if (!e.wait_noted && !e.fail_noted) {
                    waiting_[id] = e.symbol;
                    e.wait_noted = true;
                }
                if (e.fast_retries < 3 && !e.fail_noted) {
                    delay = (3LL << e.fast_retries) * 1000;
                    ++e.fast_retries;
                    waiting = true;
                } else if (!e.fail_noted) {
                    failed_[id] = e.symbol + "：" + reason;
                    e.fail_noted = true;
                }
            } else {
                e.failures = std::min(e.failures + 1, 2);
                if (e.failures >= 2 && !e.fail_noted) {
                    failed_[id] = e.symbol + "：" + reason;
                    e.fail_noted = true;
                }
            }
        }
        e.next = now + std::chrono::milliseconds(delay);
        return waiting;
    }

    void prune(const std::vector<std::string>& ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::set<std::string> keep(ids.begin(), ids.end());
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (keep.count(it->first)) { ++it; continue; }
            waiting_.erase(it->first); failed_.erase(it->first); recovered_.erase(it->first);
            it = entries_.erase(it);
        }
    }

    std::vector<Notice> take_notices() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Notice> out;
        if (!waiting_.empty())
            out.push_back({"等待新K线：" + describe(waiting_, 5) + "；保留断点，按3/6/12秒间隔重试", false});
        if (!failed_.empty())
            out.push_back({"K线持续异常：" + describe(failed_, 3) + "；暂停这些品种的新开仓，按正常周期重试", true});
        if (!recovered_.empty()) out.push_back({"K线已恢复：" + describe(recovered_, 5), false});
        waiting_.clear(); failed_.clear(); recovered_.clear();
        return out;
    }

private:
    struct Entry {
        std::string symbol;
        Clock::time_point next{};
        bool in_flight = false, problem = false, wait_noted = false, fail_noted = false;
        int fast_retries = 0, failures = 0;
    };
    static std::string describe(const std::map<std::string, std::string>& items, size_t limit) {
        std::string text = std::to_string(items.size()) + "个（";
        size_t n = 0;
        for (const auto& [id, detail] : items) {
            if (n == limit) { text += "、…"; break; }
            if (n++) text += "、";
            text += detail;
        }
        return text + "）";
    }
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::string> waiting_, failed_, recovered_;
};

} // namespace ccbot
