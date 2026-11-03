// Included after the fake exchange/clock helpers in test_trend_engine.cpp.
static void test_reentry_scenarios() {
    constexpr int64_t hour = 3'600'000, base = 1'800'000'000'000LL;
    for (const auto& [interval, period] : std::vector<std::pair<std::string, int64_t>>{
             {"15m", hour / 4}, {"30m", hour / 2}, {"1h", hour}, {"2h", 2 * hour}, {"4h", 4 * hour}}) {
        auto cli = std::make_shared<FakeClient>();
        const int64_t wall = base + period + 12345;
        auto host = inline_host();
        host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        TrendEngine e(cli, host);
        TrendBot held; held.cfg = mk_cfg(); held.cfg.interval = interval; held.is_auto = true;
        held.st.pos = trend::Pos::Long; held.st.entry_price = held.st.peak = 111;
        held.st.stop = 105; held.qty = 1;
        const auto id = e.restore_bot(held);
        cli->fill_price = 112; e.close_bot(id);
        check(bot_of(e, held.cfg.symbol).reentry_after_ms == base + 3 * period,
              "next complete candle is calculated for signal interval " + interval);
    }
    for (const bool is_long : {true, false}) {
        for (const bool profitable : {true, false}) {
            for (const bool intrabar : {false, true}) {
                auto cli = std::make_shared<FakeClient>();
                int64_t wall = base + 60'000;
                auto host = inline_host();
                host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
                TrendEngine e(cli, host);
                auto cfg = mk_cfg();
                cfg.interval = "1h";
                cfg.rule.reverse = trend::ReverseMode::None;
                cfg.rule.following.enabled = true;
                cfg.rule.following.close_confirm = !intrabar;
                cfg.rule.once_per_bar = false; // New guard must not depend on this older switch.
                auto id = e.add_bot(cfg, true);
                auto signal = [&](int64_t bar, bool confirmed) {
                    following::Signal s;
                    s.settings = bot_of(e, cfg.symbol).signal_following_config();
                    s.ready = true;
                    s.regime = is_long ? following::Regime::Up : following::Regime::Down;
                    s.long_confirmed = is_long && confirmed;
                    s.short_confirmed = !is_long && confirmed;
                    e.update_signal(id, 2, 2, true, 110, 90, bar, 0, 2, true, .8,
                                    is_long ? 111 : 89, 0, 0, s);
                };
                signal(base, true);
                e.tick(cfg.symbol, 100); // Intrabar automatic bot arms inside its channel.
                const double entry = is_long ? 111 : 89;
                cli->fill_price = entry; e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 1, "reentry scenario: initial order filled");

                wall = base + hour + 20 * 60'000; // Position was opened in an earlier candle.
                signal(base + hour, true);
                if (profitable) e.tick(cfg.symbol, is_long ? 130 : 70);
                cli->fill_price = profitable ? (is_long ? 123 : 77) : (is_long ? 104 : 96);
                e.tick(cfg.symbol, cli->fill_price);
                const auto exited = bot_of(e, cfg.symbol);
                check(cli->calls.size() == 2 && exited.st.pos == trend::Pos::Flat &&
                      exited.reentry_after_ms == base + 3 * hour,
                      "long/short profit/loss exit waits for the following full candle to close");
                e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "no immediate same-candle reopening after an older position exits");
                check(!e.remove_if_flat(id), "automatic pruning cannot erase an unfinished reentry wait");
                id = e.replace_bot(id, cfg);
                check(bot_of(e, cfg.symbol).reentry_after_ms == exited.reentry_after_ms,
                      "candidate parameter refresh preserves the reentry barrier");
                wall = base + 2 * hour;
                signal(base + 2 * hour, true); e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "closing the exit candle is insufficient: next full candle has only started");
                wall = base + 3 * hour - 1;
                signal(base + 2 * hour, true); e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "one millisecond before the next close is still blocked");
                wall = base + 3 * hour;
                e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "clock deadline alone cannot reuse the prior candle snapshot");
                signal(base + 3 * hour, false); e.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "new closed candle without breakout stays flat even in intrabar mode");

                // Restoring the flat bot must retain both the wait and the need for fresh confirmation.
                const auto snapshot = bot_of(e, cfg.symbol);
                bool defer = false;
                std::vector<std::function<void()>> jobs;
                host.submit = [&](std::function<void()> job) {
                    if (defer) jobs.push_back(std::move(job)); else job();
                };
                TrendEngine restored(cli, host);
                const auto restored_id = restored.restore_bot(snapshot);
                restored.reconcile_positions({}, std::chrono::milliseconds(0));
                restored.set_retiring(restored_id, false); // Simulate successful candidate requalification.
                following::Signal confirmed;
                confirmed.settings = snapshot.signal_following_config(); confirmed.ready = true;
                confirmed.regime = is_long ? following::Regime::Up : following::Regime::Down;
                confirmed.long_confirmed = is_long; confirmed.short_confirmed = !is_long;
                restored.update_signal(restored_id, 2, 2, true, 110, 90, base + 2 * hour,
                                       0, 2, true, .8, entry, 0, 0, confirmed);
                restored.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2, "restart cannot reuse an earlier confirmed breakout");
                restored.update_signal(restored_id, 2, 2, true, 110, 90, base + 3 * hour,
                                       0, 2, true, .8, entry, 0, 0, confirmed);
                restored.set_pause_new(true); restored.tick(cfg.symbol, entry);
                check(cli->calls.size() == 2 && bot_of(restored, cfg.symbol).waiting_reentry(),
                      "new confirmation still respects risk gates and is not consumed while blocked");
                restored.set_pause_new(false); cli->fill_price = entry;
                defer = true; restored.tick(cfg.symbol, entry);
                check(jobs.size() == 1, "confirmed reentry queues a single opening job");
                auto lost_confirmation = confirmed;
                lost_confirmation.long_confirmed = lost_confirmation.short_confirmed = false;
                restored.update_signal(restored_id, 2, 2, true, 110, 90, base + 3 * hour,
                                       0, 2, true, .8, entry, 0, 0, lost_confirmation);
                if (!jobs.empty()) { auto job = std::move(jobs.back()); jobs.clear(); job(); }
                check(cli->calls.size() == 2 && bot_of(restored, cfg.symbol).waiting_reentry(),
                      "async preflight rechecks closed breakout and preserves wait on rejection");
                defer = false;
                restored.update_signal(restored_id, 2, 2, true, 110, 90, base + 3 * hour,
                                       0, 2, true, .8, entry, 0, 0, confirmed);
                restored.tick(cfg.symbol, entry);
                check(cli->calls.size() == 3 && bot_of(restored, cfg.symbol).qty > 0 &&
                      bot_of(restored, cfg.symbol).reentry_after_ms == 0,
                      "fresh confirmed breakout can reenter; only the fill clears waiting state");
            }
        }
    }
    // Waiting starts after complete closure, using acknowledgement time even with stale candles.
    {
        auto cli = std::make_shared<FakeClient>();
        int64_t wall = base + 60'000;
        auto host = inline_host();
        host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        TrendEngine e(cli, host);
        auto cfg = mk_cfg(); cfg.rule.reverse = trend::ReverseMode::None; cfg.interval = "1h";
        cfg.budget_usdt = 1110; // 10 units; a half-fill is still aligned to the quantity step.
        auto id = e.add_bot(cfg, true);
        feed(e, id, 2, 110, 90, base); e.tick(cfg.symbol, 100);
        cli->fill_price = 111; e.tick(cfg.symbol, 111);
        const double original_qty = bot_of(e, cfg.symbol).qty;
        wall = base + 5 * hour + 60'000;
        cli->fail_next = true; e.close_bot(id);
        check(bot_of(e, cfg.symbol).qty == original_qty && !bot_of(e, cfg.symbol).waiting_reentry(),
              "failed close does not start reentry waiting or discard the position");
        cli->fill_fraction_next = .5; e.close_bot(id);
        check(std::fabs(bot_of(e, cfg.symbol).qty - original_qty / 2) < 1e-9 &&
              bot_of(e, cfg.symbol).st.pos == trend::Pos::Long && !bot_of(e, cfg.symbol).waiting_reentry(),
              "partial close keeps managing the residual position and does not start waiting");
        e.close_bot(id);
        check(bot_of(e, cfg.symbol).qty == 0 && bot_of(e, cfg.symbol).reentry_after_ms == base + 7 * hour,
              "final close starts waiting from actual closure time, ignoring stale signal timestamp");
    }
    // Direction may change only when a later closed breakout confirms it.
    {
        auto cli = std::make_shared<FakeClient>();
        int64_t wall = base + 60'000;
        auto host = inline_host();
        host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        TrendEngine e(cli, host);
        TrendBot held; held.cfg = mk_cfg(); held.cfg.interval = "1h"; held.is_auto = true;
        held.cfg.rule.following.enabled = true; held.cfg.rule.reverse = trend::ReverseMode::None;
        held.st.pos = trend::Pos::Long; held.st.entry_price = held.st.peak = 111;
        held.st.stop = 105; held.qty = 1;
        const auto id = e.restore_bot(held); e.set_retiring(id, false);
        cli->fill_price = 104; e.close_bot(id);
        following::Signal short_signal; short_signal.settings = bot_of(e, held.cfg.symbol).signal_following_config();
        short_signal.ready = short_signal.short_confirmed = true; short_signal.regime = following::Regime::Down;
        e.update_signal(id, 2, 2, true, 110, 90, base, 0, 2, true, -.8, 89, 0, 0, short_signal);
        e.tick(held.cfg.symbol, 89);
        check(cli->calls.size() == 1, "opposite signal cannot cause immediate reversal after exit");
        wall = base + hour;
        e.update_signal(id, 2, 2, true, 110, 90, wall, 0, 2, true, -.8, 89, 0, 0, short_signal);
        wall = base + 2 * hour;
        e.update_signal(id, 2, 2, true, 110, 90, wall, 0, 2, true, -.8, 89, 0, 0, short_signal);
        cli->fill_price = 89; e.tick(held.cfg.symbol, 89);
        check(cli->calls.size() == 2 && bot_of(e, held.cfg.symbol).st.pos == trend::Pos::Short,
              "after waiting, a new confirmed downside breakout may open short");
    }
    // A planned exchange hard-stop closure follows the same waiting rule.
    {
        auto cli = std::make_shared<FakeClient>();
        int64_t wall = base + 60'000;
        auto host = inline_host();
        host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        TrendEngine e(cli, host);
        auto cfg = mk_cfg(); cfg.interval = "1h"; cfg.rule.reverse = trend::ReverseMode::None;
        cfg.use_disaster_stop = true;
        auto id = e.add_bot(cfg, true);
        feed(e, id, 2, 110, 90, base); e.tick(cfg.symbol, 100);
        cli->fill_price = 111; e.tick(cfg.symbol, 111);
        wall = base + hour + 60'000;
        cli->close_verification.position_known = true;
        cli->close_verification.protective_fill.ok = true;
        cli->close_verification.protective_fill.executed_qty = bot_of(e, cfg.symbol).qty;
        cli->close_verification.protective_fill.avg_price = 104;
        cli->fail_next = true; cli->error_next = "ReduceOnly Order is rejected [-2022]";
        e.close_bot(id);
        check(bot_of(e, cfg.symbol).qty == 0 && bot_of(e, cfg.symbol).waiting_reentry() &&
              bot_of(e, cfg.symbol).reentry_after_ms == base + 3 * hour,
              "recognized exchange protective stop also waits for a new closed breakout");
    }
}
