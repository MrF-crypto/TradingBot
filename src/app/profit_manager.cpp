#include "app/profit_manager.h"
#include "core/atomic_file.h"
#include "core/profit_json.h"
#include "core/monitor_state.h"
#include "core/parallel.h"
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
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
std::set<std::string> running_hourly(const std::map<std::string, const TrendBot*>& bots) {
    std::set<std::string> members;
    for (const auto& [sym, b] : bots)
        if (!b->retiring && b->state == TrendBot::State::Running && b->cfg.interval == "1h") members.insert(sym);
    return members;
}
}
ProfitManager::ProfitManager(std::string path, std::string account) : path_(std::move(path)), account_(std::move(account)) { load(); }
void ProfitManager::configure(profit::Config cfg, bool enabled, int64_t now, const TrendConfig& tmpl) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!profit::valid(cfg)) { error_ = "利润配置不合法"; return; }
    auto breadth = tmpl;
    breadth.interval = "1h";
    breadth.rule.following.enabled = true; // The entry filter and the breadth indicator have separate switches.
    const auto key = monitor_model_key(breadth);
    if (key != breadth_signature_) {
        if (!breadth_signature_.empty())
            for (auto& g : guards_) if (g.active) g.reset_breadth(now);
        ++observation_epoch_; observer_quotes_.clear();
        breadth_signature_ = key;
    }
    breadth_model_ = breadth.rule.following; breadth_atr_ = breadth.rule.atr_period;
    breadth_channel_ = breadth.rule.donchian_period;
    if ((!enabled || !cfg.protect) && !observer_quotes_.empty()) {
        ++observation_epoch_; observer_quotes_.clear();
    }
    cfg_ = cfg; smart_enabled_ = enabled;
    if (enabled && (cfg.protect || cfg.accumulate) && !tracking_) { tracking_ = true; started_ = now; }
    if (enabled && cfg.accumulate && accumulation_since_ == 0) accumulation_since_ = now;
    if (tracking_) save_locked();
}
double ProfitManager::pending_open_margin_locked() const {
    double margin = 0;
    for (const auto& hold : submitting_margins_) margin += hold.second;
    // An unresolved opening can survive the process that submitted it. Until
    // it becomes terminal, do not also promise those funds to a different bot.
    for (const auto& [key, item] : entries_) {
        const auto& e = item.event;
        if (!e.opening || (e.completed && !e.result.uncertain) ||
            submitting_margins_.contains({e.bot_id, e.at_ms})) continue;
        if (!(e.required_margin > 0)) return std::numeric_limits<double>::infinity();
        margin += e.required_margin;
    }
    return margin;
}

bool ProfitManager::record(ExecutionEvent& e, TradingClient* client) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!tracking_) return true;
    const auto intent = std::make_pair(e.bot_id, e.at_ms);
    if (e.opening && !e.completed && purchase_.phase == 1) {
        if (!client || !(e.required_margin > 0) || !std::isfinite(e.required_margin)) {
            e.result.error = "划转待核，无法确认本单所需保证金";
            return false;
        }
        const auto sent = purchase_.sent;
        const double reserved = purchase_.reserved, pending = pending_open_margin_locked(), admitted = admitted_margin_;
        lk.unlock();
        const auto account = client->fetch_account();
        lk.lock();
        if (cancelled_ || damaged_) return false;
        if (purchase_.phase == 1) {
            if (sent != purchase_.sent || reserved != purchase_.reserved || !account.ok || !account.available_verified ||
                !std::isfinite(account.available) || account.available < 0) {
                e.result.error = "划转待核，本次未能核实合约可用保证金";
                return false;
            }
            const double required = reserved + pending + (admitted_margin_ - admitted) + e.required_margin;
            if (account.available + 1e-8 < required) {
                e.result.error = "扣除待核划转及在途开仓后，可用保证金不足";
                return false;
            }
        }
    }
    if (!record_locked(e)) return false;
    ++execution_revision_;
    if (e.opening) {
        if (e.completed) submitting_margins_.erase(intent);
        else if (purchase_.phase == 1) {
            if (submitting_margins_.emplace(intent, e.required_margin).second) admitted_margin_ += e.required_margin;
        }
    }
    return true;
}

bool ProfitManager::record_locked(ExecutionEvent& e) {
    if (!tracking_ || !e.is_auto) return true;
    if (cancelled_ || damaged_) return false;
    if (e.symbol.empty() || e.bot_id.empty() || e.at_ms <= 0 || (e.direction != 1 && e.direction != -1) ||
        !std::isfinite(e.qty) || e.qty <= 0 || !std::isfinite(e.entry) || e.entry < 0 ||
        !std::isfinite(e.required_margin) || e.required_margin < 0 ||
        !std::isfinite(e.result.executed_qty) || e.result.executed_qty < 0 ||
        !std::isfinite(e.result.avg_price) || e.result.avg_price < 0 ||
        (e.completed && e.result.ok && e.result.executed_qty > 0 && e.result.avg_price <= 0)) {
        error_ = "成交回执数值不合法，保留原账本等待核对"; return false;
    }
    ++receipt_revision_[e.symbol];
    if (e.key.empty()) {
        // The durable sequence is also the clientOrderId. Never recycle it after restart.
        e.key = "tbp" + std::to_string(started_) + "_" + std::to_string(++sequence_);
    }
    bool merged = false;
    // Exchange-stop evidence can be delivered more than once after reconnection.
    if (e.completed && !e.result.order_id.empty()) {
        for (const auto& [key, old] : entries_) if (key != e.key && old.event.symbol == e.symbol &&
            old.event.result.order_id == e.result.order_id && old.event.completed) {
                const auto alias = entries_.find(e.key);
                if (old.event.direction != e.direction || old.event.opening != e.opening ||
                    (alias != entries_.end() && (alias->second.event.symbol != e.symbol ||
                     alias->second.event.direction != e.direction || alias->second.event.opening != e.opening ||
                     (alias->second.event.result.ok && alias->second.event.result.executed_qty > 0 &&
                      alias->second.event.result.order_id != e.result.order_id)))) {
                    error_ = "重复回执的订单归属冲突，保留已有账本"; return false;
                }
                entries_.erase(e.key);
                e.key = key;
                merged = true;
                break;
            }
    }
    auto& entry = entries_[e.key];
    const auto& old = entry.event;
    if (!old.symbol.empty() && (old.symbol != e.symbol || old.direction != e.direction || old.opening != e.opening)) {
        error_ = "成交回执与原订单归属不一致，保留原账本等待核对"; return false;
    }
    // A late order-status response (including its rounded average price) must not
    // replace individually verified fills or remove their durable recovery link.
    if (!entry.recovery_open_key.empty()) {
        if (e.completed && e.result.ok && (e.result.order_id != old.result.order_id ||
            std::fabs(e.result.executed_qty - old.result.executed_qty) > std::max(1e-10, old.qty * 1e-10))) {
            error_ = "补录成交与迟到回执矛盾，保留已核实的成交"; return false;
        }
        return merged ? save_locked() : true;
    }
    // Replays and late uncertain responses must not erase exchange-settled net P&L.
    if (old.completed && old.result.ok && !old.result.uncertain &&
        (!e.completed || !e.result.ok || e.result.uncertain ||
         (e.result.ok && e.result.order_id == old.result.order_id &&
          e.result.executed_qty == old.result.executed_qty && e.result.avg_price == old.result.avg_price)))
        return merged ? save_locked() : true;
    // A late execution receipt can follow an earlier "order not found" query.
    // Never retain that query's settled flag or zero net amount for the real fill.
    entry = {}; entry.event = e;
    return save_locked();
}

void ProfitManager::evaluate(TrendEngine& engine, int64_t now) {
    std::array<profit::Guard, 2> guards;
    std::string error;
    double recovery_today = 0;
    bool enabled = false;
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
            // An old unbooked short close and a new long in the same symbol are
            // separate cycles. Netting the sides could freeze the wrong curve.
            std::map<std::string, std::array<double, 2>> expected, actual;
            for (const auto& [symbol, qty] : inherited_)
                expected[symbol][slot(qty > 0 ? 1 : -1)] = std::fabs(qty);
            std::set<std::string> recorded;
            for (const auto& [key, entry] : entries_) {
                const auto& e = entry.event; auto i = slot(e.direction);
                recorded.insert(e.symbol);
                if (!e.completed || e.result.uncertain) { prices_ok[i] = false; continue; }
                if (!e.result.ok || e.result.executed_qty <= 0) continue;
                expected[e.symbol][i] += e.result.executed_qty * (e.opening ? 1 : -1);
                const double fee = e.result.executed_qty * e.result.avg_price * TrendEngine::kEstFeeRate;
                if (!entry.recovery_open_key.empty() && entry.last_fill / 86400000 == now / 86400000)
                    recovery_today += entry.recovery_gross - fee - e.result.executed_qty * e.entry * TrendEngine::kEstFeeRate;
                if (entry.settled) realized[i] += entry.net;
                else realized[i] += (e.opening ? 0 : (e.result.avg_price - e.entry) * e.result.executed_qty * e.direction) - fee;
            }
            for (const auto& [key, f] : funding_) {
                auto it = entries_.find(key); if (it != entries_.end()) realized[slot(it->second.event.direction)] += f.net;
            }
            const auto steady = std::chrono::steady_clock::now();
            std::map<std::string, const TrendBot*> by_symbol;
            std::set<std::string> in_flight;
            std::set<std::string> reconciling;
            for (const auto& b : bots) if (b.is_auto) {
                auto& candidate = by_symbol[b.cfg.symbol];
                if (!candidate || b.state != TrendBot::State::Stopped || holding(b)) candidate = &b;
                if (b.pending || b.open_uncertain || !b.pending_close_key.empty()) in_flight.insert(b.cfg.symbol);
                if (b.needs_reconcile) reconciling.insert(b.cfg.symbol);
                if (b.pending || b.open_uncertain || !b.pending_close_key.empty() || b.needs_reconcile) {
                    const auto direction = b.st.pos != trend::Pos::Flat ? b.st.pos : b.reserved_dir;
                    if (direction != trend::Pos::Flat) { exposed[direction == trend::Pos::Long ? 0 : 1] = true; prices_ok[direction == trend::Pos::Long ? 0 : 1] = false; }
                }
                if (!holding(b)) continue;
                const auto i = b.st.pos == trend::Pos::Long ? 0U : 1U; exposed[i] = true;
                actual[b.cfg.symbol][i] += b.qty;
                entry_cost[i] += b.st.entry_price * b.qty * TrendEngine::kEstFeeRate;
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(steady - b.price_time).count();
                if (!std::isfinite(b.current_price) || b.current_price <= 0 || age < 0 || age > 30 || b.needs_reconcile) { prices_ok[i] = false; continue; }
                floating[i] += (b.current_price - b.st.entry_price) * b.qty * (i == 0 ? 1 : -1) - b.current_price * b.qty * TrendEngine::kEstFeeRate;
            }
            // Capture inherited holdings only once. Thereafter every quantity change must
            // have a receipt, including across process restarts or bot-ID reassignment.
            for (const auto& [symbol, qty] : actual) if (!recorded.count(symbol) && !inherited_.count(symbol) &&
                                                         (qty[0] == 0 || qty[1] == 0)) {
                inherited_[symbol] = qty[0] - qty[1]; expected[symbol] = qty;
            }
            for (const auto& [symbol, qty] : actual) expected.try_emplace(symbol);
            for (const auto& [symbol, sides] : inventory_discrepancies_) expected.try_emplace(symbol);
            std::map<std::string, std::array<bool, 2>> discrepancies;
            for (const auto& [symbol, qty] : expected) {
                // Receipt and engine position are updated in separate critical sections.
                // Freeze protection calculations during that window, without labelling
                // the expected transition as a broken ledger.
                const auto old = inventory_discrepancies_.find(symbol);
                if (in_flight.count(symbol) || (reconciling.count(symbol) && old != inventory_discrepancies_.end())) {
                    if (old != inventory_discrepancies_.end()) discrepancies.emplace(*old);
                    continue;
                }
                const auto& present = actual[symbol];
                for (size_t i = 0; i < qty.size(); ++i) {
                    const auto tolerance = std::max({1e-8, std::fabs(qty[i]) * 1e-8, present[i] * 1e-8});
                    if (std::fabs(present[i] - qty[i]) > tolerance) discrepancies[symbol][i] = true;
                }
            }
            inventory_discrepancies_ = std::move(discrepancies);
            const auto live_prices_ok = prices_ok;
            inventory_ok_ = {true, true};
            for (const auto& [symbol, sides] : inventory_discrepancies_)
                for (size_t i = 0; i < sides.size(); ++i) if (sides[i]) {
                    prices_ok[i] = inventory_ok_[i] = false;
                }
            const auto bar = now / 3600000 * 3600000;
            std::set<std::string> observers;
            const auto has_live_source = [&](const TrendBot& b) {
                return b.state == TrendBot::State::Running && b.cfg.interval == "1h" &&
                    b.cfg.rule.atr_period == breadth_atr_ && b.cfg.rule.donchian_period == breadth_channel_ &&
                    b.signal_following_config() == breadth_model_;
            };
            for (size_t i = 0; i < 2; ++i) {
                auto& g = guards_[i]; auto& cohort = cohorts_[i];
                const auto previous_phase = g.phase;
                if (enabled && !g.active && exposed[i] && prices_ok[i]) {
                    g.active = true; g.anchor = realized[i] + entry_cost[i];
                    cohort.clear();
                }
                if (enabled && cohort.empty() && g.active) {
                    cohort = running_hourly(by_symbol);
                    if (cohort.size() < 10) cohort.clear(); // Wait for the first successful candidate scan.
                }
                int ready = 0, support = 0;
                auto& report = coverage_[i]; report = {};
                for (const auto& sym : cohort) {
                    if (unavailable_.count(sym)) { ++report.unavailable; continue; }
                    const auto it = by_symbol.find(sym);
                    auto regime = following::Regime::Unknown;
                    if (it != by_symbol.end() && has_live_source(*it->second)) {
                        const auto& b = *it->second;
                        if (b.sig_ok && !b.monitor.recovering && b.bar_open_ms == bar && b.following.ready &&
                            b.following.settings == breadth_model_) regime = b.following.regime;
                        if (regime == following::Regime::Unknown) {
                            ++report.missing_live;
                            if (report.error.empty() && !b.monitor.data_error.empty())
                                report.error = sym + " " + b.monitor.data_error;
                        }
                    } else {
                        const auto q = observer_quotes_.find(sym);
                        if (q != observer_quotes_.end() && q->second.bar == bar) regime = q->second.regime;
                        if (regime == following::Regime::Unknown) {
                            ++report.missing_observer;
                            if (q != observer_quotes_.end() && report.error.empty() && !q->second.error.empty())
                                report.error = sym + " " + q->second.error;
                        }
                    }
                    if (regime == following::Regime::Unknown) continue;
                    ++ready;
                    if (regime == (i == 0 ? following::Regime::Up : following::Regime::Down)) ++support;
                }
                report.ready = static_cast<size_t>(ready);
                // Fixed denominator: missing data cannot masquerade as weakening breadth.
                const bool coverage = cohort.size() >= 10 && ready * 10 >= static_cast<int>(cohort.size()) * 9;
                auto guard_config = cfg_; guard_config.protect = enabled;
                if (inventory_ok_[i] || previous_phase != profit::Phase::Watching)
                    g.update(guard_config, now, realized[i] + floating[i], prices_ok[i], exposed[i], bar,
                    cohort.empty() ? 0 : static_cast<double>(support) / static_cast<double>(cohort.size()), coverage && enabled,
                    cohort.empty() ? 1 : static_cast<double>(support + static_cast<int>(cohort.size()) - ready) / static_cast<double>(cohort.size()),
                    !inventory_ok_[i] && live_prices_ok[i]);
                if (previous_phase != profit::Phase::Exiting && g.phase == profit::Phase::Exiting) {
                    auto& ev = g.exit_evidence;
                    ev = {};
                    ev.rule = g.reason;
                    ev.trigger_ms = g.exit_ms;
                    const double drawdown = (g.peak - g.profit) / g.peak;
                    ev.parameters = "启动利润 " + exit_number(cfg_.arm_usdt) + " U，组合峰值 " +
                        exit_number(g.peak) + " U，当前利润 " + exit_number(g.profit) +
                        " U，实际回撤 " + exit_number(drawdown * 100) + "%";
                    if (drawdown >= cfg_.hard_drawdown) {
                        ev.parameters += "，硬回撤阈值 " + exit_number(cfg_.hard_drawdown * 100) + "%";
                    } else {
                        ev.parameters += "，软回撤阈值 " + exit_number(cfg_.soft_drawdown * 100) +
                            "%，广度转弱阈值 " + exit_number(cfg_.weak_breadth * 100) +
                            "% 或较峰值下降 " + exit_number(cfg_.breadth_drop * 100) +
                            " 个百分点，已确认 " + std::to_string(g.weak) + "/" +
                            std::to_string(cfg_.confirm_bars) + " 根，支持 " + std::to_string(support) +
                            " 个，有效样本 " + std::to_string(ready) + "/" + std::to_string(cohort.size());
                    }
                }
                if (g.phase != profit::Phase::Exiting) g.exit_evidence = {};
                if (previous_phase != profit::Phase::Waiting && g.phase == profit::Phase::Waiting) {
                    // A completed exit starts a new recovery cohort; confirmation counts restart.
                    g.reset_breadth(now);
                    cohort = running_hourly(by_symbol); report = {};
                    if (cohort.size() < 10) cohort.clear();
                    ++observation_epoch_;
                }
                if (!g.active) { cohort.clear(); report = {}; }
                if (enabled) for (const auto& sym : cohort) {
                    if (unavailable_.count(sym)) continue;
                    const auto it = by_symbol.find(sym);
                    if (it == by_symbol.end() || !has_live_source(*it->second)) observers.insert(sym);
                }
                if (g.phase != previous_phase) notices_.push_back(std::string("组合利润保护：") + (i == 0 ? "多头 " : "空头 ") + phase_name(g.phase) + "；" + g.reason);
            }
            std::erase_if(observer_quotes_, [&](const auto& item) { return !observers.count(item.first); });
            for (const auto& sym : observers) observer_quotes_.try_emplace(sym);
            save_locked();
        }
        guards = guards_; error = error_;
    }
    // Persistence errors block new risk but never disable any existing stop.
    engine.set_persistence_error("profit", error);
    engine.set_recovered_day_pnl({now / 86400000, recovery_today});
    const bool closing = enabled && (guards[0].phase == profit::Phase::Exiting || guards[1].phase == profit::Phase::Exiting);
    for (size_t i = 0; i < 2; ++i) {
        const auto direction = i == 0 ? trend::Pos::Long : trend::Pos::Short;
        const auto after = std::max(guards[i].fresh_after, guards[1 - i].exit_ms);
        engine.set_profit_gate(direction, closing ? "组合保护退出中，暂停智能新增风险" :
            (enabled && guards[i].blocked() ? guards[i].reason : ""), enabled ? after : 0);
        if (enabled && guards[i].phase == profit::Phase::Exiting)
            engine.close_auto_direction(direction, guards[i].reason, guards[i].exit_evidence);
    }
}

bool ProfitManager::needs_breadth_quotes() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return !cancelled_ && smart_enabled_ && cfg_.protect && !damaged_ && !observer_quotes_.empty();
}

void ProfitManager::refresh_breadth(TradingClient& client) {
    constexpr int64_t hour = 3600000, retry_ms = 60000;
    const auto now = client.market_time_ms(), bar = now / hour * hour;
    std::vector<std::pair<int64_t, std::string>> work;
    following::Config model;
    int channel, atr;
    uint64_t epoch;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (cancelled_ || !smart_enabled_ || !cfg_.protect || damaged_) return;
        for (const auto& [sym, q] : observer_quotes_)
            if (q.bar != bar && (q.attempted <= 0 || now - q.attempted >= retry_ms))
                work.emplace_back(q.attempted, sym);
        // Oldest attempt first so permanently unavailable tickers cannot starve the remainder.
        std::sort(work.begin(), work.end());
        if (work.size() > 8) work.resize(8);
        for (const auto& [attempted, sym] : work) observer_quotes_.at(sym).attempted = now;
        epoch = observation_epoch_; model = breadth_model_; channel = breadth_channel_; atr = breadth_atr_;
    }
    parallel_for(static_cast<int>(work.size()), 2, [&](int i) {
        if (cancelled_) return;
        const auto& sym = work[i].second;
        TradingClient::TrendSnapshot signal;
        try { signal = client.fetch_trend_signal(sym, "1h", channel, atr, model); }
        catch (...) { signal.error = "读取收盘数据异常"; }
        const auto received = client.market_time_ms();
        std::lock_guard<std::mutex> lk(mutex_);
        auto q = observer_quotes_.find(sym);
        if (cancelled_ || epoch != observation_epoch_ || unavailable_.count(sym) || q == observer_quotes_.end() ||
            q->second.attempted != now || !smart_enabled_ || !cfg_.protect || damaged_) return;
        if (received / hour * hour != bar || signal.bar_open_ms != bar || !signal.ok ||
            !signal.following.ready || signal.following.settings != model ||
            signal.following.regime == following::Regime::Unknown) {
            q->second.error = signal.error.empty() ? "等待本小时完整收盘数据" : signal.error;
            return;
        }
        q->second.bar = bar; q->second.regime = signal.following.regime; q->second.error.clear();
    });
}

void ProfitManager::update_breadth_market(const std::set<std::string>& listed, const std::set<std::string>& settled,
                                        const std::set<std::string>& candidates, int64_t now) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (cancelled_ || !smart_enabled_ || !cfg_.protect || damaged_ || listed.empty() || now <= 0) return;
    std::set<std::string> permanent = settled;
    std::set<std::string> tracked;
    for (const auto& members : cohorts_) tracked.insert(members.begin(), members.end());
    bool absence_changed = std::erase_if(missing_from_market_, [&](const auto& item) {
        return !tracked.count(item.first) || listed.count(item.first);
    }) != 0;
    for (const auto& sym : tracked) if (!listed.count(sym)) {
        const auto [it, first] = missing_from_market_.try_emplace(sym, now);
        absence_changed |= first;
        if (!first && now - it->second >= 3600000) permanent.insert(sym);
    }
    unavailable_.clear();
    for (const auto& sym : tracked) if (permanent.count(sym)) unavailable_.insert(sym);
    bool changed = false;
    for (size_t i = 0; i < cohorts_.size(); ++i) {
        auto& cohort = cohorts_[i];
        const auto target = cohort.size();
        if (!guards_[i].active || target == 0) continue;
        auto replacement = candidates.begin();
        std::vector<std::pair<std::string, std::string>> swaps;
        for (const auto& old : cohort) if (permanent.count(old)) {
            while (replacement != candidates.end() && (cohort.count(*replacement) ||
                   !listed.count(*replacement) || permanent.count(*replacement))) ++replacement;
            if (replacement == candidates.end()) break;
            swaps.emplace_back(old, *replacement++);
        }
        if (swaps.empty()) continue;
        // One for one. A confirmed vacancy with no eligible replacement stays in the denominator.
        for (const auto& [old, fresh] : swaps) { cohort.erase(old); cohort.insert(fresh); }
        guards_[i].reset_breadth(now); coverage_[i] = {};
        notices_.push_back(std::string(i == 0 ? "多头" : "空头") + "组合广度：已核实合约下架，等量替换 " +
            std::to_string(swaps.size()) + "/" + std::to_string(target) + " 个样本（" + swaps.front().first + " → " +
            swaps.front().second + "），从下根收盘重新确认");
        changed = true;
    }
    if (changed) {
        ++observation_epoch_; observer_quotes_.clear();
        std::erase_if(missing_from_market_, [&](const auto& item) {
            return !cohorts_[0].count(item.first) && !cohorts_[1].count(item.first);
        });
    }
    if (changed || absence_changed) save_locked();
}

void ProfitManager::totals_locked(double& shorts, double& overall, bool& complete) const {
    shorts = overall = 0; complete = !damaged_ && error_.empty() && inventory_ok_[0] && inventory_ok_[1];
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
          << "U，最近有效广度下界 " << g.breadth * 100 << "%（本小时有效 " << coverage_[i].ready << "/" << cohorts_[i].size() << "）";
        if (coverage_[i].missing_live) s << "，等待机器人收盘数据 " << coverage_[i].missing_live;
        if (coverage_[i].missing_observer) s << "，等待读取独立行情 " << coverage_[i].missing_observer;
        if (coverage_[i].unavailable) s << "，合约下架待补位 " << coverage_[i].unavailable;
        if (!coverage_[i].error.empty()) s << "（" << coverage_[i].error << "）";
        s << "\n";
        if (!inventory_ok_[i]) s << "仓位变动缺少匹配成交，后台核对中。本方向利润回撤计算暂停，新信号照常检查入场条件。\n";
    }
    double shorts, overall; bool complete; totals_locked(shorts, overall, complete);
    s << "已结算空头净收益 " << shorts << "U；整体已实现 " << overall << "U；已购成本 " << used_ << "U\n"
      << "可新增预算 " << (complete ? profit::entitlement(shorts, overall, used_, purchase_.reserved, cfg_.allocation) : 0) << "U；待用现货 USDT " << spot_cash_
      << "；资金保留额 " << (cfg_.capital_floor > 0 ? cfg_.capital_floor : capital_floor_) << "U\n"
      << std::setprecision(8) << "已购 BTC " << btc_qty_ << std::setprecision(2);
    if (btc_price_ > 0) s << "；现值 " << btc_qty_ * btc_price_ << "U；持币盈亏 " << btc_qty_ * btc_price_ - used_ << "U";
    s << "\n" << (error_.empty() ? message_ : error_);
    for (const auto& [symbol, reason] : recovery_wait_) s << "\n" << symbol << " 外部平仓待核对：" << reason;
    if (!complete) s << "（成交／费用未全部核实，暂停购买）";
    if (!smart_enabled_ || !cfg_.accumulate) s << "；新增购买已暂停";
    return s.str();
}
} // namespace ccbot
