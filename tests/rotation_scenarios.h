// Included after fake exchange helpers. All orders are simulated; no account/network access.
class RotationClient : public FakeClient {
public:
    std::map<std::string, double> prices;
    std::function<void(const std::string&)> during_round;
    std::function<void(bool)> after_order;
    OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                    double qty, bool reduce_only) override {
        fill_price = prices.count(symbol) ? prices.at(symbol) : 100;
        auto result = FakeClient::place_market_order(symbol, side, qty, reduce_only);
        if (after_order) after_order(reduce_only);
        return result;
    }
    double round_qty(const std::string& symbol, double qty) override {
        if (during_round) during_round(symbol);
        return FakeClient::round_qty(symbol, qty);
    }
};

struct RotationFixture {
    static constexpr int64_t hour = 3600000, base = 1800000000000LL;
    int64_t wall = base;
    bool defer = false;
    std::vector<std::function<void()>> jobs;
    std::shared_ptr<RotationClient> cli = std::make_shared<RotationClient>();
    EngineHost host() {
        auto h = inline_host();
        h.now_wall = [this] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(wall)); };
        h.submit = [this](std::function<void()> fn) { if (defer) jobs.push_back(std::move(fn)); else fn(); };
        return h;
    }
    TrendEngine engine{cli, host()};
    RotationFixture() {
        advance(1);
        engine.set_max_open_positions(1);
        engine.set_rotation_enabled(true);
    }
    static TrendConfig config(const std::string& symbol, bool intrabar = false) {
        auto cfg = mk_cfg(symbol);
        cfg.interval = "1h";
        cfg.rule.reverse = trend::ReverseMode::None;
        cfg.rule.following.enabled = true;
        cfg.rule.following.close_confirm = !intrabar;
        cfg.rule.once_per_bar = false; // rotation must not rely on this optional guard
        cfg.rule.max_chase_atr = 3;
        return cfg;
    }
    std::string holding(const std::string& symbol = "OLDUSDT", bool is_long = true, bool automatic = true) {
        TrendBot b; b.cfg = config(symbol); b.is_auto = automatic;
        b.open_ms = base - hour;
        b.qty = 10; b.st.pos = is_long ? trend::Pos::Long : trend::Pos::Short;
        b.st.entry_price = b.st.peak = b.current_price = 100;
        b.open_stop = b.st.stop = is_long ? 70 : 130;
        b.st.entry_atr = 10;
        const auto id = engine.restore_bot(b);
        engine.set_retiring(id, false);
        return id;
    }
    void held_signal(const std::string& id, following::Regime regime = following::Regime::Range, double close = 100) {
        TrendBot b;
        for (const auto& item : engine.get_bots()) if (item.bot_id == id) b = item;
        following::Signal s;
        s.settings = b.signal_following_config(); s.ready = true; s.regime = regime;
        s.fast = b.st.pos == trend::Pos::Long ? 101 : 99;
        s.atr = 10; s.upper = 120; s.lower = 80;
        engine.update_signal(id, 10, 10, true, 120, 80, wall / hour * hour, 4, 1,
                             true, 0, close, 0, 1, s);
        engine.tick(b.cfg.symbol, 100);
    }
    void age(const std::string& id, bool is_long = true, int bars = 6) {
        for (int i = 0; i < bars; ++i) {
            wall = base + i * hour + 1000;
            held_signal(id, following::Regime::Range, i == 0 ? (is_long ? 101 : 99) : 100);
        }
    }
    std::string candidate(const std::string& symbol = "NEWUSDT", bool is_long = true, bool intrabar = false) {
        const auto id = engine.add_bot(config(symbol, intrabar), true);
        cli->prices[symbol] = is_long ? 111 : 89;
        signal(id, is_long, true);
        engine.tick(symbol, 100); // arm an intrabar candidate inside its channel
        return id;
    }
    void signal(const std::string& id, bool is_long = true, bool confirmed = true) {
        TrendBot b;
        for (const auto& item : engine.get_bots()) if (item.bot_id == id) b = item;
        following::Signal s;
        s.settings = b.signal_following_config(); s.ready = true;
        s.regime = is_long ? following::Regime::Up : following::Regime::Down;
        s.fast = is_long ? 105 : 95;
        s.atr = 2; s.upper = 110; s.lower = 90; s.box_atr = 10; s.volume_ratio = 2;
        s.er_ok = true; s.er = is_long ? .8 : -.8; s.range_atr = 1.5;
        s.long_confirmed = is_long && confirmed; s.short_confirmed = !is_long && confirmed;
        s.closed_entry = static_cast<const following::SignalData&>(s);
        s.closed_entry.settings.close_confirm = true;
        if (!s.settings.close_confirm) { s.atr = 4; s.upper = 112; s.lower = 88; }
        engine.update_signal(id, s.atr, s.atr, true, s.upper, s.lower, wall / hour * hour,
                             s.box_atr, s.volume_ratio, true, s.er, is_long ? 111 : 89, 0, 1.5, s);
    }
    void trigger(const std::string& symbol = "NEWUSDT", bool is_long = true) {
        engine.tick(symbol, is_long ? 111 : 89);
    }
    void drain() {
        while (!jobs.empty()) { auto fn = std::move(jobs.front()); jobs.erase(jobs.begin()); fn(); }
    }
};

static void test_rotation_scenarios() {
    for (bool old_long : {false, true}) for (bool new_long : {false, true}) for (bool intrabar : {false, true}) {
        RotationFixture f;
        auto old = f.holding("OLDUSDT", old_long); f.age(old, old_long);
        f.candidate("NEWUSDT", new_long, intrabar); f.trigger("NEWUSDT", new_long);
        check(f.cli->calls.size() == 2 && f.cli->call_at(0).reduce_only && !f.cli->call_at(1).reduce_only,
              "rotation long/short and intrabar/close: full close strictly precedes open");
        const auto b = bot_of(f.engine, "NEWUSDT");
        check(b.st.pos == (new_long ? trend::Pos::Long : trend::Pos::Short) && b.st.entry_atr == 2 &&
              std::fabs(b.open_stop - (new_long ? 105 : 95)) < 1e-9,
              "replacement uses pre-breakout closed ATR for position risk and initial stop");
        check(bot_of(f.engine, "OLDUSDT").waiting_reentry() && !b.pending && !b.rotation_reserved,
              "old holding stays monitored with reentry wait; reservation is released");
    }
    for (int scenario = 0; scenario < 10; ++scenario) {
        RotationFixture f;
        auto old = f.holding("OLDUSDT", true, scenario != 1);
        f.age(old, true, scenario == 2 ? 5 : 6);
        auto next = f.candidate();
        if (scenario == 0) f.engine.set_rotation_enabled(false);
        if (scenario == 3) f.held_signal(old, following::Regime::Up);
        if (scenario == 4) f.held_signal(old, following::Regime::Range, 102); // same bar cannot revise closed history
        if (scenario == 5) f.engine.stop_bot(old);
        if (scenario == 6) f.engine.set_pause_new(true);
        if (scenario == 7) f.signal(next, true, false);
        if (scenario == 8) { f.wall += RotationFixture::hour; f.signal(next); }
        if (scenario == 9) { advance(20); f.signal(next); } // old quote stale, new quote refreshed by tick
        if (scenario == 4) {
            f.wall += RotationFixture::hour;
            f.held_signal(old, following::Regime::Range, 102);
            f.signal(next);
        }
        f.trigger();
        check(f.cli->calls.empty(), "rotation protects disabled/manual/young/healthy/extending/stopped/paused/unconfirmed/stale holdings " + std::to_string(scenario));
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        RotationFixture f;
        auto old = f.holding(); f.age(old); f.candidate();
        if (scenario == 0) f.cli->fill_fraction_next = .5;
        if (scenario == 1) f.cli->fail_next = true;
        if (scenario == 2) f.cli->zero_fill_next = true;
        if (scenario == 3) f.cli->uncertain_next = true;
        f.trigger(); f.trigger();
        check(f.cli->calls.size() == 1 && f.cli->call_at(0).reduce_only && bot_of(f.engine, "OLDUSDT").qty > 0 &&
              bot_of(f.engine, "NEWUSDT").qty == 0 && !bot_of(f.engine, "NEWUSDT").pending,
              "partial/failed/zero/unknown close never opens a replacement or reuses the same signal " + std::to_string(scenario));
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        RotationFixture f;
        auto old = f.holding(); f.age(old); auto next = f.candidate();
        f.cli->during_order = [&] {
            if (scenario == 0) f.signal(next, true, false);
            if (scenario == 1) f.engine.set_rotation_enabled(false);
            if (scenario == 2) f.engine.set_risk_limits(1, 1, 20);
            if (scenario == 3) f.engine.set_pause_new(true);
            if (scenario == 4) f.wall += RotationFixture::hour;
        };
        f.trigger();
        check(f.cli->calls.size() == 1 && bot_of(f.engine, "OLDUSDT").qty == 0 &&
              bot_of(f.engine, "NEWUSDT").qty == 0,
              "after closing, changed signal/setting/risk/pause/expiry leaves slot empty " + std::to_string(scenario));
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.engine.set_risk_limits(1, 1, 20); f.trigger();
        check(f.cli->calls.empty(), "insufficient post-close risk budget never sacrifices the old holding");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.engine.set_daily_loss_limit(.5); f.trigger();
        check(f.cli->calls.empty(), "projected closing costs triggering daily loss limit prevent replacement");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); auto next = f.candidate();
        f.defer = true; f.trigger();
        f.engine.set_rotation_enabled(false); f.drain();
        check(f.cli->calls.empty() && !bot_of(f.engine, "NEWUSDT").pending,
              "disabling before a queued worker starts cancels without touching the holding");
        f.engine.set_rotation_enabled(true); f.signal(next); f.trigger(); f.drain();
        check(f.cli->calls.size() == 2, "cancelled preflight releases reservation for later evaluation");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); auto next = f.candidate();
        f.cli->during_round = [&](const std::string& symbol) { if (symbol == "OLDUSDT") f.signal(next, true, false); };
        f.trigger();
        check(f.cli->calls.empty() && !bot_of(f.engine, "OLDUSDT").pending,
              "signal lost while rounding close quantity cancels BEFORE old holding is sold");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.defer = true; f.trigger();
        f.candidate("LATERUSDT"); f.trigger("LATERUSDT");
        check(f.jobs.size() == 1, "simultaneous signals share one serialized replacement worker");
        f.drain(); f.trigger("LATERUSDT"); f.drain();
        check(f.cli->calls.size() == 2 && bot_of(f.engine, "LATERUSDT").qty == 0,
              "other candidates cannot steal the reserved slot or exceed capacity");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.defer = true; f.trigger();
        f.engine.set_max_open_positions(2); f.drain();
        check(f.cli->calls.size() == 1 && !f.cli->call_at(0).reduce_only && bot_of(f.engine, "OLDUSDT").qty == 10,
              "naturally available capacity cancels planned exit and keeps old holding");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.cli->after_order = [&](bool reducing) { if (reducing) f.cli->uncertain_next = true; };
        f.trigger();
        const auto b = bot_of(f.engine, "NEWUSDT");
        check(f.cli->calls.size() == 2 && b.open_uncertain && b.reserved_risk > 0 && !b.rotation_reserved,
              "unknown replacement open keeps ordinary unknown-order reservation and protection");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old);
        auto restored = bot_of(f.engine, "OLDUSDT");
        f.engine.remove_bot(old); old = f.engine.restore_bot(restored);
        f.engine.set_retiring(old, false); f.held_signal(old);
        f.candidate(); f.trigger();
        check(f.cli->calls.empty() && bot_of(f.engine, "OLDUSDT").rotation_health.samples == 6,
              "restart preserves observations but blocks replacement until exchange reconciliation");
        f.engine.reconcile_positions({{"OLDUSDT", 1, 10, 100}}, std::chrono::milliseconds(0));
        f.trigger();
        check(f.cli->calls.size() == 2, "reconciled restart reuses confirmed eligibility without six new candles");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old, true, 3);
        f.wall += 2 * RotationFixture::hour; f.held_signal(old);
        check(bot_of(f.engine, "OLDUSDT").rotation_health.samples == 3 &&
              bot_of(f.engine, "OLDUSDT").monitor.recovering,
              "missing candles retain confirmed progress and require chronological backfill");
    }
    {
        RotationFixture f; f.engine.set_max_open_positions(4);
        std::vector<std::string> held;
        for (int i = 0; i < 4; ++i) held.push_back(f.holding("OLD" + std::to_string(i) + "USDT"));
        for (int i = 0; i < 6; ++i) {
            f.wall = RotationFixture::base + i * RotationFixture::hour + 1000;
            for (const auto& id : held) f.held_signal(id, following::Regime::Range, i ? 100 : 101);
        }
        for (int i = 0; i < 4; ++i) {
            const auto sym = "NEW" + std::to_string(i) + "USDT";
            f.candidate(sym); f.trigger(sym);
        }
        int count = 0;
        for (const auto& b : f.engine.get_bots()) if (b.qty > 0) ++count;
        check(f.cli->calls.size() == 8 && count == 4,
              "four independent signals may rotate in the same candle: no interval/daily quota, capacity preserved");
    }
    {
        RotationFixture f; f.engine.set_max_open_positions(2);
        const auto weak = f.holding("WEAKUSDT");
        const auto strong = f.holding("STRONGUSDT", false);
        for (int i = 0; i < 6; ++i) {
            f.wall = RotationFixture::base + i * RotationFixture::hour + 1000;
            f.held_signal(weak, following::Regime::Range, i ? 100 : 101);
            f.held_signal(strong, following::Regime::Down, 100);
        }
        f.engine.set_risk_limits(1000, 320, 0);
        f.candidate("NEWUSDT", false); f.trigger("NEWUSDT", false);
        check(f.cli->calls.empty(), "selling a weak long cannot release exhausted short-side risk budget");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.cli->during_round = [&](const std::string& symbol) {
            if (symbol == "OLDUSDT") f.engine.set_max_open_positions(2);
        };
        f.trigger();
        check(f.cli->calls.size() == 1 && !f.cli->call_at(0).reduce_only && bot_of(f.engine, "OLDUSDT").qty == 10,
              "capacity rechecked immediately before close; a newly freed slot preserves the old holding");
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old); f.candidate();
        f.defer = true; f.trigger();
        const auto waiting = bot_of(f.engine, "NEWUSDT");
        auto other_cli = std::make_shared<RotationClient>();
        TrendEngine restored(other_cli, f.host());
        const auto id = restored.restore_bot(waiting);
        check(!id.empty() && !bot_of(restored, "NEWUSDT").open_uncertain &&
              !bot_of(restored, "NEWUSDT").pending,
              "restart during slot reservation does not invent an unknown opening order");
        f.engine.set_rotation_enabled(false); f.drain();
    }
    {
        RotationFixture f; auto old = f.holding(); f.age(old);
        f.engine.set_risk_limits(100, 100, 0);
        auto expensive = RotationFixture::config("EXPENSIVEUSDT"); expensive.budget_usdt = 10000;
        const auto first = f.engine.add_bot(expensive, true);
        f.signal(first); f.engine.tick(expensive.symbol, 100);
        f.defer = true; f.trigger(expensive.symbol);
        f.candidate("AFFORDABLEUSDT"); f.trigger("AFFORDABLEUSDT");
        f.drain();
        check(f.cli->calls.empty(), "first replacement request fails post-close risk preflight");
        f.trigger(expensive.symbol); f.drain();
        check(f.cli->calls.size() == 2 && bot_of(f.engine, "AFFORDABLEUSDT").qty > 0 &&
              bot_of(f.engine, expensive.symbol).qty == 0,
              "a repeatedly unaffordable signal cannot starve an already queued affordable signal");
    }
}
