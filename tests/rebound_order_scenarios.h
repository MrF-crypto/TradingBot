// Included after the deterministic order/rotation fixtures.
static void test_rebound_orders() {
    using F = RotationFixture;
    {
        F f;
        f.engine.set_max_open_positions(6);
        f.engine.set_rebound_gate(true, 0);
        f.candidate("SHORTUSDT", false); f.trigger("SHORTUSDT", false);
        check(f.cli->calls.empty() && bot_of(f.engine, "SHORTUSDT").last_decision.find("急反弹保护") != std::string::npos,
              "rebound pauses automatic shorts while flat, before leverage or order creation");
        const auto monitored = f.engine.inspect_bots();
        check(monitored.size() == 1 && monitored[0].gate.find("急反弹保护") != std::string::npos,
              "the candidate inspection shows the actual directional entry block");
        auto manual = f.engine.add_bot(F::config("MANUALUSDT"), false);
        f.signal(manual, false); f.engine.tick("MANUALUSDT", 100); f.trigger("MANUALUSDT", false);
        f.candidate("LONGUSDT"); f.trigger("LONGUSDT");
        check(bot_of(f.engine, "MANUALUSDT").st.pos == trend::Pos::Short &&
              bot_of(f.engine, "LONGUSDT").st.pos == trend::Pos::Long, "manual shorts and automatic longs stay eligible");
        f.engine.set_profit_gate(trend::Pos::Short, "other guard", 0);
        f.engine.set_rebound_gate(false, 0); f.trigger("SHORTUSDT", false);
        check(bot_of(f.engine, "SHORTUSDT").last_decision.find("other guard") != std::string::npos,
              "clearing rebound pause preserves the independent portfolio guard");
    }
    {
        F f;
        const auto release = F::base + 5 * 60'000;
        f.wall = release + 1;
        f.engine.set_rebound_gate(false, release);
        const auto id = f.candidate("NEWUSDT", false, true);
        auto tick = [&] { f.engine.tick("NEWUSDT", 87.5); };
        tick();
        check(f.cli->calls.empty(), "an otherwise valid closed signal from before release cannot open");
        f.wall = F::base + F::hour + 1000;
        f.signal(id, false, false);
        f.engine.set_rebound_breakouts({{"NEWUSDT", F::base + F::hour}});
        tick();
        check(f.cli->calls.empty(), "native hourly signal requires independent close confirmation despite intrabar breakout");
        f.signal(id, false, true);
        f.cli->zero_fill_next = true;
        tick();
        check(f.cli->calls.size() == 1 && bot_of(f.engine, "NEWUSDT").last_rebound_release_ms == 0,
              "zero fill leaves fresh-short requirement intact");
        f.signal(id, false, false); tick();
        check(f.cli->calls.size() == 1, "unfilled fresh breakout cannot unlock a later intrabar short");
        f.signal(id, false, true); tick();
        check(f.cli->calls.size() == 2 && bot_of(f.engine, "NEWUSDT").last_rebound_release_ms == release,
              "actual short fill consumes only its own release epoch");
    }
    for (bool use_ema : {false, true}) {
        F f;
        const auto release = F::base + 5 * 60'000;
        f.wall = F::base + F::hour + 1000;
        f.engine.set_rebound_gate(false, release);
        auto cfg = F::config("NEWUSDT"); cfg.interval = use_ema ? "15m" : "1h";
        cfg.rule.following.enabled = use_ema;
        auto id = f.engine.add_bot(cfg, true);
        f.signal(id, false); f.engine.tick(cfg.symbol, 100);
        f.engine.set_rebound_breakouts({{cfg.symbol, F::base}, {"OTHERUSDT", F::base + F::hour}});
        f.trigger(cfg.symbol, false);
        check(f.cli->calls.empty(), "independent hourly evidence must be current and match the symbol");
        f.engine.set_rebound_breakouts({{cfg.symbol, F::base + F::hour}});
        f.trigger(cfg.symbol, false);
        check(f.cli->calls.size() == 1 && bot_of(f.engine, cfg.symbol).last_rebound_release_ms == release,
              "independent hourly evidence releases other periods and templates without EMA");
    }
    for (bool during_save : {false, true}) {
        F f;
        f.age(f.holding());
        if (!during_save) f.engine.set_rebound_gate(true, 0);
        bool canceled = false;
        f.engine.set_execution_cb([&](ExecutionEvent& e) {
            if (during_save && !e.opening && !e.completed) {
                e.key = "synthetic-rotation-close";
                f.engine.set_rebound_gate(true, 0);
            }
            if (!e.opening && e.completed && !e.result.ok && !e.result.uncertain) canceled = true;
            return true;
        });
        f.candidate("NEWUSDT", false); f.trigger("NEWUSDT", false);
        check(f.cli->calls.empty() && bot_of(f.engine, "OLDUSDT").qty == 10 &&
              !bot_of(f.engine, "OLDUSDT").pending && canceled == during_save,
              "short rotation cannot sell an old position if rebound was active or arrived during intent save");
    }
    {
        F f; f.engine.set_max_open_positions(10);
        f.candidate("NEWUSDT", false);
        int canceled = 0;
        f.engine.set_execution_cb([&](ExecutionEvent& e) {
            if (!e.completed) { e.key = "synthetic-entry"; f.engine.set_rebound_gate(true, 0); }
            else if (!e.result.ok && !e.result.uncertain) ++canceled;
            return true;
        });
        f.trigger("NEWUSDT", false);
        const auto b = bot_of(f.engine, "NEWUSDT");
        check(canceled == 1 && f.cli->calls.empty() && !b.pending && b.reserved_risk == 0,
              "pause between durable intent and final submission cancels entry and releases its risk");
    }
    {
        F f; f.engine.set_max_open_positions(6);
        auto cfg = F::config("OLDUSDT"); cfg.use_disaster_stop = true;
        const auto id = f.engine.add_bot(cfg, true); f.cli->prices[cfg.symbol] = 89;
        f.signal(id, false); f.engine.tick(cfg.symbol, 100); f.trigger(cfg.symbol, false);
        const auto held = bot_of(f.engine, cfg.symbol);
        f.engine.set_rebound_gate(true, 0);
        check(held.st.pos == trend::Pos::Short && !held.disaster_stop_id.empty() &&
              bot_of(f.engine, cfg.symbol).disaster_stop_id == held.disaster_stop_id && f.cli->ds_count("cancel") == 0,
              "rebound pause leaves the held short and its exchange stop intact");
        f.engine.tick(cfg.symbol, held.st.stop + 1);
        check(f.cli->calls.size() == 2 && f.cli->calls.back().reduce_only &&
              bot_of(f.engine, cfg.symbol).st.pos == trend::Pos::Flat, "short stop exit still executes while rebound pause is active");
    }
    {
        F f;
        auto cfg = mk_cfg("OLDUSDT"); cfg.interval = "1h";
        const auto id = f.engine.add_bot(cfg, true);
        f.cli->prices[cfg.symbol] = 111;
        f.signal(id, true); f.engine.tick(cfg.symbol, 100); f.trigger(cfg.symbol, true);
        const auto held = bot_of(f.engine, cfg.symbol);
        f.engine.set_rebound_gate(true, 0);
        std::vector<TrendTrade> trades;
        f.engine.set_trade_cb([&](const TrendTrade& tr) { trades.push_back(tr); });
        f.cli->prices[cfg.symbol] = held.st.stop - 1;
        f.engine.tick(cfg.symbol, held.st.stop - 1);
        check(held.st.pos == trend::Pos::Long && f.cli->calls.size() == 2 && f.cli->calls.back().reduce_only &&
              trades.size() == 1 && !trades[0].reversed && trades[0].reason.find("急反弹保护") != std::string::npos,
              "long stop exits while its requested automatic reversal into a short is suppressed");
    }
}
