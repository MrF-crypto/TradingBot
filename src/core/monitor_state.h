#pragma once
#include "core/trend_engine.h"
#include <iomanip>
#include <sstream>

namespace ccbot {
inline std::string monitor_model_key(const TrendConfig& c) {
    const auto& f = c.rule.following;
    std::ostringstream s; s << std::setprecision(17);
    s << c.interval << '/' << c.rule.donchian_period << '/' << c.rule.atr_period << '/'
      << f.enabled << '/' << f.close_confirm << '/' << f.reversal_exit << '/' << f.fast_period << '/' << f.slow_period << '/'
      << f.slope_bars << '/' << f.min_slope_atr << '/' << f.min_separation_atr << '/'
      << f.breakout_buffer_atr << '/' << f.min_close_location << '/' << f.short_vol_ratio;
    return s.str();
}
inline const char* regime_name(following::Regime r) {
    switch (r) {
    case following::Regime::Up: return "上涨";
    case following::Regime::Down: return "下跌";
    case following::Regime::Range: return "震荡过渡";
    default: return "数据不足";
    }
}
inline void monitor_event(TrendBot& b, int64_t at, const std::string& kind, const std::string& text) {
    auto& events = b.monitor.events;
    size_t end = std::min<size_t>(text.size(), 4000);
    while (end < text.size() && end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    const auto clipped = text.substr(0, end);
    if (!events.empty() && events.back().kind == kind && events.back().text == clipped) return;
    events.push_back({at, kind, clipped});
    if (events.size() > 40) events.erase(events.begin(), events.begin() + static_cast<ptrdiff_t>(events.size() - 40));
}
// Shared, strict checkpoint codec used by Qt and headless. No credentials or live order queue.
std::string monitor_checkpoint_json(const TrendBot& bot);
bool restore_monitor_checkpoint(std::string_view json, TrendBot& bot, std::string& error);
} // namespace ccbot
