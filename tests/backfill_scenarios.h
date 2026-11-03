#include <sstream>
static void test_backfill_scenarios() {
    constexpr int64_t hour = 3600000, base = 1800000000000LL;
    TradingClient tc(test_cfg());
    int64_t now_bar = base + 600 * hour;
    tc.set_wall_clock_for_test([&] { return now_bar + 60000; });
    bool missing = false, future_spike = false; int requests = 0; int64_t requested_end = 0;
    tc.set_test_hook([&](const std::string& method, const std::string& path, const std::string& params, TradingClient::FakeReply& out) {
        ++requests;
        check(method == "GET" && path.find("klines") != std::string::npos, "recovery only fetches public candles");
        const auto query = path + "&" + params;
        if (requests == 1) check(query.find("symbol=%E9%BE%99%E8%99%BEUSDT") != std::string::npos,
                                "Chinese symbol backfill sends the encoded symbol");
        auto parameter = [&](const std::string& key, int64_t fallback) {
            auto pos = query.find(key + "=");
            return pos == std::string::npos ? fallback : static_cast<int64_t>(std::stoll(query.substr(pos + key.size() + 1)));
        };
        const auto limit = parameter("limit", 0);
        requested_end = parameter("endTime", now_bar + hour - 1);
        const auto end_bar = requested_end / hour * hour;
        std::ostringstream json; json.precision(17); json << '['; bool first = true;
        for (int64_t i = limit - 1; i >= 0; --i) {
            if (missing && i == 3) continue;
            const auto at = end_bar - i * hour;
            double price = 1000 + static_cast<double>((at - base) / hour) * .1;
            if (future_spike && at == now_bar) price *= 10;
            if (!first) json << ',';
            first = false;
            json << '[' << at << ",\"" << price << "\",\"" << price + 2 << "\",\"" << price - 2 << "\",\"" << price + 1 << "\",\"100\"]";
        }
        json << ']'; out.body = json.str(); return true;
    });
    following::Config cfg; cfg.enabled = true;
    auto first = tc.fetch_trend_signal("龙虾USDT", "1h", 20, 14, cfg, base);
    check(first.history_only && !first.ok && first.history.size() == 256 &&
          first.history.front().bar_ms == base + hour && first.history.back().bar_ms == base + 256 * hour &&
          requested_end == base + 257 * hour - 1,
          "long outage uses bounded chronological pages, without exposing a historical tradable snapshot");
    if (first.history.empty()) return;
    const auto second = tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, first.history.back().bar_ms);
    check(second.history_only && second.history.size() == 256, "second recovery page continues from the exact saved cursor");
    if (second.history.empty()) return;
    const auto final = tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, second.history.back().bar_ms);
    check(final.ok && !final.history_only && final.history.size() == 88 && final.bar_open_ms == now_bar,
          "only the final current-candle page releases live signal evidence");
    const auto evidence = first.history.back();
    const auto later = now_bar; now_bar = evidence.bar_ms;
    const auto online = tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, now_bar);
    check(online.ok && std::abs(evidence.signal.fast - online.following.fast) < 1e-12 &&
          std::abs(evidence.signal.atr - online.following.atr) < 1e-12 && evidence.signal.regime == online.following.regime,
          "historical and online calculation use the identical fixed indicator horizon");
    now_bar = later; future_spike = true;
    const auto spike = tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, now_bar - hour);
    check(spike.ok && std::abs(spike.following.fast - final.following.fast) < 1e-12 &&
          std::abs(spike.following.atr - final.following.atr) < 1e-12,
          "an unclosed candle spike cannot rewrite confirmed history");
    missing = true;
    const auto gap = tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, now_bar - 5 * hour);
    check(!gap.ok && gap.history.empty() && gap.error.find("不连续") != std::string::npos,
          "gapped response reports a concrete reason and never advances the checkpoint");
    check(requests == 6, "recovery uses only the expected public requests");
}
