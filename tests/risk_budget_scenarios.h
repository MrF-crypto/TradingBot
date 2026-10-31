// Included by test_trend_engine.cpp after fake exchange helpers.
static void test_risk_budget_scenarios() {
    // Interleave two opening jobs while the first market order has not returned.
    {
        auto cli = std::make_shared<FakeClient>();
        std::vector<std::function<void()>> jobs;
        auto host = inline_host();
        host.submit = [&](std::function<void()> fn) { jobs.push_back(std::move(fn)); };
        TrendEngine e(cli, host);
        e.set_risk_limits(8, 8, 20);
        auto a = mk_cfg("AAAUSDT"), b = mk_cfg("BBBUSDT");
        a.budget_usdt = b.budget_usdt = 100;
        a.rule.reverse = b.rule.reverse = trend::ReverseMode::None;
        const auto ai = e.add_bot(a), bi = e.add_bot(b);
        feed(e, ai, 2, 110, 90); feed(e, bi, 2, 110, 90);
        e.tick(a.symbol, 110); e.tick(b.symbol, 110);
        check(jobs.size() == 2, "two signals can be queued before fills");
        if (jobs.size() == 2) {
            auto first = std::move(jobs[0]), second = std::move(jobs[1]);
            cli->during_order = [&] { second(); };
            cli->fill_price = 110;
            first();
        }
        check(cli->calls.size() == 1 && bot_of(e, "AAAUSDT").qty > 0 && bot_of(e, "BBBUSDT").qty == 0,
              "in-flight reservation prevents simultaneous correlated overspending");
        check(bot_of(e, "BBBUSDT").last_action.find("预算") != std::string::npos,
              "budget rejection explains why the second signal did not trade");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host());
        e.set_risk_limits(20, 8, 20);
        for (const auto& sym : {"AAAUSDT", "BBBUSDT", "CCCUSDT"}) {
            auto c = mk_cfg(sym); c.budget_usdt = 100; c.rule.reverse = trend::ReverseMode::None;
            const auto id = e.add_bot(c); feed(e, id, 2, 110, 90);
        }
        cli->fill_price = 110; e.tick("AAAUSDT", 110); e.tick("BBBUSDT", 110);
        cli->fill_price = 90; e.tick("CCCUSDT", 90);
        check(cli->calls.size() == 2 && bot_of(e, "BBBUSDT").qty == 0 && bot_of(e, "CCCUSDT").qty > 0,
              "direction cap treats altcoin longs as one group while both sides share total risk");
        e.set_pause_new(true);
        cli->fill_price = 100; e.tick("AAAUSDT", 100);
        check(cli->last_call().reduce_only && bot_of(e, "AAAUSDT").qty == 0,
              "frozen entry gate still lets an existing position exit");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host()); e.set_risk_limits(8, 8, 20);
        auto a = mk_cfg("AAAUSDT"), b = mk_cfg("BBBUSDT");
        a.budget_usdt = b.budget_usdt = 100;
        const auto ai = e.add_bot(a), bi = e.add_bot(b);
        feed(e, ai, 2, 110, 90); feed(e, bi, 2, 110, 90);
        cli->uncertain_next = true; e.tick("AAAUSDT", 110);
        check(bot_of(e, "AAAUSDT").open_uncertain && bot_of(e, "AAAUSDT").reserved_risk > 0,
              "unknown order result keeps committed budget");
        e.tick("BBBUSDT", 110);
        check(cli->calls.size() == 1, "unknown opening blocks further new exposure");
        reconcile(e, {}); reconcile(e, {});
        check(bot_of(e, "AAAUSDT").open_uncertain, "empty positions alone do not prove an order terminal");
        e.resume_bot(ai);
        check(bot_of(e, "AAAUSDT").state == TrendBot::State::Stopped,
              "resume cannot bypass unresolved order state");
        cli->opening_terminal = true;
        e.maintain_paused_hard_stops();
        reconcile(e, {}); reconcile(e, {});
        check(!bot_of(e, "AAAUSDT").open_uncertain && bot_of(e, "AAAUSDT").reserved_risk == 0,
              "terminal order query plus two fresh empty reconciliations releases reservation");
        cli->fill_price = 110; e.tick("BBBUSDT", 110);
        check(cli->calls.size() == 2, "new entries recover after confirmed resolution");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host()); e.set_risk_limits(10, 10, 20);
        auto c = mk_cfg(); c.size_mode = TrendConfig::SizeMode::RiskBased;
        c.risk_usdt = 1; c.budget_usdt = 100;
        auto id = e.add_bot(c); feed(e, id, 2, 110, 90);
        cli->fill_price = 110; e.tick(c.symbol, 110);
        const auto b = bot_of(e, c.symbol);
        check(b.qty > 0 && b.qty * (6 + 110 * .002) <= 1 + 1e-9,
              "risk sizing includes round-trip cost allowance and rounds down");
    }
    // End-to-end confirmed signal and current-bar freshness, using an injected clock.
    {
        auto cli = std::make_shared<FakeClient>();
        const int64_t bar = 1'800'000'000'000LL;
        int64_t wall = bar + 60000;
        auto host = inline_host();
        host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        TrendEngine e(cli, host);
        auto c = mk_cfg(); c.interval = "1h"; c.rule.reverse = trend::ReverseMode::None;
        c.rule.following.enabled = true; c.rule.once_per_bar = true;
        auto id = e.add_bot(c);
        ccbot::following::Signal signal;
        signal.settings = c.rule.following; signal.ready = true;
        signal.regime = ccbot::following::Regime::Up; signal.long_confirmed = true;
        e.update_signal(id, 2, 2, true, 110, 90, bar, 0, 2, true, .8, 111, 0, 0, signal);
        cli->fill_price = 111; e.tick(c.symbol, 111);
        check(cli->calls.size() == 1 && e.get_bots()[0].st.pos == trend::Pos::Long,
              "confirmed trend signal reaches order engine");
        auto changed = c.rule;
        changed.following.fast_period = 21;
        e.update_exit_params(id, changed);
        check(!e.get_bots()[0].following.ready, "changed model invalidates its old regime snapshot");
        e.update_signal(id, 2, 2, true, 110, 90, bar, 0, 2, true, .8, 111, 0, 0, signal);
        check(!e.get_bots()[0].following.ready, "in-flight fetch for old settings cannot revive the old regime");
        wall = bar + 3600000;
        e.tick(c.symbol, 104);
        check(cli->calls.size() == 2 && cli->last_call().reduce_only,
              "stale candle snapshot still allows protective stop exit");
        e.tick(c.symbol, 111);
        check(cli->calls.size() == 2, "expired confirmation cannot reopen on a new candle");
    }
}
