#include "core/monitor_state.h"
#include <cmath>

namespace ccbot {
int64_t TrendBot::resume_after_ms() const {
    if (!cfg.rule.following.enabled) return 0;
    if (monitor.model_key == monitor_model_key(cfg) && monitor.cursor_ms > 0) return monitor.cursor_ms;
    const int64_t period = static_cast<int64_t>(TrendConfig::bar_seconds(cfg.interval)) * 1000;
    if (st.pos != trend::Pos::Flat && open_ms > 0 && period > 0) return open_ms / period * period;
    return 0;
}

void TrendEngine::mark_signal_failure(const std::string& id, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id); if (it == bots_.end()) return;
    auto& b = it->second;
    b.sig_ok = false;
    b.monitor.recovering = true;
    b.monitor.fetch_failed = true;
    const auto error = reason.empty() ? "行情拉取失败，保留断点等待恢复" : reason;
    const bool changed = b.monitor.data_error != error;
    b.monitor.data_error = error;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(host_.now_wall().time_since_epoch()).count();
    monitor_event(b, now, "数据", b.monitor.data_error);
    if (changed) log(b.cfg.symbol + " K线数据异常：" + error);
}

void TrendEngine::update_monitor_history(const std::string& id,
    const std::vector<following::Observation>& observations, int64_t target,
    const following::Config& settings) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id); if (it == bots_.end()) return;
    auto& b = it->second;
    if (!(settings == b.signal_following_config()) || !settings.enabled || target <= 0) return;
    auto& m = b.monitor;
    m.fetch_failed = false;
    const int64_t period = static_cast<int64_t>(TrendConfig::bar_seconds(b.cfg.interval)) * 1000;
    if (period <= 0) return;
    const auto key = monitor_model_key(b.cfg);
    if (m.model_key != key) {
        // Only a genuine parameter change invalidates old-model counters. A restart does not.
        m.cursor_ms = 0; m.target_ms = 0; m.trend_bars = 0; m.regime = following::Regime::Unknown;
        m.previous = following::Regime::Unknown; m.trend_since_ms = 0;
        m.model_key = key; b.rotation_health = {};
    }
    const int64_t initial_cursor = b.resume_after_ms();
    m.target_ms = std::max(target, m.target_ms);
    const auto old_cursor = m.cursor_ms;
    const bool was_recovering = m.recovering;
    for (const auto& o : observations) {
        if (o.bar_ms <= m.cursor_ms) continue; // duplicate/out-of-order responses are harmless
        const auto cursor = m.cursor_ms > 0 ? m.cursor_ms : initial_cursor;
        if (o.bar_ms > target || !o.signal.ready || !(o.signal.settings == settings) ||
            !(std::isfinite(o.close) && o.close > 0) || o.signal.regime == following::Regime::Unknown) {
            m.recovering = true; m.fetch_failed = true; m.data_error = "历史K线无效，保留已确认断点"; return;
        }
        if (cursor > 0 && o.bar_ms != cursor + period) {
            m.recovering = true; m.fetch_failed = true; m.data_error = "历史K线有缺口，等待补齐，不清零已确认进度"; return;
        }
        if (m.regime != o.signal.regime) {
            m.previous = m.regime; m.regime = o.signal.regime; m.trend_bars = 0;
            m.trend_since_ms = o.bar_ms;
            monitor_event(b, o.bar_ms, "趋势", std::string(regime_name(m.previous)) + " → " + regime_name(m.regime));
        }
        m.trend_bars = std::min(m.trend_bars + 1, 1000000);
        m.last_signal = o.signal; m.last_close = o.close; m.cursor_ms = o.bar_ms;
        m.shape = o.signal.long_confirmed ? "收盘上破确认" : o.signal.short_confirmed ? "收盘下破确认" :
            o.close > o.signal.upper ? "收盘位于通道上方，突破确认条件未齐" :
            o.close < o.signal.lower ? "收盘位于通道下方，突破确认条件未齐" : "通道内整理";
        auto& h = b.rotation_health;
        if (b.is_auto && b.st.pos != trend::Pos::Flat && b.qty > 0 && b.open_ms > 0 && o.bar_ms > b.open_ms) {
            if (h.open_ms != b.open_ms || !(h.settings == b.cfg.rule.following)) {
                h = {}; h.open_ms = b.open_ms; h.settings = b.cfg.rule.following;
            }
            if (o.bar_ms > h.last_bar_ms) {
                const bool lg = b.st.pos == trend::Pos::Long;
                h.last_bar_ms = o.bar_ms;
                if (!(h.best_close > 0) || (lg ? o.close > h.best_close : o.close < h.best_close)) {
                    h.best_close = o.close; h.last_favorable_ms = o.bar_ms;
                }
                if (o.bar_ms - period >= b.open_ms) {
                    h.samples = std::min(h.samples + 1, 1000000);
                    h.weak_bars = o.signal.regime == (lg ? following::Regime::Up : following::Regime::Down)
                        ? 0 : std::min(h.weak_bars + 1, 1000000);
                }
            }
        }
        // Advance elapsed cooldown through missing completed candles, without replaying decisions.
        // The current candle's normal tick still processes its own new_bar once.
        if (b.last_counted_bar_ms > 0 && o.bar_ms < target && o.bar_ms > b.last_counted_bar_ms) {
            if (b.st.cooldown_left > 0) --b.st.cooldown_left;
            b.last_counted_bar_ms = o.bar_ms;
        }
    }
    m.recovering = m.cursor_ms < m.target_ms;
    m.data_error = m.recovering ? "正在补齐缺失K线" : "";
    m.saved = true;
    m.updated_ms = std::chrono::duration_cast<std::chrono::milliseconds>(host_.now_wall().time_since_epoch()).count();
    if (was_recovering && !m.recovering)
        monitor_event(b, m.updated_ms, "恢复", "已接续保存的断点并补齐行情，历史订单不重放");
    if (old_cursor > 0 && m.cursor_ms - old_cursor > period)
        monitor_event(b, m.updated_ms, "补齐", "补齐 " + std::to_string((m.cursor_ms - old_cursor) / period) + " 根K线");
}

std::vector<TrendEngine::Inspection> TrendEngine::inspect_bots() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<Inspection> out;
    for (const auto& [id, b] : bots_) {
        Inspection row; row.bot = b; row.gate = open_gate_block(b);
        if (b.cfg.rule.strategy == trend::Strategy::TrendFollowing) {
            const bool fresh = b.sig_ok && host_.now_steady() >= b.sig_time &&
                host_.now_steady() - b.sig_time <= std::chrono::seconds(b.cfg.effective_max_age_sec());
            trend::Inputs in;
            in.price = b.current_price; in.atr = fresh ? b.atr : 0;
            in.dc_ok = fresh && b.dc_ok; in.dc_up = b.dc_up; in.dc_dn = b.dc_dn;
            in.following = b.following; in.following.ready = fresh && b.following.ready;
            in.box_atr = b.box_atr; in.vol_ratio = b.vol_ratio; in.er_ok = b.er_ok; in.er = b.er;
            in.range_pace = b.range_pace; in.prev_close = b.prev_close;
            in.bar_open_ms = b.bar_open_ms; in.leverage = b.cfg.leverage;
            auto state = b.st;
            const auto verdict = trend::step(state, b.cfg.rule, in);
            row.decision = verdict.reason;
            row.filtered = verdict.filtered;
            row.has_signal = fresh && (verdict.filtered || verdict.action == trend::Action::OpenLong ||
                verdict.action == trend::Action::OpenShort || b.following.long_confirmed || b.following.short_confirmed);
            if (b.waiting_reentry()) {
                const auto dir = verdict.action == trend::Action::OpenLong ? trend::Pos::Long :
                    verdict.action == trend::Action::OpenShort ? trend::Pos::Short : trend::Pos::Flat;
                if (const char* why = reentry_block(b, dir); *why) row.decision = why;
            }
        }
        row.replaceable = rotation_enabled_ && max_open_positions_ > 0 && rotation_exit_eligible(b);
        if (!rotation_enabled_) row.rotation = "满仓换仓未启用";
        else if (!b.is_auto) row.rotation = "手动持仓不参与替换";
        else if (rotation_.outgoing == id) {
            const auto next = bots_.find(rotation_.incoming);
            row.rotation = "正在平仓释放名额 → " + (next == bots_.end() ? rotation_.incoming : next->second.cfg.symbol);
        }
        else if (rotation_.incoming == id) row.rotation = rotation_.opening ? "旧仓已处理，复核新信号及预算" : "换仓预检 / 等待旧仓完全平仓";
        else if (row.replaceable) row.rotation = "旧仓符合替换条件，等待有效新信号及风险预检";
        else if (b.qty > 0) {
            row.rotation = "保留：";
            if (b.needs_reconcile) row.rotation += "等待持仓核对";
            else if (b.monitor.recovering) row.rotation += "等待补齐历史K线";
            else if (b.rotation_health.samples < 6) row.rotation += "完整持仓观察不足6根";
            else if (b.rotation_health.weak_bars < 3) row.rotation += "趋势许可未连续失效3根";
            else if (b.rotation_health.last_favorable_ms > 0 && b.bar_open_ms - b.rotation_health.last_favorable_ms <
                     static_cast<int64_t>(3) * TrendConfig::bar_seconds(b.cfg.interval) * 1000)
                row.rotation += "最近3根内仍刷新有利收盘极值";
            else if (b.st.pos == trend::Pos::Long ? b.prev_close >= b.following.fast : b.prev_close <= b.following.fast)
                row.rotation += "收盘尚未处于快EMA不利一侧";
            else if (b.pending || b.open_uncertain || b.recon_noted != TrendBot::Recon::None)
                row.rotation += "委托在途或仓位待核对";
            else row.rotation += "正常退出优先或当前报价/信号未就绪；详见执行判断";
        } else row.rotation = "新收盘突破可申请；保留原有入场及预算条件";
        out.push_back(std::move(row));
    }
    return out;
}
} // namespace ccbot
