#include "app/profit_manager.h"
#include "core/atomic_file.h"
#include "core/exit_evidence_json.h"
#include "core/profit_json.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ccbot {
std::string ProfitManager::serialize_locked() const {
    std::ostringstream s; s << std::setprecision(17) << std::boolalpha;
    s << "{\"version\":1,\"account\":" << profit::quote(account_) << ",\"started\":" << started_
      << ",\"accumulation_since\":" << accumulation_since_ << ",\"sequence\":" << sequence_ << ",\"tracking\":" << tracking_
      << ",\"capital_floor\":" << capital_floor_ << ",\"used\":" << used_ << ",\"btc_qty\":" << btc_qty_ << ",\"spot_cash\":" << spot_cash_
      << ",\"breadth_model\":" << profit::quote(breadth_signature_)
      << ",\"purchase\":{\"phase\":" << purchase_.phase << ",\"sent\":" << purchase_.sent << ",\"tag\":" << profit::quote(purchase_.tag)
      << ",\"transfer_id\":" << profit::quote(purchase_.transfer_id) << ",\"reserved\":" << purchase_.reserved << "},\"guards\":[";
    for (size_t i = 0; i < 2; ++i) {
        const auto& g = guards_[i]; if (i) s << ',';
        s << "{\"phase\":" << static_cast<int>(g.phase) << ",\"active\":" << g.active << ",\"armed\":" << g.armed
          << ",\"anchor\":" << g.anchor << ",\"peak\":" << g.peak << ",\"profit\":" << g.profit << ",\"breadth_peak\":" << g.breadth_peak
          << ",\"breadth\":" << g.breadth << ",\"weak\":" << g.weak << ",\"recovered\":" << g.recovered
          << ",\"last_bar\":" << g.last_bar << ",\"exit_ms\":" << g.exit_ms << ",\"fresh_after\":" << g.fresh_after
          << ",\"reason\":" << profit::quote(g.reason);
        if (!g.exit_evidence.empty()) s << ",\"exit_evidence\":" << exit_evidence_json(g.exit_evidence);
        s << ",\"cohort\":[";
        bool first = true; for (const auto& symbol : cohorts_[i]) { if (!first) s << ',';
        first = false; s << profit::quote(symbol); } s << "]}";
    }
    s << "],\"breadth_absent\":["; bool first = true;
    for (const auto& [symbol, since] : missing_from_market_) {
        if (!first) s << ',';
        first = false; s << "{\"symbol\":" << profit::quote(symbol) << ",\"since\":" << since << '}';
    }
    s << "],\"inherited\":["; first = true;
    for (const auto& [symbol, qty] : inherited_) {
        if (!first) s << ',';
        first = false; s << "{\"symbol\":" << profit::quote(symbol) << ",\"qty\":" << qty << '}';
    }
    s << "],\"inventory_discrepancies\":["; first = true;
    for (const auto& [symbol, sides] : inventory_discrepancies_) {
        if (!first) s << ',';
        first = false; s << "{\"symbol\":" << profit::quote(symbol) << ",\"long\":" << sides[0] << ",\"short\":" << sides[1] << '}';
    }
    s << "],\"entries\":["; first = true;
    for (const auto& [key, entry] : entries_) {
        if (!first) s << ',';
        first = false; const auto& e = entry.event; const auto& r = e.result;
        s << "{\"key\":" << profit::quote(key) << ",\"bot\":" << profit::quote(e.bot_id) << ",\"symbol\":" << profit::quote(e.symbol)
          << ",\"opening\":" << e.opening << ",\"auto\":" << e.is_auto << ",\"completed\":" << e.completed << ",\"direction\":" << e.direction
          << ",\"at\":" << e.at_ms << ",\"qty\":" << e.qty << ",\"entry\":" << e.entry
          << ",\"ok\":" << r.ok << ",\"uncertain\":" << r.uncertain << ",\"order\":" << profit::quote(r.order_id)
          << ",\"client\":" << profit::quote(r.client_order_id) << ",\"price\":" << r.avg_price << ",\"executed\":" << r.executed_qty
          << ",\"settled\":" << entry.settled << ",\"net\":" << entry.net << ",\"first\":" << entry.first_fill << ",\"last\":" << entry.last_fill;
        if (e.opening && (!e.completed || r.uncertain) && e.required_margin > 0)
            s << ",\"required_margin\":" << e.required_margin;
        if (!entry.recovery_open_key.empty())
            s << ",\"recovery_open\":" << profit::quote(entry.recovery_open_key)
              << ",\"gross\":" << entry.recovery_gross << ",\"fee\":" << entry.recovery_fee;
        s << '}';
    }
    s << "],\"funding\":["; first = true;
    for (const auto& [key, f] : funding_) {
        if (!first) s << ',';
        first = false;
        s << "{\"key\":" << profit::quote(key) << ",\"net\":" << f.net << ",\"checked\":" << f.checked << '}';
    }
    s << "]}"; return s.str();
}
bool ProfitManager::save_locked() {
    if (damaged_ || cancelled_) return false;
    if (path_.empty()) { error_ = "利润账本路径为空"; return false; }
    const auto json = serialize_locked();
    if (json == last_saved_ && error_.empty()) return true;
    auto result = write_atomic_file(path_, json);
    if (!result.ok) { error_ = "利润账本保存失败：" + result.error; return false; }
    last_saved_ = json; error_.clear(); return true;
}
void ProfitManager::load() {
    try {
        if (path_.empty()) return;
        const auto path = std::filesystem::path(std::u8string(path_.begin(), path_.end()));
        std::error_code ec; const bool exists = std::filesystem::exists(path, ec);
        if (ec) throw std::runtime_error("无法检查账本文件");
        if (!exists) return;
        std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("无法读取账本");
        std::string json((std::istreambuf_iterator<char>(file)), {});
        simdjson::dom::parser p; auto o = p.parse(simdjson::padded_string(json)).value();
        using namespace profit;
        if (integer(o, "version") != 1 || string(o, "account") != account_) throw std::runtime_error("账本版本／API 账户不匹配，保留原文件");
        if (o["breadth_model"].error() != simdjson::NO_SUCH_FIELD) breadth_signature_ = string(o, "breadth_model");
        started_ = integer(o, "started"); accumulation_since_ = integer(o, "accumulation_since"); sequence_ = integer(o, "sequence");
        tracking_ = boolean(o, "tracking"); capital_floor_ = number(o, "capital_floor"); used_ = number(o, "used"); btc_qty_ = number(o, "btc_qty"); spot_cash_ = number(o, "spot_cash");
        if (capital_floor_ < 0 || used_ < 0 || btc_qty_ < 0 || spot_cash_ < 0 || (tracking_ && started_ <= 0) || sequence_ > 1000000000)
            throw std::runtime_error("账本余额／基线不合法");
        auto buy = o["purchase"].value(); auto phase = integer(buy, "phase"); if (phase > 2) throw std::runtime_error("购买状态不合法");
        purchase_.phase = static_cast<int>(phase); purchase_.sent = integer(buy, "sent"); purchase_.tag = string(buy, "tag");
        purchase_.transfer_id = string(buy, "transfer_id"); purchase_.reserved = number(buy, "reserved");
        if (purchase_.reserved < 0 || (phase != 0 && (purchase_.reserved <= 0 || purchase_.sent <= 0)) || (phase == 2 && purchase_.tag.empty()))
            throw std::runtime_error("购买预留不完整");
        auto gs = o["guards"].get_array().value(); if (gs.size() != 2) throw std::runtime_error("保护状态不完整"); size_t i = 0;
        for (auto row : gs) {
            auto& g = guards_[i]; auto stage = integer(row, "phase"); if (stage > 3) throw std::runtime_error("保护阶段不合法");
            g.phase = static_cast<Phase>(stage); g.active = boolean(row, "active"); g.armed = boolean(row, "armed");
            g.anchor = number(row, "anchor"); g.peak = number(row, "peak"); g.profit = number(row, "profit");
            g.breadth_peak = number(row, "breadth_peak"); g.breadth = number(row, "breadth");
            const auto weak = integer(row, "weak"), recovered = integer(row, "recovered");
            if (weak > 1000000 || recovered > 1000000 || g.peak < 0 || g.breadth < 0 || g.breadth > 1 || g.breadth_peak < g.breadth || g.breadth_peak > 1)
                throw std::runtime_error("保护计数不合法");
            g.weak = static_cast<int>(weak); g.recovered = static_cast<int>(recovered); g.last_bar = integer(row, "last_bar");
            g.exit_ms = integer(row, "exit_ms"); g.fresh_after = integer(row, "fresh_after"); g.reason = string(row, "reason");
            simdjson::dom::element evidence;
            if (row["exit_evidence"].get(evidence) == simdjson::SUCCESS)
                read_exit_evidence_json(simdjson::minify(evidence), g.exit_evidence);
            if (g.peak < g.profit || (stage >= 2 && (!g.active || !g.armed || g.exit_ms == 0 || g.peak <= 0)))
                throw std::runtime_error("保护阶段与峰值不一致");
            const auto symbols = row["cohort"].get_array().value();
            for (auto symbol : symbols) { auto sym = std::string(symbol.get_string().value()); if (sym.empty() || !cohorts_[i].insert(sym).second) throw std::runtime_error("广度名单重复"); }
            ++i;
        }
        if (o["breadth_absent"].error() != simdjson::NO_SUCH_FIELD) {
            const auto absent = o["breadth_absent"].get_array().value();
            for (auto row : absent) {
                const auto symbol = string(row, "symbol");
                const auto since = integer(row, "since");
                if (cohorts_[0].count(symbol) || cohorts_[1].count(symbol))
                    missing_from_market_.emplace(symbol, since);
            }
        }
        const auto inherited = o["inherited"].get_array().value();
        for (auto row : inherited) {
            const auto symbol = string(row, "symbol"); const auto qty = number(row, "qty");
            if (symbol.empty() || qty == 0 || !inherited_.emplace(symbol, qty).second) throw std::runtime_error("期初仓位不合法");
        }
        if (o["inventory_discrepancies"].error() != simdjson::NO_SUCH_FIELD) {
            const auto pending = o["inventory_discrepancies"].get_array().value();
            for (auto row : pending) {
                const auto symbol = string(row, "symbol");
                std::array<bool, 2> sides{boolean(row, "long"), boolean(row, "short")};
                if (symbol.empty() || (!sides[0] && !sides[1]) || !inventory_discrepancies_.emplace(symbol, sides).second)
                    throw std::runtime_error("库存核对状态不合法");
                for (size_t side = 0; side < sides.size(); ++side) if (sides[side]) inventory_ok_[side] = false;
            }
        }
        const auto entries = o["entries"].get_array().value();
        for (auto row : entries) {
            Entry entry; auto& e = entry.event; auto& r = e.result;
            e.key = string(row, "key"); e.bot_id = string(row, "bot"); e.symbol = string(row, "symbol");
            e.opening = boolean(row, "opening"); e.is_auto = boolean(row, "auto"); e.completed = boolean(row, "completed");
            const auto d = number(row, "direction"); if (d != 1 && d != -1) throw std::runtime_error("成交方向不合法"); e.direction = d > 0 ? 1 : -1;
            e.at_ms = integer(row, "at"); e.qty = number(row, "qty"); e.entry = number(row, "entry");
            r.ok = boolean(row, "ok"); r.uncertain = boolean(row, "uncertain"); r.order_id = string(row, "order"); r.client_order_id = string(row, "client");
            r.avg_price = number(row, "price"); r.executed_qty = number(row, "executed");
            if (row["required_margin"].error() != simdjson::NO_SUCH_FIELD) {
                e.required_margin = number(row, "required_margin");
                if (e.required_margin <= 0) throw std::runtime_error("在途开仓保证金不合法");
            }
            entry.settled = boolean(row, "settled"); entry.net = number(row, "net"); entry.first_fill = integer(row, "first"); entry.last_fill = integer(row, "last");
            if (row["recovery_open"].error() != simdjson::NO_SUCH_FIELD) {
                entry.recovery_open_key = string(row, "recovery_open");
                entry.recovery_gross = number(row, "gross"); entry.recovery_fee = number(row, "fee");
                if (entry.recovery_open_key.empty() || e.opening || !e.completed || !r.ok || r.uncertain ||
                    !entry.settled || !e.is_auto || r.executed_qty <= 0 || r.avg_price <= 0 || entry.first_fill <= 0 ||
                    r.order_id.empty() || std::fabs(entry.net - (entry.recovery_gross - entry.recovery_fee)) > 1e-8)
                    throw std::runtime_error("外部平仓补录回执不完整");
            }
            if (e.key.empty() || e.bot_id.empty() || e.symbol.empty() || e.qty <= 0 || r.executed_qty < 0 || r.avg_price < 0 ||
                entry.last_fill < entry.first_fill || !entries_.emplace(e.key, entry).second) throw std::runtime_error("成交账本不合法");
        }
        for (const auto& [key, entry] : entries_) if (!entry.recovery_open_key.empty()) {
            auto it = entries_.find(entry.recovery_open_key);
            if (it == entries_.end() || !it->second.settled || !it->second.event.opening ||
                it->second.event.symbol != entry.event.symbol || it->second.event.direction != entry.event.direction)
                throw std::runtime_error("外部平仓补录缺少对应开仓成交");
        }
        const auto funding = o["funding"].get_array().value();
        for (auto row : funding) {
            auto key = string(row, "key"); if (!entries_.count(key) || !funding_.emplace(key, Funding{number(row, "net"), integer(row, "checked")}).second)
                throw std::runtime_error("资金费归属不合法");
        }
        last_saved_ = serialize_locked(); message_ = "已恢复利润账本，等待行情与订单核对";
    } catch (const std::exception& e) { damaged_ = true; tracking_ = true; error_ = std::string("利润账本恢复失败：") + e.what(); }
}
} // namespace ccbot
