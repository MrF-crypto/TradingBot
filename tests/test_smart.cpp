// 智能模式纯逻辑测试：门槛、ATR、统一候选池增删、配置往返。不连网络。
#include "core/smart_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ccbot;
using namespace ccbot::smart;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static constexpr int64_t kDay = 24LL * 3600 * 1000;
static constexpr int64_t kNow = 1'800'000'000'000LL;

static SymbolMeta meta(const std::string& sym) {
    SymbolMeta m;
    m.symbol = sym;
    m.base = sym.substr(0, sym.size() - 4);
    m.quote = "USDT";
    m.contract_type = "PERPETUAL";
    m.status = "TRADING";
    m.onboard_ms = kNow - 100 * kDay;
    m.delivery_ms = 4133404800000LL;
    m.tick = 0.0001;
    return m;
}

static void test_gates() {
    Config c;
    c.atr_pct_min = 0.5; c.atr_pct_max = 2; c.max_abs_chg24 = 50;
    Ticker24 t{20e6, 5, 1.0};
    const double f = 0.01;
    check(pre_gate(meta("AAAUSDT"), &t, &f, c, kNow, false).empty(), "门槛：正常币通过");

    auto m = meta("AAAUSDT"); m.quote = "USDC";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "USDT"), "门槛：非 USDT 计价");
    m = meta("AAAUSDT"); m.contract_type = "CURRENT_QUARTER";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "永续"), "门槛：非永续");
    m = meta("AAAUSDT"); m.delivery_ms = kNow + 10 * kDay;
    check(has(pre_gate(m, &t, &f, c, kNow, false), "下架") && delisting(m, kNow),
          "门槛：交割日期被改到近期 = 即将下架");
    m = meta("AAAUSDT"); m.status = "SETTLING";
    check(delisting(m, kNow), "下架：结算中");
    m = meta("AAAUSDT"); m.status = "PENDING_TRADING";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "状态"), "门槛：非交易中");
    check(has(pre_gate(meta("BTCUSDT"), &t, &f, c, kNow, false), "内置"), "门槛：BTC 内置排除");
    check(has(pre_gate(meta("USDCUSDT"), &t, &f, c, kNow, false), "内置"), "门槛：稳定币内置排除");
    Config cb = c; cb.blacklist = {"AAAUSDT"};
    check(has(pre_gate(meta("AAAUSDT"), &t, &f, cb, kNow, false), "拉黑"), "门槛：拉黑");
    m = meta("AAAUSDT"); m.onboard_ms = kNow - 5 * kDay;
    check(has(pre_gate(m, &t, &f, c, kNow, false), "上线不满 14 天"), "门槛：上线不满 14 天");
    m = meta("AAAUSDT"); m.tick = 0.01;   // 0.01 / 1.0 = 1%
    check(has(pre_gate(m, &t, &f, c, kNow, false), "精度"), "门槛：价格精度太粗");

    Ticker24 low{4.5e6, 5, 1.0};
    check(has(pre_gate(meta("AAAUSDT"), &low, &f, c, kNow, false), "成交额"), "门槛：成交额 4.5M < 5M");
    check(pre_gate(meta("AAAUSDT"), &low, &f, c, kNow, true).empty(),
          "  已在池里：按 80% 判（4.5M ≥ 4M）不淘汰——滞回");
    Ticker24 pump{20e6, 65, 1.0};
    check(has(pre_gate(meta("AAAUSDT"), &pump, &f, c, kNow, false), "涨跌"), "门槛：24h 涨 65% 不新加");
    check(pre_gate(meta("AAAUSDT"), &pump, &f, c, kNow, true).empty(), "  已在池里的不因此淘汰");
    const double hot = 0.15;
    check(has(pre_gate(meta("AAAUSDT"), &t, &hot, c, kNow, false), "资金费率"), "门槛：资金费率 0.15% 过高");
    check(pre_gate(meta("AAAUSDT"), nullptr, nullptr, c, kNow, false).empty(),
          "门槛：行情/费率没拿到时不据此判不合格（由调用方归为未知）");

    Features fe; fe.ok = true; fe.atr_pct = 0.4;
    check(has(atr_gate(fe, c, false), "太低"), "ATR% 0.4 < 0.5 太低");
    fe.atr_pct = 0.45;
    check(atr_gate(fe, c, true).empty(), "  已在池里放宽 20%（0.4）");
    fe.atr_pct = 2.2;
    check(has(atr_gate(fe, c, false), "太高"), "ATR% 2.2 > 2 太高（回测 > 2% 期望≈0）");
    check(atr_gate(fe, c, true).empty(), "  已在池里放宽 20%（2.4）");
}

// 一串收盘价生成K线：高低各 ±0.5，量固定
static std::vector<Bar> bars_from(const std::vector<double>& closes, double vol = 100) {
    std::vector<Bar> v;
    double prev = closes.front();
    for (size_t i = 0; i < closes.size(); ++i) {
        Bar b;
        b.open_ms = (int64_t)i * 3600000;
        b.open = prev; b.close = closes[i];
        b.high = std::max(b.open, b.close) + 0.5;
        b.low  = std::min(b.open, b.close) - 0.5;
        b.volume = vol;
        v.push_back(b);
        prev = closes[i];
    }
    return v;
}

static void test_features() {
    check(!compute_features(bars_from(std::vector<double>(14, 100))).ok, "ATR14 至少需要 15 根已收盘K线");
    const auto f = compute_features(bars_from(std::vector<double>(200, 100)));
    check(f.ok && near(f.close, 100) && near(f.atr_pct, 1), "固定高低范围得到已知 ATR%，无需 BTC 数据");
    auto invalid = bars_from(std::vector<double>(200, 100)); invalid.back().close = 0;
    check(!compute_features(invalid).ok, "价格无效时不能成为候选");
    // ── 持仓量变化 ──
    {
        const int64_t h = 3600000LL, t = 100 * h;
        std::vector<OiPoint> pts;
        for (int i = 24; i >= 0; --i) pts.push_back({t - i * h, 1000.0 + (24 - i) * 10, 5e6});
        auto s = oi_stat(pts);   // 最新 1240，1h 前 1230，4h 前 1200，24h 前 1000
        check(s.ok && s.ms == t && near(s.value, 5e6) && near(s.chg1, (1240.0 / 1230 - 1) * 100) &&
                  near(s.chg4, (1240.0 / 1200 - 1) * 100) && near(s.chg24, 24.0),
              "持仓量：1/4/24h 变化按持仓量算");
        pts.erase(pts.begin() + 20);   // 拿掉 4h 前那一点
        s = oi_stat(pts);
        check(s.ok && std::isnan(s.chg4) && !std::isnan(s.chg1) && !std::isnan(s.chg24),
              "  4h 前那点缺了：4h 变化是 NaN，不拿相邻的凑");
        check(!oi_stat({}).ok, "  一个点都没有：不 ok");
    }

}

static void test_pool() {
    Config c;
    Pool pool;
    ScanOutcome scan; scan.ok = true;
    // 超过旧榜单容量，所有合格币仍应进入同一观察池。
    for (int i = 0; i < 250; ++i) scan.candidates["COIN" + std::to_string(i) + "USDT"] = {true, "", 2, 100};
    auto p = pool.plan(scan, {}, c, kNow);
    check(p.add.size() == 250, "250 个合格币全部进入候选，容量不由旧多空榜单限制");
    check(std::all_of(p.add.begin(), p.add.end(), [](const auto& a) { return a.dir == trend::Direction::Both; }),
          "全部候选允许多空，由突破信号决定实际方向");
    c.dir_long = false;
    p = pool.plan(scan, {}, c, kNow);
    check(p.add.size() == 250 && p.add[0].dir == trend::Direction::ShortOnly, "方向权限不改变候选品种范围");
    c.dir_short = false;
    check(pool.plan(scan, {}, c, kNow).add.empty(), "方向均禁用时不创建新候选");
    c.dir_long = c.dir_short = true;

    std::vector<BotView> bots{{"manual", "COIN0USDT", false}, {"held", "HELDUSDT", true, true},
                              {"pending", "PENDINGUSDT", true, true}, {"stopped", "STOPUSDT", true, false, true},
                              {"unknown", "UNKNOWNUSDT", true}, {"flat", "FLATUSDT", true}};
    scan.hard_fail = {{"HELDUSDT", "流动性不足"}, {"PENDINGUSDT", "不合格"}, {"STOPUSDT", "不合格"}, {"FLATUSDT", "不合格"}};
    scan.unknown.insert("UNKNOWNUSDT"); scan.delisting.insert("HELDUSDT");
    p = pool.plan(scan, bots, c, kNow);
    check(p.add.size() == 249, "已有手动 bot 的品种不重复添加");
    check(p.remove.size() == 1 && p.remove[0].bot_id == "flat", "只移除明确不合格且空仓运行中的自动 bot");
    check(p.alerts.size() == 1, "即将下架的持仓保留并告警");
    check(std::none_of(p.meta.begin(), p.meta.end(), [](const auto& m) { return m.bot_id == "unknown"; }),
          "数据未知不改变候选状态");
    check(std::all_of(p.meta.begin(), p.meta.end(), [](const auto& m) { return !m.eligible; }),
          "被保护的不合格 bot 标记为禁止新增风险");
    auto bad_scan = scan; bad_scan.ok = false;
    const auto no_op = pool.plan(bad_scan, bots, c, kNow);
    check(no_op.add.empty() && no_op.remove.empty() && no_op.meta.empty(), "整轮失败不增删也不改变状态");
    scan.hard_fail.clear(); scan.candidates["HELDUSDT"] = {true, "", 2, 100};
    p = pool.plan(scan, bots, c, kNow + kDay);
    check(std::any_of(p.meta.begin(), p.meta.end(), [](const auto& m) { return m.bot_id == "held" && m.eligible; }),
          "重新合格的持仓可以恢复候选资格");

    ScanOutcome mass; mass.ok = true;
    std::vector<BotView> many;
    for (int i = 0; i < 6; ++i) {
        const auto sym = "B" + std::to_string(i);
        mass.hard_fail[sym] = "不合格"; many.push_back({sym, sym, true});
    }
    p = pool.plan(mass, many, c, kNow);
    check(p.remove.empty() && !p.alerts.empty() && p.meta.size() == 6,
          "批量不合格保留供复核，同时禁止这些候选开新仓");

    Pool old;
    check(old.from_json(R"({"v":1,"entries":[{"s":"OLDUSDT","a":123,"m":99,"g":85,"o":3}]})"), "旧候选池状态可迁移");
    check(old.entries().at("OLDUSDT").added_ms == 123 && !has(old.to_json(), "\"g\"") && !has(old.to_json(), "\"m\""),
          "迁移保留加入时间，丢弃旧分组与掉榜计数");
    old.on_added("NEWUSDT", kNow); old.on_removed("OLDUSDT");
    Pool restored;
    check(restored.from_json(old.to_json()) && restored.entries().size() == 1 && restored.entries().count("NEWUSDT"),
          "统一候选池状态可以往返保存");
}

static void test_config_json() {
    Config c;
    c.enabled = true;
    c.rotate_when_full = true;
    c.dir_short = false;
    c.min_quote_vol = 8e6;
    c.blacklist = {"XUSDT", "YUSDT"};
    c.daily_loss_limit = 123.5;
    c.tmpl.rule.atr_mult = 2.5;
    c.tmpl.size_mode = TrendConfig::SizeMode::RiskBased;
    c.tmpl.risk_usdt = 7;
    c.tmpl.rule.following.short_vol_ratio = 0.8;
    Config d;
    std::string err;
    check(config_from_json(config_to_json(c), d, &err), "配置 JSON 往返：能读回");
    check(d.rotate_when_full, "满仓换仓开关往返保留");
    check(!Config{}.rotate_when_full, "旧配置默认不启用满仓换仓");
    const auto before_rotation = config_to_json(d);
    for (const auto bad : {R"({"rotate_when_full":1})", R"({"rotate_when_full":"true"})", R"({"rotate_when_full":null})"})
        check(!config_from_json(bad, d, &err) && config_to_json(d) == before_rotation,
              "换仓开关类型错误拒绝，原配置不变");
    check(d.enabled && !d.dir_short && near(d.min_quote_vol, 8e6) && d.blacklist.size() == 2 &&
          near(d.daily_loss_limit, 123.5), "  各项一致");
    check(near(d.tmpl.rule.atr_mult, 2.5) && d.tmpl.size_mode == TrendConfig::SizeMode::RiskBased &&
          near(d.tmpl.risk_usdt, 7), "  模板一致");
    check(near(d.tmpl.rule.following.short_vol_ratio, 0.8), "空头量比配置保留");
    Config legacy;
    std::string previous = config_to_json(legacy);
    check(config_from_json(R"({"dir_mode":"mixed","long_n":1,"short_n":1,"strong_n":0,"mid_n":0,"min_score":100,"prio_reserve":999,"prio_min_score":100,"drop_rank_factor":9})", legacy, &err) &&
          config_to_json(legacy) == previous, "旧评分和分榜字段不再改变配置或隐藏限制候选");
    check(!has(previous, "min_score") && !has(previous, "dir_mode") && !has(previous, "long_n"), "新配置不写出已移除字段");
    Config f;
    check(!config_from_json("{\"atr_pct_min\":5,\"atr_pct_max\":2}", f, &err) && has(err, "ATR%"),
          "非法值整份拒绝");
    check(f.atr_pct_min == 0.1, "  拒绝时原配置不动");
    check(!config_from_json("{\"template\":{\"interval\":\"5m\"}}", f, &err), "模板周期只认 1h/2h/4h");

    const auto t = make_bot_config(c, "ABCUSDT");
    check(t.symbol == "ABCUSDT" && t.rule.strategy == trend::Strategy::TrendFollowing &&
          t.rule.reverse == trend::ReverseMode::None && t.use_disaster_stop &&
          t.rule.direction == trend::Direction::LongOnly,
          "bot 配置：趋势跟踪、不反手、硬止损强制开、方向跟勾选");
    Config upgraded, decoded;
    upgraded.tmpl.rule.following.min_slope_atr = 0.035;
    upgraded.max_total_risk_usdt = 12; upgraded.max_direction_risk_usdt = 7;
    check(config_from_json(config_to_json(upgraded), decoded, &err) &&
          decoded.tmpl.rule.following == upgraded.tmpl.rule.following && decoded.max_total_risk_usdt == 12 &&
          decoded.max_direction_risk_usdt == 7, "趋势参数与组合风险预算无损往返");
    const auto unchanged = config_to_json(decoded);
    for (const std::string invalid : {"{\"template\":{\"tf_fast_period\":70,\"tf_slow_period\":60}}",
                                      "{\"template\":{\"tf_close_confirm\":\"yes\"}}",
                                      "{\"template\":{\"tf_min_close_location\":1.2}}",
                                      "{\"max_total_risk_usdt\":-1}"})
        check(!config_from_json(invalid, decoded, &err) && config_to_json(decoded) == unchanged,
              "非法新参数拒绝且原配置不变");
    const auto dt = Config::default_template();
    check(dt.interval == "1h" && dt.rule.donchian_period == 12 && dt.rule.lock_atr && !dt.rule.accel_exit && dt.rule.following.enabled && dt.rule.following.close_confirm &&
          near(dt.rule.squeeze_box_atr, 0) && near(dt.rule.trend_er_min, 0.4) &&
          near(dt.rule.breakout_vol_ratio, 1.5), "默认模板：1h、通道 12、收盘趋势确认、关闭加速收紧与箱体硬过滤");
    check(dt.rule.vol_paced && dt.rule.fail_exit && !dt.rule.fail_exit_short,
          "  量比按时间折算开；假突破提前出场默认只判多单");
    Config def;
    check(near(def.atr_pct_min, 0.1) && near(def.atr_pct_max, 8.0), "  1h ATR% 研究范围默认 0.1~8");
    check(!dt.rule.roi_trail, "  收益率追踪止盈默认关");
    Config roi;
    roi.tmpl.rule.roi_trail = true; roi.tmpl.rule.roi_trail_start = 12; roi.tmpl.rule.roi_trail_back = 4;
    Config roi2;
    std::string e3;
    check(config_from_json(config_to_json(roi), roi2, &e3) && roi2.tmpl.rule.roi_trail &&
              near(roi2.tmpl.rule.roi_trail_start, 12) && near(roi2.tmpl.rule.roi_trail_back, 4),
          "  追踪止盈随趋势跟踪模板往返");
    check(!config_from_json("{\"template\":{\"roi_trail_back\":0}}", roi2, &e3), "  回撤 0 个点拒绝");

    Config rt;
    rt.tmpl.rule.vol_paced = false;
    rt.tmpl.rule.fail_exit = false;
    rt.tmpl.rule.fail_exit_short = true;
    Config rt2;
    std::string e2;
    check(config_from_json(config_to_json(rt), rt2, &e2) && !rt2.tmpl.rule.vol_paced &&
              !rt2.tmpl.rule.fail_exit && rt2.tmpl.rule.fail_exit_short,
          "  这三项随模板往返");
    Config old;
    check(config_from_json("{\"template\":{\"lock_atr\":true}}", old, &e2) && old.tmpl.rule.vol_paced &&
              old.tmpl.rule.fail_exit && !old.tmpl.rule.fail_exit_short,
          "  老配置没有这些键：按默认（空单不判）");

    check(dt.rule.once_per_bar && make_bot_config(c, "ABCUSDT").rule.once_per_bar,
          "每根K线最多开一次：模板默认开，自动 bot 跟着开");
    Config ob;
    ob.tmpl.rule.once_per_bar = false;
    Config ob2;
    check(config_from_json(config_to_json(ob), ob2, &e2) && !ob2.tmpl.rule.once_per_bar,
          "  关掉后随模板往返");
    check(old.tmpl.rule.once_per_bar, "  老配置没有这项：按开");

    check(near(dt.rule.max_chase_atr, 1.5) && dt.rule.max_init_loss_pct == 0 &&
              near(make_bot_config(c, "ABCUSDT").rule.max_chase_atr, 1.5),
          "追高默认 1.5 ATR、初始止损上限默认关，自动 bot 跟着模板");
    Config ch;
    ch.tmpl.rule.max_chase_atr = 0;
    ch.tmpl.rule.max_init_loss_pct = 6;
    Config ch2;
    check(config_from_json(config_to_json(ch), ch2, &e2) && ch2.tmpl.rule.max_chase_atr == 0 &&
              near(ch2.tmpl.rule.max_init_loss_pct, 6),
          "  两项随模板往返（含关掉追高）");
    check(near(old.tmpl.rule.max_chase_atr, 1.5) && old.tmpl.rule.max_init_loss_pct == 0,
          "  老配置没有这两项：追高 1.5、上限关");
    Config bad1;
    check(!config_from_json("{\"template\":{\"max_chase_atr\":-1}}", bad1, &e2), "  追高为负拒绝");
    check(!config_from_json("{\"template\":{\"max_init_loss_pct\":100}}", bad1, &e2), "  上限 100% 拒绝");

    check(!dt.rule.breakeven && near(dt.rule.breakeven_start, 6) && !old.tmpl.rule.breakeven,
          "保本止损默认关、触发线 6；老配置没有这项按关");
    Config be;
    be.tmpl.rule.breakeven = true;
    be.tmpl.rule.breakeven_start = 7;
    be.tmpl.rule.roi_trail = true;
    be.tmpl.rule.roi_trail_start = 10;
    Config be2;
    check(config_from_json(config_to_json(be), be2, &e2) && be2.tmpl.rule.breakeven &&
              near(be2.tmpl.rule.breakeven_start, 7),
          "  随模板往返");
    be.tmpl.rule.breakeven_start = 10;
    check(!config_from_json(config_to_json(be), be2, &e2) &&
              e2.find("低于追踪止盈启动线") != std::string::npos,
          "  等于追踪止盈启动线：参数错误，拒绝保存");
    be.tmpl.rule.roi_trail = false;
    check(config_from_json(config_to_json(be), be2, &e2), "  追踪止盈关着：单独开保本可以");

    check(!dt.rule.expand_long && near(dt.rule.expand_long_min, 2.5) && !old.tmpl.rule.expand_long,
          "多单波动放大默认关、门槛 2.5；老配置没有这项按关");
    Config ex;
    ex.tmpl.rule.expand_long = true;
    ex.tmpl.rule.expand_long_min = 3;
    Config ex2;
    check(config_from_json(config_to_json(ex), ex2, &e2) && ex2.tmpl.rule.expand_long &&
              near(ex2.tmpl.rule.expand_long_min, 3) &&
              make_bot_config(ex2, "ABCUSDT").rule.expand_long,
          "  随模板往返，自动趋势跟踪 bot 生效");
    check(!config_from_json("{\"template\":{\"expand_long_min\":0}}", bad1, &e2), "  门槛 0 拒绝");
}

static void test_single_strategy_config() {
    Config c;
    std::string err;
    check(config_from_json("{\"strategy\":\"turtle\",\"template\":{\"atr_mult\":2.7}}", c, &err) &&
          near(c.tmpl.rule.atr_mult, 2.7), "旧 turtle 配置保留参数并迁移为趋势跟踪");
    const auto json = config_to_json(c);
    check(has(json, "\"strategy\":\"trend_following\"") &&
          !has(json, "\"bare\":") && !has(json, "\"vol\":"),
          "配置仅输出趋势跟踪，不再保存已删除策略参数");
    Config restored;
    check(config_from_json(json, restored, &err) && near(restored.tmpl.rule.atr_mult, 2.7),
          "新策略名称配置往返");
    for (const auto* invalid : {"{\"strategy\":\"bare\"}", "{\"strategy\":\"vol\"}",
                                "{\"strategy\":\"unknown\"}", "{\"strategy\":7}"}) {
        check(!config_from_json(invalid, c, &err) && has(err, "trend_following") &&
              near(c.tmpl.rule.atr_mult, 2.7), "不支持的策略明确拒绝，原配置保持不变");
    }
    check(!config_from_json("{\"enabled\":true,\"dir_long\":false,\"dir_short\":false}", c, &err),
          "启用趋势跟踪必须允许至少一个方向");
    check(excluded_asset_base("PAXG") && excluded_asset_base("XAUT") &&
          !excluded_asset_base("SOL"), "保留趋势选币共用的黄金代币排除");
}

int main() {
    test_single_strategy_config();
    test_gates();
    test_features();
    test_pool();
    test_config_json();
    if (g_fail == 0) {
        std::printf("OK: 智能模式测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
