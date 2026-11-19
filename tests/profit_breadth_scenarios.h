// Included in test_profit.cpp. All prices, symbols, and checkpoints are synthetic.
std::string sample_symbol(int n) { return "S" + std::to_string(100 + n) + "USDT"; }

void restore_waiting_sample(Fixture& f, const TrendConfig& tmpl, int count = 100) {
    auto saved = read(f.path);
    const auto signature = saved.find(",\"breadth_model\":");
    saved.erase(signature, saved.find(",\"purchase\":", signature) - signature);
    const auto guard = saved.find("{\"phase\":", saved.find("\"guards\":["));
    std::ostringstream row;
    row << R"({"phase":3,"active":true,"armed":true,"anchor":123,"peak":300,"profit":100,"breadth_peak":0.8,"breadth":0.4,"weak":2,"recovered":0,"last_bar":)"
        << now << ",\"exit_ms\":" << now << R"(,"fresh_after":0,"reason":"wait","cohort":[)";
    for (int i = 0; i < count; ++i) row << (i ? "," : "") << profit::quote(sample_symbol(i));
    row << "]}";
    saved.replace(guard, saved.find("]}", guard) + 2 - guard, row.str());
    f.manager.reset();
    std::ofstream(f.path, std::ios::binary) << saved;
    f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
    f.cfg.protect = true;
    f.manager->configure(f.cfg, true, now, tmpl);
}

int64_t guard_int(const Fixture& f, const char* name) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(simdjson::padded_string(read(f.path))).value();
    return profit::integer(doc["guards"].at(0).value(), name);
}

std::string observer_bars(int64_t bar) {
    std::ostringstream rows;
    rows << '[';
    for (int i = 0; i < 230; ++i) {
        const auto p = 100 + i;
        rows << (i ? "," : "") << "[" << (bar - (229 - i) * hour) << ",\"" << p << "\",\"" << p + 2
             << "\",\"" << p - 1 << "\",\"" << p + 1 << "\",\"100\"]";
    }
    rows << ']';
    return rows.str();
}

void breadth_recovery_tests() {
    Fixture f("breadth-observer");
    auto tmpl = smart::Config::default_template();
    restore_waiting_sample(f, tmpl);
    int64_t stamp = now + hour + 100;
    f.client->set_wall_clock_for_test([&] { return stamp; });
    std::vector<std::string> ids;
    for (int i = 0; i < 89; ++i) {
        auto b = tmpl; b.symbol = sample_symbol(i);
        ids.push_back(f.engine->add_bot(b, true));
    }
    auto publish = [&] {
        following::Signal s; s.settings = tmpl.rule.following; s.ready = true; s.regime = following::Regime::Up;
        for (const auto& id : ids)
            f.engine->update_signal(id, 2, 2, true, 110, 90, stamp / hour * hour, 0, 0, false, 0, 0, 0, 0, s);
        f.manager->evaluate(*f.engine, stamp);
    };
    publish();
    check(f.manager->status().find("89/100") != std::string::npos && guard_int(f, "last_bar") == now &&
          guard_int(f, "peak") == 300, "11 departed samples keep their original denominator, peak and recovery checkpoint");
    std::atomic<int> reads{0}, unexpected{0};
    bool stale = true;
    f.client->set_test_hook([&](const auto& method, const auto& path, const auto&, auto& out) {
        if (method == "GET" && path.starts_with("/fapi/v1/klines?symbol=")) {
            ++reads; out.body = observer_bars(stamp / hour * hour - (stale ? hour : 0)); out.code = 200;
        } else { ++unexpected; out.code = 400; out.body = "{}"; }
        return true;
    });
    f.manager->refresh_breadth(*f.client);
    check(reads == 8, "one read-only batch caps requests at eight symbols");
    f.manager->refresh_breadth(*f.client);
    f.manager->refresh_breadth(*f.client);
    publish();
    check(reads == 11 && guard_int(f, "recovered") == 0 && f.manager->status().find("新K线尚未就绪") != std::string::npos,
          "old exchange candles cannot confirm recovery, all eleven are tried before retries");
    stale = false; stamp += 61000;
    f.manager->refresh_breadth(*f.client);
    f.manager->refresh_breadth(*f.client);
    publish(); publish();
    check(f.manager->status().find("100/100") != std::string::npos && guard_int(f, "recovered") == 1,
          "independent observations restore coverage without counting the same close twice");
    f.manager.reset();
    f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
    f.manager->configure(f.cfg, true, stamp, tmpl);
    stamp += hour;
    publish();
    check(guard_int(f, "phase") == 3 && guard_int(f, "recovered") == 1, "restart retains first confirmed hour and awaits current data");
    f.manager->refresh_breadth(*f.client);
    publish();
    check(guard_int(f, "phase") == 0 && guard_int(f, "fresh_after") == stamp / hour * hour && !f.manager->needs_breadth_quotes(),
          "next confirmed close releases recovery, retires observers and requires a later breakout");
    check(f.engine->get_bots().size() == 89 && f.fake->opens == 0 && f.fake->closes == 0 && unexpected == 0,
          "departed samples are observed with public candles only and do not consume trading slots");
}

void breadth_replacement_tests() {
    Fixture f("breadth-replacement");
    auto tmpl = smart::Config::default_template();
    restore_waiting_sample(f, tmpl);
    std::set<std::string> listed;
    for (int i = 1; i <= 100; ++i) listed.insert(sample_symbol(i));
    const std::set<std::string> candidates{sample_symbol(100)};
    auto includes = [&](int n) { return read(f.path).find(profit::quote(sample_symbol(n))) != std::string::npos; };
    f.manager->update_breadth_market(listed, {}, candidates, now + hour);
    f.manager->update_breadth_market(listed, {}, candidates, now + hour + 5);
    check(includes(0) && !includes(100) && guard_int(f, "weak") == 2, "quick repeat scans or normal retirement do not replace the fixed sample");
    f.manager.reset();
    f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
    f.manager->configure(f.cfg, true, now + hour, tmpl);
    f.manager->update_breadth_market(listed, {}, {}, now + 2 * hour);
    check(includes(0), "confirmed vacancy stays in the denominator until an eligible replacement exists");
    f.manager->update_breadth_market(listed, {}, candidates, now + 2 * hour);
    check(!includes(0) && includes(100) && guard_int(f, "weak") == 0 && guard_int(f, "last_bar") == now + 2 * hour &&
          guard_int(f, "peak") == 300 && guard_int(f, "anchor") == 123 && guard_int(f, "phase") == 3,
          "persistent absence replaces one symbol and resets only breadth confirmations and baseline");
    f.manager->evaluate(*f.engine, now + 2 * hour);
    check(f.manager->status().find("0/100") != std::string::npos, "replacement keeps exact sample size with zero valid observations");
    tmpl.rule.following.fast_period += 1;
    f.manager->configure(f.cfg, true, now + 3 * hour + 500, tmpl);
    check(guard_int(f, "last_bar") == now + 3 * hour && guard_int(f, "peak") == 300 && guard_int(f, "phase") == 3,
          "new trend parameters wait for a subsequent close without resetting protection or profit peak");

    const auto stamp = now + 4 * hour;
    f.client->set_wall_clock_for_test([&] { return stamp; });
    f.manager->evaluate(*f.engine, stamp);
    auto next = tmpl; next.rule.following.slow_period++;
    std::atomic<bool> changed{false};
    const auto candles = observer_bars(stamp);
    f.client->set_test_hook([&](const auto&, const auto&, const auto&, auto& out) {
        if (!changed.exchange(true)) f.manager->configure(f.cfg, true, stamp, next);
        out.code = 200; out.body = candles; return true;
    });
    f.manager->refresh_breadth(*f.client);
    f.manager->evaluate(*f.engine, stamp);
    check(changed && f.manager->status().find("0/100") != std::string::npos && guard_int(f, "recovered") == 0,
          "same-hour results already in flight cannot enter the replacement model generation");
}

void delayed_breadth_vote_tests() {
    profit::Guard g; profit::Config c; c.protect = true;
    g.phase = profit::Phase::Waiting; g.active = g.armed = true; g.peak = 300;
    g.exit_ms = now; g.last_bar = now + hour; g.recovered = 1;
    g.update(c, now + 2 * hour, 123, true, false, now + 2 * hour, .59, true, .69);
    check(g.last_bar == now + hour && g.recovered == 1, "a missing vote that could cross 60% keeps that close pending");
    g.update(c, now + 2 * hour + 100, 123, true, false, now + 2 * hour, .61, true, .62);
    check(g.phase == profit::Phase::Watching && g.fresh_after == now + 2 * hour, "late current-hour votes confirm recovery exactly once");
    g = {}; g.phase = profit::Phase::Caution; g.active = g.armed = true; g.peak = 300;
    g.last_bar = now; g.weak = c.confirm_bars;
    g.update(c, now + hour, 228, true, true, now + hour, .42, true, .49);
    check(g.phase == profit::Phase::Caution && g.last_bar == now, "unresolved new hour cannot reuse prior weak bars for a soft exit");
    g.update(c, now + 2 * hour, 228, true, true, now + 2 * hour, .42, true, .43);
    check(g.phase == profit::Phase::Caution && g.weak == 1, "an entire unconfirmed hour breaks the weakness streak");
}

void new_breadth_sample_tests() {
    Fixture f("new-breadth-sample");
    auto tmpl = smart::Config::default_template();
    restore_waiting_sample(f, tmpl, 0);
    for (int i = 0; i < 10; ++i) {
        auto cfg = tmpl; cfg.symbol = sample_symbol(i);
        f.engine->add_bot(cfg, true);
    }
    auto stopped = tmpl; stopped.symbol = "STOPPEDUSDT";
    f.engine->stop_bot(f.engine->add_bot(stopped, true));
    f.manager->evaluate(*f.engine, now);
    check(f.manager->status().find("0/10") != std::string::npos && read(f.path).find("STOPPEDUSDT") == std::string::npos,
          "a new fixed recovery sample includes running hourly bots and excludes stopped bots");
}
