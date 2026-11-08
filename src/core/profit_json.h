#pragma once
#include "core/profit_policy.h"
#include <simdjson.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace ccbot::profit {
inline std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') out << '\\' << ch;
        else if (ch < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch) << std::dec;
        else out << ch;
    }
    out << '"'; return out.str();
}
inline double number(simdjson::dom::element o, const char* key) {
    double v; if (o[key].get(v) != simdjson::SUCCESS || !std::isfinite(v)) throw std::runtime_error(key); return v;
}
inline int64_t integer(simdjson::dom::element o, const char* key) {
    int64_t v; if (o[key].get(v) != simdjson::SUCCESS || v < 0) throw std::runtime_error(key); return v;
}
inline bool boolean(simdjson::dom::element o, const char* key) {
    bool v; if (o[key].get(v) != simdjson::SUCCESS) throw std::runtime_error(key); return v;
}
inline std::string string(simdjson::dom::element o, const char* key) {
    std::string_view v; if (o[key].get(v) != simdjson::SUCCESS || v.size() > 4096) throw std::runtime_error(key); return std::string(v);
}
inline std::string config_json(const Config& c) {
    std::ostringstream o; o << std::setprecision(17) << std::boolalpha;
    o << "{\"protect\":" << c.protect << ",\"accumulate\":" << c.accumulate << ",\"wait_btc_up\":" << c.wait_btc_up
      << ",\"arm_usdt\":" << c.arm_usdt << ",\"soft_drawdown\":" << c.soft_drawdown << ",\"hard_drawdown\":" << c.hard_drawdown
      << ",\"weak_breadth\":" << c.weak_breadth << ",\"breadth_drop\":" << c.breadth_drop << ",\"recover_breadth\":" << c.recover_breadth
      << ",\"confirm_bars\":" << c.confirm_bars << ",\"recovery_bars\":" << c.recovery_bars << ",\"allocation\":" << c.allocation
      << ",\"batch_usdt\":" << c.batch_usdt << ",\"capital_floor\":" << c.capital_floor
      << ",\"max_batch_usdt\":" << c.max_batch_usdt << ",\"slippage_bps\":" << c.slippage_bps << '}'; return o.str();
}
inline void read_config(simdjson::dom::element o, Config& c) {
    const auto n = [&](const char* k, double& v) { if (o[k].error() != simdjson::NO_SUCH_FIELD) v = number(o, k); };
    const auto b = [&](const char* k, bool& v) { if (o[k].error() != simdjson::NO_SUCH_FIELD) v = boolean(o, k); };
    b("protect", c.protect); b("accumulate", c.accumulate); b("wait_btc_up", c.wait_btc_up);
    n("arm_usdt", c.arm_usdt); n("soft_drawdown", c.soft_drawdown); n("hard_drawdown", c.hard_drawdown);
    n("weak_breadth", c.weak_breadth); n("breadth_drop", c.breadth_drop); n("recover_breadth", c.recover_breadth);
    n("allocation", c.allocation); n("batch_usdt", c.batch_usdt); n("capital_floor", c.capital_floor);
    n("max_batch_usdt", c.max_batch_usdt); n("slippage_bps", c.slippage_bps);
    if (o["confirm_bars"].error() != simdjson::NO_SUCH_FIELD) {
        auto v = integer(o, "confirm_bars"); if (v > 24) throw std::runtime_error("confirm_bars"); c.confirm_bars = static_cast<int>(v);
    }
    if (o["recovery_bars"].error() != simdjson::NO_SUCH_FIELD) {
        auto v = integer(o, "recovery_bars"); if (v > 24) throw std::runtime_error("recovery_bars"); c.recovery_bars = static_cast<int>(v);
    }
    if (!valid(c)) throw std::runtime_error("利润保护／BTC 积累参数超出范围");
}
}
