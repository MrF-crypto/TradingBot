// Offline UI fixture. No TradingClient, credentials, exchange, or engine execution is constructed.
#include "gui/candidate_monitor.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextBrowser>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include "core/monitor_state.h"
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ccg::CandidateSnapshot snapshot; snapshot.connected = true;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 4; ++i) {
        ccbot::TrendEngine::Inspection row; auto& b = row.bot;
        b.cfg.symbol = std::vector<std::string>{"ARBUSDT", "BTCUSDT", "CHIPUSDT", "ERAUSDT"}[i];
        b.is_auto = true; b.cfg.interval = "1h"; b.cfg.rule.following.enabled = true;
        b.sig_ok = true; b.armed = true; b.sig_time = b.price_time = std::chrono::steady_clock::now();
        b.current_price = 101; b.atr = 2; b.dc_up = 103; b.dc_dn = 95;
        b.bar_open_ms = now / 3600000 * 3600000;
        b.monitor.cursor_ms = b.monitor.target_ms = b.bar_open_ms; b.monitor.updated_ms = now;
        b.monitor.regime = ccbot::following::Regime::Up; b.monitor.trend_bars = 8;
        b.monitor.shape = "通道内整理"; b.monitor.trend_since_ms = b.bar_open_ms - 7 * 3600000;
        b.following.ready = true; b.following.settings = b.cfg.rule.following; b.following.regime = b.monitor.regime;
        b.following.fast = 100; b.following.slow = 98; b.following.slope_atr = .08; b.following.separation_atr = 1;
        b.monitor.last_signal = b.following; b.monitor.last_close = 101;
        row.decision = "等待收盘突破"; row.rotation = "等待新信号";
        if (i == 0) {
            b.st.pos = ccbot::trend::Pos::Long; b.qty = 10; b.st.entry_price = 99; b.st.stop = 98; b.open_stop = 95;
            b.open_ms = now - 10 * 3600000; b.rotation_health.samples = 9; b.rotation_health.weak_bars = 3;
            row.replaceable = true; row.rotation = "旧仓符合替换条件，等待有效新信号及风险预检";
        } else if (i == 1) {
            row.has_signal = true; row.gate = "同时持仓已达上限20个"; row.decision = "上破收盘确认";
            b.following.long_confirmed = true;
        } else if (i == 2) b.reentry_after_ms = b.bar_open_ms + 2 * 3600000;
        else {
            b.monitor.recovering = b.monitor.fetch_failed = true; b.retiring = true;
            b.monitor.data_error = "K线请求失败：[-1100] symbol参数格式无效";
            row.gate = "等待历史补齐";
        }
        b.monitor.events.push_back({now, "恢复", "已从保存的断点继续监控"});
        snapshot.bots.push_back(row);
    }
    auto* widget = ccg::make_candidate_monitor([&] { return snapshot; });
    widget->resize(1480, 850); widget->show(); app.processEvents();
    auto* table = widget->findChild<QTableWidget*>(); auto* filter = widget->findChild<QComboBox*>();
    auto* search = widget->findChild<QLineEdit*>(); auto* details = widget->findChild<QTextBrowser*>();
    bool ok = table && filter && search && details;
    if (ok) {
        ok = table->rowCount() == 4 && table->columnCount() == 8 && details->toPlainText().contains("9/6");
        filter->setCurrentIndex(4); ok = ok && table->rowCount() == 1 && table->item(0, 0)->text() == "ARBUSDT";
        filter->setCurrentIndex(2); ok = ok && table->rowCount() == 1 && table->item(0, 0)->text() == "BTCUSDT";
        filter->setCurrentIndex(5); ok = ok && table->rowCount() == 1 && details->toPlainText().contains("再入场");
        filter->setCurrentIndex(6); ok = ok && table->rowCount() == 1 && table->item(0, 0)->text() == "ERAUSDT";
        ok = ok && table->item(0, 5)->text().contains("-1100") && table->item(0, 6)->text() == "K线数据异常";
        filter->setCurrentIndex(0); search->setText("btc"); ok = ok && table->rowCount() == 1;
        search->clear(); table->setCurrentCell(0, 0); app.processEvents();
        if (argc > 1) ok = widget->grab().save(QString::fromLocal8Bit(argv[1])) && ok;
    }
    QTemporaryDir checkpoint_dir;
    if (checkpoint_dir.isValid()) {
        const auto& saved = snapshot.bots.front().bot;
        ccbot::smart::Config stored; stored.tmpl = saved.cfg;
        QJsonObject record;
        record["auto"] = true; record["symbol"] = "ARBUSDT"; record["interval"] = "1h";
        record["pos"] = static_cast<int>(saved.st.pos); record["qty"] = saved.qty;
        record["auto_config"] = QJsonDocument::fromJson(QByteArray::fromStdString(ccbot::smart::config_to_json(stored))).object();
        record["monitor_checkpoint"] = QJsonDocument::fromJson(QByteArray::fromStdString(ccbot::monitor_checkpoint_json(saved))).object();
        const auto path = checkpoint_dir.filePath("sar_bots.json");
        QFile file(path);
        ok = file.open(QIODevice::WriteOnly) && ok;
        file.write(QJsonDocument(QJsonArray{record}).toJson()); file.close();
        const auto offline = ccg::read_candidate_checkpoint(path);
        ok = ok && !offline.connected && offline.bots.size() == 1;
        if (!offline.bots.empty()) ok = ok && offline.bots.front().bot.rotation_health.samples == 9 &&
            offline.bots.front().bot.monitor.cursor_ms == saved.monitor.cursor_ms &&
            offline.bots.front().bot.cfg.rule.following.enabled;
    } else ok = false;
    delete widget;
    std::puts(ok ? "PASS: candidate monitor filters, explanations and offline fixture" : "FAIL: candidate monitor fixture");
    return ok ? 0 : 1;
}
