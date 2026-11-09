// Entirely offline: quotes deliberately differ from exchange execution prices.
class ReceiptClient : public FakeClient {
public:
    OrderOutcome receipt;
    int queries = 0;
    std::string queried_key;
    ReceiptClient() { receipt.uncertain = true; }
    OrderOutcome confirmed_order(const std::string&, const std::string& key) override {
        ++queries; queried_key = key; return receipt;
    }
};

static void test_accounting_engine_scenarios() {
    {
        advance(1);
        auto cli = std::make_shared<ReceiptClient>();
        TrendEngine engine(cli, inline_host());
        auto cfg = mk_cfg(); cfg.rule.reverse = trend::ReverseMode::None; cfg.use_disaster_stop = true;
        const auto id = engine.add_bot(cfg);
        feed(engine, id, 2, 110, 90);
        cli->fill_price = 0;
        engine.tick(cfg.symbol, 110);
        const auto pending = engine.get_bots()[0];
        check(pending.open_uncertain && pending.uncertain_qty > 0 && pending.st.entry_price == 0 && pending.qty == 0,
              "unpriced opening is not booked at the quote and cannot be resubmitted");
        check(!pending.disaster_stop_id.empty() && pending.disaster_stop_price > 0 && cli->ds_count("cancel") == 0,
              "known executed quantity receives a hard stop while average price is unresolved");
        for (int i = 0; i < 10; ++i) engine.tick(cfg.symbol, 111);
        check(cli->calls.size() == 1, "unpriced opening never submits a duplicate market order");
        cli->receipt = {true, "M1", "", 111.25, pending.uncertain_qty, false, "test-client-id"};
        advance(61); engine.maintain_paused_hard_stops();
        const auto recovered = engine.get_bots()[0];
        check(!recovered.open_uncertain && recovered.st.entry_price == 111.25 && recovered.qty == pending.uncertain_qty &&
              recovered.st.stop == pending.uncertain_stop && recovered.disaster_stop_id == pending.disaster_stop_id &&
              recovered.state == TrendBot::State::Stopped && cli->calls.size() == 1,
              "original opening receipt recovers actual cost without moving stops or silently resuming");
    }
    for (bool partial : {false, true}) {
        advance(1);
        auto cli = std::make_shared<ReceiptClient>();
        TrendEngine engine(cli, inline_host());
        auto cfg = mk_cfg(); cfg.rule.reverse = trend::ReverseMode::None; cfg.use_disaster_stop = true;
        const auto id = engine.add_bot(cfg);
        feed(engine, id, 2, 110, 90);
        cli->fill_price = 110.25; engine.tick(cfg.symbol, 110);
        const auto opened = engine.get_bots()[0];
        std::vector<TrendTrade> trades;
        engine.set_trade_cb([&](const auto& t) { trades.push_back(t); });
        cli->fill_price = 0; engine.tick(cfg.symbol, 100);
        auto pending = engine.get_bots()[0];
        check(trades.empty() && pending.pending_close_key == "test-client-id" && pending.qty == opened.qty &&
              pending.disaster_stop_id == opened.disaster_stop_id && cli->ds_count("cancel") == 0,
              "unpriced close retains position/stop and emits no fabricated P&L");
        for (int i = 0; i < 20; ++i) engine.tick(cfg.symbol, 99);
        check(cli->calls.size() == 2 && cli->queries == 0, "pending close throttles queries and never reposts");
        // Restart from a checkpoint: a stale account snapshot cannot erase its pending receipt.
        TrendEngine resumed(cli, inline_host());
        resumed.restore_bot(pending); resumed.set_trade_cb([&](const auto& t) { trades.push_back(t); });
        advance(20); resumed.reconcile_positions({}, std::chrono::milliseconds(0));
        check(resumed.get_bots()[0].qty == opened.qty && cli->ds_count("cancel") == 0,
              "restart and an empty account snapshot retain unresolved close accounting and protection");
        const double filled = partial ? opened.qty / 2 : opened.qty;
        cli->receipt = {true, "M2", "", 99.75, filled, false, "test-client-id"};
        resumed.maintain_paused_hard_stops();
        const auto resolved = resumed.get_bots()[0];
        check(trades.size() == 1 && trades[0].entry_price == 110.25 && trades[0].exit_price == 99.75 &&
              std::fabs(trades[0].pnl - (99.75 - 110.25) * filled) < 1e-8 && cli->calls.size() == 2 &&
              cli->queried_key == "test-client-id" && resolved.pending_close_key.empty(),
              "late close uses original order and actual fills exactly once after restart");
        check(partial ? (resolved.qty > 0 && resolved.disaster_stop_id == opened.disaster_stop_id && cli->ds_count("cancel") == 0)
                      : (resolved.qty == 0 && cli->ds_count("cancel") == 1),
              "partial close keeps protective order; only confirmed full close cancels it");
        advance(20); resumed.maintain_paused_hard_stops();
        check(trades.size() == 1, "maintenance cannot account a confirmed close twice");
    }
    {
        RotationFixture f;
        auto old = f.holding(); f.age(old);
        f.candidate();
        int rounds = 0; std::vector<std::string> logs;
        f.cli->during_round = [&](const auto&) { ++rounds; };
        f.engine.set_log_cb([&](const auto& s) { logs.push_back(s); });
        f.engine.set_profit_gate(trend::Pos::Long, "等待广度恢复", 0);
        for (int i = 0; i < 100; ++i) f.trigger();
        check(rounds == 0 && f.cli->calls.empty() && logs.empty(),
              "portfolio recovery gate prevents repeated rotation preflight/network/log traffic");
        f.engine.set_profit_gate(trend::Pos::Long, "", 0); f.trigger();
        check(f.cli->calls.size() == 2, "rotation re-evaluates when portfolio gate recovers");
    }
    {
        RotationFixture f;
        auto old = f.holding(); f.age(old); f.candidate();
        int rounds = 0, cancels = 0;
        f.cli->during_round = [&](const auto&) { ++rounds; };
        f.cli->qty_step = 100000;
        f.engine.set_log_cb([&](const auto& s) { if (s.find("满仓换仓取消") != std::string::npos) ++cancels; });
        for (int i = 0; i < 100; ++i) f.trigger();
        check(rounds == 1 && cancels == 1 && f.cli->calls.empty(), "failed quantity preflight has retry backoff and one reason notice");
        advance(16); f.engine.tick("OLDUSDT", 100); f.trigger();
        check(rounds == 2 && cancels == 1, "bounded retry retains one notice per candle and reason");
    }
}
