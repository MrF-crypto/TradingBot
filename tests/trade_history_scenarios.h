#pragma once
#include "gui/trade_history.h"

static bool test_trade_history_scenarios() {
    using namespace ccbot;
    using namespace ccg;
    // Synthetic CHIP close in the v1.4.4 format, before trigger snapshots existed.
    QJsonObject chip{{"symbol", "CHIPUSDT"}, {"side", 1}, {"entry_price", 0.05},
        {"exit_price", 0.05135}, {"qty", 8000}, {"pnl", 10.8},
        {"close_time_ms", 1704196800000LL}, {"mfe_atr", 4.0},
        {"reason", "追踪止盈出场，等待新信号"}};
    auto legacy = read_trade_history_json(chip);
    const auto& old = legacy.exit_evidence;
    if (old.rule != "ATR 移动止盈" || !old.parameters.empty() || old.trigger_price || old.threshold_price ||
        old.trigger_ms || exit_fill_text(legacy.exit_price, legacy.reason) != "0.05135") return false;
    const auto detail = trade_history_details(legacy);
    if (!detail.contains("ATR 移动止盈") || !detail.contains("触发时参数=未记录") ||
        !detail.contains("触发价=未记录") || !detail.contains("成交均价=0.05135") ||
        !detail.contains("追踪止盈出场，等待新信号")) return false;
    auto exported = trade_history_csv({legacy});
    if (!exported.contains("ATR 移动止盈,未记录,未记录,未记录,未记录,0.05135,追踪止盈出场，等待新信号") ||
        read_trade_history_json(trade_history_json(legacy)).exit_evidence != old) return false;
    chip.remove("mfe_atr");
    if (read_trade_history_json(chip).exit_evidence.rule != "移动止盈（策略未记录）") return false;
    chip["reason"] = "交易所硬止损触发（按触发价估算）";
    const auto estimated = read_trade_history_json(chip);
    if (!trade_history_csv({estimated}).contains("未记录,交易所硬止损触发（按触发价估算）") ||
        !trade_history_details(estimated).contains("历史估算平仓价 0.05135")) return false;

    // New records take the saved trigger evidence even when the legacy text or later settings differ.
    auto fresh = legacy;
    fresh.exit_evidence = {"收益率回撤止盈", "启动 14%, 回撤 6 点", 0.051389, 0.0514, 1704196799500};
    const auto restored = read_trade_history_json(trade_history_json(fresh));
    exported = trade_history_csv({restored});
    return restored.exit_evidence == fresh.exit_evidence &&
        exported.contains("\"启动 14%, 回撤 6 点\",0.0514,0.051389,0.05135,") &&
        exported.contains("收益率回撤止盈") && restored.exit_price == fresh.exit_price &&
        get_exit_evidence(QJsonObject{{"rule", true}}).empty();
}
