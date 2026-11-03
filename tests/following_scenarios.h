// Included by test_trend.cpp: causal strategy scenarios, no network or account.
#include "core/market_wake.h"

static void test_following_scenarios() {
    namespace tf = ccbot::following;
    tf::Config f; f.enabled = true;
    auto path = [](double slope) {
        std::vector<Ohlc> bars;
        for (int i = 0; i < 240; ++i) {
            const double o = 150 + slope * i, c = o + slope;
            bars.push_back({std::max(o, c) + .05, std::min(o, c) - .05, c, o});
        }
        return bars;
    };
    std::vector<double> volume(240, 100);
    auto up = path(.2), down = path(-.2), flat = path(0);
    const auto a = tf::compute(up, volume, f, 12, 14);
    const auto d = tf::compute(down, volume, f, 12, 14);
    const auto r = tf::compute(flat, volume, f, 12, 14);
    auto intrabar_cfg = f; intrabar_cfg.close_confirm = false;
    const auto intrabar = tf::compute(up, volume, intrabar_cfg, 12, 14);
    const auto confirmed = tf::closed_signal(intrabar);
    CHECK(confirmed.ready && confirmed.long_confirmed == a.long_confirmed &&
          near(confirmed.atr, a.atr) && near(confirmed.upper, a.upper) &&
          near(confirmed.volume_ratio, a.volume_ratio) && confirmed.settings.close_confirm,
          "intrabar mode supplies independent pre-breakout closed-candle evidence for rotation");
    auto stale = intrabar; stale.ready = false;
    CHECK(!tf::closed_signal(stale).ready, "secondary evidence cannot bypass a rejected network snapshot");
    CHECK(a.ready && a.regime == tf::Regime::Up && a.long_confirmed && !a.short_confirmed,
          "persistent rise permits a confirmed long");
    CHECK(d.ready && d.regime == tf::Regime::Down && d.short_confirmed && !d.long_confirmed,
          "persistent slow decline permits a confirmed short without volume expansion");
    CHECK(r.ready && r.regime == tf::Regime::Range && !r.long_confirmed && !r.short_confirmed,
          "flat market remains flat");
    auto changed = up;
    changed.back() = {10000, .01, 9999, 1};
    const auto b = tf::compute(changed, volume, f, 12, 14);
    CHECK(near(a.atr, b.atr) && near(a.upper, b.upper) && near(a.fast, b.fast) &&
          a.long_confirmed == b.long_confirmed, "unfinished pump cannot alter closed direction or confirmation");
    changed[238].close += 10; changed[238].high = changed[238].close + .05;
    const auto spike = tf::compute(changed, volume, f, 12, 14);
    CHECK(near(a.atr, spike.atr) && near(a.upper, spike.upper),
          "signal candle cannot inflate its own ATR or breakout channel");
    auto wick = up;
    wick[238].high += 10;
    CHECK(!tf::compute(wick, volume, f, 12, 14).long_confirmed,
          "long upper wick fails close-location confirmation");
    auto bad = up; bad[100].close = std::numeric_limits<double>::quiet_NaN();
    CHECK(!tf::compute(bad, volume, f, 12, 14).ready, "invalid historical data fails closed");
    bad.resize(70); volume.resize(70);
    CHECK(!tf::compute(bad, volume, f, 12, 14).ready, "short history is unknown, not a trend");

    Config cfg; cfg.following = f; cfg.max_chase_atr = 1.5; cfg.breakout_vol_ratio = 1.5;
    auto input = [&](const tf::Signal& s, bool is_long) {
        auto in = mk(is_long ? s.upper + .2 * s.atr : s.lower - .2 * s.atr, s.atr, s.upper, s.lower);
        in.following = s; in.vol_ratio = 2; in.bar_open_ms = 10;
        return in;
    };
    State state;
    auto li = input(a, true), si = input(d, false);
    CHECK(step(state, cfg, li).action == Action::OpenLong, "uptrend + accepted close opens long");
    si.vol_ratio = .1;
    CHECK(step(state, cfg, si).action == Action::OpenShort, "shorts use separate volume requirement");
    li.price = li.dc_up + 3 * li.atr;
    CHECK(step(state, cfg, li).action == Action::Hold, "explosive extension is not chased");
    li = input(a, true); li.price = li.dc_up - .1;
    CHECK(step(state, cfg, li).action == Action::Hold, "breakout that already failed before fill is rejected");
    li = input(a, true); li.following.long_confirmed = false;
    CHECK(step(state, cfg, li).action == Action::Hold, "intrabar spike does not bypass close mode");
    cfg.following.close_confirm = false;
    CHECK(step(state, cfg, li).action == Action::OpenLong, "explicit intrabar mode still requires uptrend");
    li.following.regime = tf::Regime::Down;
    CHECK(step(state, cfg, li).action == Action::Hold, "intrabar mode never buys against downtrend permission");
    cfg.following = f;
    State held; on_filled(held, Pos::Long, 100, 90, cfg, false, 1, 2);
    auto exit = mk(105, 2); exit.following = d;
    CHECK(step(held, cfg, exit).action == Action::Close, "confirmed opposite trend exits without immediate reversal");
    exit.following = {}; exit.atr = 0; exit.price = 89;
    CHECK(step(held, cfg, exit).action == Action::Close, "missing trend data never disables existing stop");

    ccbot::smart::MarketWake wake;
    std::map<std::string, double> prices{{"A", 100}, {"B", 100}}, atr{{"A", .5}, {"B", .5}};
    const int64_t t = 1'000'000;
    CHECK(!wake.observe(prices, atr, t, 60, 300, 2, 1), "first event poll establishes baseline");
    prices["A"] = 102; prices["B"] = 98;
    CHECK(wake.observe(prices, atr, t + 60000, 60, 300, 2, 1), "simultaneous pump and crash coalesce to one wake");
    prices["A"] = 110;
    CHECK(!wake.observe(prices, atr, t + 120000, 60, 300, 2, 1), "event cooldown prevents request storm");
    prices["A"] = 130;
    CHECK(!wake.observe(prices, atr, t + 900000, 60, 300, 2, 1), "data gap resets baseline without invented event");
    prices["A"] = 134;
    CHECK(wake.observe(prices, atr, t + 960000, 60, 300, 2, 1), "fresh observation can wake after recovery");
}
