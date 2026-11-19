#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace ccbot {

// The observed quote is distinct from a stop/order threshold and from the later fill.
// Absence is meaningful, including for legacy trades and exchange-side exits.
struct ExitEvidence {
    std::string rule, parameters;
    std::optional<double> trigger_price, threshold_price;
    int64_t trigger_ms = 0;

    bool empty() const {
        return rule.empty() && parameters.empty() && !trigger_price && !threshold_price && trigger_ms == 0;
    }
    bool operator==(const ExitEvidence&) const = default;
};

inline std::string exit_number(double value) {
    if (!std::isfinite(value)) return "未记录";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 15);
    return result.ec == std::errc{} ? std::string(buffer, result.ptr) : "未记录";
}

inline std::string exit_price_text(const std::optional<double>& price) {
    return price && *price > 0 ? exit_number(*price) : "未记录";
}

inline std::string exit_fill_text(double price, const std::string& original_reason = {}) {
    if (original_reason.find("按挂单价估算") != std::string::npos ||
        original_reason.find("成交价为近似值") != std::string::npos ||
        original_reason.find("按触发价估算") != std::string::npos) return "未记录";
    return exit_price_text(price);
}

inline ExitEvidence legacy_exit_evidence(const std::string& reason, bool trend_following_recorded = false) {
    ExitEvidence evidence;
    if (reason == "追踪止盈出场，等待新信号")
        evidence.rule = trend_following_recorded ? "ATR 移动止盈" : "移动止盈（策略未记录）";
    else if (reason == "追踪止盈：收益率从高点回撤")
        evidence.rule = "收益率回撤止盈";
    else if (reason == "保本止损出场") evidence.rule = "保本止损";
    else if (reason.starts_with("交易所硬止损触发")) evidence.rule = "交易所硬止损";
    return evidence;
}

inline std::string exit_rule_label(const ExitEvidence& evidence, const std::string& original_reason) {
    if (!evidence.rule.empty()) return evidence.rule;
    const auto legacy = legacy_exit_evidence(original_reason);
    if (!legacy.rule.empty()) return legacy.rule;
    return original_reason.empty() ? "未记录" : original_reason;
}

inline std::string format_exit_evidence(const ExitEvidence& evidence, double fill,
                                        const std::string& original_reason = {}) {
    return "平仓规则=" + exit_rule_label(evidence, original_reason) +
           "｜触发时参数=" + (evidence.parameters.empty() ? "未记录" : evidence.parameters) +
           "｜触发价=" + exit_price_text(evidence.trigger_price) +
           "｜触发线=" + exit_price_text(evidence.threshold_price) +
           "｜成交均价=" + exit_fill_text(fill, original_reason);
}
} // namespace ccbot
