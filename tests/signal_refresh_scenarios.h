#include "core/signal_refresh.h"
static void test_signal_refresh_scenarios() {
    using R = SignalRefresh::Result;
    const auto t = SignalRefresh::Clock::time_point(std::chrono::seconds(100));
    const auto sec = [](int n) { return std::chrono::seconds(n); };
    {
        SignalRefresh refresh;
        for (int i = 0; i < 160; ++i) {
            const auto id = std::to_string(i);
            check(refresh.claim(id, "S" + id, t), "new candidates fetch immediately");
            check(!refresh.claim(id, "S" + id, t, true), "in-flight fetches cannot be duplicated");
            check(refresh.finish(id, R::Waiting, "new candle pending", 60, 3600000, 100000, t),
                  "first rollover wait preserves bounded retry eligibility");
        }
        auto notices = refresh.take_notices();
        check(notices.size() == 1 && !notices[0].warning && notices[0].text.find("160") != std::string::npos &&
              notices[0].text.size() < 500, "160 simultaneous waits produce one bounded summary");
        check(!refresh.claim("0", "S0", t + sec(2), true), "force refresh cannot bypass retry backoff");
        check(refresh.claim("0", "S0", t + sec(3)), "first retry runs at 3s, not the next minute");
        refresh.finish("0", R::Waiting, "still pending", 60, 3600000, 103000, t + sec(3));
        check(!refresh.claim("0", "S0", t + sec(8)) && refresh.claim("0", "S0", t + sec(9)),
              "second retry waits another 6s");
        refresh.finish("0", R::Waiting, "still pending", 60, 3600000, 109000, t + sec(9));
        check(!refresh.claim("0", "S0", t + sec(20)) && refresh.claim("0", "S0", t + sec(21)),
              "third retry waits another 12s");
        check(!refresh.finish("0", R::Waiting, "still pending", 60, 3600000, 121000, t + sec(21)),
              "exhausted fast retries become a persistent error");
        notices = refresh.take_notices();
        check(notices.size() == 1 && notices[0].warning, "exhaustion reports one warning, not per retry");
        check(!refresh.claim("0", "S0", t + sec(24), true) && refresh.claim("0", "S0", t + sec(81)),
              "persistent failure returns to the normal 60s rate");
        refresh.finish("0", R::Waiting, "changed details", 60, 3600000, 181000, t + sec(81));
        check(refresh.take_notices().empty(), "changing failure details does not flood the main log");
        check(refresh.claim("0", "S0", t + sec(141)), "persistent outage remains monitored");
        refresh.finish("0", R::Ready, "", 60, 3600000, 241000, t + sec(141));
        notices = refresh.take_notices();
        check(notices.size() == 1 && notices[0].text.find("S0") != std::string::npos &&
              notices[0].text.find("S1") == std::string::npos, "partial batches report only actually recovered symbols");
    }
    {
        SignalRefresh refresh;
        refresh.claim("A", "A", t);
        refresh.finish("A", R::Ready, "", 60, 3600000, 3599500, t);
        check(!refresh.claim("A", "A", t + std::chrono::milliseconds(1499)) &&
              refresh.claim("A", "A", t + sec(3)), "success just before rollover schedules the next candle promptly");
        refresh.finish("A", R::Failed, "HTTP failure", 60, 3600000, 100000, t + sec(3));
        check(refresh.take_notices().empty(), "one real network failure is visible in details without a log storm");
        refresh.claim("A", "A", t + sec(63));
        refresh.finish("A", R::Failed, "HTTP failure", 60, 3600000, 163000, t + sec(63));
        auto notices = refresh.take_notices();
        check(notices.size() == 1 && notices[0].warning, "consecutive real failures are not silenced");
        refresh.claim("A", "A", t + sec(123));
        refresh.finish("A", R::Backfill, "", 60, 3600000, 223000, t + sec(123));
        check(refresh.take_notices().empty(), "historical progress alone is not reported as a restored live signal");
        refresh.claim("A", "A", t + sec(183));
        refresh.finish("A", R::Ready, "", 60, 3600000, 283000, t + sec(183));
        check(refresh.take_notices().size() == 1 && refresh.take_notices().empty(), "recovery is emitted exactly once");
        refresh.prune({});
        check(refresh.claim("A", "A", t), "removed or reconnected candidates start with fresh runtime scheduling");
    }
}
