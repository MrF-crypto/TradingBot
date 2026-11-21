#pragma once
#include "core/rebound_guard.h"
#include "core/trend_decision.h"
#include "net/trading_client.h"
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace ccbot::rebound {

int confirmation_history(const trend::Config& rule);
bool confirmed_short(const std::vector<TradingClient::Bar>& bars, int64_t hour_close_ms,
                     const trend::Config& rule);

// A round uses one frozen candidate list and one exchange-aligned bar boundary.
// Call sample from a background worker. Hourly market scans can seed it concurrently.
class Sampler {
public:
    void seed_hourly(const std::string& symbol, const std::vector<TradingClient::Bar>& bars,
                     int64_t hour_close_ms);
    Breadth sample(TradingClient& client, const std::set<std::string>& symbols,
                   int64_t close_ms, const std::function<bool()>& cancelled);
private:
    struct Hourly {
        int64_t close_ms = 0;
        double atr = 0;
        std::vector<TradingClient::Bar> bars;
    };
    std::mutex mutex_;
    int64_t latest_hour_ = 0;
    std::map<std::string, Hourly> hourly_;
    int64_t moves_at_ = 0;
    std::map<std::string, double> moves_;
};

} // namespace ccbot::rebound
