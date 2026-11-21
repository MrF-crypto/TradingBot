#pragma once
#include "core/rebound_guard.h"

static void test_rebound_guard() {
    namespace rb = ccbot::rebound;
    constexpr auto step = rb::kFiveMinutes;
    constexpr auto hour = rb::kHour;
    constexpr int64_t t = 200 * hour;
    std::vector<smart::Bar> hourly;
    for (int i = 0; i < 190; ++i)
        hourly.push_back({t - (190 - i) * hour, 10, 11, 9, 10});
    // An old wide bar outside the 187-bar window and a currently forming bar
    // must have no influence on the completed ATR.
    hourly.front().high = 1e6;
    hourly.push_back({t, 10, 9999, .1, 53});
    check(near(rb::closed_hour_atr(hourly, t), 2), "急反弹 ATR14 使用已收盘小时线");
    auto gap = hourly;
    gap.erase(gap.end() - 4);
    check(std::isnan(rb::closed_hour_atr(gap, t)), "急反弹 ATR 拒绝断档");

    const int64_t close = t + 3 * step;
    std::vector<smart::Bar> recent;
    for (int i = 0; i < 4; ++i)
        recent.push_back({close - (4 - i) * step, 0, 0, 0, 100.0 + i});
    recent.push_back({close, 0, 0, 0, 8000});
    check(near(rb::normalized_move(recent, close, 2, t), 1.5),
          "15分钟比较4根收盘端点，当前尚未收盘的急涨不可见");
    check(std::isnan(rb::normalized_move(recent, close, 2, t - hour)) &&
          std::isnan(rb::normalized_move(recent, close, 2, t + hour)),
          "只接受对应最近收盘小时的 ATR");
    recent.erase(recent.begin() + 1);
    check(std::isnan(rb::normalized_move(recent, close, 2, t)), "5分钟线缺根不能缩短观察周期");

    const auto vote = [](int up, int total, double move) {
        std::vector<double> data(total, -.01);
        std::fill_n(data.begin(), up, move);
        return rb::summarize(data.size(), data);
    };
    rb::State state;
    check(rb::advance(state, t, vote(177, 202, .2345)) == rb::Transition::None &&
          rb::advance(state, t + step, vote(193, 202, .4808)) == rb::Transition::None,
          "11月21日14:35和14:40上涨已普遍，未到反弹强度");
    check(rb::advance(state, t + 2 * step, vote(195, 202, .8786)) == rb::Transition::Paused &&
          state.paused, "14:45首次同时满足强度和广度，当根暂停开空");
    check(rb::advance(state, t + 3 * step, vote(195, 202, 1.1096)) == rb::Transition::None &&
          state.paused, "持续反弹不重复触发");
    const auto low = vote(47, 202, .75);
    rb::advance(state, t + 4 * step, low);
    rb::advance(state, t + 4 * step, low);
    rb::advance(state, t + 3 * step, low);
    check(state.paused && state.low_streak == 1, "同根重复或旧快照不补恢复次数");
    rb::advance(state, t + 5 * step, vote(101, 202, .75));
    check(state.low_streak == 0 && state.paused, "上涨占比恰好50%不计恢复");
    rb::advance(state, t + 6 * step, low);
    const auto sparse = rb::summarize(202, std::vector<double>(181, -2.));
    rb::advance(state, t + 7 * step, sparse);
    check(state.last_close_ms == t + 6 * step && state.paused, "覆盖不足90%保留暂停，允许补齐同一根");
    rb::advance(state, t + 8 * step, low);
    check(state.paused && state.low_streak == 1, "两根不相邻的低广度不能解除暂停");
    check(rb::advance(state, t + 9 * step, low) == rb::Transition::Released &&
          !state.paused && state.released_ms == t + 9 * step, "相邻两根低广度解除，保存收盘边界");

    rb::State edge;
    auto inclusive = vote(7, 10, .5);
    check(rb::advance(edge, t, inclusive) == rb::Transition::Paused, "70%和0.50 ATR阈值含等号");
    auto partial = rb::summarize(10, {-1, -1, 0, 0, .1, .2, .3, .4, .5, rb::kMissing});
    check(partial.covered() && partial.up == 5 && partial.valid == 9 &&
          near(partial.up_fraction, 5.0 / 9) && near(partial.median_atr, .1),
          "覆盖恰好90%有效，平盘不计上涨，缺数据不计入中位数");
    const auto even = rb::summarize(4, {1.2, -.2, 2.0, .2});
    check(near(even.median_atr, .7), "双数样本取中间两项的均值");
}
