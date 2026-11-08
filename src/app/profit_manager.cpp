#include "app/profit_manager.h"
#include "core/atomic_file.h"
#include "core/profit_json.h"
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace ccbot {
namespace {
size_t slot(int direction) { return direction > 0 ? 0 : 1; }
std::string phase_name(profit::Phase p) {
    switch (p) {
        case profit::Phase::Watching: return "跟踪";
        case profit::Phase::Caution: return "暂停新增";
        case profit::Phase::Exiting: return "保护退出中";
        case profit::Phase::Waiting: return "等待广度恢复";
    }
    return "未知";
}
bool holding(const TrendBot& b) { return b.qty > 0 && b.st.pos != trend::Pos::Flat; }
}
ProfitManager::ProfitManager(std::string path, std::string account) : path_(std::move(path)), account_(std::move(account)) { load(); }
void ProfitManager::configure(profit::Config cfg, bool enabled, int64_t now) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!profit::valid(cfg)) { error_ = "利润配置不合法"; return; }
    cfg_ = cfg; smart_enabled_ = enabled;
    if (enabled && (cfg.protect || cfg.accumulate) && !tracking_) { tracking_ = true; started_ = now; }
    if (enabled && cfg.accumulate && accumulation_since_ == 0) accumulation_since_ = now;
    if (tracking_) save_locked();
}
bool ProfitManager::record(ExecutionEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!tracking_ || !e.is_auto) return true;
    if (cancelled_ || damaged_) return false;
    if (e.symbol.empty() || e.bot_id.empty() || e.at_ms <= 0 || (e.direction != 1 && e.direction != -1) ||
        !std::isfinite(e.qty) || e.qty <= 0 || !std::isfinite(e.entry) || e.entry < 0 ||
        !std::isfinite(e.result.executed_qty) || e.result.executed_qty < 0 ||
        !std::isfinite(e.result.avg_price) || e.result.avg_price < 0 ||
        (e.completed && e.result.ok && e.result.executed_qty > 0 && e.result.avg_price <= 0)) {
        error_ = "成交回执数值不合法，保留原账本等待核对"; return false;
    }
    if (e.opening && !e.completed && purchase_.phase != 0) return false;
    if (e.key.empty()) {
        // The durable sequence is also the clientOrderId. Never recycle it after restart.
        e.key = "tbp" + std::to_string(started_) + "_" + std::to_string(++sequence_);
    }
    auto& entry = entries_[e.key];
    // Exchange-stop evidence can be delivered more than once after reconnection.
    if (e.completed && !e.result.order_id.empty()) {
        for (const auto& [key, old] : entries_) if (key != e.key && old.event.symbol == e.symbol &&
            old.event.result.order_id == e.result.order_id && old.event.completed) { entries_.erase(e.key); return save_locked(); }
    }
    // A late execution receipt can follow an earlier "order not found" query.
    // Never retain that query's settled flag or zero net amount for the real fill.
    entry = {}; entry.event = e;
    return save_locked();
}

void ProfitManager::evaluate(TrendEngine& engine, int64_t now) {
    std::array<profit::Guard, 2> guards;
    std::string error;
    bool enabled = false;
    std::array<bool, 2> inventory_ok{true, true};
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (cancelled_) return;
        // Exclude concurrent receipt updates while reading holdings. Otherwise a closed
        // position's old floating P&L and its new realized P&L can fabricate a double peak.
        const auto bots = engine.get_bots();
        enabled = smart_enabled_ && cfg_.protect && !damaged_;
        if (tracking_ && !damaged_) {
            std::array<double, 2> realized{}, floating{}, entry_cost{};
            std::array<bool, 2> exposed{}, prices_ok{true, true};
            auto expected = inherited_;
            std::set<std::string> recorded;
            for (const auto& [key, entry] : entries_) {
                const auto& e = entry.event; auto i = slot(e.direction);
                recorded.insert(e.symbol);
                if (!e.completed || e.result.uncertain) { prices_ok[i] = false; continue; }
                if (!e.result.ok || e.result.executed_qty <= 0) continue;
                expected[e.symbol] += e.result.executed_qty * e.direction * (e.opening ? 1 : -1);
                const double fee = e.result.executed_qty * e.result.avg_price * TrendEngine::kEstFeeRate;
                if (entry.settled) realized[i] += entry.net;
                else realized[i] += (e.opening ? 0 : (e.result.avg_price - e.entry) * e.result.executed_qty * e.direction) - fee;
            }
            for (const auto& [key, f] : funding_) {
                auto it = entries_.find(key); if (it != entries_.end()) realized[slot(it->second.event.direction)] += f.net;
            }
            const auto steady = std::chrono::steady_clock::now();
            std::map<std::string, const TrendBot*> by_symbol;
            std::map<std::string, double> actual;
            for (const auto& b : bots) if (b.is_auto) {
                auto& candidate = by_symbol[b.cfg.symbol];
                if (!candidate || b.state != TrendBot::State::Stopped || holding(b)) candidate = &b;
                if (b.pending || b.open_uncertain || b.needs_reconcile) {
                    const auto direction = b.st.pos != trend::Pos::Flat ? b.st.pos : b.reserved_dir;
                    if (direction != trend::Pos::Flat) { exposed[direction == trend::Pos::Long ? 0 : 1] = true; prices_ok[direction == trend::Pos::Long ? 0 : 1] = false; }
                }
                if (!holding(b)) continue;
                const auto i = b.st.pos == trend::Pos::Long ? 0U : 1U; exposed[i] = true;
                actual[b.cfg.symbol] += b.qty * (i == 0 ? 1 : -1);
                entry_cost[i] += b.st.entry_price * b.qty * TrendEngine::kEstFeeRate;
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(steady - b.price_time).count();
                if (!std::isfinite(b.current_price) || b.current_price <= 0 || age < 0 || age > 30 || b.needs_reconcile) { prices_ok[i] = false; continue; }
                floating[i] += (b.current_price - b.st.entry_price) * b.qty * (i == 0 ? 1 : -1) - b.current_price * b.qty * TrendEngine::kEstFeeRate;
            }
            // Capture inherited holdings only once. Thereafter every quantity change must
            // have a receipt, including across process restarts or bot-ID reassignment.
            for (const auto& [symbol, qty] : actual) if (!recorded.count(symbol) && !inherited_.count(symbol)) {
                inherited_[symbol] = qty; expected[symbol] = qty;
            }
            for (const auto& [symbol, qty] : actual) { (void)qty; expected.try_emplace(symbol, 0); }
            for (const auto& [symbol, qty] : expected) {
                const double present = actual[symbol];
                if (std::fabs(present - qty) <= std::max({1e-8, std::fabs(qty) * 1e-8, std::fabs(present) * 1e-8})) continue;
                for (const double signed_qty : {qty, present}) if (std::fabs(signed_qty) > 1e-8) {
                    const auto i = slot(signed_qty > 0 ? 1 : -1);
                    prices_ok[i] = inventory_ok[i] = false; exposed[i] = true;
                }
            }
            inventory_ok_ = inventory_ok;
            const auto bar = now / 3600000 * 3600000;
            for (size_t i = 0; i < 2; ++i) {
                auto& g = guards_[i]; auto& cohort = cohorts_[i];
                const auto previous_phase = g.phase;
                if (enabled && !g.active && exposed[i] && prices_ok[i]) {
                    g.active = true; g.anchor = realized[i] + entry_cost[i];
                    cohort.clear();
                }
                if (enabled && cohort.empty() && g.active) {
                    for (const auto& [sym, b] : by_symbol) if (!b->retiring && b->state != TrendBot::State::Stopped && b->cfg.interval == "1h") cohort.insert(sym);
                    if (cohort.size() < 10) cohort.clear(); // Wait for the first successful candidate scan.
                }
                int ready = 0, support = 0;
                for (const auto& sym : cohort) {
                    const auto it = by_symbol.find(sym); if (it == by_symbol.end()) continue;
                    const auto& b = *it->second;
                    if (!b.sig_ok || b.monitor.recovering || b.bar_open_ms != bar || !b.following.ready || b.cfg.interval != "1h") continue;
                    ++ready;
                    if (b.following.regime == (i == 0 ? following::Regime::Up : following::Regime::Down)) ++support;
                }
                // Fixed denominator: missing data cannot masquerade as weakening breadth.
                const bool coverage = cohort.size() >= 10 && ready * 10 >= static_cast<int>(cohort.size()) * 9;
                auto guard_config = cfg_; guard_config.protect = enabled;
                g.update(guard_config, now, realized[i] + floating[i], prices_ok[i], exposed[i], bar,
                    cohort.empty() ? 0 : static_cast<double>(support) / static_cast<double>(cohort.size()), coverage && enabled,
                    cohort.empty() ? 1 : static_cast<double>(support + static_cast<int>(cohort.size()) - ready) / static_cast<double>(cohort.size()));
                if (previous_phase != profit::Phase::Waiting && g.phase == profit::Phase::Waiting) {
                    // A completed exit starts a new recovery cohort; confirmation counts restart.
                    cohort.clear(); g.last_bar = bar; g.recovered = 0;
                    for (const auto& [sym, b] : by_symbol) if (!b->retiring && b->cfg.interval == "1h") cohort.insert(sym);
                    if (cohort.size() < 10) cohort.clear();
                }
                if (g.phase != previous_phase) notices_.push_back(std::string("组合利润保护：") + (i == 0 ? "多头 " : "空头 ") + phase_name(g.phase) + "；" + g.reason);
            }
            save_locked();
        }
        guards = guards_; error = error_;
    }
    // Persistence errors block new risk but never disable any existing stop.
    engine.set_persistence_error("profit", error);
    const bool closing = enabled && (guards[0].phase == profit::Phase::Exiting || guards[1].phase == profit::Phase::Exiting);
    for (size_t i = 0; i < 2; ++i) {
        const auto direction = i == 0 ? trend::Pos::Long : trend::Pos::Short;
        const auto after = std::max(guards[i].fresh_after, guards[1 - i].exit_ms);
        engine.set_profit_gate(direction, closing ? "组合保护退出中，暂停智能新增风险" :
            (enabled && !inventory_ok[i] ? "仓位与成交账本不一致，暂停该方向新增风险，保留单币止损" :
            (enabled && guards[i].blocked() ? guards[i].reason : "")), enabled ? after : 0);
        if (enabled && guards[i].phase == profit::Phase::Exiting) engine.close_auto_direction(direction, guards[i].reason);
    }
}

void ProfitManager::totals_locked(double& shorts, double& overall, bool& complete) const {
    shorts = overall = 0; complete = !damaged_ && error_.empty();
    struct Lot { double qty = 0, net = 0; int direction = 0; int64_t opened = 0; std::string key; bool eligible = false; };
    std::map<std::string, Lot> lots;
    std::vector<const Entry*> sorted;
    for (const auto& [key, e] : entries_) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->event.at_ms < b->event.at_ms || (a->event.at_ms == b->event.at_ms && a->event.opening > b->event.opening); });
    for (const auto* ptr : sorted) {
        const auto& e = ptr->event; const auto& result = e.result;
        if (!e.completed || result.uncertain) { complete = false; continue; }
        if (!result.ok || result.executed_qty <= 0) continue;
        if (!ptr->settled) { complete = false; continue; }
        if (e.at_ms >= accumulation_since_) overall += ptr->net;
        auto& lot = lots[e.symbol];
        if (e.opening) {
            if (lot.qty > 1e-9) complete = false;
            lot = {result.executed_qty, ptr->net, e.direction, ptr->first_fill, e.key, e.at_ms >= accumulation_since_};
        } else if (lot.qty > 0) {
            if (e.direction != lot.direction || result.executed_qty > lot.qty + 1e-8) { complete = false; continue; }
            lot.qty -= result.executed_qty; lot.net += ptr->net;
            if (lot.qty < 1e-8 && lot.eligible) {
                const auto f = funding_.find(lot.key);
                if (f == funding_.end() || f->second.checked < ptr->last_fill) { complete = false; continue; }
                overall += f->second.net;
                if (lot.direction < 0) shorts += lot.net + f->second.net;
            }
        } else if (e.direction < 0 && e.at_ms >= accumulation_since_) {
            // An inherited position is not an eligible source; its losses still reduce the global cap.
        }
    }
    // Open short losses/fees must not be ignored while harvesting other winning shorts.
    for (const auto& [bot, lot] : lots) if (lot.qty > 1e-8 && lot.eligible && lot.direction < 0) shorts += std::min(0.0, lot.net);
}

void ProfitManager::finish_purchase_locked(const TradingClient::SpotPurchase& result) {
    if (result.rejected) { purchase_ = {}; message_ = "BTC 订单被拒，预算保留：" + result.error; save_locked(); return; }
    if (!result.ok || !result.terminal) { message_ = "BTC 订单待核对：" + result.error; return; }
    if (result.cost < 0 || result.qty < 0 || result.cost > spot_cash_ + 1e-7 || result.cost > purchase_.reserved + 1e-7) {
        damaged_ = true; error_ = "BTC 实际成交超出预留预算，暂停自动分配"; return;
    }
    used_ += result.cost; btc_qty_ += result.qty; spot_cash_ = std::max(0.0, spot_cash_ - result.cost);
    purchase_ = {}; message_ = "BTC 现货成交已核对，保留持币"; save_locked();
    std::ostringstream notice;
    notice << message_ << "，成本 " << std::fixed << std::setprecision(2) << result.cost << " U，净增加 BTC " << std::setprecision(8) << result.qty;
    notices_.push_back(notice.str());
}

std::vector<std::string> ProfitManager::take_notices() {
    std::lock_guard<std::mutex> lk(mutex_); auto result = std::move(notices_); notices_.clear(); return result;
}

std::string ProfitManager::status() const {
    std::lock_guard<std::mutex> lk(mutex_); std::ostringstream s; s << std::fixed << std::setprecision(2);
    if (!tracking_) return "组合利润保护／BTC 积累：未启用";
    for (size_t i = 0; i < 2; ++i) {
        const auto& g = guards_[i];
        s << (i == 0 ? "多头" : "空头") << "：" << phase_name(g.phase) << "，本轮 " << g.profit << "U / 峰值 " << g.peak
          << "U，最近有效广度下界 " << g.breadth * 100 << "%（固定候选 " << cohorts_[i].size() << "）\n";
        if (!inventory_ok_[i]) s << "仓位变动缺少匹配成交，暂停回撤计算，保留已有止损\n";
    }
    double shorts, overall; bool complete; totals_locked(shorts, overall, complete);
    s << "已结算空头净收益 " << shorts << "U；整体已实现 " << overall << "U；已购成本 " << used_ << "U\n"
      << "可新增预算 " << (complete ? profit::entitlement(shorts, overall, used_, purchase_.reserved, cfg_.allocation) : 0) << "U；待用现货 USDT " << spot_cash_
      << "；资金保留额 " << (cfg_.capital_floor > 0 ? cfg_.capital_floor : capital_floor_) << "U\n"
      << std::setprecision(8) << "已购 BTC " << btc_qty_ << std::setprecision(2);
    if (btc_price_ > 0) s << "；现值 " << btc_qty_ * btc_price_ << "U；持币盈亏 " << btc_qty_ * btc_price_ - used_ << "U";
    s << "\n" << (error_.empty() ? message_ : error_);
    if (!complete) s << "（成交／费用未全部核实，暂停购买）";
    if (!smart_enabled_ || !cfg_.accumulate) s << "；新增购买已暂停";
    return s.str();
}
} // namespace ccbot
