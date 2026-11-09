#include "app/profit_manager.h"
#include <algorithm>
#include <cmath>

namespace ccbot {
void ProfitManager::service(TradingClient& client, TrendEngine& engine, int64_t now) {
    std::unique_lock<std::mutex> run(service_mutex_, std::try_to_lock);
    if (!run.owns_lock() || cancelled_) return;
    std::vector<Entry> work;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!tracking_ || damaged_ || !error_.empty()) return;
        for (const auto& [key, e] : entries_) {
            if (!e.settled && (!e.event.completed || e.event.result.uncertain || e.event.result.ok)) work.push_back(e);
            if (work.size() >= 8) break;
        }
    }
    for (auto entry : work) {
        if (cancelled_) return;
        const auto before = entry.event;
        auto& e = entry.event;
        if (!e.completed || e.result.uncertain) {
            auto q = client.query_order(e.symbol, e.key);
            if (!q.terminal || q.uncertain) continue;
            e.result = {q.ok, q.order_id, q.error, q.avg_price, q.executed_qty, q.uncertain, q.client_order_id};
            e.completed = true;
        }
        std::string error;
        if (e.result.ok && e.result.executed_qty > 0) {
            std::vector<TradingClient::ProfitFill> fills;
            if (!client.profit_fills(e.symbol, e.result.order_id, e.result.executed_qty, fills, error)) {
                std::lock_guard<std::mutex> lk(mutex_); message_ = "成交待结算：" + e.symbol + " " + error; continue;
            }
            entry.net = 0; entry.first_fill = 0; entry.last_fill = 0;
            bool sides_ok = true;
            const auto side = (e.opening == (e.direction > 0)) ? "BUY" : "SELL";
            for (const auto& f : fills) {
                if (f.side != side || (e.opening && std::fabs(f.pnl) > 1e-8)) sides_ok = false;
                entry.net += f.pnl - f.fee;
                entry.first_fill = entry.first_fill == 0 ? f.time : std::min(entry.first_fill, f.time);
                entry.last_fill = std::max(entry.last_fill, f.time);
            }
            if (!sides_ok) { std::lock_guard<std::mutex> lk(mutex_); message_ = "成交方向／开仓盈亏异常，暂停分配"; continue; }
        }
        entry.settled = true;
        std::lock_guard<std::mutex> lk(mutex_);
        if (cancelled_) return;
        const auto current = entries_.find(e.key);
        if (current == entries_.end()) continue;
        const auto& latest = current->second.event;
        if (latest.completed != before.completed || latest.result.ok != before.result.ok ||
            latest.result.uncertain != before.result.uncertain || latest.result.order_id != before.result.order_id ||
            latest.result.executed_qty != before.result.executed_qty || latest.result.avg_price != before.result.avg_price)
            continue; // A receipt arrived during network I/O; settle that newer evidence next time.
        entries_[e.key] = entry; if (!save_locked()) return;
    }

    // Only complete owned position cycles contribute positive short profit. Partial closes
    // remain in the cycle until the remainder is flat; losses are never discarded.
    struct Cycle { std::string key, symbol; int direction = 0; double qty = 0; int64_t start = 0, end = 0; };
    std::vector<Cycle> cycles; std::map<std::string, Cycle> active;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<const Entry*> sorted;
        for (const auto& [key, e] : entries_) sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->event.at_ms < b->event.at_ms || (a->event.at_ms == b->event.at_ms && a->event.opening > b->event.opening); });
        for (const auto* ptr : sorted) {
            const auto& e = ptr->event;
            if (!ptr->settled || !e.result.ok || e.result.executed_qty <= 0) continue;
            if (e.opening) active[e.symbol] = {e.key, e.symbol, e.direction, e.result.executed_qty, ptr->first_fill, 0};
            else {
                auto it = active.find(e.symbol); if (it == active.end()) continue;
                auto& cycle = it->second; cycle.qty -= e.result.executed_qty; cycle.end = ptr->last_fill;
                if (cycle.qty <= 1e-8) { cycles.push_back(cycle); active.erase(it); }
            }
        }
    }
    // Do not use funding from hedge-mode positions: the income record does not identify a leg.
    if (!client.is_dual_mode()) {
        int checked = 0;
        for (const auto& cycle : cycles) {
            if (now - cycle.end < 300000 || cancelled_) continue;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                const auto it = funding_.find(cycle.key);
                if (it != funding_.end() && (now - it->second.checked < 3600000 || now - cycle.end > 7LL * 86400000)) continue;
            }
            double amount = 0; std::string error;
            std::set<std::string> owned_orders;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (const auto& [key, entry] : entries_) if (entry.event.symbol == cycle.symbol &&
                    entry.first_fill >= cycle.start && entry.last_fill <= cycle.end && !entry.event.result.order_id.empty())
                    owned_orders.insert(entry.event.result.order_id);
            }
            if (client.profit_owned_window(cycle.symbol, cycle.start, cycle.end, owned_orders, error) &&
                client.profit_funding(cycle.symbol, cycle.start, cycle.end, amount, error)) {
                std::lock_guard<std::mutex> lk(mutex_); funding_[cycle.key] = {amount, now}; if (!save_locked()) return;
            } else { std::lock_guard<std::mutex> lk(mutex_); funding_.erase(cycle.key); message_ = "持仓结算待核对：" + error; save_locked(); }
            if (++checked >= 4) break;
        }
    }

    Purchase pending;
    {
        std::lock_guard<std::mutex> lk(mutex_); if (cancelled_ || damaged_ || !error_.empty()) return; pending = purchase_;
    }
    if (pending.phase == 1) {
        const auto result = client.query_profit_transfer(pending.reserved, pending.sent, pending.transfer_id);
        std::lock_guard<std::mutex> lk(mutex_);
        if (result.ok) { spot_cash_ += pending.reserved; purchase_ = {}; message_ = "利润已转入现货，等待购买条件"; save_locked(); }
        else message_ = result.error;
        return;
    }
    if (pending.phase == 2) {
        auto result = client.query_btc_profit(pending.tag);
        std::lock_guard<std::mutex> lk(mutex_); if (!cancelled_) finish_purchase_locked(result); return;
    }

    bool monitor_btc = false;
    { std::lock_guard<std::mutex> lk(mutex_); monitor_btc = btc_qty_ > 0; }
    if (monitor_btc) {
        const auto quote = client.btc_spot_quote();
        std::lock_guard<std::mutex> lk(mutex_); btc_price_ = quote.ok ? quote.ask : 0;
    }

    profit::Config cfg; double amount = 0, cash_spot = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!smart_enabled_ || !cfg_.accumulate || cancelled_) return;
        cfg = cfg_;
        if (cfg_.protect && (guards_[0].phase == profit::Phase::Exiting || guards_[1].phase == profit::Phase::Exiting)) { message_ = "组合保护退出优先，暂缓利润分配"; return; }
        double shorts, total; bool complete; totals_locked(shorts, total, complete);
        if (!complete) { message_ = "等待成交与资金费结算"; return; }
        amount = profit::entitlement(shorts, total, used_, 0, cfg.allocation); cash_spot = spot_cash_;
    }
    if (engine.pause_new()) { std::lock_guard<std::mutex> lk(mutex_); message_ = "暂停开仓期间暂停新增 BTC 购买"; return; }
    const auto cash = client.profit_cash();
    if (!cash.ok) { std::lock_guard<std::mutex> lk(mutex_); message_ = cash.error; return; }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (capital_floor_ == 0) { capital_floor_ = cash.equity; if (!save_locked()) return; }
        const double floor = cfg.capital_floor > 0 ? cfg.capital_floor : capital_floor_;
        // Earmarked cash already outside futures needs no second deduction from equity.
        const double transfer_room = std::max(0.0, std::min({cash.transferable, cash.available * .90, cash.equity - floor}));
        amount = std::min({amount, cfg.max_batch_usdt, cash_spot + transfer_room});
        if (cash.equity < floor) { message_ = "合约权益低于保留额，暂停购买"; return; }
    }
    if (amount + 1e-8 < cfg.batch_usdt) { std::lock_guard<std::mutex> lk(mutex_); message_ = "利润预算／安全资金不足最低购买批次"; return; }
    // Fresh exchange snapshot plus the local ownership journal; untracked changes stop spending.
    bool positions_ok = false; const auto positions = client.fetch_positions(&positions_ok);
    const auto bots = engine.get_bots();
    if (!positions_ok) return;
    for (const auto& b : bots) {
        if (b.pending || b.open_uncertain || !b.pending_close_key.empty() || b.needs_reconcile) { std::lock_guard<std::mutex> lk(mutex_); message_ = "在途交易／持仓核对优先"; return; }
    }
    for (const auto& [bot_id, cycle] : active) {
        double actual = 0;
        for (const auto& p : positions) if (p.symbol == cycle.symbol && p.direction == cycle.direction) actual += p.qty;
        if (std::fabs(actual - cycle.qty) > std::max(1e-8, cycle.qty * 1e-8)) {
            std::lock_guard<std::mutex> lk(mutex_); message_ = cycle.symbol + " 有未归属的仓位变化，暂停购买"; return;
        }
    }
    if (cfg.wait_btc_up) {
        following::Config model; model.enabled = true; model.close_confirm = true;
        const auto signal = client.fetch_trend_signal("BTCUSDT", "1h", 20, 14, model);
        if (!signal.ok || signal.bar_open_ms != now / 3600000 * 3600000 || !signal.following.ready || signal.following.regime != following::Regime::Up) {
            std::lock_guard<std::mutex> lk(mutex_); message_ = "等待 BTC 1h 收盘上涨趋势许可"; return;
        }
    }
    const auto quote = client.btc_spot_quote();
    if (!quote.ok) { std::lock_guard<std::mutex> lk(mutex_); message_ = quote.error; return; }
    // Caller holds mutex_; like evaluate(), take engine snapshots in this order.
    // Network waits must not carry an old pause/config/ownership decision into a POST.
    const auto spending_blocked = [&] {
        if (!(cfg_ == cfg)) { message_ = "利润配置已改变，等待按新参数重新评估"; return true; }
        if (engine.pause_new()) { message_ = "暂停开仓期间暂停新增 BTC 购买"; return true; }
        for (const auto& b : engine.get_bots()) {
            if (b.pending || b.open_uncertain || !b.pending_close_key.empty() || b.needs_reconcile) {
                message_ = "在途交易／持仓核对优先"; return true;
            }
        }
        return false;
    };
    {
        std::lock_guard<std::mutex> lk(mutex_); btc_price_ = quote.ask;
        if (!smart_enabled_ || !cfg_.accumulate || cancelled_ || !error_.empty() || purchase_.phase != 0) return;
        if (spending_blocked()) return;
        // Recompute after network I/O: a newly settled loss must shrink unused entitlement.
        double shorts, total; bool complete; totals_locked(shorts, total, complete);
        if (!complete) return;
        amount = std::min(amount, profit::entitlement(shorts, total, used_, 0, cfg_.allocation));
        if (amount < cfg_.batch_usdt) return;
    }
    if (cash_spot + 1e-8 < amount) {
        // Transfer only the shortfall. The intent is durable BEFORE the non-idempotent POST.
        const double transfer = std::floor((amount - cash_spot) * 100000000) / 100000000;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            double shorts, total; bool complete; totals_locked(shorts, total, complete);
            if (cancelled_ || !smart_enabled_ || !cfg_.accumulate || !complete || purchase_.phase != 0 ||
                !error_.empty() || spending_blocked() ||
                (cfg_.protect && (guards_[0].phase == profit::Phase::Exiting || guards_[1].phase == profit::Phase::Exiting)) ||
                profit::entitlement(shorts, total, used_, 0, cfg_.allocation) + 1e-8 < amount) return;
            purchase_.phase = 1; purchase_.sent = client.market_time_ms(); purchase_.reserved = transfer;
            if (!save_locked()) return;
        }
        const auto result = client.transfer_profit(transfer);
        std::lock_guard<std::mutex> lk(mutex_);
        if (cancelled_) return;
        if (result.ok) { spot_cash_ += transfer; purchase_ = {}; message_ = "利润已转入现货，下一次核验后买入 BTC"; }
        else if (result.rejected) { purchase_ = {}; message_ = "划转被拒：" + result.error; }
        else message_ = "划转待核对，不重复提交：" + result.error;
        if (!result.ok && !result.rejected) purchase_.transfer_id = result.id;
        save_locked(); return;
    }
    // A capped IOC avoids unbounded market slippage. Keep 1% of this batch for fees/dust.
    const double limit = std::floor(quote.ask * (1 + cfg.slippage_bps / 10000) / quote.tick) * quote.tick;
    const double qty = std::floor(amount * .99 / limit / quote.step) * quote.step;
    if (limit < quote.ask || qty < quote.min_qty || qty * limit < quote.min_notional) {
        std::lock_guard<std::mutex> lk(mutex_); message_ = "等待满足 BTC 现货最小下单量／金额"; return;
    }
    std::string tag;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        double shorts, total; bool complete; totals_locked(shorts, total, complete);
        if (!smart_enabled_ || !cfg_.accumulate || cancelled_ || !complete || purchase_.phase != 0 ||
            !error_.empty() || spending_blocked() ||
            (cfg_.protect && (guards_[0].phase == profit::Phase::Exiting || guards_[1].phase == profit::Phase::Exiting)) ||
            profit::entitlement(shorts, total, used_, 0, cfg_.allocation) + 1e-8 < amount) return;
        tag = "tbpbtc" + std::to_string(started_) + "_" + std::to_string(++sequence_);
        purchase_ = {2, client.market_time_ms(), tag, {}, amount}; if (!save_locked()) return;
    }
    auto result = client.buy_btc_profit(qty, limit, tag);
    std::lock_guard<std::mutex> lk(mutex_); if (!cancelled_) finish_purchase_locked(result);
}
} // namespace ccbot
