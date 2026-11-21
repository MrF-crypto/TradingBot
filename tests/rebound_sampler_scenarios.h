#pragma once
#include "app/rebound_sampler.h"
#include <atomic>
#include <cmath>
#include <sstream>

namespace {
void rebound_sampler_scenarios() {
    using namespace ccbot;
    constexpr auto H = rebound::kHour, step = rebound::kFiveMinutes;
    constexpr int64_t hour = 500*H;
    int64_t close = hour + step;
    TradingClient client({});
    client.set_wall_clock_for_test([&]{ return close + 1500; });
    std::atomic<int> hour_requests{0}, five_requests{0}, wrong_window{0};
    std::atomic<bool> omit_b{true}, congested{false}, skip_first{false};
    auto hour_bars = [](int64_t until) {
        std::vector<TradingClient::Bar> result;
        for (int i = 187; i > 0; --i) {
            TradingClient::Bar b;
            b.open_ms = until-i*rebound::kHour;
            b.open = b.close = 100;
            b.low = 99; b.high = 101; b.volume = 10;
            result.push_back(b);
        }
        return result;
    };
    client.set_test_hook([&](const std::string&, const std::string& path, const std::string&,
                             TradingClient::FakeReply& out) {
        const bool five = path.find("interval=5m") != std::string::npos;
        const auto expected = five ? close-1 : close/H*H-1;
        if (path.find("endTime=" + std::to_string(expected)) == std::string::npos) ++wrong_window;
        if (five) ++five_requests; else ++hour_requests;
        if (congested) out.headers = "X-MBX-USED-WEIGHT-1M: 2100\r\n";
        if (five && omit_b && path.find("symbol=B") != std::string::npos) { out.body = "[]"; return true; }
        const auto count = std::stoi(path.substr(path.find("limit=")+6));
        const auto width = five ? step : H;
        std::ostringstream json;
        json << '[';
        const int first = skip_first && five && count >= 12 ? 1 : 0;
        for (int i = first; i < count; ++i) {
            const auto t = expected+1 - (count-i)*width;
            const double p = five ? 100 + i : 100;
            if (i != first) json << ',';
            json << '[' << t << ",\"" << p << "\",\"" << p+1 << "\",\"" << p-1
                 << "\",\"" << p << "\",\"10\"]";
        }
        out.body = json.str() + ']';
        return true;
    });
    rebound::Sampler sampler;
    sampler.seed_hourly("A", hour_bars(hour), hour);
    auto b = sampler.sample(client, {"A","B"}, close, {});
    check(b.pool == 2 && b.valid == 1 && !b.covered() && hour_requests == 0 && five_requests == 2,
          "急反弹采集按固定候选保留缺口，并复用扫描的小时 ATR");
    omit_b = false;
    b = sampler.sample(client, {"A","B"}, close, {});
    check(b.covered() && b.up == 2 && b.median_atr == 1.5 && hour_requests == 1 && five_requests == 3,
          "同一收盘只重试缺失品种，冷启动从已收小时数据计算 ATR14");
    sampler.sample(client, {"A","B"}, close, {});
    check(five_requests == 3, "同根五分钟收盘重复采集复用结果");
    close = hour + H;
    b = sampler.sample(client, {"A","B"}, close, {});
    // Prior hours had TR=2. Twelve 5m bars span 99–112 (TR=13).
    check(b.covered() && std::fabs(b.median_atr - 3./((13*2.+13)/14.)) < 1e-10 &&
          hour_requests == 1 && five_requests == 5 && wrong_window == 0,
          "整点由十二根已收5m续上最新小时 ATR，无额外1h请求");
    auto stale = sampler.sample(client, {"A"}, close-step, {});
    auto stopped = sampler.sample(client, {"A"}, close, []{return true;});
    check(!stale.covered() && !stopped.covered() && five_requests == 5,
          "过期采样和断开中的运行器不取行情");
    close += H;
    skip_first = true;
    b = sampler.sample(client, {"A"}, close, {});
    check(b.covered() && hour_requests == 2 && five_requests == 6 && b.median_atr == 1.5,
          "整点聚合缺一根时补拉该品种的已收小时K，保留有效的15分钟涨幅");
    skip_first = false;
    close += step;
    congested = true;
    sampler.sample(client, {"A"}, close, {});
    b = sampler.sample(client, {"B"}, close, {});
    check(!b.covered() && five_requests == 7, "共享权重逼近软阈值即让行情与订单优先");

    auto bars = hour_bars(hour);
    for (size_t i=0; i<bars.size(); ++i) {
        const double p = 400-static_cast<double>(i);
        bars[i].open = p+.3; bars[i].close=p; bars[i].low=p-.1; bars[i].high=p+.4;
    }
    auto current = bars.back();
    current.open_ms = hour;
    bars.push_back(current);
    trend::Config rule;
    rule.following.enabled=false; rule.following.close_confirm=false;
    check(rebound::confirmed_short(bars,hour,rule) && !rebound::confirmed_short(bars,hour+H,rule),
          "恢复入场独立检查真实小时的向下趋势与收盘突破");
    bars[bars.size()-3].open_ms -= H;
    check(!rebound::confirmed_short(bars,hour,rule), "小时行情不连续时无法确认恢复入场");
}
}
