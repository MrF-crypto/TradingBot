#include "gui/trade_history.h"
#include "core/exit_evidence_json.h"
#include "core/journal.h"
#include <QDateTime>
#include <QJsonDocument>
#include <cmath>
#include <limits>
#include <sstream>

namespace ccg {
using namespace ccbot;

void put_exit_evidence(QJsonObject& record, const char* key, const ExitEvidence& evidence) {
    if (evidence.empty()) return;
    record[key] = QJsonDocument::fromJson(QByteArray::fromStdString(exit_evidence_json(evidence))).object();
}

ExitEvidence get_exit_evidence(const QJsonValue& value) {
    ExitEvidence evidence;
    if (value.isObject())
        read_exit_evidence_json(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).toStdString(), evidence);
    return evidence;
}

QJsonObject trade_history_json(const TrendTrade& t) {
    QJsonObject o;
    o["symbol"] = QString::fromStdString(t.symbol);
    o["side"] = static_cast<int>(t.side);
    o["entry_price"] = t.entry_price;
    o["exit_price"] = t.exit_price;
    o["qty"] = t.qty;
    o["pnl"] = t.pnl;
    o["reversed"] = t.reversed;
    o["reason"] = QString::fromStdString(t.reason);
    o["close_time_ms"] = static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        t.close_time.time_since_epoch()).count());
    put_exit_evidence(o, "exit_evidence", t.exit_evidence);
    auto put = [&o](const char* key, double v) { if (std::isfinite(v)) o[key] = v; };
    put("mfe_usd", t.mfe_usd); put("mae_usd", t.mae_usd);
    put("mfe_pct", t.mfe_pct); put("mae_pct", t.mae_pct);
    put("mfe_atr", t.mfe_atr); put("mae_atr", t.mae_atr);
    put("mfe_r", t.mfe_r);     put("mae_r", t.mae_r);
    return o;
}

TrendTrade read_trade_history_json(const QJsonObject& o) {
    TrendTrade t;
    t.symbol = o["symbol"].toString().toStdString();
    // Older DCA records used a different numeric enum in "direction".
    t.side = static_cast<trend::Pos>(o["side"].toInt(static_cast<int>(trend::Pos::Flat)));
    t.entry_price = o["entry_price"].toDouble();
    t.exit_price = o["exit_price"].toDouble();
    t.qty = o["qty"].toDouble();
    t.pnl = o["pnl"].toDouble();
    t.reversed = o["reversed"].toBool();
    t.reason = o["reason"].toString().toStdString();
    t.close_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(o["close_time_ms"].toInteger()));
    auto get = [&o](const char* key) { return o[key].toDouble(std::numeric_limits<double>::quiet_NaN()); };
    t.mfe_usd = get("mfe_usd"); t.mae_usd = get("mae_usd");
    t.mfe_pct = get("mfe_pct"); t.mae_pct = get("mae_pct");
    t.mfe_atr = get("mfe_atr"); t.mae_atr = get("mae_atr");
    t.mfe_r   = get("mfe_r");   t.mae_r   = get("mae_r");
    // The trend engine recorded an ATR excursion only for positions opened with a trend-following ATR.
    t.exit_evidence = o.contains("exit_evidence") ? get_exit_evidence(o["exit_evidence"])
        : legacy_exit_evidence(t.reason, std::isfinite(t.mfe_atr));
    return t;
}

QString trade_history_details(const TrendTrade& t) {
    auto text = QString::fromStdString(format_exit_evidence(t.exit_evidence, t.exit_price, t.reason));
    if (std::isfinite(t.exit_price) && t.exit_price > 0 && exit_fill_text(t.exit_price, t.reason) == "未记录")
        text += "\n历史估算平仓价 " + QString::fromStdString(exit_number(t.exit_price));
    if (t.exit_evidence.trigger_ms > 0)
        text += "\n触发时间 " + QDateTime::fromMSecsSinceEpoch(t.exit_evidence.trigger_ms).toString("yyyy-MM-dd HH:mm:ss.zzz");
    if (!t.reason.empty() && t.reason != exit_rule_label(t.exit_evidence, t.reason))
        text += "\n原始原因 " + QString::fromStdString(t.reason);
    return text;
}

QByteArray trade_history_csv(const std::vector<TrendTrade>& trades) {
    std::ostringstream csv;
    csv << "平仓时间UTC,品种,方向,开仓价,数量,毛盈亏U,预估手续费U,预估净盈亏U,"
           "浮盈亏最高U,浮盈亏最低U,反手,平仓规则,触发时间UTC,触发时参数,规则触发线,触发行情价,平仓成交均价,原始原因\r\n";
    for (const auto& t : trades) {
        const auto& e = t.exit_evidence;
        const auto closed = std::chrono::duration_cast<std::chrono::milliseconds>(t.close_time.time_since_epoch()).count();
        csv << (closed > 0 ? Journal::utc_time(closed) : "未记录") << ',' << Journal::csv_field(t.symbol) << ','
            << (t.side == trend::Pos::Long ? "多" : t.side == trend::Pos::Short ? "空" : "未记录") << ','
            << exit_number(t.entry_price) << ',' << exit_number(t.qty) << ',' << exit_number(t.pnl) << ','
            << exit_number(t.estimated_fee(TrendEngine::kEstFeeRate)) << ',' << exit_number(t.estimated_net(TrendEngine::kEstFeeRate)) << ','
            << exit_number(t.mfe_usd) << ',' << exit_number(t.mae_usd) << ',' << (t.reversed ? "是" : "否") << ','
            << Journal::csv_field(exit_rule_label(e, t.reason)) << ','
            << (e.trigger_ms > 0 ? Journal::utc_time(e.trigger_ms) : "未记录") << ','
            << Journal::csv_field(e.parameters.empty() ? "未记录" : e.parameters) << ','
            << exit_price_text(e.threshold_price) << ',' << exit_price_text(e.trigger_price) << ','
            << exit_fill_text(t.exit_price, t.reason) << ',' << Journal::csv_field(t.reason) << "\r\n";
    }
    return QByteArray::fromStdString(csv.str());
}
} // namespace ccg
