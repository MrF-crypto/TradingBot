static void test_leverage_engine_scenarios() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        f.candidate(); int canceled = 0;
        f.engine.set_execution_cb([&](ExecutionEvent& e) {
            if (!e.completed) {
                e.key = "offline-durable-intent";
                if (scenario == 0) f.engine.set_pause_new(true);
                if (scenario == 1) f.engine.tick("NEWUSDT", 120);
                if (scenario == 2) { advance(20); f.wall += 20000; }
                if (scenario == 3) f.engine.set_risk_limits(1, 1, 20);
            } else if (!e.result.ok && !e.result.uncertain) ++canceled;
            return true;
        });
        f.trigger(); const auto b = bot_of(f.engine, "NEWUSDT");
        check(f.cli->calls.empty() && !b.pending && b.reserved_risk == 0 && b.reserved_margin == 0 && canceled == 1,
              "durable intent save cannot bypass a newer pause, quote, expiry or risk gate " + std::to_string(scenario));
    }
    for (bool is_long : {false, true}) for (bool risk_based : {false, true}) {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        f.engine.set_risk_limits(0, 0, 20);
        auto cfg = RotationFixture::config("NEWUSDT"); cfg.use_disaster_stop = true;
        if (risk_based) { cfg.size_mode = TrendConfig::SizeMode::RiskBased; cfg.risk_usdt = 10; }
        const auto id = f.engine.add_bot(cfg, true); f.signal(id, is_long); f.engine.tick(cfg.symbol, 100);
        f.cli->qty_step = 0;
        const double price = is_long ? 112 : 88;
        f.cli->prices[cfg.symbol] = price;
        f.cli->during_leverage = [&] {
            advance(20); f.wall += 20000;
            f.engine.tick(cfg.symbol, price); // valid signal; move > old 20bps snapshot tolerance
        };
        f.trigger(cfg.symbol, is_long);
        const auto b = bot_of(f.engine, cfg.symbol);
        const double want_qty = risk_based ? 10 / (6 + price * .002) : 1000 / price;
        check(f.cli->calls.size() == 1 && std::fabs(b.qty - want_qty) < 1e-8 &&
              std::fabs(b.open_stop - (is_long ? price - 6 : price + 6)) < 1e-8,
              "delayed leverage confirmation re-sizes from fresh price/stop for both directions and sizing modes");
        check(b.st.last_entry_bar_ms > 0 && !b.pending && b.reserved_risk == 0 &&
              f.cli->ds_count("place") == 1 && f.cli->ds_count("cancel") == 0,
              "one confirmed entry consumes the candle once and installs its hard stop");
    }
    for (int scenario = 0; scenario < 12; ++scenario) {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        const auto id = f.candidate();
        f.cli->during_leverage = [&] {
            advance(20); f.wall += 20000;
            if (scenario != 0) f.trigger();
            if (scenario == 1) f.signal(id, true, false);
            if (scenario == 2) f.engine.tick("NEWUSDT", 120); // chase cap exceeded
            if (scenario == 3) f.engine.set_pause_new(true);
            if (scenario == 4) f.engine.set_risk_limits(1, 1, 20);
            if (scenario == 5) f.wall += RotationFixture::hour; // previous candle signal expired
            if (scenario == 6) f.engine.stop_bot(id);
            if (scenario == 7) f.engine.set_retiring(id);
            if (scenario == 8) f.engine.set_profit_gate(trend::Pos::Long, "profit protection", 0);
            if (scenario == 9) { advance(130); f.wall += 130000; f.trigger(); } // quote fresh, signal stale
            if (scenario == 10) f.engine.tick("NEWUSDT", 100); // breakout lost while awaiting leverage
            if (scenario == 11) f.signal(id, false); // trend flipped, original long must not be replayed
        };
        f.trigger();
        const auto b = bot_of(f.engine, "NEWUSDT");
        check(f.cli->calls.empty() && !b.pending && b.st.last_entry_bar_ms == 0 &&
              b.reserved_risk == 0 && b.reserved_margin == 0,
              "after leverage recovery, stale/invalid signal, gates and stop requests still cancel entry " + std::to_string(scenario));
    }
    {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        f.candidate();
        f.cli->during_round = [&](const auto&) { f.engine.set_pause_new(true); };
        f.trigger();
        check(f.cli->calls.empty() && !bot_of(f.engine, "NEWUSDT").pending,
              "entry conditions checked again after metadata/quantity HTTP, not only after leverage");
    }
    {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        auto cfg = RotationFixture::config("NEWUSDT"); cfg.size_mode = TrendConfig::SizeMode::RiskBased; cfg.risk_usdt = 10;
        const auto id = f.engine.add_bot(cfg, true); f.signal(id); f.engine.tick(cfg.symbol, 100);
        f.cli->qty_step = 0;
        f.cli->during_leverage = [&] {
            auto signal = bot_of(f.engine, cfg.symbol).following;
            signal.atr = 4;
            signal.closed_entry.atr = 4;
            f.engine.update_signal(id, 4, 4, true, 110, 90, f.wall / RotationFixture::hour * RotationFixture::hour,
                                   5, 2, true, .8, 111, 0, 1.5, signal);
            f.trigger();
        };
        f.trigger();
        const auto b = bot_of(f.engine, cfg.symbol);
        check(f.cli->calls.size() == 1 && b.st.entry_atr == 4 && std::fabs(b.open_stop - 99) < 1e-8 &&
              std::fabs(b.qty - 10.0 / 12) < 1e-8, "changed ATR during leverage recovery updates both stop and risk-based size");
    }
    {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        auto cfg = RotationFixture::config("NEWUSDT"); cfg.size_mode = TrendConfig::SizeMode::RiskBased; cfg.risk_usdt = 10;
        const auto id = f.engine.add_bot(cfg, true); f.signal(id); f.engine.tick(cfg.symbol, 100);
        f.cli->during_round = [&](const auto&) { f.engine.set_risk_limits(0, 0, 100); };
        f.trigger();
        check(f.cli->calls.empty() && !bot_of(f.engine, cfg.symbol).pending,
              "increased cost estimate during sizing cannot overrun the single-trade risk budget");
    }
    {
        RotationFixture f; f.engine.set_rotation_enabled(false); f.engine.set_max_open_positions(10);
        f.candidate(); f.cli->leverage_ok = false;
        std::vector<std::string> logs;
        f.engine.set_log_cb([&](const std::string& line) { logs.push_back(line); });
        f.trigger();
        for (int i = 0; i < 29; ++i) { advance(1); f.wall += 1000; f.trigger(); }
        check(f.cli->leverage_calls == 1 && f.cli->calls.empty(), "failed confirmation does not retry on every tick");
        advance(1); f.wall += 1000; f.trigger();
        check(f.cli->leverage_calls == 2 && logs.size() == 1, "30-second retry is bounded and repeated failure does not spam logs");
        f.cli->leverage_ok = true; advance(30); f.wall += 30000; f.trigger();
        check(f.cli->calls.size() == 1 && bot_of(f.engine, "NEWUSDT").st.last_entry_bar_ms > 0,
              "failed leverage confirmation does not consume the entry candle; valid signal can recover later");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.cli->during_leverage = [&] { advance(20); f.wall += 20000; f.cli->prices["NEWUSDT"] = 112; f.engine.tick("NEWUSDT", 112); };
        f.trigger();
        const auto b = bot_of(f.engine, "NEWUSDT");
        check(f.cli->calls.size() == 2 && f.cli->call_at(0).reduce_only &&
              std::fabs(b.open_stop - 106) < 1e-8 && !b.rotation_reserved,
              "rotation can recover leverage with a fresh still-valid quote after closing the old holding");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.cli->during_leverage = [&] { f.engine.set_pause_new(true); };
        f.trigger();
        check(f.cli->calls.size() == 1 && f.cli->call_at(0).reduce_only &&
              !bot_of(f.engine, "NEWUSDT").pending && !bot_of(f.engine, "NEWUSDT").rotation_reserved,
              "rotation rechecks gates after leverage and releases the replacement reservation");
    }
    {
        auto cli = std::make_shared<FakeClient>(); TrendEngine engine(cli, inline_host());
        auto held_cfg = mk_cfg("HELDUSDT"); held_cfg.use_disaster_stop = true;
        held_cfg.rule.reverse = trend::ReverseMode::None;
        const auto held = engine.add_bot(held_cfg); feed(engine, held, 2, 110, 90); engine.tick(held_cfg.symbol, 111);
        const auto before = bot_of(engine, held_cfg.symbol);
        const auto fresh = engine.add_bot(mk_cfg("NEWUSDT")); feed(engine, fresh, 2, 110, 90);
        cli->leverage_ok = false; engine.tick("NEWUSDT", 111);
        const auto after = bot_of(engine, held_cfg.symbol);
        check(after.qty == before.qty && after.st.stop == before.st.stop &&
              after.disaster_stop_id == before.disaster_stop_id && cli->ds_count("cancel") == 0,
              "another symbol's leverage failure preserves held quantity and existing stop protection");
        engine.tick(held_cfg.symbol, 90);
        check(cli->calls.size() == 2 && cli->last_call().reduce_only,
              "leverage backoff never blocks an existing position's stop-loss exit");
    }
}
