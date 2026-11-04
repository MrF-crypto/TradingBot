#include "gui/candidate_monitor.h"
#include "core/monitor_state.h"
#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <map>

namespace ccg {
namespace {
using namespace ccbot;
QString qs(const std::string& s) { return QString::fromStdString(s); }
QString num(double x, int digits = 3) { return std::isfinite(x) ? QString::number(x, 'f', digits) : "未知"; }
QString stamp(int64_t ms) { return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm:ss") : "—"; }
struct Row {
    QString symbol, stage, trend, shape, summary, reason, data, updated, details;
    bool signal = false, blocked = false, held = false, eligible = false, reentry = false, error = false;
};
Row project(const TrendEngine::Inspection& view, bool connected) {
    const auto& b = view.bot; const auto& m = b.monitor; const auto& h = b.rotation_health;
    const auto& f = b.following.ready ? static_cast<const following::SignalData&>(b.following) : m.last_signal;
    const auto& cfg = b.cfg.rule; const auto& fc = cfg.following;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const int64_t period = static_cast<int64_t>(TrendConfig::bar_seconds(b.cfg.interval)) * 1000;
    const auto steady = std::chrono::steady_clock::now();
    const bool stale = !b.sig_ok || b.sig_time > steady || steady - b.sig_time > std::chrono::seconds(b.cfg.effective_max_age_sec()) ||
        (fc.enabled && (b.bar_open_ms <= 0 || now < b.bar_open_ms || now - b.bar_open_ms >= period));
    const bool quote_live = connected && b.current_price > 0 && steady >= b.price_time && steady - b.price_time <= std::chrono::seconds(15);
    Row r;
    r.symbol = qs(b.cfg.symbol); r.held = b.qty > 0 && b.st.pos != trend::Pos::Flat;
    r.signal = connected && !stale && view.has_signal; r.eligible = connected && view.replaceable;
    r.reentry = !r.held && b.waiting_reentry();
    r.error = !connected || stale || !quote_live || m.recovering || b.needs_reconcile || b.open_uncertain || b.recon_noted != TrendBot::Recon::None;
    r.blocked = !r.held && r.signal && (!view.gate.empty() || view.filtered || !b.armed || b.retiring || r.error);
    r.trend = qs(regime_name(m.regime)) + QString(" · 已观察%1根").arg(m.trend_bars);
    if (stale) r.trend += "（保存值）";
    r.shape = m.shape.empty() ? "等待收盘数据" : qs(m.shape);
    if (r.held && b.st.entry_edge > 0 && m.last_close > 0 &&
        (b.st.pos == trend::Pos::Long ? m.last_close < b.st.entry_edge : m.last_close > b.st.entry_edge))
        r.shape = "收盘退回入场通道";
    if (!stale && quote_live && !b.signal_following_config().close_confirm) {
        if (b.current_price > b.dc_up) r.shape = "盘中上破，尚未收盘";
        else if (b.current_price < b.dc_dn) r.shape = "盘中下破，尚未收盘";
        else if (f.long_confirmed || f.short_confirmed) r.shape = "突破后回到通道内";
    }
    r.data = !connected ? "未连接 · 保留断点" : b.needs_reconcile ? "等待仓位/委托核对" :
        m.waiting_for_bar ? "等待新K线 · 保留进度" : m.fetch_failed ? "K线数据异常" : m.recovering ? "补齐中 · 保留进度" : stale ? "行情过期/缺失" : !quote_live ? "等待最新报价" : "实时监控";
    r.updated = stamp(m.updated_ms);
    r.reason = !m.data_error.empty() && r.error ? qs(m.data_error) : qs(view.decision);
    if (!r.held && !view.gate.empty() && !m.fetch_failed && !m.waiting_for_bar) r.reason = qs(view.gate);
    if (r.held) r.stage = b.st.pos == trend::Pos::Long ? "持有多仓" : "持有空仓";
    else if (r.reentry) r.stage = "平仓后监控";
    else if (r.blocked) r.stage = "入场受阻";
    else if (m.waiting_for_bar) r.stage = "等待新K线";
    else if (r.error) r.stage = "等待数据恢复";
    else if (!b.armed) r.stage = "等待入场准备";
    else if (r.signal) r.stage = "信号就绪 · 待复核";
    else if (m.regime == following::Regime::Range || m.regime == following::Regime::Unknown) r.stage = "等待趋势许可";
    else r.stage = b.signal_following_config().close_confirm ? "等待收盘突破" : "等待盘中突破";
    if (b.pending) r.stage = b.rotation_reserved ? "换仓预检/等待" : r.held ? "平仓处理中" : "开仓处理中";
    if (b.open_uncertain) r.stage = "委托结果待核对";
    if (b.state == TrendBot::State::Stopped) r.stage = "已停止 · 保留记录";
    if (b.retiring && !b.pending && !m.fetch_failed && !m.waiting_for_bar) r.reason = "已移出候选，保留持仓管理/再入场记录";
    QStringList detail;
    detail << r.symbol + "  |  " + r.stage + "  |  " + r.data
           << "趋势：" + r.trend + "；转变时间 " + stamp(m.trend_since_ms) + "；前态 " + qs(regime_name(m.previous))
           << "形态：" + r.shape << "当前原因：" + r.reason;
    if (b.retiring) detail << "候选资格：当前暂停新仓，等待候选重新通过过滤；已有持仓继续管理。";
    if (r.held) {
        const double change = (b.current_price - b.st.entry_price) * (b.st.pos == trend::Pos::Long ? 1 : -1);
        const double initial = std::abs(b.st.entry_price - b.open_stop);
        r.summary = QString("%1 · %2U · %3R").arg(b.st.pos == trend::Pos::Long ? "多仓" : "空仓")
            .arg(quote_live ? num(change * b.qty, 2) : "报价待更新")
            .arg(quote_live && initial > 0 ? num(change / initial, 2) : "—");
        detail << QString("持仓：%1，数量 %2；入场 %3；持有 %4 小时（开仓 %5）")
            .arg(b.st.pos == trend::Pos::Long ? "多" : "空").arg(num(b.qty, 6)).arg(num(b.st.entry_price, 8))
            .arg(b.open_ms > 0 ? num(static_cast<double>(std::max<int64_t>(0, now - b.open_ms)) / 3600000, 1) : "未知").arg(stamp(b.open_ms))
            << "浮盈亏：" + r.summary + "（按最新报价估算，未扣手续费/资金费）"
            << QString("初始止损 %1；当前移动止损 %2；持仓有利极值 %3")
                .arg(num(b.open_stop, 8), num(b.st.stop, 8), num(b.st.peak, 8))
            << QString("当前价到止损：%1%；硬止损目标 %2；委托 %3")
                .arg(quote_live ? num(std::abs(b.current_price - b.st.stop) / b.current_price * 100, 2) : "未知")
                .arg(num(b.disaster_stop_price, 8), qs(b.disaster_stop_id));
    } else {
        r.summary = f.long_confirmed ? "上破收盘证据" : f.short_confirmed ? "下破收盘证据" : "尚无确认突破";
        if (stale) r.summary += "（历史值）";
    }
    detail << "" << "入场依据（数值为最近快照；过期值仅供查看，实际下单前重新复核）";
    const auto condition = [&](const QString& name, bool ready, bool pass, const QString& values) {
        detail << QString("[%1] %2：%3").arg(!ready ? "未知" : pass ? "满足" : "未满足", name, values);
    };
    condition("趋势许可", f.ready, f.regime == following::Regime::Up || f.regime == following::Regime::Down,
        QString("EMA%1=%2，EMA%3=%4；收盘 %5").arg(fc.fast_period).arg(num(f.fast, 8)).arg(fc.slow_period).arg(num(f.slow, 8), num(m.last_close, 8)));
    condition("慢线斜率", f.ready, std::abs(f.slope_atr) > fc.min_slope_atr,
        QString("%1 ATR/根，门槛绝对值 > %2；正负决定方向").arg(num(f.slope_atr), num(fc.min_slope_atr)));
    condition("均线间距", f.ready, std::abs(f.separation_atr) > fc.min_separation_atr,
        QString("%1 ATR，门槛绝对值 > %2；需与斜率同向").arg(num(f.separation_atr), num(fc.min_separation_atr)));
    const auto confirmed = b.following.ready ? following::closed_signal(b.following) : following::Signal{};
    const auto& close_evidence = confirmed.ready ? static_cast<const following::SignalData&>(confirmed) : f;
    condition("收盘确认（盘中入场时仅供观察）", close_evidence.ready && close_evidence.settings.close_confirm,
        close_evidence.long_confirmed || close_evidence.short_confirmed,
        QString("上破 %1，下破 %2；缓冲 %3 ATR；收盘位置 多%4/空%5，门槛 ≥ %6")
        .arg(close_evidence.long_confirmed ? "是" : "否", close_evidence.short_confirmed ? "是" : "否", num(fc.breakout_buffer_atr),
            num(close_evidence.close_location), num(1 - close_evidence.close_location), num(fc.min_close_location)));
    trend::Inputs filter_input;
    filter_input.box_atr = b.box_atr; filter_input.vol_ratio = b.vol_ratio; filter_input.er_ok = b.er_ok; filter_input.er = b.er;
    const bool is_long = f.regime == following::Regime::Up;
    const bool direction_ready = f.regime == following::Regime::Up || f.regime == following::Regime::Down;
    condition("箱体/效率比过滤", !stale && direction_ready, trend::squeeze_box_pass(filter_input, cfg, is_long),
        QString("箱体 %1 ATR ≤ %2；或同向效率比 ≥ %3（门槛0表示关闭）")
            .arg(num(b.box_atr), num(cfg.squeeze_box_atr), num(cfg.trend_er_min)));
    condition("量比过滤", !stale && direction_ready, trend::squeeze_vol_pass(filter_input, cfg, is_long),
        QString("%1，当前方向需 ≥ %2（0表示关闭）").arg(num(b.vol_ratio), num(is_long ? cfg.breakout_vol_ratio : fc.enabled ? fc.short_vol_ratio : cfg.breakout_vol_ratio)));
    const double chase = b.atr > 0 ? (is_long ? b.current_price - b.dc_up : b.dc_dn - b.current_price) / b.atr : 0;
    condition("追价过滤", quote_live && !stale && direction_ready && b.atr > 0,
        cfg.max_chase_atr <= 0 || chase <= cfg.max_chase_atr,
        QString("通道外距离 %1 ATR / 上限 %2（负数=通道内，0上限=关闭）").arg(num(chase), num(cfg.max_chase_atr)));
    detail << QString("通道 [%1, %2]；ATR %3；入场方式 %4")
        .arg(num(b.dc_dn, 8), num(b.dc_up, 8), num(b.atr, 8), b.signal_following_config().close_confirm ? "收盘确认" : "盘中突破")
        << QString("量比 %1；多单门槛 %2；空单门槛 %3（0=关闭）")
        .arg(num(b.vol_ratio), num(cfg.breakout_vol_ratio), num(fc.enabled ? fc.short_vol_ratio : cfg.breakout_vol_ratio))
        << QString("箱体 %1 ATR / 上限 %2；效率比 %3 / 顺势豁免门槛 %4（0=关闭）")
        .arg(num(b.box_atr), num(cfg.squeeze_box_atr), b.er_ok ? num(b.er) : "未知", num(cfg.trend_er_min))
        << QString("到上沿距离 %1 ATR；到下沿距离 %2 ATR；追价上限 %3 ATR（0=关闭）")
        .arg(quote_live && b.atr > 0 ? num((b.dc_up - b.current_price) / b.atr) : "未知")
        .arg(quote_live && b.atr > 0 ? num((b.current_price - b.dc_dn) / b.atr) : "未知").arg(num(cfg.max_chase_atr))
        << "执行层最近判断：" + qs(b.last_decision)
        << "最近执行结果：" + qs(b.last_action)
        << "开仓闸门：" + (view.gate.empty() ? QString("当前未拦截；数量取整、保证金和风险预算在下单前复核") : qs(view.gate));
    detail << "" << "换仓：" + qs(view.rotation)
        << QString("完整持仓观察 %1/6 根；连续失去原方向趋势许可 %2/3 根；距有利收盘极值 %3/3 根")
            .arg(h.samples).arg(h.weak_bars).arg(h.last_favorable_ms > 0 ? QString::number(std::max<int64_t>(0, (h.last_bar_ms - h.last_favorable_ms) / period)) : "未知")
        << "有利收盘极值 " + num(h.best_close, 8) + "；时间 " + stamp(h.last_favorable_ms)
        << "最近换仓记录：" + (m.rotation_reason.empty() ? QString("无") : qs(m.rotation_reason));
    if (r.reentry) detail << "" << "再入场：继续监控，等待新收盘突破；最早信号确认时间 " + stamp(b.reentry_after_ms)
        << QString("尚需确认 %1 根；到期后仍需通过趋势、突破和风险闸门")
            .arg(std::max<int64_t>(0, (b.reentry_after_ms - m.cursor_ms + period - 1) / period));
    detail << "" << "数据与断点"
        << "最后已处理收盘 " + stamp(m.cursor_ms) + "；补齐目标 " + stamp(m.target_ms)
        << QString("待补齐 %1 根；最后成功更新 %2；冷却剩余 %3 根；入场准备 %4")
            .arg(std::max<int64_t>(0, (m.target_ms - m.cursor_ms) / period)).arg(stamp(m.updated_ms)).arg(b.st.cooldown_left).arg(b.armed ? "已就绪" : "等待")
        << "信号所属收盘 " + stamp(b.bar_open_ms) + "；K线有效边界 " + stamp(b.bar_open_ms > 0 ? b.bar_open_ms + period : 0)
        << QString("快照最长有效 %1 秒；报价最长有效15秒；任一失效均需重新获取").arg(b.cfg.effective_max_age_sec())
        << "" << "最近状态记录（最多40条；历史判断不会重放成订单）";
    for (auto it = m.events.rbegin(); it != m.events.rend(); ++it)
        detail << stamp(it->at_ms) + "  [" + qs(it->kind) + "] " + qs(it->text);
    r.details = detail.join('\n');
    return r;
}

class CandidateMonitor final : public QWidget {
    std::function<CandidateSnapshot()> provider_;
    QLineEdit* search_; QComboBox* filter_; QLabel* count_;
    QTableWidget* table_; QTextBrowser* detail_;
    std::map<QString, Row> rows_;
public:
    CandidateMonitor(std::function<CandidateSnapshot()> provider, QWidget* parent) : QWidget(parent), provider_(std::move(provider)) {
        auto* layout = new QVBoxLayout(this); auto* toolbar = new QHBoxLayout;
        search_ = new QLineEdit; search_->setPlaceholderText("搜索品种，如 BTC");
        filter_ = new QComboBox; filter_->addItems({"全部", "有信号", "入场受阻", "持仓中", "可被替换", "等待再入场", "数据异常"});
        count_ = new QLabel; toolbar->addWidget(search_); toolbar->addWidget(filter_); toolbar->addWidget(count_); layout->addLayout(toolbar);
        auto* split = new QSplitter(Qt::Vertical);
        table_ = new QTableWidget(0, 8); table_->setHorizontalHeaderLabels({"品种", "当前阶段", "最近趋势", "形态描述", "信号/持仓摘要", "当前原因", "数据状态", "更新时间"});
        table_->verticalHeader()->hide(); table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        for (int i = 0; i < 8; ++i) table_->setColumnWidth(i, i == 0 ? 110 : i == 5 ? 280 : 170);
        detail_ = new QTextBrowser; detail_->setPlaceholderText("选择品种，查看具体条件、换仓进度与恢复记录");
        split->addWidget(table_); split->addWidget(detail_); split->setSizes({330, 260}); layout->addWidget(split);
        connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
        connect(filter_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { show_detail(); });
        auto* timer = new QTimer(this); connect(timer, &QTimer::timeout, this, [this] { refresh(); }); timer->start(2000);
        refresh();
    }
    void show_detail() {
        const auto* item = table_->item(table_->currentRow(), 0);
        if (!item) { detail_->clear(); return; }
        auto it = rows_.find(item->text());
        if (it == rows_.end()) return;
        const auto scroll = detail_->verticalScrollBar()->value();
        detail_->setPlainText(it->second.details); detail_->verticalScrollBar()->setValue(scroll);
    }
    void refresh() {
        const auto snapshot = provider_();
        const auto* item = table_->item(table_->currentRow(), 0); const auto selected = item ? item->text() : QString();
        const auto scroll = table_->verticalScrollBar()->value();
        rows_.clear();
        for (const auto& c : snapshot.report.candidates) {
            Row r; r.symbol = qs(c.symbol); r.stage = "等待监控初始化"; r.reason = "已通过最近一轮候选硬过滤";
            r.summary = "扫描价 " + num(c.close, 8) + " / ATR " + num(c.atr_pct, 2) + "%";
            r.data = "扫描记录"; r.updated = stamp(snapshot.report.at_ms);
            r.details = r.symbol + "\n" + r.reason + "\n" + r.summary + "\n扫描时间 " + r.updated;
            rows_[r.symbol] = std::move(r);
        }
        for (const auto& b : snapshot.bots) if (b.bot.is_auto || rows_.count(qs(b.bot.cfg.symbol))) {
            auto r = project(b, snapshot.connected); rows_[r.symbol] = std::move(r);
        }
        table_->blockSignals(true); table_->setRowCount(0);
        const int filter = filter_->currentIndex(); int select = -1;
        for (const auto& [sym, r] : rows_) {
            if (!sym.contains(search_->text().trimmed(), Qt::CaseInsensitive)) continue;
            const bool flags[] = {true, r.signal, r.blocked, r.held, r.eligible, r.reentry, r.error};
            if (!flags[filter]) continue;
            const auto row = table_->rowCount(); table_->insertRow(row);
            const QStringList values{sym, r.stage, r.trend, r.shape, r.summary, r.reason, r.data, r.updated};
            for (int i = 0; i < values.size(); ++i) {
                auto* cell = new QTableWidgetItem(values[i]); cell->setToolTip(values[i]); table_->setItem(row, i, cell);
            }
            if (sym == selected) select = row;
        }
        if (select >= 0) table_->setCurrentCell(select, 0);
        else if (table_->rowCount() > 0) table_->setCurrentCell(0, 0);
        table_->verticalScrollBar()->setValue(scroll); table_->blockSignals(false); show_detail();
        count_->setText(QString("显示 %1 / %2 · 每2秒刷新").arg(table_->rowCount()).arg(rows_.size()));
    }
};
}
QWidget* make_candidate_monitor(std::function<CandidateSnapshot()> provider, QWidget* parent) {
    return new CandidateMonitor(std::move(provider), parent);
}
CandidateSnapshot read_candidate_checkpoint(const QString& path) {
    CandidateSnapshot snapshot;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return snapshot;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return snapshot;
    for (const auto& value : doc.array()) {
        const auto o = value.toObject();
        if (!o["auto"].toBool()) continue;
        ccbot::TrendEngine::Inspection row; auto& b = row.bot; b.is_auto = true;
        ccbot::smart::Config stored;
        if (o["auto_config"].isObject() && ccbot::smart::config_from_json(
            QJsonDocument(o["auto_config"].toObject()).toJson(QJsonDocument::Compact).toStdString(), stored, nullptr)) b.cfg = stored.tmpl;
        b.cfg.symbol = o["symbol"].toString().toStdString();
        b.cfg.interval = o["interval"].toString("1h").toStdString();
        b.state = static_cast<ccbot::TrendBot::State>(o["state"].toInt());
        b.st.pos = static_cast<ccbot::trend::Pos>(o["pos"].toInt());
        b.qty = o["qty"].toDouble(); b.st.entry_price = o["entry_price"].toDouble();
        b.st.peak = o["peak"].toDouble(); b.st.stop = o["stop"].toDouble(); b.open_stop = o["open_stop"].toDouble();
        b.open_ms = o["open_ms"].toInteger(); b.reentry_after_ms = o["reentry_after_ms"].toInteger();
        b.st.cooldown_left = o["cooldown_left"].toInt(); b.open_uncertain = o["open_uncertain"].toBool();
        b.disaster_stop_id = o["disaster_stop_id"].toString().toStdString(); b.disaster_stop_price = o["disaster_stop_price"].toDouble();
        std::string error;
        if (o["monitor_checkpoint"].isObject()) ccbot::restore_monitor_checkpoint(
            QJsonDocument(o["monitor_checkpoint"].toObject()).toJson(QJsonDocument::Compact).toStdString(), b, error);
        b.monitor.data_error = error.empty() ? "未连接：显示保存的状态，连接后核对仓位与行情" : error;
        row.rotation = "离线保存值，连接后复核换仓资格";
        snapshot.bots.push_back(std::move(row));
    }
    return snapshot;
}
}
