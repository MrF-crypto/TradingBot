#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ccbot::profit {
// Research defaults. The switches deliberately default to off.
struct Config {
    bool operator==(const Config&) const = default;
    bool protect = false, accumulate = false, wait_btc_up = true;
    double arm_usdt = 100, soft_drawdown = 0.20, hard_drawdown = 0.35;
    double weak_breadth = 0.45, breadth_drop = 0.15, recover_breadth = 0.60;
    int confirm_bars = 2, recovery_bars = 2;
    double allocation = 0.30, batch_usdt = 50, capital_floor = 0;
    double max_batch_usdt = 500, slippage_bps = 20;
};
inline bool valid(const Config& c) {
    const auto range = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    return range(c.arm_usdt, 0.01, 1e9) && range(c.soft_drawdown, .01, .99) &&
        range(c.hard_drawdown, c.soft_drawdown, 1) && range(c.weak_breadth, 0, .9) &&
        range(c.breadth_drop, .01, 1) && range(c.recover_breadth, c.weak_breadth + .01, 1) &&
        c.confirm_bars >= 1 && c.confirm_bars <= 24 && c.recovery_bars >= 1 && c.recovery_bars <= 24 &&
        range(c.allocation, .01, 1) && range(c.batch_usdt, 5, 1e7) &&
        range(c.max_batch_usdt, c.batch_usdt, 1e7) && range(c.capital_floor, 0, 1e12) &&
        range(c.slippage_bps, 1, 100);
}
enum class Phase { Watching, Caution, Exiting, Waiting };
struct Guard {
    Phase phase = Phase::Watching;
    bool active = false, armed = false;
    double anchor = 0, peak = 0, breadth_peak = 0, profit = 0, breadth = 0;
    int weak = 0, recovered = 0;
    int64_t last_bar = 0, exit_ms = 0, fresh_after = 0;
    std::string reason;
    bool blocked() const { return phase != Phase::Watching; }
    // curve = realized + marked unrealized, including estimated exit costs.
    // An invalid mark must never create a peak or an artificial drawdown.
    void update(const Config& c, int64_t now, double curve, bool prices_ok, bool exposed,
                int64_t bar, double support, bool breadth_ok, double support_upper = -1) {
        if (!c.protect) return;
        if (!active) {
            if (!exposed || !prices_ok) return;
            active = true; anchor = curve; peak = profit = 0;
        }
        if (phase == Phase::Exiting && !exposed) { phase = Phase::Waiting; recovered = 0; }
        if (phase == Phase::Caution && !exposed) {
            // Individual stops may finish the exit before portfolio drawdown fires.
            // Keep processing recovery bars rather than leaving an inactive blocked guard.
            phase = Phase::Waiting; exit_ms = now; recovered = 0;
            reason = "广度转弱期间已自然清仓，等待广度恢复";
        }
        if (prices_ok) {
            profit = curve - anchor;
            peak = std::max(peak, profit);
            armed = armed || peak >= c.arm_usdt;
        }
        if (breadth_ok && bar > last_bar && bar <= now) {
            const bool contiguous = last_bar == 0 || bar - last_bar == 3600000;
            if (!contiguous) { weak = 0; recovered = 0; }
            last_bar = bar; breadth = support;
            breadth_peak = std::max(breadth_peak, support);
            // Unknown members may support the direction. Only weaken on the upper bound;
            // recover and raise the breadth peak on the lower bound.
            const double upper = support_upper < 0 ? support : support_upper;
            const bool weakening = upper < c.weak_breadth || breadth_peak - upper >= c.breadth_drop;
            weak = weakening ? weak + 1 : 0;
            if (phase == Phase::Waiting) {
                recovered = bar > exit_ms && support >= c.recover_breadth ? recovered + 1 : 0;
                if (recovered >= c.recovery_bars && prices_ok) {
                    // Require a later, newly closed breakout even on previously flat candidates.
                    fresh_after = bar; phase = Phase::Watching; active = exposed;
                    anchor = curve; peak = profit = 0; armed = false; weak = recovered = 0;
                    breadth_peak = support; reason = "恢复趋势许可，等待新的收盘突破";
                }
            } else if (phase != Phase::Exiting && armed) {
                if (weak >= c.confirm_bars) { phase = Phase::Caution; reason = "市场广度转弱，暂停该方向新增风险"; }
                else if (!weakening && support >= c.recover_breadth) {
                    phase = Phase::Watching; reason.clear();
                }
            }
        }
        if (phase == Phase::Waiting || phase == Phase::Exiting) return;
        if (armed && prices_ok && exposed && peak > 0) {
            const double dd = (peak - profit) / peak;
            if (dd >= c.hard_drawdown || (breadth_ok && weak >= c.confirm_bars && dd >= c.soft_drawdown)) {
                phase = Phase::Exiting; exit_ms = now; recovered = 0;
                reason = dd >= c.hard_drawdown ? "组合利润触及硬回撤线" : "市场广度转弱且组合利润回撤";
            }
        }
        if (!exposed && phase != Phase::Waiting && phase != Phase::Exiting) {
            active = armed = false; peak = profit = 0; weak = recovered = 0;
            breadth_peak = breadth = 0; last_bar = 0; phase = Phase::Watching; reason.clear();
        }
    }
};
// This is NEW entitlement. Money reserved for an in-flight operation is not allocated twice.
inline double entitlement(double shorts, double overall, double used, double reserved, double ratio) {
    if (!std::isfinite(shorts) || !std::isfinite(overall) || !std::isfinite(used) ||
        !std::isfinite(reserved) || !std::isfinite(ratio) || used < 0 || reserved < 0 || ratio < 0 || ratio > 1) return 0;
    return std::max(0.0, std::min(std::max(0.0, shorts) * ratio, std::max(0.0, overall)) - used - reserved);
}
} // namespace ccbot::profit
