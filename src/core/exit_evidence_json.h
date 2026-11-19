#pragma once
#include "core/exit_evidence.h"
#include "core/profit_json.h"
#include <limits>
#include <locale>

namespace ccbot {
inline std::string exit_evidence_json(const ExitEvidence& evidence) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "{\"rule\":" << profit::quote(evidence.rule) << ",\"parameters\":" << profit::quote(evidence.parameters);
    if (evidence.trigger_price && std::isfinite(*evidence.trigger_price) && *evidence.trigger_price > 0)
        out << ",\"trigger_price\":" << *evidence.trigger_price;
    if (evidence.threshold_price && std::isfinite(*evidence.threshold_price) && *evidence.threshold_price > 0)
        out << ",\"threshold_price\":" << *evidence.threshold_price;
    if (evidence.trigger_ms > 0) out << ",\"trigger_ms\":" << evidence.trigger_ms;
    out << '}';
    return out.str();
}

// Optional audit metadata must not prevent an order or a position from being restored.
inline bool read_exit_evidence_json(std::string_view json, ExitEvidence& output) {
    output = {};
    try {
        simdjson::dom::parser parser;
        auto obj = parser.parse(simdjson::padded_string(json)).get_object().value();
        ExitEvidence evidence;
        evidence.rule = std::string(obj["rule"].get_string().value());
        evidence.parameters = std::string(obj["parameters"].get_string().value());
        const auto read_price = [&](const char* key, std::optional<double>& price) {
            auto v = obj[key];
            if (v.error() == simdjson::NO_SUCH_FIELD) return true;
            double d;
            if (v.get(d) != simdjson::SUCCESS || !std::isfinite(d) || d <= 0) return false;
            price = d;
            return true;
        };
        if (!read_price("trigger_price", evidence.trigger_price) ||
            !read_price("threshold_price", evidence.threshold_price)) return false;
        if (obj["trigger_ms"].error() != simdjson::NO_SUCH_FIELD &&
            (obj["trigger_ms"].get(evidence.trigger_ms) != simdjson::SUCCESS || evidence.trigger_ms <= 0)) return false;
        output = std::move(evidence);
        return true;
    } catch (const simdjson::simdjson_error&) { return false; }
}
} // namespace ccbot
