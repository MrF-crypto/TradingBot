#include "core/monitor_state.h"
#include <simdjson.h>
#include <cmath>
#include <stdexcept>

namespace ccbot {
namespace {
std::string quote(const std::string& v) {
    std::ostringstream s; s << '"';
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') s << '\\' << c;
        else if (c < 32) s << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
        else s << c;
    }
    s << '"'; return s.str();
}
int64_t integer(simdjson::dom::element o, const char* key, int64_t max = 9000000000000000LL) {
    int64_t n;
    if (o[key].get(n) != simdjson::SUCCESS || n < 0 || n > max) throw std::runtime_error(key);
    return n;
}
double number(simdjson::dom::element o, const char* key) {
    double n;
    if (o[key].get(n) != simdjson::SUCCESS || !std::isfinite(n)) throw std::runtime_error(key);
    return n;
}
std::string string(simdjson::dom::element o, const char* key) {
    std::string_view v;
    if (o[key].get(v) != simdjson::SUCCESS || v.size() > 4096) throw std::runtime_error(key);
    return std::string(v);
}
bool boolean(simdjson::dom::element o, const char* key) {
    bool b;
    if (o[key].get(b) != simdjson::SUCCESS) throw std::runtime_error(key);
    return b;
}
}

std::string monitor_checkpoint_json(const TrendBot& b) {
    const auto& m = b.monitor; const auto& h = b.rotation_health; const auto& f = m.last_signal;
    std::ostringstream s; s << std::setprecision(17);
    s << "{\"version\":1,\"armed\":" << (b.armed ? "true" : "false")
      << ",\"last_counted\":" << b.last_counted_bar_ms << ",\"rotation_attempt\":" << b.rotation_attempt_bar_ms
      << ",\"key\":" << quote(m.model_key) << ",\"cursor\":" << m.cursor_ms << ",\"target\":" << m.target_ms
      << ",\"updated\":" << m.updated_ms << ",\"trend_since\":" << m.trend_since_ms << ",\"trend_bars\":" << m.trend_bars
      << ",\"regime\":" << static_cast<int>(m.regime) << ",\"previous\":" << static_cast<int>(m.previous)
      << ",\"close\":" << m.last_close << ",\"shape\":" << quote(m.shape) << ",\"rotation_reason\":" << quote(m.rotation_reason)
      << ",\"health\":{\"open\":" << h.open_ms << ",\"last\":" << h.last_bar_ms << ",\"favorable\":" << h.last_favorable_ms
      << ",\"samples\":" << h.samples << ",\"weak\":" << h.weak_bars << ",\"best\":" << h.best_close << "}"
      << ",\"signal\":{\"fast\":" << f.fast << ",\"slow\":" << f.slow << ",\"slope\":" << f.slope_atr
      << ",\"separation\":" << f.separation_atr << ",\"atr\":" << f.atr << ",\"upper\":" << f.upper << ",\"lower\":" << f.lower
      << ",\"volume\":" << f.volume_ratio << ",\"box\":" << f.box_atr << ",\"er\":" << f.er
      << ",\"location\":" << f.close_location << ",\"range\":" << f.range_atr
      << ",\"er_ok\":" << (f.er_ok ? "true" : "false") << ",\"long\":" << (f.long_confirmed ? "true" : "false")
      << ",\"short\":" << (f.short_confirmed ? "true" : "false") << "},\"events\":[";
    bool first = true;
    for (const auto& e : m.events) {
        if (!first) s << ',';
        first = false;
        s << "{\"at\":" << e.at_ms << ",\"kind\":" << quote(e.kind) << ",\"text\":" << quote(e.text) << '}';
    }
    s << "]}"; return s.str();
}

bool restore_monitor_checkpoint(std::string_view json, TrendBot& b, std::string& error) {
    try {
        simdjson::dom::parser parser; simdjson::dom::element o;
        if (parser.parse(simdjson::padded_string(json)).get(o) != simdjson::SUCCESS) throw std::runtime_error("JSON");
        if (integer(o, "version") != 1) throw std::runtime_error("version");
        TrendBot copy = b; auto& m = copy.monitor; auto& h = copy.rotation_health;
        copy.armed = boolean(o, "armed"); copy.last_counted_bar_ms = integer(o, "last_counted");
        copy.rotation_attempt_bar_ms = integer(o, "rotation_attempt");
        m.model_key = string(o, "key"); m.cursor_ms = integer(o, "cursor"); m.target_ms = integer(o, "target");
        m.updated_ms = integer(o, "updated"); m.trend_since_ms = integer(o, "trend_since");
        m.trend_bars = static_cast<int>(integer(o, "trend_bars", 1000000));
        m.regime = static_cast<following::Regime>(integer(o, "regime", 3));
        m.previous = static_cast<following::Regime>(integer(o, "previous", 3));
        m.last_close = number(o, "close"); m.shape = string(o, "shape"); m.rotation_reason = string(o, "rotation_reason");
        auto health = o["health"].value();
        h.open_ms = integer(health, "open"); h.last_bar_ms = integer(health, "last"); h.last_favorable_ms = integer(health, "favorable");
        h.samples = static_cast<int>(integer(health, "samples", 1000000)); h.weak_bars = static_cast<int>(integer(health, "weak", 1000000));
        h.best_close = number(health, "best"); h.settings = copy.cfg.rule.following;
        if (h.weak_bars > h.samples || h.last_favorable_ms > h.last_bar_ms || h.last_bar_ms > m.cursor_ms ||
            m.cursor_ms > m.target_ms || m.trend_since_ms > m.cursor_ms || h.best_close < 0 || m.last_close < 0)
            throw std::runtime_error("checkpoint ordering");
        auto sig = o["signal"].value(); auto& f = m.last_signal;
        f.fast = number(sig, "fast"); f.slow = number(sig, "slow"); f.slope_atr = number(sig, "slope");
        f.separation_atr = number(sig, "separation"); f.atr = number(sig, "atr"); f.upper = number(sig, "upper");
        f.lower = number(sig, "lower"); f.volume_ratio = number(sig, "volume"); f.box_atr = number(sig, "box");
        f.er = number(sig, "er"); f.er_ok = boolean(sig, "er_ok");
        f.close_location = number(sig, "location"); f.range_atr = number(sig, "range");
        f.long_confirmed = boolean(sig, "long"); f.short_confirmed = boolean(sig, "short");
        f.regime = m.regime; f.settings = copy.signal_following_config(); f.ready = m.cursor_ms > 0;
        simdjson::dom::array events;
        if (o["events"].get_array().get(events) != simdjson::SUCCESS || events.size() > 40) throw std::runtime_error("events");
        m.events.clear();
        for (auto e : events) m.events.push_back({integer(e, "at"), string(e, "kind"), string(e, "text")});
        m.saved = true; m.recovering = true; m.data_error = "等待核对与补齐行情";
        b = std::move(copy); error.clear(); return true;
    } catch (const std::exception& e) {
        error = "监控断点损坏（" + std::string(e.what()) + "），原状态保持不变";
        return false;
    }
}
} // namespace ccbot
