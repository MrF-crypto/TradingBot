// 智能模式的界面部分：设置弹窗、候选名单、顶部徽标、调度。
// 规则与执行都在 core/smart_mode + app/smart_runner（GUI 与 headless 共用），
// 这里只负责"什么时候调"和"怎么显示"。
#include "gui/main_window.h"
#include "gui/candidate_monitor.h"
#include "core/symbol_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <tuple>

namespace ccg {

std::string MainWindow::smart_cfg_path() const {
    return (portable_data_dir() + "/ccg_smart.json").toStdString();
}
std::string MainWindow::smart_state_path() const {
    return (portable_data_dir() + "/ccg_smart_state.json").toStdString();
}

void MainWindow::save_smart_cfg() const {
    QSaveFile f(QString::fromStdString(smart_cfg_path()));
    if (!f.open(QIODevice::WriteOnly)) return;
    const auto s = smart::config_to_json(smartCfg_);
    f.write(s.data(), (qint64)s.size());
    f.commit();
}

void MainWindow::load_smart_cfg() {
    QFile f(QString::fromStdString(smart_cfg_path()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();
    std::string err;
    smart::Config c;
    if (smart::config_from_json(std::string_view(raw.constData(), (size_t)raw.size()), c, &err)) {
        smartCfg_ = c;
    } else {
        // 坏文件不能静默当成"关"——那等于用户以为在跑、其实没跑
        log(QString("⚠ 智能模式配置读取失败（%1），已按关闭处理，请重新设置")
                .arg(QString::fromStdString(err)), "WARN");
        smartCfg_.enabled = false;
    }
}

void MainWindow::startSmartRunner() {
    if (!trend_engine_ || !client_) return;
    // 断线重连：旧运行器持有旧引擎，它若有一轮扫描在跑，作废掉——不落地、不写文件
    if (smart_) smart_->cancel();
    smart_ = std::make_shared<SmartRunner>(
        client_, trend_engine_, smart_state_path(),
        [this](const std::string& m, bool warn) {
            QMetaObject::invokeMethod(this, [this, m, warn]() {
                log(QString::fromStdString(m), warn ? "WARN" : "INFO");
                if (warn) sendAlert(QString::fromStdString(m));
            }, Qt::QueuedConnection);
        });
    // 增删品种的回调可能来自扫描线程：订阅/退订一律回到 GUI 线程做
    smart_->on_symbol_added = [this](const std::string& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            if (ticker_) ticker_->subscribe(s);
            trendSigForce_.store(true);   // 新 bot 立刻拉信号，不干等一个周期
        }, Qt::QueuedConnection);
    };
    smart_->on_symbol_removed = [this](const std::string& s) {
        QMetaObject::invokeMethod(this, [this, s]() { unsubscribeIfUnused(s); },
                                  Qt::QueuedConnection);
    };
    smart_->set_journal(journal_);
    smart_->load();
    // 状态文件里没有今天的数（升级后第一次运行）：用成交明细补，亏损限额不从 0 算
    smart_->seed_day_from_history(trades_);
    applySmartConfig(smartCfg_);
    if (smartCfg_.enabled) log("智能模式已启用，稍后开始扫描全市场", "OK");
}

void MainWindow::applySmartConfig(const smart::Config& c) {
    smartCfg_ = c;
    save_smart_cfg();
    if (smart_) smart_->set_config(c);
    // 同时持仓上限：开着时由智能模式设，关掉时 GUI 本身没有这项设置 ⇒ 不限
    if (trend_engine_ && !c.enabled) trend_engine_->set_max_open_positions(0);
    refreshSmartBadge();
}

void MainWindow::smartTick() {
    if (!smart_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (smart_->due(now) && !smartBusy_.exchange(true)) {
        auto sr = smart_;
        run_async([this, sr, now]() {
            try { sr->run_once(now); } catch (...) {}
            smartBusy_.store(false);
            QMetaObject::invokeMethod(this, [this]() {
                save_trend_bots();
                refreshBotTable();
                refreshSmartBadge();
            }, Qt::QueuedConnection);
        });
        return;
    }
    // 轻量维护每分钟一次：关闭后清空仓自动 bot、当日盈亏落盘
    if (slowTickCount_ % 20 == 7 && !smartBusy_.load()) {
        smart_->housekeep(now);
        save_trend_bots();
        refreshSmartBadge();
    }
}

void MainWindow::refreshSmartBadge() {
    if (!smartBtn_) return;
    int autos = 0, holding = 0;
    if (trend_engine_)
        for (const auto& b : trend_engine_->get_bots()) {
            if (!b.is_auto) continue;
            ++autos;
            if (b.qty > 0) ++holding;
        }
    QString text, color = "#8b949e", tip;
    if (!smartCfg_.enabled) {
        text = autos > 0 ? QString("智能：关（%1 个待走完）").arg(autos) : QString("智能：关");
    } else {
        text = QString("智能 监控 %1 · 持仓 %2").arg(autos).arg(holding);
        color = "#58a6ff";
        if (smart_ && smartBusy_.load()) text += " · 扫描中";
    }
    if (smartCfg_.pause_new) { text += " · 暂停开新仓"; color = "#d29922"; }
    if (trend_engine_ && smartCfg_.enabled && smartCfg_.daily_loss_limit > 0) {
        const double r = trend_engine_->day_pnl().realized;
        tip = QString("今日（UTC）已实现 %1U（含估算手续费）/ 限额 %2U")
                  .arg(r, 0, 'f', 2).arg(smartCfg_.daily_loss_limit, 0, 'f', 0);
        if (r <= -smartCfg_.daily_loss_limit) { text += " · 已达亏损限额"; color = "#f85149"; }
    }
    if (smart_) {
        const auto rep = smart_->last_report();
        if (rep.at_ms > 0)
            tip += QString("%1上次扫描 %2：%3")
                       .arg(tip.isEmpty() ? "" : "\n")
                       .arg(QDateTime::fromMSecsSinceEpoch(rep.at_ms).toString("HH:mm"))
                       .arg(QString::fromStdString(rep.summary));
    }
    smartBtn_->setText(text);
    smartBtn_->setToolTip(tip.isEmpty() ? QString("点击打开智能模式设置") : tip);
    smartBtn_->setStyleSheet(QString("QPushButton{background:#21262d;color:%1;font-size:11px;"
                                     "padding:0 8px;}").arg(color));
}

// 智能模式只保留趋势跟踪：设置、候选名单与账户风控。
void MainWindow::openSmartDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("智能模式");
    dlg.resize(820, 720);
    auto* root = new QVBoxLayout(&dlg);
    const smart::Config c0 = smartCfg_;

    auto* top = new QHBoxLayout();
    auto* cbEnable = new QCheckBox("启用");
    cbEnable->setChecked(c0.enabled);
    cbEnable->setToolTip("扫描全市场、自动建 bot。使用趋势跟踪规则等待通道突破。");
    top->addWidget(cbEnable);
    top->addWidget(new QLabel("策略：趋势跟踪"));
    top->addStretch();
    auto* cbPause = new QCheckBox("暂停开新仓");
    cbPause->setChecked(c0.pause_new);
    cbPause->setToolTip("应急：全部 bot（含手动）暂停开新仓；不删 bot、不动已有仓位。");
    top->addWidget(cbPause);
    root->addLayout(top);

    auto* status = new QLabel();
    status->setWordWrap(true);
    status->setStyleSheet("color:#8b949e;font-size:11px;");
    root->addWidget(status);

    auto* tabs = new QTabWidget();
    root->addWidget(tabs, 1);

    // ── 控件工具 ──────────────────────────────────────────────────────────
    auto group = [](QVBoxLayout* pv, const QString& title) {
        auto* g = new QGroupBox(title);
        g->setStyleSheet("QGroupBox{color:#58a6ff;font-size:11px;font-weight:bold;"
                         "border:1px solid #21262d;border-radius:4px;margin-top:8px;"
                         "padding:10px 10px 6px 10px;}"
                         "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}");
        auto* f = new QFormLayout(g);
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pv->addWidget(g);
        return f;
    };
    auto dspin = [](double lo, double hi, double step, int dec, double val,
                    const QString& suffix = {}, const QString& special = {}) {
        auto* s = new QDoubleSpinBox();
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(dec);
        if (!suffix.isEmpty())  s->setSuffix(suffix);
        if (!special.isEmpty()) s->setSpecialValueText(special);
        s->setValue(val);
        return s;
    };
    auto ispin = [](int lo, int hi, int val, const QString& suffix = {}, const QString& special = {}) {
        auto* s = new QSpinBox();
        s->setRange(lo, hi);
        if (!suffix.isEmpty())  s->setSuffix(suffix);
        if (!special.isEmpty()) s->setSpecialValueText(special);
        s->setValue(val);
        return s;
    };
    auto hint = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet("color:#8b949e;font-size:11px;");
        return l;
    };
    auto roiRowOf = [&dlg](bool on, double start, double back) {
        auto* cb = new QCheckBox("收益率追踪止盈");
        cb->setChecked(on);
        cb->setToolTip("收益率 = 浮动盈亏 ÷ 保证金，和主表「收益率」那一列同一个数（带杠杆）。\n"
                       "最高收益率到过激活线后，从最高点回撤指定的百分点就平仓\n"
                       "（例：激活 10%、回撤 5：最高到过 +15% 后回到 +10% 就平）。\n"
                       "和止损线同时生效，谁先触发听谁的。");
        auto* s1 = new QDoubleSpinBox();
        s1->setRange(0.5, 1000); s1->setDecimals(1); s1->setSuffix(" %"); s1->setValue(start);
        auto* s2 = new QDoubleSpinBox();
        s2->setRange(0.1, 1000); s2->setDecimals(1); s2->setSuffix(" 个点"); s2->setValue(back);
        auto* row = new QHBoxLayout();
        row->addWidget(cb);
        row->addWidget(new QLabel("收益率到"));
        row->addWidget(s1);
        row->addWidget(new QLabel("后，回撤"));
        row->addWidget(s2);
        row->addWidget(new QLabel("就平"));
        row->addStretch();
        auto sync = [s1, s2](bool en) { s1->setEnabled(en); s2->setEnabled(en); };
        sync(on);
        connect(cb, &QCheckBox::toggled, &dlg, sync);
        return std::make_tuple(cb, s1, s2, row);
    };
    auto makePage = [](QVBoxLayout*& pv) {
        auto* page = new QWidget();
        pv = new QVBoxLayout(page);
        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        return scroll;
    };

    // ── 门槛 / 仓位 / 风控控件 ───────────────────────────────────
    struct Shared {
        QDoubleSpinBox *vol = nullptr, *chg = nullptr, *fund = nullptr;
        QSpinBox*       age = nullptr;
        QLineEdit*      black = nullptr;
        QComboBox*      size = nullptr;
        QDoubleSpinBox *bud = nullptr, *risk = nullptr, *buf = nullptr;
        QSpinBox*       lev = nullptr;
        QDoubleSpinBox *day = nullptr, *flt = nullptr;
    };
    const auto& t0 = c0.tmpl;
    auto mkShared = [&](double vol, int age, double chg) {
        Shared s;
        s.vol = dspin(0, 100000, 0.5, 1, vol / 1e6, " M U");
        s.vol->setToolTip("24h 成交额下限。");
        s.age = ispin(0, 3650, age, " 天");
        s.age->setToolTip("太新的币K线不够，ATR 与箱体都算不准。");
        s.chg = dspin(0, 1000, 5, 0, chg, " %", "关");
        s.chg->setToolTip("24h 涨跌幅绝对值超过这个不新加：已经走完一大段的不追。");
        s.fund = dspin(0.001, 5, 0.01, 3, c0.max_abs_funding, " %");
        s.fund->setToolTip("资金费率绝对值（每 8 小时）超过这个不新加：太拥挤，或被操纵。");
        QStringList bl;
        for (const auto& x : c0.blacklist) bl << QString::fromStdString(x);
        s.black = new QLineEdit(bl.join(", "));
        s.black->setPlaceholderText("例如 XXXUSDT, YYYUSDT");
        s.black->setToolTip("永远不进名单。已有的自动 bot 空仓时会被移除，持仓中的等走完。\n"
                            "BTC、ETH、稳定币已内置排除。");
        s.size = new QComboBox();
        s.size->addItem("固定名义", (int)TrendConfig::SizeMode::Notional);
        s.size->addItem("固定单笔风险", (int)TrendConfig::SizeMode::RiskBased);
        s.size->setCurrentIndex(t0.size_mode == TrendConfig::SizeMode::RiskBased ? 1 : 0);
        s.bud = dspin(1, 1e7, 10, 0, t0.budget_usdt, " U");
        s.bud->setToolTip("固定名义：每笔名义金额。固定单笔风险：名义上限。");
        s.risk = dspin(0, 1e6, 1, 2, t0.risk_usdt, " U");
        s.risk->setToolTip("固定单笔风险时：一笔止损愿意亏多少。数量 = 它 ÷ |开仓价 − 止损线|。");
        s.lev = ispin(1, 125, t0.leverage, " x");
        s.buf = dspin(0.01, 20, 0.05, 2, t0.disaster_stop_buffer_pct, " %");
        s.buf->setToolTip("交易所侧硬止损挂在止损线外这么远（占价格 %）。程序崩溃时它是最后一道保护。");
        s.day = dspin(0, 1e7, 10, 0, c0.daily_loss_limit, " U", "关");
        s.day->setToolTip("全部 bot（手动 + 自动）当日已实现盈亏，减去按名义 × 0.05% × 2 估算的手续费。\n"
                          "≤ −限额 就停开新仓，已有仓位照常管理。按 UTC 日计，北京时间早 8 点恢复。");
        s.flt = dspin(0, 1e7, 10, 0, c0.float_loss_limit, " U", "关");
        s.flt->setToolTip("全部持仓的浮动盈亏合计 ≤ −限额 就停开新仓，回落后自动恢复。");
        return s;
    };
    auto addGateRows = [](QFormLayout* f, const Shared& s) {
        f->addRow("24h 成交额 ≥", s.vol);
        f->addRow("上线天数 ≥", s.age);
        f->addRow("24h 涨跌上限", s.chg);
        f->addRow("资金费率 <", s.fund);
        f->addRow("拉黑", s.black);
    };
    auto addSizeRows = [](QFormLayout* f, const Shared& s) {
        f->addRow("仓位模式", s.size);
        f->addRow("名义金额", s.bud);
        f->addRow("单笔风险", s.risk);
        f->addRow("杠杆", s.lev);
        f->addRow("硬止损外扩", s.buf);
    };
    const Shared sT = mkShared(c0.min_quote_vol, c0.min_age_days, c0.max_abs_chg24);
    // ════════════════════════════ 趋势跟踪 1h 设置页 ════════════════════════════
    QVBoxLayout* tv = nullptr;
    auto* trendPage = makePage(tv);

    auto* fTGate = group(tv, "硬门槛（不满足直接排除）");
    addGateRows(fTGate, sT);
    auto* sAtrLo = dspin(0, 50, 0.1, 2, c0.atr_pct_min, " %");
    auto* sAtrHi = dspin(0.1, 100, 0.1, 2, c0.atr_pct_max, " %");
    auto* atrRow = new QHBoxLayout();
    atrRow->addWidget(sAtrLo);
    atrRow->addWidget(new QLabel("~"));
    atrRow->addWidget(sAtrHi);
    atrRow->addStretch();
    sAtrHi->setToolTip("候选波动上限，需按历史样本验证；高波动不自动产生反向信号。");
    sAtrLo->setToolTip("低波动仍可能有趋势；需评估手续费与资金费占比。");
    fTGate->addRow("1h ATR% 区间", atrRow);

    auto* fList = group(tv, "交易方向权限");
    auto* cbLong  = new QCheckBox("允许向上突破做多");
    auto* cbShort = new QCheckBox("允许向下突破做空");
    cbLong->setChecked(c0.dir_long);
    cbShort->setChecked(c0.dir_short);
    cbLong->setToolTip("两个都勾：统一候选可做多也可做空，由自身趋势与突破信号决定方向。\n"
                       "只勾一个：bot 只做那个方向。");
    cbShort->setToolTip(cbLong->toolTip());
    auto* dirRow = new QHBoxLayout();
    dirRow->addWidget(cbLong);
    dirRow->addWidget(cbShort);
    dirRow->addStretch();
    fList->addRow("方向", dirRow);
    fList->addRow(hint("通过硬过滤的品种全部进入统一候选池，不评分、不分多空榜、不限制候选数量。"
                       "向上突破做多、向下突破做空，仍需通过趋势确认和账户风控。"
                       "明确不合格时暂停新仓，空仓 bot 可移除；持仓和在途委托继续管理。"));

    auto* fTpl = group(tv, "趋势跟踪参数（不反手、硬止损强制开）");
    auto* cbIv = new QComboBox();
    cbIv->addItems({"15m", "30m", "1h", "2h", "4h"});
    cbIv->setCurrentText(QString::fromStdString(t0.interval));
    fTpl->addRow("信号周期", cbIv);
    auto* fTrend = group(tv, "趋势许可与入场确认");
    auto* cbTrend = new QCheckBox("启用绝对趋势过滤（已收盘 EMA）");
    cbTrend->setChecked(t0.rule.following.enabled);
    auto* cbClose = new QCheckBox("收盘确认突破（取消勾选为盘中突破）");
    cbClose->setToolTip("首次入场可选择盘中触发；止盈或止损完全平仓后，始终等待下一根完整K线收盘，确认新的突破再入场。");
    cbClose->setChecked(t0.rule.following.close_confirm);
    fTrend->addRow(hint("平仓后继续监控，等下一根完整K线收盘形成新突破才可再入场。"
                        "例如 1h 周期 13:20 平仓，最早 15:00 检查新信号；无突破继续等待。"));
    auto* cbReversal = new QCheckBox("确认相反趋势时离场；不立即反手");
    cbReversal->setChecked(t0.rule.following.reversal_exit);
    auto* sFast = ispin(2, 199, t0.rule.following.fast_period);
    auto* sSlow = ispin(3, 200, t0.rule.following.slow_period);
    auto* sSlopeBars = ispin(1, 20, t0.rule.following.slope_bars);
    auto* sSlope = dspin(0, 2, 0.01, 3, t0.rule.following.min_slope_atr);
    auto* sSeparation = dspin(0, 10, 0.05, 2, t0.rule.following.min_separation_atr);
    auto* sBuffer = dspin(0, 5, 0.05, 2, t0.rule.following.breakout_buffer_atr);
    auto* sLocation = dspin(0.5, 1, 0.05, 2, t0.rule.following.min_close_location);
    auto* sShortVol = dspin(0, 20, 0.1, 1, t0.rule.following.short_vol_ratio, " 倍", "不要求放量");
    fTrend->addRow(cbTrend); fTrend->addRow(cbClose); fTrend->addRow(cbReversal);
    fTrend->addRow("快 EMA", sFast); fTrend->addRow("慢 EMA", sSlow);
    fTrend->addRow("斜率回看根数", sSlopeBars); fTrend->addRow("慢线每根斜率 / ATR ≥", sSlope);
    fTrend->addRow("均线间距 / ATR ≥", sSeparation); fTrend->addRow("突破缓冲 / ATR", sBuffer);
    fTrend->addRow("收盘在振幅中的方向位置 ≥", sLocation); fTrend->addRow("做空量比 ≥", sShortVol);
    fTrend->addRow(hint("上涨许可只做多，下跌许可只做空，其余等待。收盘确认使用突破前通道和 ATR；追价上限仍生效。参数为研究起点，需回测验证。"));

    auto* sDc = ispin(2, 200, t0.rule.donchian_period, " 根");
    fTpl->addRow("通道周期", sDc);
    auto* sAtrP = ispin(2, 100, t0.rule.atr_period, " 根");
    fTpl->addRow("ATR 周期", sAtrP);
    auto* sK = dspin(0.5, 20, 0.5, 1, t0.rule.atr_mult);
    fTpl->addRow("ATR 倍数 k", sK);
    auto* sBox = dspin(0, 20, 0.5, 1, t0.rule.squeeze_box_atr, " ATR", "关");
    fTpl->addRow("横盘 箱体≤", sBox);
    auto* sEr = dspin(0, 1, 0.05, 2, t0.rule.trend_er_min, {}, "关");
    fTpl->addRow("趋势 效率比≥", sEr);
    auto* sVr = dspin(0, 20, 0.1, 1, t0.rule.breakout_vol_ratio, " 倍", "关");
    fTpl->addRow("做多量比≥（趋势过滤启用时）", sVr);
    auto* cbLock  = new QCheckBox("锁定 ATR");
    auto* cbAccel = new QCheckBox("盈利后加速收紧");
    cbLock->setChecked(t0.rule.lock_atr);
    cbAccel->setChecked(t0.rule.accel_exit);
    auto* exRow = new QHBoxLayout();
    exRow->addWidget(cbLock);
    exRow->addWidget(cbAccel);
    exRow->addStretch();
    fTpl->addRow("出场优化", exRow);
    auto* cbFail = new QCheckBox("多单");
    auto* cbFailShort = new QCheckBox("空单");
    cbFail->setChecked(t0.rule.fail_exit);
    cbFailShort->setChecked(t0.rule.fail_exit_short);
    const QString failTip = "开仓后有一根 1h 收盘跌回开仓时的通道边沿以内，不等 k×ATR 止损直接平。\n"
                            "假突破最常见的样子就是很快跌回箱体，等止损要亏满 1R。\n"
                            "多空分开勾：空单反弹回通道后多半又接着跌，判了反而砍掉能赚的单，默认只勾多单。";
    cbFail->setToolTip(failTip);
    cbFailShort->setToolTip(failTip);
    auto* failRow = new QHBoxLayout();
    failRow->addWidget(new QLabel("收盘跌回通道内就走（假突破提前出场）："));
    failRow->addWidget(cbFail);
    failRow->addWidget(cbFailShort);
    failRow->addStretch();
    fTpl->addRow("", failRow);
    auto* cbPaced = new QCheckBox("突破量比按已走时间折算");
    cbPaced->setChecked(t0.rule.vol_paced);
    cbPaced->setToolTip("不折算的话，1h 开盘 5 分钟就突破时累计量只有整根的 1/12，\n"
                        "多数突破被拦到后半小时才开，那时价格已远离通道边沿。\n"
                        "折算有下限：已走时间按至少 1/4 根算，开盘头几分钟的一点量不会被放大十几倍。");
    fTpl->addRow("", cbPaced);
    auto* cbOnce = new QCheckBox("每根K线最多开一次");
    cbOnce->setChecked(t0.rule.once_per_bar);
    cbOnce->setToolTip("同一根 1h 里出场后（止盈、止损、假突破都算）不再开，等下一根。\n"
                       "不勾的话平仓后价格还在通道外，几秒后就会原地再开一笔。");
    fTpl->addRow("", cbOnce);
    auto* sChase = dspin(0, 10, 0.1, 1, t0.rule.max_chase_atr, " ATR", "关");
    sChase->setToolTip("价格离被突破的通道边沿超过这么多倍 ATR 就不开，回到以内再开。\n"
                       "大阳线之后追进去，3×ATR 的止损很宽，掉头就是一笔大亏。");
    fTpl->addRow("追高 离边沿≤", sChase);
    auto* cbExp = new QCheckBox("多单要波动放大");
    cbExp->setChecked(t0.rule.expand_long);
    const QString expTip = "只管做多，空单不受影响。这根 1h 开盘到现在的振幅（最高 − 最低）\n"
                           "÷（已收盘K线的 ATR × √已走比例）≥ 门槛才开多。\n"
                           "振幅按时间的平方根增长：走了半根，正常振幅约是 0.71 个 ATR。\n"
                           "没到门槛就等，这根里波动起来了照样开。";
    cbExp->setToolTip(expTip);
    auto* sExp = dspin(0.1, 20, 0.1, 1, t0.rule.expand_long_min, " 倍");
    sExp->setToolTip(expTip);
    sExp->setEnabled(t0.rule.expand_long);
    connect(cbExp, &QCheckBox::toggled, sExp, &QWidget::setEnabled);
    auto* expRow = new QHBoxLayout();
    expRow->addWidget(cbExp);
    expRow->addWidget(new QLabel("折算振幅 ≥"));
    expRow->addWidget(sExp);
    expRow->addStretch();
    fTpl->addRow("波动", expRow);
    auto* sInitLoss = dspin(0, 50, 0.5, 1, t0.rule.max_init_loss_pct, " %", "关");
    sInitLoss->setToolTip("开仓时止损最多离开仓价这么多（按价格，不乘杠杆），k×ATR 更近时用 k×ATR。\n"
                          "设得比赢单常见的最大浮亏还紧，会把能赚的单提前扫掉。");
    fTpl->addRow("初始止损≤", sInitLoss);
    // 不用结构化绑定：下面的 collect lambda 要捕获它们，而捕获结构化绑定是 C++20 才放开的，
    // 老一些的 Apple clang 不认
    QCheckBox* cbTRoi = nullptr;
    QDoubleSpinBox *sTRoiStart = nullptr, *sTRoiBack = nullptr;
    QHBoxLayout* tRoiRow = nullptr;
    std::tie(cbTRoi, sTRoiStart, sTRoiBack, tRoiRow) =
        roiRowOf(t0.rule.roi_trail, t0.rule.roi_trail_start, t0.rule.roi_trail_back);
    fTpl->addRow("止盈", tRoiRow);
    auto* cbBe = new QCheckBox("保本止损");
    cbBe->setChecked(t0.rule.breakeven);
    cbBe->setToolTip("最高收益率（同追踪止盈，带杠杆）到过这条线后，止损挪到开仓价 + 手续费，只上不下。\n"
                     "管的是涨了一点、没到追踪止盈启动线又跌回来打满止损的单子，\n"
                     "所以要低于追踪止盈启动线，不然追踪止盈总是先平。\n"
                     "交易所硬止损不跟着挪，保本靠程序在线执行。");
    auto* sBe = dspin(0.5, 1000, 0.5, 1, t0.rule.breakeven_start, " %");
    sBe->setEnabled(t0.rule.breakeven);
    connect(cbBe, &QCheckBox::toggled, sBe, &QWidget::setEnabled);
    auto* beRow = new QHBoxLayout();
    beRow->addWidget(cbBe);
    beRow->addWidget(new QLabel("收益率到"));
    beRow->addWidget(sBe);
    beRow->addWidget(new QLabel("后，止损挪到开仓价"));
    beRow->addStretch();
    fTpl->addRow("保本", beRow);
    fTpl->addRow(hint("改参数后，空仓的自动 bot 在下一轮扫描时换上新参数；持仓中的立即换出场参数，周期、杠杆、仓位等走完再换。"));

    auto* fTSize = group(tv, "仓位");
    addSizeRows(fTSize, sT);

    auto* fTRisk = group(tv, "风控（全部 bot 生效）");
    auto* sMaxPos = ispin(0, 500, c0.max_positions, " 个", "不限");
    fTRisk->addRow("同时持仓上限", sMaxPos);
    auto* cbRotate = new QCheckBox("满仓换仓");
    cbRotate->setChecked(c0.rotate_when_full);
    cbRotate->setToolTip("满仓且有新的收盘突破时，评估旧的智能持仓。\n"
                        "旧仓至少持有6根完整K线、连续3根失去原方向趋势许可，且最近3根没有新的有利收盘极值，才可被替换。\n"
                        "先平后开，重新核对信号和风险预算。无每日次数或额外换仓间隔限制；任务串行执行。\n"
                        "仅在设置了持仓上限并启用趋势过滤时生效。重启后读取断点，补齐缺失K线后继续观察。");
    fTRisk->addRow("智能持仓", cbRotate);
    const auto syncRotate = [=] { cbRotate->setEnabled(cbTrend->isChecked() && sMaxPos->value() > 0); };
    connect(cbTrend, &QCheckBox::toggled, cbRotate, syncRotate);
    connect(sMaxPos, qOverload<int>(&QSpinBox::valueChanged), cbRotate, syncRotate);
    syncRotate();
    fTRisk->addRow("当日亏损限额", sT.day);
    fTRisk->addRow("持仓浮亏限额", sT.flt);
    auto* sTotalRisk = dspin(0, 1e7, 1, 2, c0.max_total_risk_usdt, " U", "关");
    auto* sDirectionRisk = dspin(0, 1e7, 1, 2, c0.max_direction_risk_usdt, " U", "关");
    auto* sRiskCost = dspin(0, 1000, 1, 1, c0.risk_cost_bps, " bps");
    fTRisk->addRow("组合计划风险上限", sTotalRisk);
    fTRisk->addRow("同向计划风险上限", sDirectionRisk);
    fTRisk->addRow("往返手续费与滑点估算", sRiskCost);
    fTRisk->addRow(hint("含在途开仓，所有山寨币同向共享预算。按初始风险预留至平仓；估算不能限制跳价下的实际损失。异动每分钟观察，最多每五分钟触发一次重扫。"));
    tv->addStretch();

    tabs->addTab(trendPage, "设置");

    // ── 候选名单──────────────────────────────────────
    const auto rep = smart_ ? smart_->last_report() : SmartRunner::Report{};
    auto* candidateMonitor = make_candidate_monitor([this] {
        if (!trend_engine_) return read_candidate_checkpoint(QString::fromStdString(trend_cfg_path()));
        CandidateSnapshot snapshot;
        snapshot.connected = trend_engine_ != nullptr && client_ != nullptr;
        if (smart_) snapshot.report = smart_->last_report();
        if (trend_engine_) snapshot.bots = trend_engine_->inspect_bots();
        return snapshot;
    }, &dlg);
    tabs->addTab(candidateMonitor, "统一候选 · 监控详情");

    // ── 状态行 ────────────────────────────────────────────────────────────
    {
        QString s = c0.enabled
            ? QString("状态：已启用（趋势跟踪），每小时整点后 1 分钟扫描一次。")
            : QString("状态：关闭。");
        if (rep.at_ms > 0)
            s += QString("\n上次扫描 %1：%2")
                     .arg(QDateTime::fromMSecsSinceEpoch(rep.at_ms).toString("MM-dd HH:mm"))
                     .arg(QString::fromStdString(rep.summary));
        else if (!smart_)
            s += "\n未连接交易所：设置可以先保存，连接后生效。";
        if (trend_engine_) {
            const auto d = trend_engine_->day_pnl();
            s += QString("\n今日（UTC）已实现 %1U（含估算手续费）").arg(d.realized, 0, 'f', 2);
            if (c0.daily_loss_limit > 0)
                s += QString(" / 限额 %1U").arg(c0.daily_loss_limit, 0, 'f', 0);
        }
        status->setText(s);
    }

    // ── 回读 ──────────────────────────────────────────────────────────────
    auto collect = [&]() {
        smart::Config c = c0;
        const Shared& s = sT;
        c.enabled   = cbEnable->isChecked();
        c.pause_new = cbPause->isChecked();

        c.min_quote_vol   = sT.vol->value() * 1e6;
        c.min_age_days    = sT.age->value();
        c.max_abs_chg24   = sT.chg->value();
        c.max_abs_funding = s.fund->value();
        c.blacklist.clear();
        for (auto x : s.black->text().split(QRegularExpression("[,，\\s]+"), Qt::SkipEmptyParts)) {
            x = QString::fromStdString(symbol_text::upper_ascii(x.trimmed().toStdString()));
            if (!x.isEmpty()) c.blacklist.insert(x.toStdString());
        }
        c.daily_loss_limit = s.day->value();
        c.float_loss_limit = s.flt->value();
        auto& t = c.tmpl;
        t.size_mode   = (TrendConfig::SizeMode)s.size->currentData().toInt();
        t.budget_usdt = s.bud->value();
        t.risk_usdt   = s.risk->value();
        t.leverage    = s.lev->value();
        t.disaster_stop_buffer_pct = s.buf->value();

        // 趋势跟踪专属
        c.atr_pct_min = sAtrLo->value();
        c.atr_pct_max = sAtrHi->value();
        c.dir_long  = cbLong->isChecked();
        c.dir_short = cbShort->isChecked();
        c.max_positions  = sMaxPos->value();
        c.rotate_when_full = cbRotate->isChecked();
        t.interval = cbIv->currentText().toStdString();
        t.rule.donchian_period    = sDc->value();
        t.rule.atr_period         = sAtrP->value();
        t.rule.atr_mult           = sK->value();
        t.rule.squeeze_box_atr    = sBox->value();
        t.rule.trend_er_min       = sEr->value();
        t.rule.following.enabled = cbTrend->isChecked();
        t.rule.following.close_confirm = cbClose->isChecked();
        t.rule.following.reversal_exit = cbReversal->isChecked();
        t.rule.following.fast_period = sFast->value();
        t.rule.following.slow_period = sSlow->value();
        t.rule.following.slope_bars = sSlopeBars->value();
        t.rule.following.min_slope_atr = sSlope->value();
        t.rule.following.min_separation_atr = sSeparation->value();
        t.rule.following.breakout_buffer_atr = sBuffer->value();
        t.rule.following.min_close_location = sLocation->value();
        t.rule.following.short_vol_ratio = sShortVol->value();

        c.max_total_risk_usdt = sTotalRisk->value();
        c.max_direction_risk_usdt = sDirectionRisk->value();
        c.risk_cost_bps = sRiskCost->value();

        t.rule.breakout_vol_ratio = sVr->value();
        t.rule.lock_atr           = cbLock->isChecked();
        t.rule.accel_exit         = cbAccel->isChecked();
        t.rule.fail_exit          = cbFail->isChecked();
        t.rule.fail_exit_short    = cbFailShort->isChecked();
        t.rule.vol_paced          = cbPaced->isChecked();
        t.rule.once_per_bar       = cbOnce->isChecked();
        t.rule.max_chase_atr      = sChase->value();
        t.rule.expand_long        = cbExp->isChecked();
        t.rule.expand_long_min    = sExp->value();
        t.rule.max_init_loss_pct  = sInitLoss->value();
        t.rule.breakeven          = cbBe->isChecked();
        t.rule.breakeven_start    = sBe->value();
        t.rule.roi_trail          = cbTRoi->isChecked();
        t.rule.roi_trail_start    = sTRoiStart->value();
        t.rule.roi_trail_back     = sTRoiBack->value();

        return c;
    };
    // 校验走和 headless 同一套（config_from_json），保证两边认同一份规则
    auto validate = [&](const smart::Config& c, smart::Config& out) {
        std::string err;
        out = c;
        if (!smart::config_from_json(smart::config_to_json(c), out, &err)) {
            QMessageBox::warning(&dlg, "设置不合法", QString::fromStdString(err));
            return false;
        }
        if (out.enabled &&
            !out.dir_long && !out.dir_short) {
            QMessageBox::warning(&dlg, "设置不合法", "启用智能模式至少要勾一个方向。");
            return false;
        }
        return true;
    };
    auto confirm_enable = [&](const smart::Config& c) {
        if (!c.enabled || c0.enabled) return true;
        return confirmDanger(
            "启用智能模式（趋势跟踪）",
            QString("智能模式会扫描全市场，在你【没有逐个看过】的币上自动建 bot 并开仓。\n\n"
                    "当前设置：同时持仓上限 %1 个，每笔名义 %2U、%3x 杠杆，当日亏损限额 %4。\n"
                    "确认这些数字在你能承受的范围内。")
                .arg(c.max_positions)
                .arg(c.tmpl.budget_usdt, 0, 'f', 0)
                .arg(c.tmpl.leverage)
                .arg(c.daily_loss_limit > 0 ? QString::number(c.daily_loss_limit, 'f', 0) + "U"
                                            : QString("未设")),
            "启用");
    };

    auto* btnRow = new QHBoxLayout();
    auto* btnScan = new QPushButton("保存并立即扫描");
    btnScan->setToolTip("保存设置，并马上扫描一次（不等下一轮）。");
    btnRow->addWidget(btnScan);
    btnRow->addStretch();
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Save)->setText("保存");
    bb->button(QDialogButtonBox::Cancel)->setText("关闭");
    btnRow->addWidget(bb);
    root->addLayout(btnRow);

    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&]() {
        smart::Config c;
        if (!validate(collect(), c) || !confirm_enable(c)) return;
        applySmartConfig(c);
        log(QString("智能模式设置已保存（%1）")
                .arg(c.enabled ? QString("启用 · 趋势跟踪") : QString("关闭")),
            "OK");
        dlg.accept();
    });
    connect(btnScan, &QPushButton::clicked, &dlg, [&]() {
        smart::Config c;
        if (!validate(collect(), c) || !confirm_enable(c)) return;
        if (!c.enabled) {
            QMessageBox::information(&dlg, "智能模式", "智能模式没有启用，不扫描。");
            return;
        }
        applySmartConfig(c);
        if (!smart_) {
            QMessageBox::information(&dlg, "智能模式", "还没连接交易所：设置已保存，连接后自动扫描。");
            dlg.accept();
            return;
        }
        smart_->force();
        smartTick();
        log("智能模式：开始扫描全市场", "OK");
        dlg.accept();
    });

    dlg.exec();
}

} // namespace ccg
