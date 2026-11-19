#pragma once

// Synthetic prices and size reproduce the CHIP exit paths. No live trade is stored.
static void test_exit_evidence_scenarios() {
    auto chip = [] {
        TrendBot b; b.cfg = mk_cfg("CHIPUSDT");
        b.cfg.use_disaster_stop = false;
        b.cfg.leverage = 2;
        b.cfg.rule.reverse = trend::ReverseMode::None;
        b.cfg.rule.roi_trail = true; b.cfg.rule.roi_trail_start = 20; b.cfg.rule.roi_trail_back = 5;
        b.st.pos = trend::Pos::Long; b.st.entry_price = b.st.peak = 0.05;
        b.open_stop = b.st.stop = 0.0475;
        b.qty = 8000;
        return b;
    };
    constexpr double crossing = 0.0514, fill = 0.05135, peak = 0.054, atr = 0.00085;
    {
        auto cli = std::make_shared<FakeClient>(); cli->fill_price = fill;
        TrendEngine engine(cli, inline_host());
        std::vector<TrendTrade> trades; std::vector<std::string> log;
        engine.set_trade_cb([&](const TrendTrade& tr) { trades.push_back(tr); });
        engine.set_log_cb([&](const std::string& text) { log.push_back(text); });
        auto b = chip();
        const auto id = engine.restore_bot(b);
        feed(engine, id, atr, peak, crossing); engine.tick(b.cfg.symbol, peak);
        auto changed = b.cfg.rule; changed.atr_mult = 4;
        engine.update_exit_params(id, changed);
        feed(engine, id, atr * 2, peak, crossing);
        cli->fill_fraction_next = 0.5;
        engine.tick(b.cfg.symbol, crossing);
        check(trades.size() == 1 && bot_of(engine, b.cfg.symbol).qty == b.qty / 2,
              "CHIP 部分成交仍保留剩余仓位");
        if (!trades.empty()) {
            const auto& tr = trades.front(); const auto& e = tr.exit_evidence;
            check(e.rule == "ATR 移动止盈" && e.trigger_price == crossing && tr.exit_price == fill &&
                  e.threshold_price && std::fabs(*e.threshold_price - (peak - 3 * atr)) < 1e-12,
                  "CHIP 的棘轮线、行情触发价与成交均价分别记录");
            check(e.parameters.find("有效ATR=0.00085") != std::string::npos &&
                  e.parameters.find("实际倍数=3") != std::string::npos && e.trigger_ms > 0,
                  "热切换参数后仍记录真正形成该棘轮线的ATR与倍数");
            const auto detail = format_exit_evidence(e, fill, tr.reason);
            check(std::any_of(log.begin(), log.end(), [&](const auto& line) {
                return line.find("部分平") != std::string::npos && line.find(detail) != std::string::npos;
            }), "部分平仓日志与交易明细共用同一条证据");
        }
        engine.tick(b.cfg.symbol, crossing - .00001);
        check(trades.size() == 2 && trades.back().exit_evidence.trigger_price == crossing - .00001 &&
              bot_of(engine, b.cfg.symbol).qty == 0, "剩余仓位在下次独立平仓时使用新的触发快照");
    }
    {
        class Receipt : public FakeClient {
        public:
            OrderOutcome confirmed_order(const std::string&, const std::string& id) override {
                return {true, "late", "", 0.05135, 8000, false, id};
            }
        };
        auto cli = std::make_shared<Receipt>();
        TrendEngine engine(cli, inline_host());
        auto b = chip(); b.cfg.rule.roi_trail_start = 12; b.cfg.rule.roi_trail_back = 6;
        const auto id = engine.restore_bot(b);
        feed(engine, id, atr, peak, crossing);
        engine.tick(b.cfg.symbol, peak);
        cli->uncertain_next = true;
        engine.tick(b.cfg.symbol, crossing);
        auto saved = bot_of(engine, b.cfg.symbol);
        const auto decision = saved.pending_close_evidence;
        check(decision.rule == "收益率回撤止盈" && decision.parameters.find("回撤=6") != std::string::npos &&
              decision.trigger_price == crossing && decision.threshold_price && *decision.threshold_price > crossing,
              "收益率与ATR线同时触碰时只记录实际执行的收益率退出");
        saved.cfg.rule.roi_trail_start = 35;
        saved.close_last_query = {};
        TrendEngine restarted(cli, inline_host());
        restarted.restore_bot(saved);
        TrendTrade received;
        restarted.set_trade_cb([&](const auto& tr) { received = tr; });
        restarted.maintain_paused_hard_stops();
        check(received.exit_evidence == decision && received.exit_price == fill && cli->calls.size() == 1 &&
              bot_of(restarted, b.cfg.symbol).pending_close_evidence.empty(),
              "重启查原单保留全部触发证据，使用确认均价且不重发委托");
    }
}
