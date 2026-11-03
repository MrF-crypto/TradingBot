#pragma once
#include "core/indicators.h"
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ccbot::following {

// A causal, fixed-horizon trend model. No forecasts, cross-sectional direction,
// or P&L-driven parameter changes. Disabled for legacy/manual bots by default.
struct Config {
    bool enabled = false;
    bool close_confirm = true;
    bool reversal_exit = true;
    int fast_period = 20, slow_period = 60, slope_bars = 3;
    double min_slope_atr = 0.02, min_separation_atr = 0.10;
    double breakout_buffer_atr = 0.10, min_close_location = 0.60;
    double short_vol_ratio = 0; // slow declines need not expand in volume
    bool operator==(const Config&) const = default;
};

inline bool valid(const Config& c) {
    return c.fast_period >= 2 && c.slow_period > c.fast_period && c.slow_period <= 200 &&
           c.slope_bars >= 1 && c.slope_bars <= 20 &&
           std::isfinite(c.min_slope_atr) && c.min_slope_atr >= 0 && c.min_slope_atr <= 2 &&
           std::isfinite(c.min_separation_atr) && c.min_separation_atr >= 0 && c.min_separation_atr <= 10 &&
           std::isfinite(c.breakout_buffer_atr) && c.breakout_buffer_atr >= 0 && c.breakout_buffer_atr <= 5 &&
           std::isfinite(c.min_close_location) && c.min_close_location >= 0.5 && c.min_close_location <= 1 &&
           std::isfinite(c.short_vol_ratio) && c.short_vol_ratio >= 0;
}

enum class Regime { Unknown, Range, Up, Down };
struct SignalData {
    Config settings;
    bool ready = false;
    Regime regime = Regime::Unknown;
    bool long_confirmed = false, short_confirmed = false;
    double fast = 0, slow = 0, slope_atr = 0, separation_atr = 0;
    double atr = 0, upper = 0, lower = 0, box_atr = 0, volume_ratio = 0;
    double er = 0;
    bool er_ok = false;
    double range_atr = 0; // completed signal candle's range / pre-breakout ATR
    double close_location = 0; // (completed close - low) / (high - low); data display only
};
struct Signal : SignalData {
    // Independent closed-candle evidence, also available when initial entry is intrabar.
    SignalData closed_entry;
};
struct Observation {
    int64_t bar_ms = 0; // close time of the observation / open time of the following candle
    double close = 0;
    SignalData signal;
};

inline std::vector<double> ema(const std::vector<indicators::Ohlc>& b, int period) {
    std::vector<double> out(b.size(), 0);
    if (period < 1 || b.size() < static_cast<size_t>(period)) return out;
    double v = 0;
    for (int i = 0; i < period; ++i) v += b[i].close;
    v /= period;
    out[period - 1] = v;
    const double a = 2.0 / (period + 1);
    for (size_t i = period; i < b.size(); ++i) {
        v += a * (b[i].close - v);
        out[i] = v;
    }
    return out;
}

// Input includes the current unclosed candle. It never participates in regime,
// ATR or confirmed breakout. For close-confirmed entry the channel/ATR exclude
// BOTH the current candle and the signal candle, freezing pre-breakout risk.
inline SignalData compute_model(const std::vector<indicators::Ohlc>& bars,
                      const std::vector<double>& volumes, const Config& cfg,
                      int channel_period, int atr_period) {
    SignalData s;
    if (!valid(cfg) || channel_period < 2 || atr_period < 2 || volumes.size() != bars.size() ||
        bars.size() < static_cast<size_t>(std::max({cfg.slow_period * 3 + cfg.slope_bars,
                                                   channel_period + 2, atr_period + 3}))) return s;
    // Validate closed observations only. An evolving current candle is not evidence.
    for (size_t i = 0; i + 1 < bars.size(); ++i) {
        const auto& b = bars[i];
        if (!(std::isfinite(b.open) && std::isfinite(b.high) && std::isfinite(b.low) &&
              std::isfinite(b.close) && std::isfinite(volumes[i]) && volumes[i] >= 0 &&
              b.low > 0 && b.high >= std::max(b.open, b.close) &&
              b.low <= std::min(b.open, b.close))) return s;
    }
    const std::vector<indicators::Ohlc> closed(bars.begin(), bars.end() - 1);
    const auto fast = ema(closed, cfg.fast_period), slow = ema(closed, cfg.slow_period);
    const size_t last = closed.size() - 1;
    const int exclude = cfg.close_confirm ? 2 : 1;
    const std::vector<indicators::Ohlc> reference(bars.begin(), bars.end() - exclude);
    s.atr = indicators::atr(reference, atr_period);
    if (!(std::isfinite(s.atr) && s.atr > 0)) return Signal{};
    s.fast = fast[last]; s.slow = slow[last];
    s.slope_atr = (slow[last] - slow[last - cfg.slope_bars]) / (cfg.slope_bars * s.atr);
    s.separation_atr = (s.fast - s.slow) / s.atr;
    s.regime = Regime::Range;
    if (s.separation_atr > cfg.min_separation_atr && s.slope_atr > cfg.min_slope_atr &&
        closed.back().close > s.fast) s.regime = Regime::Up;
    if (s.separation_atr < -cfg.min_separation_atr && s.slope_atr < -cfg.min_slope_atr &&
        closed.back().close < s.fast) s.regime = Regime::Down;
    // Classic high/low Donchian, deliberately distinct from the legacy body channel.
    s.upper = reference.back().high; s.lower = reference.back().low;
    for (size_t i = reference.size() - channel_period; i < reference.size(); ++i) {
        s.upper = std::max(s.upper, reference[i].high);
        s.lower = std::min(s.lower, reference[i].low);
    }
    s.box_atr = (s.upper - s.lower) / s.atr;
    s.volume_ratio = indicators::volume_ratio(volumes, static_cast<int>(bars.size()) - exclude, channel_period);
    const auto er = indicators::efficiency_ratio(reference, channel_period, 0);
    s.er_ok = er.ok; s.er = er.er;
    const auto& b = closed.back();
    const double span = b.high - b.low;
    s.range_atr = span / s.atr;
    if (span > 0) {
        const double loc = (b.close - b.low) / span;
        s.close_location = loc;
        s.long_confirmed = b.close > s.upper + cfg.breakout_buffer_atr * s.atr && loc >= cfg.min_close_location;
        s.short_confirmed = b.close < s.lower - cfg.breakout_buffer_atr * s.atr && 1 - loc >= cfg.min_close_location;
    }
    s.settings = cfg;
    s.ready = true;
    return s;
}

inline Signal compute(const std::vector<indicators::Ohlc>& bars,
                      const std::vector<double>& volumes, const Config& cfg,
                      int channel_period, int atr_period) {
    Signal s;
    static_cast<SignalData&>(s) = compute_model(bars, volumes, cfg, channel_period, atr_period);
    auto confirmed = cfg;
    confirmed.close_confirm = true;
    s.closed_entry = cfg.close_confirm ? static_cast<const SignalData&>(s)
        : compute_model(bars, volumes, confirmed, channel_period, atr_period);
    return s;
}

inline Signal closed_signal(const Signal& source) {
    Signal s;
    static_cast<SignalData&>(s) = source.settings.close_confirm
        ? static_cast<const SignalData&>(source) : source.closed_entry;
    // Invalid network snapshots cannot become valid through the secondary model.
    s.ready = s.ready && source.ready;
    return s;
}
} // namespace ccbot::following
