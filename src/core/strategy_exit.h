#pragma once

#include "core/trend_engine.h"

namespace ccbot {

// Called at the sizing decision; the fill can arrive after price, indicators or settings change.
inline ExitEvidence initial_stop_evidence(const TrendConfig& cfg, trend::Pos side, double price,
                                          const trend::Inputs& in, double line) {
    ExitEvidence e;
    e.rule = "初始止损";
    e.threshold_price = line;
    e.parameters = "初始参考价=" + exit_number(price);
    const auto& r = cfg.rule;
    if (r.strategy == trend::Strategy::TrendFollowing) {
        e.parameters += "，ATR周期=" + std::to_string(r.atr_period) + "，开仓ATR=" + exit_number(in.atr) +
            "，初始倍数=" + exit_number(r.atr_mult) + "，最大初始距离=" +
            (r.max_init_loss_pct > 0 ? exit_number(r.max_init_loss_pct) + "%" : "不限");
    } else if (r.strategy == trend::Strategy::BareK) {
        e.parameters += "，摆动回看=" + std::to_string(r.swing_bars) +
            "，摆动极值=" + exit_number(side == trend::Pos::Long ? in.swing_low : in.swing_high);
    } else {
        e.parameters += "，前根边界=" + exit_number(side == trend::Pos::Long ? in.psar_prev_low : in.psar_prev_high);
    }
    return e;
}

inline std::string breakeven_parameters(const TrendBot& b, const trend::Inputs& in) {
    return "启动收益率=" + exit_number(b.cfg.rule.breakeven_start) + "%，杠杆=" + exit_number(in.leverage) +
        "x，开仓价=" + exit_number(b.st.entry_price) + "，往返费用缓冲=" + exit_number(trend::kBreakevenFeePct) + "%";
}

// Capture when a line actually advances. A retained line may have been formed with
// different ATR, settings or peak from the tick that ultimately crosses it.
inline ExitEvidence advanced_stop_evidence(const TrendBot& b, const trend::Inputs& in, trend::StopChange change) {
    ExitEvidence e;
    e.threshold_price = b.st.stop;
    const auto& cfg = b.cfg.rule;
    if (change == trend::StopChange::Breakeven) {
        e.rule = "保本止损";
        e.parameters = breakeven_parameters(b, in);
    } else if (cfg.strategy == trend::Strategy::TrendFollowing) {
        e.rule = "ATR 移动止损";
        e.parameters = "形成该线时极值=" + exit_number(b.st.peak) + "，ATR周期=" + std::to_string(cfg.atr_period) +
            "，有效ATR=" + exit_number(trend::trailing_atr(b.st, cfg, in)) +
            "，基础倍数=" + exit_number(cfg.atr_mult) + "，实际倍数=" + exit_number(trend::trailing_atr_mult(b.st, cfg)) +
            "，锁定ATR=" + (cfg.lock_atr ? "开" : "关") + "，加速收紧=" + (cfg.accel_exit ? "开" : "关");
    } else if (cfg.strategy == trend::Strategy::ParabolicSar) {
        e.rule = "SAR 移动止损";
        e.parameters = "形成该线时极值=" + exit_number(b.st.peak) + "，AF=" + exit_number(b.st.af) +
            "，AF步长=" + exit_number(cfg.af_step) + "，AF上限=" + exit_number(cfg.af_max);
    } else {
        e.rule = "摆动极值止损";
        e.parameters = "摆动回看=" + std::to_string(cfg.swing_bars) +
            "，摆动极值=" + exit_number(b.st.pos == trend::Pos::Long ? in.swing_low : in.swing_high);
    }
    return e;
}

inline ExitEvidence strategy_exit_evidence(const TrendBot& b, const trend::Inputs& in,
                                            const trend::Verdict& v, int64_t now) {
    ExitEvidence e;
    const auto& cfg = b.cfg.rule;
    switch (v.exit_path) {
    case trend::ExitPath::StopLine:
    case trend::ExitPath::Breakeven:
        if (b.stop_evidence.threshold_price == b.st.stop) e = b.stop_evidence;
        if (v.exit_path == trend::ExitPath::Breakeven && e.rule != "保本止损") {
            e.rule = "保本止损";
            e.parameters = breakeven_parameters(b, in);
        }
        if (e.rule.empty()) e.rule = b.open_stop == b.st.stop ? "初始止损" : "本地止损线触发";
        if (e.rule == "ATR 移动止损" && v.profitable_exit) e.rule = "ATR 移动止盈";
        e.threshold_price = b.st.stop;
        e.trigger_price = in.price;
        break;
    case trend::ExitPath::RoiTrail: {
        const double peak = trend::roi_pct(b.st.pos, b.st.entry_price, b.st.peak, in.leverage);
        const double current = trend::roi_pct(b.st.pos, b.st.entry_price, in.price, in.leverage);
        const double limit = peak - cfg.roi_trail_back;
        const double denominator = 1.0 - (b.st.pos == trend::Pos::Long ? 1.0 : -1.0) * limit / (100.0 * in.leverage);
        if (denominator > 0) e.threshold_price = b.st.entry_price / denominator;
        e.rule = "收益率回撤止盈";
        e.parameters = "启动=" + exit_number(cfg.roi_trail_start) + "%，回撤=" + exit_number(cfg.roi_trail_back) +
            "个百分点，杠杆=" + exit_number(in.leverage) + "x，峰值=" + exit_number(peak) +
            "%，退出线=" + exit_number(limit) + "%，触发时收益率=" + exit_number(current) + "%";
        e.trigger_price = in.price;
        break;
    }
    case trend::ExitPath::FailedBreakout:
        e.rule = "假突破收盘退出";
        e.parameters = "周期=" + b.cfg.interval + "，已收盘K线收回入场通道内，判定时行情=" + exit_number(in.price);
        e.trigger_price = in.prev_close; e.threshold_price = b.st.entry_edge;
        break;
    case trend::ExitPath::TrendReversal: {
        e.rule = "收盘趋势反转";
        const auto& f = in.following;
        e.parameters = "周期=" + b.cfg.interval + "，反向许可=" + (b.st.pos == trend::Pos::Long ? "下跌" : "上涨") +
            "，快EMA(" + std::to_string(f.settings.fast_period) + ")=" + exit_number(f.fast) +
            "，慢EMA(" + std::to_string(f.settings.slow_period) + ")=" + exit_number(f.slow) +
            "，斜率回看=" + std::to_string(f.settings.slope_bars) + "，斜率/ATR=" + exit_number(f.slope_atr) +
            "（门槛 " + exit_number(f.settings.min_slope_atr) + "），均线间距/ATR=" + exit_number(f.separation_atr) +
            "（门槛 " + exit_number(f.settings.min_separation_atr) + "）";
        if (f.regime_close > 0) e.trigger_price = f.regime_close;
        break;
    }
    case trend::ExitPath::None: break;
    }
    e.trigger_ms = now;
    return e;
}
} // namespace ccbot
