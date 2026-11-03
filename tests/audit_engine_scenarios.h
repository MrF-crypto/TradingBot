static void test_audit_engine_scenarios() {
    for (const int case_id : {0, 1, 2, 3}) {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine engine(cli, inline_host());
        auto cfg = mk_cfg("REJECTUSDT"); cfg.rule.reverse = trend::ReverseMode::None;
        cfg.use_disaster_stop = true;
        const auto id = engine.add_bot(cfg);
        feed(engine, id, 2, 110, 90); engine.tick(cfg.symbol, 111);
        const auto before = bot_of(engine, cfg.symbol);
        cli->close_verification.position_known = case_id != 0;
        cli->close_verification.remaining_qty = case_id == 1 ? before.qty : 0;
        if (case_id == 3) {
            auto& fill = cli->close_verification.protective_fill;
            fill.ok = true; fill.executed_qty = before.qty; fill.avg_price = 90;
        }
        cli->fail_next = true; cli->error_next = "[-2022] ReduceOnly Order is rejected.";
        engine.close_bot(id);
        const auto after = bot_of(engine, cfg.symbol);
        if (case_id <= 1)
            check(after.qty == before.qty && after.st.stop == before.st.stop &&
                  after.disaster_stop_id == before.disaster_stop_id && cli->ds_count("cancel") == 0,
                  "rejected close: unknown/still-held position retains quantity, stop and protection");
        else if (case_id == 2)
            check(after.qty == 0 && after.state == TrendBot::State::Stopped && after.trade_count == 0,
                  "verified external zero without a fill: stop for review, never invent PnL");
        else
            check(after.qty == 0 && after.state == TrendBot::State::Running &&
                  after.trade_count == 1 && std::fabs(after.realized_pnl - (90-before.st.entry_price)*before.qty) < 1e-8,
                  "verified protective execution uses real average fill, not trigger price");
    }
    {
        class ReplacementClient : public FakeClient {
        public:
            std::vector<std::string> queried;
            CloseVerification verify_close(const std::string&, const std::string&,
                                             const std::string& stop) override {
                queried.push_back(stop);
                auto result = close_verification;
                if (stop == "canceled-old-stop") result.protective_fill = {};
                return result;
            }
        };
        auto cli = std::make_shared<ReplacementClient>(); TrendEngine engine(cli, inline_host());
        auto cfg = mk_cfg(); cfg.use_disaster_stop = true;
        const auto id = engine.add_bot(cfg);
        feed(engine,id,2,110,90); engine.tick(cfg.symbol,111);
        auto held = bot_of(engine,cfg.symbol);
        engine.stop_bot(id);
        held.ds_vanished_id = "canceled-old-stop";
        TrendEngine restored(cli,inline_host());
        const auto restored_id = restored.restore_bot(held);
        cli->close_verification.position_known = true;
        auto& fill = cli->close_verification.protective_fill;
        fill.ok = true; fill.executed_qty = held.qty; fill.avg_price = 90;
        cli->fail_next = true; cli->error_next = "[-2022] ReduceOnly Order is rejected.";
        restored.close_bot(restored_id);
        check(cli->queried == std::vector<std::string>{"canceled-old-stop",held.disaster_stop_id} &&
              bot_of(restored,cfg.symbol).trade_count == 1 && cli->ds_count("cancel") == 0,
              "restored canceled old stop does not hide the replacement stop's verified fill");
    }
    {
        auto cli = std::make_shared<FakeClient>(); cli->leverage_ok = false;
        TrendEngine engine(cli, inline_host());
        auto cfg = mk_cfg(); cfg.leverage = 10;
        engine.set_max_total_margin(150);
        const auto id = engine.add_bot(cfg);
        feed(engine,id,2,110,90); engine.tick(cfg.symbol,111);
        check(cli->calls.empty() && !bot_of(engine,cfg.symbol).pending,
              "leverage not confirmed: cancel entry before margin can exceed the configured cap");
        cli->leverage_ok = true; engine.tick(cfg.symbol,111);
        check(cli->calls.size() == 1, "entry can retry after leverage is confirmed");
    }
    for (bool refresh_price : {false, true}) {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        f.candidate(); f.defer = true; f.trigger();
        advance(30); f.wall += 30000;
        if (refresh_price) f.trigger();
        f.drain();
        check(f.cli->calls.size() == (refresh_price ? 1u : 0u),
              "queued entry requires a fresh quote; a fresh unchanged price remains valid");
    }
    {
        auto cli = std::make_shared<FakeClient>(); TrendEngine engine(cli, inline_host());
        const auto cfg = mk_cfg(); const auto id = engine.add_bot(cfg); feed(engine,id,2,110,90);
        engine.set_persistence_error("trend", "disk full");
        engine.set_persistence_error("smart", "permission denied");
        engine.tick(cfg.symbol,111);
        engine.set_persistence_error("trend", ""); engine.tick(cfg.symbol,111);
        check(cli->calls.empty(), "one recovered store cannot clear another persistence failure");
        engine.set_persistence_error("smart", ""); engine.set_pause_new(true); engine.tick(cfg.symbol,111);
        check(cli->calls.empty(), "persistence recovery preserves user's manual pause");
        engine.set_pause_new(false); engine.tick(cfg.symbol,111);
        engine.set_persistence_error("trend", "disk full"); engine.close_bot(id);
        check(cli->calls.size() == 2 && cli->last_call().reduce_only,
              "persistence failure blocks entries but still permits closing held risk");
    }
}
