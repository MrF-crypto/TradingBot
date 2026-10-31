#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

namespace ccbot::smart {
// Lightweight, bounded event detector. Prices are compared to the preceding
// successful poll; ATR percentages come from the PREVIOUS completed scan.
// It requests a scan only; it cannot authorize a trade.
class MarketWake {
public:
    bool observe(const std::map<std::string, double>& prices,
                 const std::map<std::string, double>& atr_pct, int64_t now,
                 int poll_sec, int cooldown_sec, double multiple, double floor_pct) {
        bool event = false;
        const bool contiguous = last_poll_ > 0 && now > last_poll_ &&
                                now - last_poll_ <= static_cast<int64_t>(poll_sec) * 2500;
        if (contiguous && now - last_wake_ >= static_cast<int64_t>(cooldown_sec) * 1000) {
            for (const auto& [sym, price] : prices) {
                auto p = previous_.find(sym);
                auto a = atr_pct.find(sym);
                if (p == previous_.end() || a == atr_pct.end() ||
                    !(std::isfinite(a->second) && a->second > 0) ||
                    !(std::isfinite(price) && price > 0 && std::isfinite(p->second) && p->second > 0)) continue;
                const double move = std::fabs(price / p->second - 1) * 100;
                if (move >= std::max(floor_pct, multiple * a->second)) event = true;
            }
        }
        previous_ = prices;
        last_poll_ = now;
        if (event) last_wake_ = now;
        return event;
    }
private:
    std::map<std::string, double> previous_;
    int64_t last_poll_ = 0, last_wake_ = 0;
};
} // namespace ccbot::smart
