#include "core/trend_engine.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace ccbot {
namespace {
int64_t wall_ms(const EngineHost& host) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(host.now_wall().time_since_epoch()).count();
}

trend::Inputs rotation_inputs(const TrendBot& b, bool confirmed) {
    trend::Inputs in;
    in.price = b.current_price;
    in.following = confirmed ? following::closed_signal(b.following) : b.following;
    const auto& f = in.following;
    in.atr = confirmed ? f.atr : b.atr;
    in.dc_ok = confirmed ? f.ready : b.dc_ok;
    in.dc_up = confirmed ? f.upper : b.dc_up;
    in.dc_dn = confirmed ? f.lower : b.dc_dn;
    in.box_atr = confirmed ? f.box_atr : b.box_atr;
    in.vol_ratio = confirmed ? f.volume_ratio : b.vol_ratio;
    in.er_ok = confirmed ? f.er_ok : b.er_ok;
    in.er = confirmed ? f.er : b.er;
    in.range_pace = confirmed ? f.range_atr : b.range_pace;
    in.prev_close = b.prev_close;
    in.bar_open_ms = b.bar_open_ms;
    in.leverage = b.cfg.leverage;
    return in;
}

bool fresh(const TrendBot& b, const EngineHost& host, bool quote) {
    const int64_t period = static_cast<int64_t>(TrendConfig::bar_seconds(b.cfg.interval)) * 1000;
    const auto now = wall_ms(host);
    const auto age = host.now_steady() - b.sig_time;
    return b.sig_ok && b.following.ready && b.following.settings == b.signal_following_config() &&
        period > 0 && b.bar_open_ms > 0 && now >= b.bar_open_ms && now - b.bar_open_ms < period &&
        age >= std::chrono::steady_clock::duration::zero() && age <= std::chrono::seconds(b.cfg.effective_max_age_sec()) &&
        (!quote || (std::isfinite(b.current_price) && b.current_price > 0 &&
                    host.now_steady() >= b.price_time && host.now_steady() - b.price_time <= std::chrono::seconds(15)));
}
} // namespace

void TrendEngine::set_rotation_enabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (rotation_enabled_ != enabled)
        for (auto& [id, b] : bots_) b.rotation_retry_after = {};
    rotation_enabled_ = enabled;
    if (!enabled) rotation_queue_.clear();
    // An already submitted reduce-only order must finish; the worker rechecks this flag
    // before opening. Do not release its reservation while the exchange is still responding.
}

void TrendEngine::update_rotation_health(TrendBot& b) {
    if (!fresh(b, host_, false) || !b.cfg.rule.following.enabled || !(b.prev_close > 0)) return;
    following::Observation o;
    o.bar_ms = b.bar_open_ms; o.close = b.prev_close;
    o.signal = static_cast<const following::SignalData&>(b.following);
    update_monitor_history(b.bot_id, {o}, b.bar_open_ms, b.signal_following_config());
}

bool TrendEngine::rotation_exit_eligible(const TrendBot& b, bool allow_pending) const {
    const auto& h = b.rotation_health;
    if (!b.is_auto || b.cfg.rule.strategy != trend::Strategy::TrendFollowing || !b.cfg.rule.following.enabled ||
        b.needs_reconcile || b.monitor.recovering || b.state != TrendBot::State::Running || (b.pending && !allow_pending) || b.open_uncertain ||
        !b.pending_close_key.empty() ||
        b.st.pos == trend::Pos::Flat || !(std::isfinite(b.qty) && b.qty > 0) ||
        !(std::isfinite(b.st.entry_price) && b.st.entry_price > 0 && std::isfinite(b.st.stop) && b.st.stop > 0) ||
        b.ext_gone_seen || b.opp_leg_noted ||
        b.recon_noted != TrendBot::Recon::None || !fresh(b, host_, true) ||
        h.open_ms != b.open_ms || !(h.settings == b.cfg.rule.following) ||
        h.samples < 6 || h.weak_bars < 3 || h.last_bar_ms != b.bar_open_ms) return false;
    const int64_t period = static_cast<int64_t>(TrendConfig::bar_seconds(b.cfg.interval)) * 1000;
    if (h.last_favorable_ms <= 0 || b.bar_open_ms - h.last_favorable_ms < 3 * period) return false;
    const bool is_long = b.st.pos == trend::Pos::Long;
    if (b.following.regime == (is_long ? following::Regime::Up : following::Regime::Down) ||
        !(is_long ? b.prev_close < b.following.fast : b.prev_close > b.following.fast)) return false;
    // Existing stop/reversal/failed-breakout exits have priority over opportunity rotation.
    auto state = b.st;
    auto in = rotation_inputs(b, false);
    in.new_bar = b.bar_open_ms != b.last_counted_bar_ms;
    return trend::step(state, b.cfg.rule, in).action == trend::Action::Hold;
}

std::string TrendEngine::rotation_entry_block(const TrendBot& b, trend::Pos dir,
                                              int64_t signal_bar, trend::Inputs* out) const {
    if (!rotation_enabled_ || max_open_positions_ <= 0) return "满仓换仓已关闭或未设置持仓上限";
    if (auto why = profit_entry_block(b, dir); !why.empty()) return why;
    if (!b.is_auto || b.cfg.rule.strategy != trend::Strategy::TrendFollowing || !b.cfg.rule.following.enabled ||
        b.needs_reconcile || b.monitor.recovering || b.state != TrendBot::State::Running || b.retiring || b.open_uncertain || !b.armed ||
        b.st.pos != trend::Pos::Flat || b.qty != 0 || (b.pending && rotation_.incoming != b.bot_id))
        return "新候选当前不允许换仓入场";
    if (!fresh(b, host_, true) || signal_bar != b.bar_open_ms) return "收盘突破或报价已过期";
    if (const char* why = reentry_block(b, dir); *why) return why;
    auto in = rotation_inputs(b, true);
    auto rule = b.cfg.rule;
    rule.following.close_confirm = true;
    if (!in.following.ready || !(in.following.settings == rule.following)) return "等待独立的收盘突破证据";
    auto state = b.st;
    const auto verdict = trend::step(state, rule, in);
    const auto action = dir == trend::Pos::Long ? trend::Action::OpenLong : trend::Action::OpenShort;
    if (dir == trend::Pos::Flat || verdict.action != action) return "收盘突破、趋势许可或入场过滤未通过";
    if (out) *out = in;
    return {};
}

void TrendEngine::pump_rotation() {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        if (!rotation_enabled_ || !rotation_.incoming.empty()) return;
        auto chosen = rotation_queue_.end();
        for (auto it = rotation_queue_.begin(); it != rotation_queue_.end();) {
            const auto bot = bots_.find(it->first);
            if (bot == bots_.end() || bot->second.rotation_attempt_bar_ms == it->second.bar_ms ||
                !rotation_entry_block(bot->second, it->second.dir, it->second.bar_ms).empty()) {
                it = rotation_queue_.erase(it);
                continue;
            }
            if (host_.now_steady() < bot->second.rotation_retry_after) { ++it; continue; }
            if (chosen == rotation_queue_.end() ||
                std::tie(it->second.arrival, it->first) < std::tie(chosen->second.arrival, chosen->first)) chosen = it;
            ++it;
        }
        if (chosen == rotation_queue_.end()) return;
        auto& b = bots_.at(chosen->first);
        const auto gate = open_gate_block(b);
        if (!gate.empty() && !gate.starts_with("同时持仓品种数已达上限")) return;
        // Avoid scheduling an HTTP worker on every price tick if all holdings remain healthy.
        bool victim = gate.empty();
        for (const auto& [id, held] : bots_) if (rotation_exit_eligible(held)) { victim = true; break; }
        if (!victim) {
            if (b.rotation_noted_bar_ms != b.bar_open_ms) {
                b.rotation_noted_bar_ms = b.bar_open_ms;
                log("满仓换仓：" + b.cfg.symbol + " 收盘突破有效，但现有持仓均未满足替换条件，继续监控");
            }
            return;
        }
        rotation_ = {};
        rotation_.incoming = b.bot_id;
        rotation_.dir = chosen->second.dir;
        rotation_.bar_ms = chosen->second.bar_ms;
        b.pending = true;
        b.rotation_reserved = true;
        rotation_queue_.erase(chosen);
    }
    try { host_.submit([this] { execute_rotation(); }); }
    catch (...) { finish_rotation(); throw; }
}

void TrendEngine::finish_rotation() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (auto it = bots_.find(rotation_.incoming); it != bots_.end()) {
        if (it->second.rotation_reserved) it->second.pending = false;
        it->second.rotation_reserved = false;
    }
    rotation_ = {};
}

void TrendEngine::cancel_rotation(const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (auto it = bots_.find(rotation_.incoming); it != bots_.end()) {
        auto& b = it->second;
        b.last_action = "换仓预检等待：" + reason;
        b.rotation_retry_after = host_.now_steady() + std::chrono::seconds(15);
        if (b.rotation_notice_bar_ms != rotation_.bar_ms || b.rotation_notice != reason) {
            b.rotation_notice_bar_ms = rotation_.bar_ms; b.rotation_notice = reason;
            log("满仓换仓取消：" + b.cfg.symbol + " " + reason);
        }
    }
    finish_rotation();
}

void TrendEngine::execute_rotation() {
    // A single worker owns the reservation until the full close and opening response finish.
    // No extra interval/daily quota. A failed attempt cannot consume more holdings for the
    // same candle's signal; later independently confirmed signals may apply again.
    try {
        TrendConfig cfg;
        trend::Inputs in;
        std::string incoming;
        trend::Pos dir;
        double stop = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            incoming = rotation_.incoming;
            dir = rotation_.dir;
            auto it = bots_.find(incoming);
            if (it == bots_.end()) { finish_rotation(); return; }
            auto& b = it->second;
            const auto why = rotation_entry_block(b, dir, rotation_.bar_ms, &in);
            if (!why.empty()) { cancel_rotation(why); return; }
            cfg = b.cfg;
            stop = trend::initial_stop(dir, in.price, cfg.rule, in);
        }
        // Quantity precision may fetch exchange rules. Never do that under the engine lock.
        const double qty = plan_qty(cfg, in.price, stop);
        if (!(qty > 0 && stop > 0) || !(dir == trend::Pos::Long ? stop < in.price : stop > in.price)) {
            cancel_rotation("新仓数量或初始止损无效"); return;
        }
        std::string outgoing;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(incoming);
            trend::Inputs latest;
            if (it == bots_.end() || !rotation_entry_block(it->second, dir, rotation_.bar_ms, &latest).empty() ||
                latest.price != in.price || latest.atr != in.atr) {
                cancel_rotation("数量预检期间信号或报价改变"); return;
            }
            auto& b = it->second;
            if (b.cfg.budget_usdt != cfg.budget_usdt || b.cfg.risk_usdt != cfg.risk_usdt ||
                b.cfg.leverage != cfg.leverage || b.cfg.size_mode != cfg.size_mode ||
                trend::initial_stop(dir, latest.price, b.cfg.rule, latest) != stop) {
                finish_rotation(); return;
            }
            const double risk = qty * (std::fabs(in.price - stop) + in.price * risk_cost_rate_);
            if ((cfg.size_mode == TrendConfig::SizeMode::RiskBased && risk > cfg.risk_usdt + 1e-9) ||
                (cfg.budget_usdt > 0 && qty * in.price > cfg.budget_usdt + 1e-9)) {
                cancel_rotation("新仓数量超出当前单笔预算"); return;
            }
            rotation_.planned_qty = qty;
            rotation_.planned_price = in.price;
            rotation_.planned_atr = in.atr;
            rotation_.planned_stop = stop;
            rotation_.cost_rate = risk_cost_rate_;
            rotation_.config = cfg;
            const auto gate = open_gate_block(b);
            if (gate.empty()) {
                if (const auto risk_block = risk_gate_block(b, dir, risk); !risk_block.empty()) {
                    cancel_rotation(risk_block); return;
                }
                log("满仓换仓：已有自然释放的空位，按收盘信号入场，不平旧仓");
            } else {
                if (!gate.starts_with("同时持仓品种数已达上限")) {
                    cancel_rotation(gate); return;
                }
                const TrendBot* best = nullptr;
                for (const auto& [id, held] : bots_) {
                    if (!rotation_exit_eligible(held) || !open_gate_block(b, &held).empty() ||
                        !risk_gate_block(b, dir, risk, &held).empty()) continue;
                    // Compare actual elapsed invalidation time across different candle intervals.
                    const int64_t held_since = held.bar_open_ms - static_cast<int64_t>(held.rotation_health.weak_bars) *
                        TrendConfig::bar_seconds(held.cfg.interval) * 1000;
                    const int64_t best_since = best ? best->bar_open_ms - static_cast<int64_t>(best->rotation_health.weak_bars) *
                        TrendConfig::bar_seconds(best->cfg.interval) * 1000 : 0;
                    if (!best || std::tie(held_since, held.open_ms, held.cfg.symbol) <
                                 std::tie(best_since, best->open_ms, best->cfg.symbol)) best = &held;
                }
                if (!best) {
                    if (b.rotation_noted_bar_ms != b.bar_open_ms) {
                        b.rotation_noted_bar_ms = b.bar_open_ms;
                        log("满仓换仓：" + b.cfg.symbol + " 无满足退出条件且可释放足够预算的旧仓");
                    }
                    finish_rotation(); return;
                }
                outgoing = best->bot_id;
                rotation_.outgoing = outgoing;
                auto& held = bots_.at(outgoing);
                log("满仓换仓：" + b.cfg.symbol + " 收盘突破确认；替换 " + held.cfg.symbol +
                    "（原方向趋势许可连续失效 " + std::to_string(held.rotation_health.weak_bars) + " 根），风险预检通过，先平后开");
                held.pending = true;
            }
            b.rotation_attempt_bar_ms = rotation_.bar_ms;
        }
        bool fully_closed = outgoing.empty();
        if (!outgoing.empty()) execute_close(outgoing, "满仓换仓：持续失去原方向趋势许可", trend::Pos::Flat, &fully_closed);
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            if (!fully_closed) {
                log("满仓换仓终止：旧仓未确认完全平仓，不开新仓"); finish_rotation(); return;
            }
            rotation_.opening = true;
        }
        execute_open(incoming, dir, false); // checks current signal and all budgets again
    } catch (const std::exception& e) {
        log("满仓换仓异常：" + std::string(e.what()));
    } catch (...) { log("满仓换仓未知异常"); }
    finish_rotation();
}

std::string TrendEngine::rotation_close_block(const TrendBot& outgoing) const {
    const auto it = bots_.find(rotation_.incoming);
    if (it == bots_.end()) return "新候选已移除";
    const auto& b = it->second;
    trend::Inputs in;
    if (auto why = rotation_entry_block(b, rotation_.dir, rotation_.bar_ms, &in); !why.empty()) return why;
    if (!rotation_exit_eligible(outgoing, true)) return "旧仓退出条件已改变，继续按原策略管理";
    const auto& cfg = rotation_.config;
    if (in.price != rotation_.planned_price || in.atr != rotation_.planned_atr ||
        risk_cost_rate_ != rotation_.cost_rate || b.cfg.budget_usdt != cfg.budget_usdt ||
        b.cfg.risk_usdt != cfg.risk_usdt || b.cfg.size_mode != cfg.size_mode || b.cfg.leverage != cfg.leverage ||
        trend::initial_stop(rotation_.dir, in.price, b.cfg.rule, in) != rotation_.planned_stop)
        return "平仓前新信号、报价或仓位参数已改变";
    if (auto why = open_gate_block(b, &outgoing); !why.empty()) return why;
    return risk_gate_block(b, rotation_.dir, rotation_.planned_qty *
        (std::fabs(in.price - rotation_.planned_stop) + in.price * risk_cost_rate_), &outgoing);
}
} // namespace ccbot
