#include "app/profit_manager.h"
#include <algorithm>
#include <cmath>
#include <span>

namespace ccbot {
namespace {
struct Receipt {
    ExecutionEvent e;
    int64_t first = 0, last = 0;
    double net = 0;
};
struct Cycle {
    Receipt opening;
    std::map<std::string, Receipt> closes;
    double remaining = 0;
};
struct Fills {
    double qty = 0, value = 0, gross = 0, fee = 0;
    int64_t first = 0, last = 0;
    void add(const TradingClient::HistoryFill& f) {
        qty += f.qty; value += f.price * f.qty; gross += f.pnl; fee += f.fee;
        if (first == 0) first = f.time;
        last = f.time;
    }
};
struct RecoveredCycle {
    double entry_price = 0;
    std::map<std::string, Fills> missing;
};
bool same_qty(double a, double b) {
    return std::fabs(a - b) <= std::max(1e-10, std::max(std::fabs(a), std::fabs(b)) * 1e-11);
}
bool matches(const Fills& fills, const Receipt& receipt) {
    // Order average prices are rounded by the exchange. Compare against the full
    // fill-weighted amount, with only a small allowance for quote precision.
    const double reported = receipt.e.result.avg_price;
    const double price_tolerance = std::max(reported * 1e-6, std::min(1e-5, reported * 0.001));
    return same_qty(fills.qty, receipt.e.result.executed_qty) && fills.first == receipt.first &&
        fills.last == receipt.last && std::fabs(fills.gross - fills.fee - receipt.net) < 1e-7 &&
        fills.qty > 0 && std::fabs(fills.value / fills.qty - reported) <= price_tolerance;
}
bool local_flat(const std::vector<TrendBot>& bots, const std::string& symbol) {
    for (const auto& b : bots) if (b.cfg.symbol == symbol &&
        (b.st.pos != trend::Pos::Flat || b.qty != 0 || b.pending || b.open_uncertain || b.needs_reconcile ||
         b.rotation_reserved || !b.pending_close_key.empty() || b.reserved_dir != trend::Pos::Flat)) return false;
    return true;
}
// Each cycle starts at its owned opening order and must be fully closed
// before the next recorded opening, including a change of direction.
std::string prove_cycle(const Cycle& cycle, std::span<const TradingClient::HistoryFill> history,
                        int64_t before_flat_query, bool hedge, RecoveredCycle& result) {
    const auto& open = cycle.opening;
    const bool short_side = open.e.direction < 0;
    const auto position_side = hedge ? (short_side ? "SHORT" : "LONG") : "BOTH";
    Fills opening;
    std::map<std::string, Fills> closes;
    double covered = 0;
    for (const auto& f : history) {
        if (f.position_side != position_side || f.time > before_flat_query)
            return "仓位方向混用或查询期间有成交";
        if (f.order_id == open.e.result.order_id) {
            if (f.side != (short_side ? "SELL" : "BUY") || std::fabs(f.pnl) > 1e-8 || !closes.empty()) return "开仓成交方向或顺序不符";
            opening.add(f);
        } else {
            if (f.side != (short_side ? "BUY" : "SELL") || opening.qty <= 0 || !same_qty(opening.qty, open.e.result.executed_qty))
                return "原开仓未完整确认或夹有其他开仓";
            const double expected = open.e.direction * f.qty * (f.price - opening.value / opening.qty);
            if (std::fabs(expected - f.pnl) > std::max(1e-6, opening.value / opening.qty * f.qty * 1e-6))
                return "交易所平仓盈亏与本仓成本不符";
            covered += f.qty;
            if (covered > opening.qty && !same_qty(covered, opening.qty)) return "平仓数量超过原仓位";
            closes[f.order_id].add(f);
        }
    }
    if (!matches(opening, open) || !same_qty(covered, opening.qty)) return "开仓锚点或完整平仓数量不符";
    for (const auto& [id, receipt] : cycle.closes) {
        const auto it = closes.find(id);
        if (it == closes.end() || !matches(it->second, receipt)) return "已记部分平仓与成交历史不符";
        closes.erase(it);
    }
    double recovered = 0;
    for (const auto& [id, order] : closes) recovered += order.qty;
    if (!same_qty(recovered, cycle.remaining)) return "缺失回执的数量不能完整核实";
    result.entry_price = opening.value / opening.qty;
    result.missing = std::move(closes);
    return {};
}
std::string prove_history(const std::vector<Cycle>& cycles, std::span<const TradingClient::HistoryFill> history,
                          int64_t before_flat_query, bool hedge, std::vector<RecoveredCycle>& result) {
    for (size_t i = 0; i < cycles.size(); ++i) {
        size_t count = history.size();
        if (i + 1 < cycles.size()) {
            const auto& next_order = cycles[i + 1].opening.e.result.order_id;
            const auto next = std::find_if(history.begin(), history.end(), [&](const auto& f) { return f.order_id == next_order; });
            if (next == history.end()) return "后续开仓订单的成交未能核实";
            count = static_cast<size_t>(next - history.begin());
        }
        auto error = prove_cycle(cycles[i], history.first(count), before_flat_query, hedge, result[i]);
        if (!error.empty()) return error;
        history = history.subspan(count);
    }
    return {};
}
} // namespace

void ProfitManager::recover_external_close(TradingClient& client, TrendEngine& engine, int64_t now) {
    std::vector<Cycle> target;
    std::string symbol;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!tracking_ || damaged_ || cancelled_ || !error_.empty()) return;
        if (inventory_discrepancies_.empty()) {
            recovery_wait_.clear(); recovery_checks_.clear();
            return;
        }
        const auto bots = engine.get_bots();
        std::map<std::string, std::vector<Cycle>> cycles;
        std::set<std::string> ambiguous;
        std::vector<const Entry*> events;
        for (const auto& [key, item] : entries_) {
            const auto& e = item.event;
            if (!e.completed || e.result.uncertain || (e.result.ok && e.result.executed_qty > 0 && !item.settled))
                ambiguous.insert(e.symbol);
            if (e.completed && e.result.ok && e.result.executed_qty > 0 && item.settled) events.push_back(&item);
        }
        std::sort(events.begin(), events.end(), [](auto a, auto b) {
            if (a->first_fill != b->first_fill) return a->first_fill < b->first_fill;
            return a->event.opening > b->event.opening;
        });
        for (const auto* item : events) {
            const auto& e = item->event;
            if (inherited_.count(e.symbol) || ambiguous.count(e.symbol)) continue;
            auto& series = cycles[e.symbol];
            Receipt receipt{e, item->first_fill, item->last_fill, item->net};
            if (!e.is_auto || e.result.order_id.empty() || item->first_fill <= 0) { ambiguous.insert(e.symbol); continue; }
            if (e.opening) {
                series.push_back({std::move(receipt), {}, e.result.executed_qty});
            } else {
                if (series.empty()) { ambiguous.insert(e.symbol); continue; }
                auto& cycle = series.back();
                if (cycle.opening.e.direction != e.direction || !cycle.closes.emplace(e.result.order_id, std::move(receipt)).second) {
                    ambiguous.insert(e.symbol); continue;
                }
                cycle.remaining -= e.result.executed_qty;
                if (same_qty(cycle.remaining, 0)) {
                    cycle.remaining = 0;
                    // Closed cycles before the first gap are outside the recovery window.
                    if (series.size() == 1) cycles.erase(e.symbol);
                } else if (cycle.remaining < 0) ambiguous.insert(e.symbol);
            }
        }
        // A late normal receipt can resolve a mismatch while a retry was waiting.
        // Obsolete failure messages should disappear with the open cycle.
        for (auto it = recovery_wait_.begin(); it != recovery_wait_.end();) {
            if (!cycles.count(it->first) && !ambiguous.count(it->first)) it = recovery_wait_.erase(it);
            else ++it;
        }
        int64_t oldest = 0;
        for (const auto& [sym, series] : cycles) {
            const auto mismatch = inventory_discrepancies_.find(sym);
            if (series.empty() || mismatch == inventory_discrepancies_.end() ||
                !mismatch->second[series.front().opening.e.direction > 0 ? 0 : 1] ||
                ambiguous.count(sym) || !local_flat(bots, sym)) continue;
            auto check = recovery_checks_.find(sym);
            const int64_t previous = check == recovery_checks_.end() ? 0 : check->second.last;
            if (previous && now >= previous && now < check->second.next) continue;
            if (symbol.empty() || previous < oldest) { symbol = sym; oldest = previous; target = series; }
        }
        if (symbol.empty()) return;
        revision = receipt_revision_[symbol];
        recovery_checks_[symbol] = {now, now + 30 * 60000};
    }
    std::vector<RecoveredCycle> recovered(target.size());
    std::string error;
    bool retry_soon = false;
    const auto before = client.market_time_ms();
    if (client.profit_confirm_flat(symbol, error, &retry_soon)) {
        const bool hedge = client.is_dual_mode();
        std::vector<TradingClient::HistoryFill> history;
        if (client.profit_history(symbol, target.front().opening.first, client.market_time_ms(), history, error, &retry_soon))
            error = prove_history(target, history, before, hedge, recovered);
        // Pagination may take time. A manual re-entry or account-mode change
        // while fetching fills invalidates the original flat snapshot.
        if (error.empty() && (!client.profit_confirm_flat(symbol, error, &retry_soon) || client.is_dual_mode() != hedge)) {
            if (error.empty()) error = "成交核对期间账户持仓模式发生变化";
        }
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (cancelled_ || damaged_ || !error_.empty()) return;
    if (revision != receipt_revision_[symbol] || inherited_.count(symbol) || !local_flat(engine.get_bots(), symbol)) {
        recovery_checks_[symbol].next = now + 60000;
        return;
    }
    if (recovered.front().missing.empty() || !error.empty()) {
        recovery_wait_[symbol] = error.empty() ? "未取得完整成交" : error;
        if (retry_soon) recovery_checks_[symbol].next = now + 60000;
        return;
    }
    std::map<std::string, Entry> corrections;
    std::set<std::string> known_orders;
    for (const auto& [key, item] : entries_)
        if (item.event.symbol == symbol) known_orders.insert(item.event.result.order_id);
    for (size_t i = 0; i < target.size(); ++i) {
        const auto& opening = target[i].opening.e;
        for (const auto& [order_id, fill] : recovered[i].missing) {
            const auto key = "history/" + symbol + "/" + order_id;
            // Refuse an order already in the ledger or shared by two cycles.
            if (entries_.count(key) || !known_orders.insert(order_id).second) {
                recovery_wait_[symbol] = "成交编号与其他回执冲突"; return;
            }
            Entry item;
            item.event = {key, opening.bot_id, symbol, false, true, true, opening.direction, fill.last,
                          fill.qty, recovered[i].entry_price,
                          {true, order_id, {}, fill.value / fill.qty, fill.qty, false, {}}};
            item.settled = true; item.net = fill.gross - fill.fee;
            item.first_fill = fill.first; item.last_fill = fill.last;
            item.recovery_open_key = opening.key;
            item.recovery_gross = fill.gross; item.recovery_fee = fill.fee;
            corrections.emplace(key, std::move(item));
        }
    }
    // Commit the verified cycles together, including invalidating any provisional
    // funding amount. The normal funding service verifies the final full window.
    std::map<std::string, Funding> previous_funding;
    for (size_t i = 0; i < target.size(); ++i) if (!recovered[i].missing.empty()) {
        auto prior = funding_.extract(target[i].opening.e.key);
        if (prior) previous_funding.insert(std::move(prior));
    }
    entries_.insert(corrections.begin(), corrections.end());
    if (!save_locked()) {
        for (const auto& [key, item] : corrections) entries_.erase(key);
        funding_.merge(previous_funding);
        return;
    }
    recovery_wait_.erase(symbol);
    recovery_checks_.erase(symbol);
    for (size_t i = 0; i < target.size(); ++i) {
        const auto& missing = recovered[i].missing;
        if (missing.empty()) continue;
        double qty = 0, value = 0;
        for (const auto& item : missing) { qty += item.second.qty; value += item.second.value; }
        const auto side = target[i].opening.e.direction > 0 ? " 外部平多" : " 外部平空";
        notices_.push_back(symbol + side + "已核对并补录 " + std::to_string(missing.size()) +
            " 笔平仓委托，" + format_exit_evidence({}, value / qty, "外部平仓（具体规则未记录）"));
    }
}

std::vector<TrendTrade> ProfitManager::recovered_trades() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<std::string, TrendTrade> cycles;
    if (damaged_) return {};
    for (const auto& [key, entry] : entries_) {
        if (entry.recovery_open_key.empty()) continue;
        const auto& open = entries_.at(entry.recovery_open_key);
        auto& t = cycles[entry.recovery_open_key];
        t.symbol = entry.event.symbol; t.is_auto = true;
        t.side = open.event.direction > 0 ? trend::Pos::Long : trend::Pos::Short;
        t.reason = t.side == trend::Pos::Long ? "外部平多（交易所成交已核对）" : "外部平空（交易所成交已核对）";
        t.exit_evidence.rule = "外部平仓（具体规则未记录）";
        t.entry_price = entry.event.entry;
        using Time = std::chrono::system_clock::time_point;
        t.open_time = Time(std::chrono::milliseconds(open.first_fill));
        t.close_time = std::max(t.close_time, Time(std::chrono::milliseconds(entry.last_fill)));
        t.qty += entry.event.result.executed_qty;
        t.exit_price += entry.event.result.executed_qty * entry.event.result.avg_price;
        t.pnl += entry.recovery_gross;
    }
    std::vector<TrendTrade> result;
    for (auto& [key, t] : cycles) { t.exit_price /= t.qty; result.push_back(std::move(t)); }
    return result;
}
} // namespace ccbot
