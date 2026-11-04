// Uses the fake exchange and deterministic clock from rotation_scenarios.h.
static void test_resume_scenarios() {
    for (bool lg : {true, false}) {
        RotationFixture f;
        TrendBot held;
        held.cfg = RotationFixture::config("GUARDEDUSDT");
        held.cfg.use_disaster_stop = true;
        held.is_auto = true; held.open_ms = RotationFixture::base - RotationFixture::hour;
        held.qty = 10; held.st.pos = lg ? trend::Pos::Long : trend::Pos::Short;
        held.st.entry_price = held.st.peak = held.current_price = 100;
        held.st.entry_atr = 10; held.open_stop = held.st.stop = lg ? 95 : 105;
        held.disaster_stop_id = "existing-protection"; held.disaster_stop_price = lg ? 94 : 106;
        const auto id = f.engine.restore_bot(held);
        f.engine.set_retiring(id, false); f.age(id, lg, 3);
        const auto before = bot_of(f.engine, held.cfg.symbol);
        std::vector<std::string> logs;
        f.engine.set_log_cb([&](const std::string& s) { logs.push_back(s); });
        f.cli->stop_log.clear();
        f.engine.mark_signal_failure(id, "new candle pending", true);
        f.engine.mark_signal_failure(id, "new candle pending", true);
        f.engine.tick(held.cfg.symbol, 100);
        const auto waiting = bot_of(f.engine, held.cfg.symbol);
        check(!waiting.sig_ok && waiting.monitor.waiting_for_bar && !waiting.monitor.fetch_failed &&
              waiting.monitor.cursor_ms == before.monitor.cursor_ms && waiting.monitor.trend_bars == before.monitor.trend_bars &&
              waiting.rotation_health.samples == before.rotation_health.samples && waiting.st.stop == before.st.stop &&
              waiting.qty == before.qty && waiting.disaster_stop_id == before.disaster_stop_id,
              "rollover wait preserves long/short holdings, stop, checkpoint and observation counts");
        check(logs.empty() && f.cli->stop_log.empty() && f.cli->calls.empty(),
              "waiting never cancels protection, submits a trade or prints per-symbol global warnings");
        f.engine.mark_signal_failure(id, "persistent candle failure");
        check(bot_of(f.engine, held.cfg.symbol).monitor.fetch_failed &&
              !bot_of(f.engine, held.cfg.symbol).monitor.waiting_for_bar, "retry exhaustion changes status without resetting progress");
        f.cli->fill_price = lg ? 94 : 106;
        f.engine.tick(held.cfg.symbol, f.cli->fill_price);
        check(f.cli->calls.size() == 1 && f.cli->last_call().reduce_only &&
              bot_of(f.engine, held.cfg.symbol).qty == 0,
              "existing long/short stop still closes the position while candle data is unavailable");
    }
    for (bool lg : {true, false}) {
        RotationFixture f;
        const auto id = f.candidate("WAITUSDT", lg);
        f.engine.mark_signal_failure(id, "new candle pending", true);
        f.trigger("WAITUSDT", lg);
        check(f.cli->calls.empty(), "a previous breakout cannot open a new long/short during candle wait");
        f.signal(id, lg, true);
        check(!bot_of(f.engine, "WAITUSDT").monitor.waiting_for_bar,
              "fresh confirmed history clears the temporary wait state");
    }
    for (bool lg : {true, false}) {
        RotationFixture f; const auto id = f.holding("RESUMEUSDT", lg); f.age(id, lg, 3);
        const auto before = bot_of(f.engine, "RESUMEUSDT");
        f.engine.mark_signal_failure(id, "simulated disconnect");
        auto saved = bot_of(f.engine, "RESUMEUSDT");
        check(saved.monitor.fetch_failed && saved.monitor.data_error == "simulated disconnect",
              "data failure is distinct from normal historical backfill");
        check(saved.rotation_health.samples == 3 && saved.rotation_health.best_close == before.rotation_health.best_close &&
              saved.st.stop == before.st.stop, "network loss preserves counts, extrema and protective stop");
        const auto cursor = saved.monitor.cursor_ms;
        const auto target = cursor + 4 * RotationFixture::hour;
        following::Observation o{cursor + 2 * RotationFixture::hour, 100, saved.monitor.last_signal};
        f.engine.update_monitor_history(id, {o}, target, saved.signal_following_config());
        check(bot_of(f.engine, "RESUMEUSDT").monitor.cursor_ms == cursor,
              "out-of-order backfill cannot skip a missing candle");
        std::vector<following::Observation> history;
        for (int i = 1; i <= 4; ++i) {
            o.bar_ms = cursor + i * RotationFixture::hour;
            o.signal.regime = i == 2 ? (lg ? following::Regime::Up : following::Regime::Down) : following::Regime::Range;
            o.signal.long_confirmed = lg; o.signal.short_confirmed = !lg;
            history.push_back(o);
        }
        f.engine.update_monitor_history(id, history, target, saved.signal_following_config());
        saved = bot_of(f.engine, "RESUMEUSDT");
        check(saved.rotation_health.samples == 7 && saved.rotation_health.weak_bars == 2 &&
              saved.monitor.cursor_ms == target && !saved.monitor.recovering && !saved.monitor.fetch_failed,
              "chronological recovery counts missing candles and resets weakness at an intervening valid trend");
        check(f.cli->calls.empty() && saved.st.stop == before.st.stop && saved.qty == before.qty,
              "historical breakout signals never submit orders or rewrite actual fills/stops");
        f.engine.update_monitor_history(id, history, target, saved.signal_following_config());
        check(bot_of(f.engine, "RESUMEUSDT").rotation_health.samples == 7,
              "duplicate backfill is idempotent");
        f.engine.remove_bot(id);
        const auto restored_id = f.engine.restore_bot(saved);
        check(bot_of(f.engine, "RESUMEUSDT").rotation_health.weak_bars == 2 &&
              bot_of(f.engine, "RESUMEUSDT").needs_reconcile,
              "restart retains exact progress but requires fresh exchange reconciliation");
        f.engine.reconcile_positions({{"RESUMEUSDT", lg ? 1 : -1, saved.qty, saved.st.entry_price}}, std::chrono::milliseconds(6000));
        check(bot_of(f.engine, "RESUMEUSDT").needs_reconcile, "stale exchange snapshot cannot unlock restored bot");
        f.engine.reconcile_positions({{"RESUMEUSDT", lg ? 1 : -1, saved.qty, saved.st.entry_price}}, std::chrono::milliseconds(0));
        f.engine.update_monitor_history(restored_id, {}, target, saved.signal_following_config());
        check(!bot_of(f.engine, "RESUMEUSDT").needs_reconcile && !bot_of(f.engine, "RESUMEUSDT").monitor.recovering,
              "fresh reconciliation plus current cursor completes recovery without rebuilding progress");
    }
    {
        RotationFixture f;
        auto cfg = RotationFixture::config("FLATUSDT");
        const auto id = f.engine.add_bot(cfg, true);
        f.signal(id, true, false); f.engine.tick(cfg.symbol, 100);
        auto saved = bot_of(f.engine, cfg.symbol);
        saved.st.cooldown_left = 4; saved.last_counted_bar_ms = saved.monitor.cursor_ms;
        f.engine.remove_bot(id); const auto restored = f.engine.restore_bot(saved);
        const auto target = saved.monitor.cursor_ms + 3 * RotationFixture::hour;
        std::vector<following::Observation> history;
        for (int i = 1; i <= 3; ++i) history.push_back({saved.monitor.cursor_ms + i * RotationFixture::hour, 100, saved.monitor.last_signal});
        f.engine.update_monitor_history(restored, history, target, cfg.rule.following);
        auto b = bot_of(f.engine, cfg.symbol);
        check(b.armed == saved.armed && b.st.cooldown_left == 2 && b.last_counted_bar_ms == target - RotationFixture::hour,
              "restart retains arming and advances cooldown only for completed missing candles");
        const auto replacement = f.engine.replace_bot(restored, cfg);
        b = bot_of(f.engine, cfg.symbol);
        check(!replacement.empty() && b.monitor.cursor_ms == target && b.st.cooldown_left == 2 && b.armed == saved.armed,
              "candidate refresh preserves the checkpoint and entry state");
    }
}
