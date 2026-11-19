#pragma once

#include "core/trend_engine.h"
#include <QJsonObject>
#include <QByteArray>

namespace ccg {

void put_exit_evidence(QJsonObject& record, const char* key, const ccbot::ExitEvidence& evidence);
ccbot::ExitEvidence get_exit_evidence(const QJsonValue& value);
QJsonObject trade_history_json(const ccbot::TrendTrade& trade);
ccbot::TrendTrade read_trade_history_json(const QJsonObject& record);
QString trade_history_details(const ccbot::TrendTrade& trade);
QByteArray trade_history_csv(const std::vector<ccbot::TrendTrade>& trades);

} // namespace ccg
