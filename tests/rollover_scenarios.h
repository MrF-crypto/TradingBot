#include <sstream>
// Real fetch_trend_signal path with deterministic time and HTTP; no live account.
static void test_rollover_scenarios() {
    constexpr int64_t hour = 3600000, boundary = 1800000000000LL;
    auto fetch = [&](int64_t start, int64_t end, int64_t newest, bool gap = false, bool compute_rollover = false) {
        TradingClient tc(test_cfg());
        int clock_reads = 0;
        bool replied = false;
        tc.set_wall_clock_for_test([&] {
            ++clock_reads;
            if (compute_rollover && clock_reads >= 4) return boundary + hour;
            return replied ? end : start;
        });
        tc.set_test_hook([&](const std::string& method, const std::string& path,
                            const std::string&, TradingClient::FakeReply& out) {
            check(method == "GET" && path.find("/fapi/v1/klines?") == 0, "rollover only requests public candles");
            const int count = std::stoi(path.substr(path.find("limit=") + 6));
            std::ostringstream body; body.precision(17); body << '[';
            bool first = true;
            for (int i = count - 1; i >= 0; --i) {
                if (gap && i == 3) continue;
                if (!first) body << ',';
                first = false;
                const double price = 1000 - i * .1;
                body << '[' << newest - static_cast<int64_t>(i) * hour << ",\"" << price
                     << "\",\"" << price + 2 << "\",\"" << price - 2 << "\",\"" << price + 1 << "\",\"100\"]";
            }
            body << ']'; out.body = body.str(); replied = true; return true;
        });
        following::Config cfg; cfg.enabled = true;
        return tc.fetch_trend_signal("FAKEUSDT", "1h", 20, 14, cfg, boundary - hour);
    };
    const auto early = fetch(boundary + 500, boundary + 700, boundary);
    check(early.ok && early.bar_open_ms == boundary && early.history.size() == 1,
          "current candle in first second is not rejected by signing timestamp offset");
    const auto crossed = fetch(boundary - 100, boundary + 2000, boundary);
    check(crossed.ok && crossed.history_target_ms == boundary && crossed.history.size() == 1,
          "HTTP crossing the hour accepts the current response and advances exactly one closed candle");
    const auto stale = fetch(boundary + 1000, boundary + 2000, boundary - hour);
    check(!stale.ok && stale.waiting_for_bar && stale.history.empty() && stale.history_target_ms == 0,
          "exchange lag is retryable waiting, never a tradable snapshot or advanced checkpoint");
    const auto future = fetch(boundary + 5000, boundary + 6000, boundary + hour);
    check(!future.ok && !future.waiting_for_bar && future.history.empty(),
          "future candles remain rejected as a real data error");
    const auto missing = fetch(boundary + 5000, boundary + 6000, boundary, true);
    check(!missing.ok && !missing.waiting_for_bar && missing.history.empty(),
          "internal candle gaps cannot be hidden as an ordinary rollover wait");
    const auto late = fetch(boundary + hour - 20, boundary + hour - 10, boundary, false, true);
    check(!late.ok && late.waiting_for_bar && late.history.empty(),
          "rollover during computation discards the stale signal and keeps the saved cursor");
    const auto recovered = fetch(boundary + 7000, boundary + 7100, boundary);
    check(recovered.ok && recovered.history.size() == 1 &&
          std::abs(recovered.following.fast - early.following.fast) < 1e-12,
          "retry recovers the same completed-candle model without rebuilding or replaying trades");
}
