// Run the same public scan/sampling entry points used by the GUI and the Linux service.
// The mock honors the exchange kline limit/endTime and never reads the live data directory.
static void test_rebound_runner(const std::string& dir) {
    constexpr int64_t minute = 60'000, hour = 60 * minute;
    constexpr int64_t start = kNow / hour * hour;
    int64_t now = start + 31 * minute;
    std::atomic<int> five_calls{0};
    int submitted = 0;
    Rig r(dir + "/rebound.json");
    r.mk.syms["AAAUSDT"] = {};
    r.mk.syms["BBBUSDT"] = {};
    r.tc->set_wall_clock_for_test([&] { return now; });

    const auto close_at = [=](int64_t end) {
        switch ((end-start)/minute) {
        case 35: return 100.45;
        case 40: return 100.96;
        case 45: return 101.80;
        case 50: return 102.50;
        case 55: return 99.80;
        case 60: return 99.50;
        default: return 100.;
        }
    };
    r.mk.candle_reply = [&](const std::string& sym, const std::string& path) {
        const bool five = has(path, "interval=5m");
        if (five) {
            ++five_calls;
            if (sym == "BBBUSDT" && (now - start)/minute == 50) return std::string("[]");
        }
        const int64_t period = five ? 5 * minute : hour;
        const auto pos = path.find("&endTime=");
        const auto end = pos == std::string::npos ? now : std::stoll(path.substr(pos + 9));
        const int limit = std::stoi(path.substr(path.find("&limit=") + 7));
        const int64_t last = end/period*period;
        std::string rows = "[";
        for (int i = 0; i < limit; ++i) {
            const auto t = last - (limit - 1 - i)*period;
            double price = five ? close_at(t+period) : 100.;
            if (five && sym.starts_with("NEW")) price = 100. + (t+period-start)/minute*0.1;
            rows += std::string(i ? ",[" : "[") + std::to_string(t) + ",\"" +
                    std::to_string(price) + "\",\"" + std::to_string(price + 1.) + "\",\"" +
                    std::to_string(price - 1.) + "\",\"" + std::to_string(price) + "\",\"1000\"]";
        }
        return rows + "]";
    };
    EngineHost host;
    host.now_wall = [&] { return std::chrono::system_clock::time_point(std::chrono::milliseconds(now)); };
    host.submit = [&](std::function<void()>) { ++submitted; }; // Observe admission without executing an order.
    const auto restart = [&] {
        r.run->cancel();
        r.eng = std::make_shared<TrendEngine>(r.tc, host);
        r.run = std::make_unique<SmartRunner>(r.tc, r.eng, r.path, [&](const std::string& msg, bool) {
            r.logs.push_back(msg);
        });
        r.run->load();
    };
    restart();
    auto cfg = r.cfg();
    cfg.tmpl.rule.following.close_confirm = false; // Intrabar shorts must pass the independent recovery gate.
    cfg.tmpl.rule.once_per_bar = false;
    r.run->set_config(cfg);
    r.run->run_once(now);
    r.run->refresh_rebound();
    check(!r.run->rebound_due(now) && five_calls == 0, "关闭保护时扫描和定时维护都不拉取5m行情");
    cfg.short_rebound_guard = true;
    r.run->set_config(cfg);
    r.run->force();
    const auto scan = r.run->run_once(now);
    check(scan.ok && scan.candidates.size() == 2 && !r.run->rebound_due(now),
          "成功扫描冻结候选，首次评估从下一根5分钟收盘开始");
    const auto sample = [&](int m) {
        now = start + m * minute + 15'000;
        check(r.run->rebound_due(now), "扫描后的第 " + std::to_string(m) + " 分钟有新广度样本");
        r.run->refresh_rebound();
        return r.run->rebound_status();
    };
    const auto short_decision = [&](bool confirmed) {
        const auto b = r.bot("AAAUSDT");
        following::Signal s;
        s.settings = b.signal_following_config();
        s.ready = true;
        s.regime = following::Regime::Down;
        s.closed_entry.settings = s.settings;
        s.closed_entry.ready = true;
        s.closed_entry.short_confirmed = confirmed;
        s.closed_entry.regime = following::Regime::Down;
        r.eng->update_signal(b.bot_id, 2, 2, true, 110, 90, now/hour*hour, 0, 2,
                             true, -.8, 89, 0, 0, s);
        r.eng->tick("AAAUSDT", 100);
        r.eng->tick("AAAUSDT", 89);
        return r.bot("AAAUSDT").last_decision;
    };
    check(has(sample(35), "监测中") && has(sample(40), "监测中"), "广度达到但强度不足，继续监测");
    check(has(sample(45), "暂停新增智能空仓") && has(short_decision(true), "急反弹保护") && submitted == 0,
          "普遍反弹且超过0.50 ATR时，在实际入场路径拦住智能空单");
    const auto logs = r.logs.size();
    const auto requests = five_calls.load();
    r.run->refresh_rebound();
    check(!r.run->rebound_due(now) && r.logs.size() == logs && five_calls == requests,
          "同一根已评估K线不重复请求和记录预警");
    const auto sparse = sample(50);
    check(has(sparse, "覆盖不足") && has(sparse, "暂停新增智能空仓"), "缺少一半行情时保留暂停状态");
    const auto first_low = sample(55);
    check(has(first_low, "暂停新增智能空仓") && has(first_low, "1/2"), "第一次低广度收盘只计一次");

    restart();
    r.run->set_config(cfg);
    const auto restored = r.run->rebound_status();
    check(has(restored, "暂停新增智能空仓") && has(restored, "1/2"), "重启后保留暂停和第一根恢复计数");
    r.mk.syms["NEWAAAUSDT"] = {};
    r.mk.syms["NEWBBBUSDT"] = {};
    r.run->force();
    check(r.run->run_once(now).candidates.size() == 4, "重启后新扫描加入两个上涨品种");
    const auto released = sample(60);
    check(has(released, "有效样本 2/2"), "恢复仍用触发暂停时的两个候选，新加入的上涨品种留待下一轮");
    check(has(released, "等待新的1h收盘向下突破") && has(short_decision(true), "急反弹保护已解除") && submitted == 0,
          "跨重启的第二根低广度收盘解除暂停，解除时已存在的整点突破仍被拦截");
    now = start + 2 * hour + 15'000;
    check(has(short_decision(false), "急反弹保护已解除") && submitted == 0,
          "新的1h尚无收盘下破，盘中开空继续等待");
    short_decision(true);
    check(submitted == 1 && r.bot("AAAUSDT").pending, "新的下跌趋势与1h收盘下破确认后入场通过");
    cfg.short_rebound_guard = false;
    r.run->set_config(cfg);
    const auto previous_calls = five_calls.load();
    r.run->refresh_rebound();
    check(has(r.run->rebound_status(), "已关闭") && !r.run->rebound_due(now) && five_calls == previous_calls,
          "运行中取消勾选后停止采样");
}
