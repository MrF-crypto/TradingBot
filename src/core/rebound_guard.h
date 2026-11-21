#pragma once
#include "core/indicators.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace ccbot::rebound {

inline constexpr int64_t kFiveMinutes = 300'000;
inline constexpr int64_t kHour = 3'600'000;
inline constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();

// Klines are ordered by open time. The argument is the exclusive close boundary,
// not the last millisecond included in the exchange's candle.
template<class Bar>
double closed_hour_atr(const std::vector<Bar>& bars, int64_t hour_close_ms) {
    if (hour_close_ms <= 0 || hour_close_ms % kHour != 0) return kMissing;
    const auto end = std::lower_bound(bars.begin(), bars.end(), hour_close_ms,
        [](const Bar& b, int64_t t) { return b.open_ms < t; });
    // Same Wilder ATR(14) warm-up as the 187 closed hourly candles in the replay.
    const auto count = std::min<std::ptrdiff_t>(end - bars.begin(), 187);
    if (count < 15) return kMissing;
    std::vector<indicators::Ohlc> closed;
    closed.reserve(static_cast<std::size_t>(count));
    int64_t expected = hour_close_ms - count * kHour;
    for (auto it = end - count; it != end; ++it, expected += kHour) {
        const auto& b = *it;
        if (b.open_ms != expected || !std::isfinite(b.high) || !std::isfinite(b.low) ||
            !std::isfinite(b.close) || !std::isfinite(b.open) || b.low <= 0 ||
            b.low > std::min(b.open, b.close) || b.high < std::max(b.open, b.close))
            return kMissing;
        closed.push_back({b.high, b.low, b.close, b.open});
    }
    const double atr = indicators::atr(closed, 14);
    return std::isfinite(atr) && atr > 0 ? atr : kMissing;
}

template<class Bar>
double normalized_move(const std::vector<Bar>& bars, int64_t close_ms,
                       double atr, int64_t atr_close_ms) {
    if (close_ms <= 0 || close_ms % kFiveMinutes != 0 ||
        atr_close_ms != close_ms / kHour * kHour || !std::isfinite(atr) || atr <= 0)
        return kMissing;
    const auto end = std::lower_bound(bars.begin(), bars.end(), close_ms,
        [](const Bar& b, int64_t t) { return b.open_ms < t; });
    if (end - bars.begin() < 4) return kMissing;
    auto first = end - 4;
    int64_t expected = close_ms - 4 * kFiveMinutes;
    for (auto it = first; it != end; ++it, expected += kFiveMinutes) {
        if (it->open_ms != expected || !std::isfinite(it->close) || it->close <= 0)
            return kMissing;
    }
    const double move = ((end - 1)->close - first->close) / atr;
    return std::isfinite(move) ? move : kMissing;
}

struct Breadth {
    std::size_t pool = 0, valid = 0, up = 0;
    double up_fraction = 0, median_atr = kMissing;
    bool covered() const {
        return pool > 0 && valid <= pool && valid >= pool - pool / 10;
    }
};

// One slot per distinct member of the frozen candidate pool. Absent quotes
// count against coverage. Only valid observations enter the fraction and median.
inline Breadth summarize(std::size_t pool_size, const std::vector<double>& moves) {
    Breadth b;
    b.pool = pool_size;
    if (moves.size() > pool_size) return b;
    std::vector<double> valid;
    valid.reserve(moves.size());
    for (double move : moves) {
        if (!std::isfinite(move)) continue;
        valid.push_back(move);
        if (move > 0) ++b.up;
    }
    b.valid = valid.size();
    if (valid.empty()) return b;
    b.up_fraction = static_cast<double>(b.up) / static_cast<double>(b.valid);
    std::sort(valid.begin(), valid.end());
    const auto mid = valid.size() / 2;
    b.median_atr = valid.size() % 2 ? valid[mid] : std::midpoint(valid[mid - 1], valid[mid]);
    return b;
}

struct State {
    bool paused = false;
    int low_streak = 0;
    int64_t last_close_ms = 0, released_ms = 0;
};

enum class Transition { None, Paused, Released };

inline Transition advance(State& state, int64_t close_ms, const Breadth& b) {
    // An incomplete bin may be retried. Arriving at a later, nonadjacent bin
    // resets the release streak; one recovered observation cannot stand for two.
    if (close_ms <= state.last_close_ms || close_ms % kFiveMinutes != 0 ||
        !b.covered() || !std::isfinite(b.median_atr))
        return Transition::None;
    if (close_ms - state.last_close_ms != kFiveMinutes) state.low_streak = 0;
    state.last_close_ms = close_ms;
    if (!state.paused) {
        if (b.up_fraction >= .70 && b.median_atr >= .50) {
            state.paused = true;
            state.low_streak = 0;
            return Transition::Paused;
        }
    } else {
        state.low_streak = b.up_fraction < .50 ? state.low_streak + 1 : 0;
        if (state.low_streak >= 2) {
            state.paused = false;
            state.low_streak = 0;
            state.released_ms = close_ms;
            return Transition::Released;
        }
    }
    return Transition::None;
}

} // namespace ccbot::rebound
