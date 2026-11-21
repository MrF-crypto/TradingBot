#include "app/rebound_sampler.h"
#include "core/parallel.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ccbot::rebound {
namespace {
using Bar = TradingClient::Bar;
constexpr size_t kHistory = 187;

// At the top of the hour the scheduled scan may not have finished yet. The
// exact twelve closed 5m candles give the same hourly OHLC as an extra 1h call.
bool append_closed_hour(std::vector<Bar>& hours, const std::vector<Bar>& five, int64_t until) {
    auto it = std::find_if(five.begin(), five.end(), [&](const Bar& b){ return b.open_ms == until - kHour; });
    if (it == five.end() || five.end() - it < 12) return false;
    Bar h = *it;
    for (int i = 0; i < 12; ++i) {
        const auto& b = it[i];
        if (b.open_ms != until - kHour + i*kFiveMinutes ||
            !std::isfinite(b.high) || !std::isfinite(b.low) ||
            !std::isfinite(b.open) || !std::isfinite(b.close) || !(b.low > 0) ||
            b.high < std::max(b.open, b.close) || b.low > std::min(b.open, b.close)) return false;
        h.high = std::max(h.high, b.high);
        h.low = std::min(h.low, b.low);
        h.close = b.close;
    }
    hours.push_back(h);
    return true;
}
}

int confirmation_history(const trend::Config& rule) {
    if (!following::valid(rule.following) || rule.atr_period < 2 || rule.donchian_period < 2) return 0;
    const int64_t n = std::max<int64_t>({int64_t(rule.donchian_period) + 1, int64_t(rule.atr_period)*4,
                                        rule.following.slow_period*3 + rule.following.slope_bars}) + 5;
    return n <= 1500 ? static_cast<int>(n) : 0;
}

bool confirmed_short(const std::vector<Bar>& bars, int64_t hour_close_ms, const trend::Config& rule) {
    const int need = confirmation_history(rule);
    if (!need || bars.size() < static_cast<size_t>(need) || hour_close_ms <= 0 ||
        hour_close_ms % kHour || bars.back().open_ms != hour_close_ms) return false;
    std::vector<indicators::Ohlc> window;
    std::vector<double> volumes;
    window.reserve(need);
    volumes.reserve(need);
    const auto first = bars.end() - need;
    for (auto it = first; it != bars.end(); ++it) {
        if (it != first && it->open_ms != (it-1)->open_ms + kHour) return false;
        window.push_back({it->high, it->low, it->close, it->open});
        volumes.push_back(it->volume);
    }
    auto config = rule.following;
    config.enabled = config.close_confirm = true;
    const auto signal = following::compute(window, volumes, config, rule.donchian_period, rule.atr_period);
    return signal.ready && signal.regime == following::Regime::Down && signal.short_confirmed;
}

void Sampler::seed_hourly(const std::string& symbol, const std::vector<Bar>& bars,
                          int64_t hour_close_ms) {
    const double atr = closed_hour_atr(bars, hour_close_ms);
    if (!std::isfinite(atr) || !(atr > 0)) return;
    const auto end = std::find_if(bars.begin(), bars.end(), [&](const Bar& b) { return b.open_ms >= hour_close_ms; });
    const auto count = std::min<size_t>(kHistory, static_cast<size_t>(end - bars.begin()));
    Hourly updated{hour_close_ms, atr, {end - count, end}};
    std::lock_guard lock(mutex_);
    if (hour_close_ms > latest_hour_) {
        latest_hour_ = hour_close_ms;
        std::erase_if(hourly_, [&](const auto& item){ return item.second.close_ms < latest_hour_ - kHour; });
    }
    if (hour_close_ms < latest_hour_ - kHour) return;
    auto& h = hourly_[symbol];
    if (h.close_ms <= hour_close_ms) h = std::move(updated);
}

Breadth Sampler::sample(TradingClient& client, const std::set<std::string>& symbols,
                        int64_t close_ms, const std::function<bool()>& cancelled) {
    const int64_t hour = close_ms/kHour*kHour;
    const auto current = [&] {
        return close_ms > 0 && client.market_time_ms()/kFiveMinutes*kFiveMinutes == close_ms &&
               (!cancelled || !cancelled());
    };
    if (!current()) return summarize(symbols.size(), {});
    // Skip optional market requests when the venue is throttling. Critical mark
    // polling and order traffic use dedicated slots/priority in TradingClient.
    const auto can_fetch = [&] {
        if (!current()) return false;
        const auto rate = client.rate_status();
        // Public USD-M candles share the fapi IP budget even for PAPI accounts.
        const int limit = std::min(rate.limit, 2400);
        return !rate.banned && (limit <= 0 || 4LL*rate.used_weight < 3LL*limit);
    };
    std::vector<std::string> missing;
    std::vector<double> values;
    {
        std::lock_guard lock(mutex_);
        if (moves_at_ != close_ms) {
            moves_.clear();
            moves_at_ = close_ms;
        }
        for (const auto& symbol : symbols) {
            const auto it = moves_.find(symbol);
            if (it == moves_.end()) missing.push_back(symbol);
            else values.push_back(it->second);
        }
    }
    std::vector<double> fetched(missing.size(), std::numeric_limits<double>::quiet_NaN());
    parallel_for(static_cast<int>(missing.size()), 2, [&](int i) {
        try {
            if (!can_fetch()) return;
            const auto& sym = missing[i];
            std::vector<Bar> rolling;
            double atr = 0;
            {
                std::lock_guard lock(mutex_);
                if (const auto it = hourly_.find(sym); it != hourly_.end()) {
                    if (it->second.close_ms == hour) atr = it->second.atr;
                    else if (it->second.close_ms == hour-kHour) rolling = it->second.bars;
                }
            }
            const int count = rolling.empty() ? 4 : 12 + static_cast<int>((close_ms-hour)/kFiveMinutes);
            const auto five = client.fetch_klines(sym, "5m", count, close_ms-1);
            if (five.empty()) return;
            if (!(atr > 0)) {
                if (rolling.empty() || !append_closed_hour(rolling, five, hour)) {
                    if (!can_fetch()) return;
                    rolling = client.fetch_klines(sym, "1h", static_cast<int>(kHistory), hour-1);
                }
                seed_hourly(sym, rolling, hour);
                std::lock_guard lock(mutex_);
                if (const auto it = hourly_.find(sym); it != hourly_.end() && it->second.close_ms == hour)
                    atr = it->second.atr;
            }
            fetched[i] = normalized_move(five, close_ms, atr, hour);
        } catch (const std::exception&) {
            // An individual failure remains missing from coverage and can retry.
        }
    });
    if (!current()) return summarize(symbols.size(), {});
    {
        std::lock_guard lock(mutex_);
        if (moves_at_ != close_ms) return summarize(symbols.size(), {});
        for (size_t i = 0; i < fetched.size(); ++i) {
            if (!std::isfinite(fetched[i])) continue;
            moves_[missing[i]] = fetched[i];
            values.push_back(fetched[i]);
        }
    }
    return summarize(symbols.size(), values);
}
} // namespace ccbot::rebound
