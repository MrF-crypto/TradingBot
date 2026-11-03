#include "core/smart_mode.h"
#include "core/indicators.h"

#include <simdjson.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>

namespace ccbot::smart {

namespace {

constexpr int64_t kDayMs  = 24LL * 3600 * 1000;
// 交割日期在这么久以内就算"即将下架"。永续的交割日期是 2100 年，币安公告下架后
// 会把它改到下架那天——提前一个月起就不再新加，持仓中的醒目告警
constexpr int64_t kDelistHorizonMs = 30 * kDayMs;
// 批量淘汰保护的下限：池子很小时（比如只剩 3 个）一轮淘汰 2 个是正常的
constexpr int kMassRemoveMin = 4;

std::string fmt(double v, int prec) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

} // namespace

// ── 配置 ────────────────────────────────────────────────────────────────────

TrendConfig Config::default_template() {
    TrendConfig t;
    t.interval    = "1h";
    t.rule.following.enabled = true;
    t.size_mode = TrendConfig::SizeMode::RiskBased;
    t.risk_usdt = 1;
    t.signal_max_age_sec = 120;
    t.budget_usdt = 100;
    t.leverage    = 3;
    t.rule.strategy        = trend::Strategy::TrendFollowing;
    t.rule.donchian_period = 12;
    t.rule.atr_period      = 14;
    t.rule.atr_mult        = 3.0;
    t.rule.squeeze_box_atr    = 0; // EMA state filters ranges; allow wide trending channels.
    t.rule.trend_er_min       = 0.4;
    t.rule.breakout_vol_ratio = 1.5;
    t.rule.lock_atr   = true;
    t.rule.accel_exit = false;
    t.rule.vol_paced  = true;   // 量比按已走时间折算：不然多数突破被拦到后半小时才开，追得太远
    t.rule.fail_exit  = true;   // 多单收盘跌回通道内就走，不等止损亏满 1R
    t.rule.fail_exit_short = false;   // 空单反弹回通道多半又接着跌，默认不判（见 trend_decision.h）
    t.rule.once_per_bar    = true;    // 同一根里出场后不原地再开（实盘追踪止盈 4 秒后再开、亏回去）
    t.rule.max_chase_atr   = 1.5;     // 离通道边沿 > 1.5 ATR 不追（实盘高位买回的两笔在 1.9~2.4）
    t.rule.max_init_loss_pct = 0;     // 初始止损上限默认关，用户按赢单的最大浮亏分布再定
    t.use_disaster_stop        = true;
    t.disaster_stop_buffer_pct = 0.2;
    return t;
}

trend::Direction bot_direction(const Config& cfg) {
    if (cfg.dir_long && !cfg.dir_short) return trend::Direction::LongOnly;
    if (cfg.dir_short && !cfg.dir_long) return trend::Direction::ShortOnly;
    return trend::Direction::Both;
}

TrendConfig make_bot_config(const Config& cfg, const std::string& symbol) {
    return make_bot_config(cfg, symbol, bot_direction(cfg));
}

TrendConfig make_bot_config(const Config& cfg, const std::string& symbol, trend::Direction dir) {
    TrendConfig c = cfg.tmpl;
    c.symbol = symbol;
    // 智能模式只建趋势跟踪。下面三项不随模板：
    //   · 方向跟勾选走（两个都勾 = 多空，向上突破做多、向下突破做空）
    //   · 不反手：自动 bot 在没看过的币上交易，亏损止损后立刻反向开一笔是在放大
    //     不确定性。多空模式下反方向真突破了，照样会按正常信号开
    //   · 硬止损强制开：程序崩溃时这些仓位不能没有交易所侧的底
    c.rule.strategy  = trend::Strategy::TrendFollowing;
    c.rule.direction = dir;
    c.rule.reverse   = trend::ReverseMode::None;
    c.use_disaster_stop = true;
    return c;
}

bool excluded_asset_base(const std::string& base) {
    static const std::set<std::string> kGold = {"PAXG", "XAUT"};
    return kGold.count(base) > 0;
}


// ── 硬门槛 ──────────────────────────────────────────────────────────────────

bool builtin_excluded(const std::string& base) {
    // BTC / ETH 不是"山寨币"；稳定币没有趋势；BTCDOM 是指数合约
    static const std::set<std::string> kSkip = {
        "BTC", "ETH", "BTCDOM",
        "USDC", "FDUSD", "TUSD", "USDP", "DAI", "BUSD", "USDE", "PYUSD", "RLUSD", "USD1",
    };
    return kSkip.count(base) > 0;
}

bool delisting(const SymbolMeta& m, int64_t now_ms) {
    if (m.status == "SETTLING" || m.status == "CLOSE" || m.status == "DELIVERING") return true;
    return m.delivery_ms > 0 && m.delivery_ms < now_ms + kDelistHorizonMs;
}

std::string pre_gate(const SymbolMeta& m, const Ticker24* t, const double* funding_pct,
                     const Config& cfg, int64_t now_ms, bool relax) {
    if (m.quote != "USDT")                return "非 USDT 计价";
    if (m.contract_type != "PERPETUAL")   return "非永续";
    if (delisting(m, now_ms))             return "即将下架";
    if (m.status != "TRADING")            return "状态 " + m.status;
    if (builtin_excluded(m.base))         return "内置排除";
    if (cfg.blacklist.count(m.symbol))    return "已拉黑";
    if (m.onboard_ms > 0 && now_ms - m.onboard_ms < cfg.min_age_days * kDayMs)
        return "上线不满 " + std::to_string(cfg.min_age_days) + " 天";
    if (t) {
        if (m.tick > 0 && t->last > 0 && m.tick / t->last * 100.0 > cfg.max_tick_pct)
            return "价格精度太粗";
        const double need = relax ? cfg.min_quote_vol * 0.8 : cfg.min_quote_vol;
        if (!(t->quote_vol >= need))
            return "24h 成交额 " + fmt(t->quote_vol / 1e6, 1) + "M 不足";
        // 涨跌幅和资金费率上限只限制新加入，已在池中的趋势由入场及出场规则管理。
        if (!relax && cfg.max_abs_chg24 > 0 && std::fabs(t->chg_pct) > cfg.max_abs_chg24)
            return "24h 涨跌 " + fmt(t->chg_pct, 1) + "% 过大";
    }
    if (funding_pct && !relax && std::fabs(*funding_pct) > cfg.max_abs_funding)
        return "资金费率 " + fmt(*funding_pct, 3) + "% 过高";
    return {};
}

OiStat oi_stat(const std::vector<OiPoint>& pts) {
    OiStat s;
    if (pts.empty() || !(pts.back().oi > 0)) return s;
    const auto& last = pts.back();
    s.ok    = true;
    s.ms    = last.ms;
    s.value = last.value;
    auto chg = [&](int hours) {
        const int64_t want = last.ms - hours * 3600LL * 1000;
        for (const auto& p : pts)
            if (p.ms == want && p.oi > 0) return (last.oi / p.oi - 1.0) * 100.0;
        return std::numeric_limits<double>::quiet_NaN();
    };
    s.chg1  = chg(1);
    s.chg4  = chg(4);
    s.chg24 = chg(24);
    return s;
}

std::string atr_gate(const Features& f, const Config& cfg, bool relax) {
    const double lo = relax ? cfg.atr_pct_min * 0.8 : cfg.atr_pct_min;
    const double hi = relax ? cfg.atr_pct_max * 1.2 : cfg.atr_pct_max;
    if (f.atr_pct < lo) return "ATR% " + fmt(f.atr_pct, 2) + " 太低";
    if (f.atr_pct > hi) return "ATR% " + fmt(f.atr_pct, 2) + " 太高";
    return {};
}

// ── 特征 ────────────────────────────────────────────────────────────────────

Features compute_features(const std::vector<Bar>& bars) {
    Features f;
    if (bars.size() < kMinBars) { f.why = "K线不足"; return f; }
    std::vector<indicators::Ohlc> oh;
    oh.reserve(bars.size());
    for (const auto& b : bars) oh.push_back({b.high, b.low, b.close, b.open});
    const double atr = indicators::atr(oh, 14);
    f.close = bars.back().close;
    if (!(std::isfinite(atr) && atr > 0 && std::isfinite(f.close) && f.close > 0)) {
        f.why = "ATR或价格无效"; return f;
    }
    f.atr_pct = atr / f.close * 100;
    f.ok = std::isfinite(f.atr_pct);
    return f;
}

Plan Pool::plan(const ScanOutcome& scan, const std::vector<BotView>& bots,
                const Config& cfg, int64_t now_ms) {
    Plan p;
    if (!scan.ok) return p;
    std::set<std::string> present, auto_syms;
    std::vector<Plan::Remove> remove;
    int judged = 0;
    for (const auto& b : bots) {
        present.insert(b.symbol);
        if (!b.is_auto) continue;
        auto_syms.insert(b.symbol);
        auto& entry = entries_[b.symbol];
        if (!entry.added_ms) entry.added_ms = now_ms;
        if (scan.unknown.count(b.symbol)) continue;
        const auto fail = scan.hard_fail.find(b.symbol);
        const bool eligible = scan.candidates.count(b.symbol) && fail == scan.hard_fail.end();
        if (!eligible && fail == scan.hard_fail.end()) continue; // 未明确判定，不猜测不合格
        p.meta.push_back({b.bot_id, eligible});
        if (b.has_pos) {
            if (scan.delisting.count(b.symbol))
                p.alerts.push_back(b.symbol + " 即将下架，持仓中——请尽快手动处理");
            continue;
        }
        if (b.stopped) continue;
        ++judged;
        if (fail != scan.hard_fail.end()) remove.push_back({b.bot_id, b.symbol, fail->second});
    }
    if (remove.size() >= kMassRemoveMin && static_cast<int>(remove.size()) * 2 > judged)
        p.alerts.push_back("智能模式：本轮大量候选不满足硬过滤，保留 bot 待复核；不合格品种暂停新开仓");
    else
        p.remove = std::move(remove);

    if (cfg.dir_long || cfg.dir_short) {
        for (const auto& [symbol, f] : scan.candidates) {
            if (!f.ok || present.count(symbol) || scan.unknown.count(symbol) || scan.hard_fail.count(symbol)) continue;
            p.add.push_back({symbol, bot_direction(cfg)});
        }
    }
    for (auto it = entries_.begin(); it != entries_.end();)
        it = auto_syms.count(it->first) ? std::next(it) : entries_.erase(it);
    return p;
}

void Pool::on_added(const std::string& symbol, int64_t now_ms) { entries_[symbol] = {now_ms}; }
void Pool::on_removed(const std::string& symbol) { entries_.erase(symbol); }

std::string Pool::to_json() const {
    std::ostringstream o;
    o << "{\"v\":2,\"entries\":[";
    bool first = true;
    for (const auto& [symbol, entry] : entries_) {
        o << (first ? "" : ",") << "{\"s\":\"" << symbol << "\",\"a\":" << entry.added_ms << "}";
        first = false;
    }
    o << "]}";
    return o.str();
}

bool Pool::from_json(std::string_view s) {
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    auto padded = simdjson::padded_string(s);
    if (parser.parse(padded).get(root) != simdjson::SUCCESS) return false;
    simdjson::dom::array arr;
    if (root["entries"].get_array().get(arr) != simdjson::SUCCESS) return false;
    std::map<std::string, Entry> got;
    for (auto el : arr) {
        std::string_view symbol;
        int64_t added = 0;
        if (el["s"].get(symbol) != simdjson::SUCCESS || symbol.empty()) continue;
        if (el["a"].get(added) != simdjson::SUCCESS) added = 0;
        got[std::string(symbol)] = {added}; // 旧状态中的排名、分组和掉榜计数不再使用
    }
    entries_ = std::move(got);
    return true;
}

// ── 配置 JSON ───────────────────────────────────────────────────────────────

std::string config_to_json(const Config& c) {
    std::ostringstream o;
    o << std::setprecision(std::numeric_limits<double>::max_digits10);
    auto b = [](bool v) { return v ? "true" : "false"; };
    o << "{"
      << "\"enabled\":" << b(c.enabled) << ","
      << "\"strategy\":\"trend_following\","
      << "\"dir_long\":" << b(c.dir_long) << ","
      << "\"dir_short\":" << b(c.dir_short) << ","
      << "\"min_quote_vol\":" << c.min_quote_vol << ","
      << "\"min_age_days\":" << c.min_age_days << ","
      << "\"atr_pct_min\":" << c.atr_pct_min << ","
      << "\"atr_pct_max\":" << c.atr_pct_max << ","
      << "\"max_abs_chg24\":" << c.max_abs_chg24 << ","
      << "\"max_abs_funding\":" << c.max_abs_funding << ","
      << "\"max_tick_pct\":" << c.max_tick_pct << ","
      << "\"blacklist\":[";
    bool first = true;
    for (const auto& s : c.blacklist) { o << (first ? "" : ",") << "\"" << s << "\""; first = false; }
    o << "],"
      << "\"max_positions\":" << c.max_positions << ","
      << "\"rotate_when_full\":" << b(c.rotate_when_full) << ","
      << "\"daily_loss_limit\":" << c.daily_loss_limit << ","
      << "\"float_loss_limit\":" << c.float_loss_limit << ","
      << "\"pause_new\":" << b(c.pause_new) << ",";
    o << "\"max_total_risk_usdt\":" << c.max_total_risk_usdt << ",";
    o << "\"max_direction_risk_usdt\":" << c.max_direction_risk_usdt << ",";
    o << "\"risk_cost_bps\":" << c.risk_cost_bps << ",";
    o << "\"event_poll_sec\":" << c.event_poll_sec << ",";
    o << "\"event_cooldown_sec\":" << c.event_cooldown_sec << ",";
    o << "\"event_move_atr\":" << c.event_move_atr << ",";
    o << "\"event_min_move_pct\":" << c.event_min_move_pct << ",";
    const auto& t = c.tmpl;
    o << "\"template\":{";
    o << "\"tf_enabled\":" << b(t.rule.following.enabled) << ",";
    o << "\"tf_close_confirm\":" << b(t.rule.following.close_confirm) << ",";
    o << "\"tf_reversal_exit\":" << b(t.rule.following.reversal_exit) << ",";
    o << "\"tf_fast_period\":" << t.rule.following.fast_period << ",";
    o << "\"tf_slow_period\":" << t.rule.following.slow_period << ",";
    o << "\"tf_slope_bars\":" << t.rule.following.slope_bars << ",";
    o << "\"tf_min_slope_atr\":" << t.rule.following.min_slope_atr << ",";
    o << "\"tf_min_separation_atr\":" << t.rule.following.min_separation_atr << ",";
    o << "\"tf_breakout_buffer_atr\":" << t.rule.following.breakout_buffer_atr << ",";
    o << "\"tf_min_close_location\":" << t.rule.following.min_close_location << ",";
    o << "\"tf_short_vol_ratio\":" << t.rule.following.short_vol_ratio << ",";
    o << ""
      << "\"interval\":\"" << t.interval << "\","
      << "\"budget_usdt\":" << t.budget_usdt << ","
      << "\"leverage\":" << t.leverage << ","
      << "\"size_mode\":\"" << (t.size_mode == TrendConfig::SizeMode::RiskBased ? "risk" : "notional") << "\","
      << "\"risk_usdt\":" << t.risk_usdt << ","
      << "\"donchian_period\":" << t.rule.donchian_period << ","
      << "\"atr_period\":" << t.rule.atr_period << ","
      << "\"atr_mult\":" << t.rule.atr_mult << ","
      << "\"squeeze_box_atr\":" << t.rule.squeeze_box_atr << ","
      << "\"trend_er_min\":" << t.rule.trend_er_min << ","
      << "\"breakout_vol_ratio\":" << t.rule.breakout_vol_ratio << ","
      << "\"lock_atr\":" << b(t.rule.lock_atr) << ","
      << "\"accel_exit\":" << b(t.rule.accel_exit) << ","
      << "\"vol_paced\":" << b(t.rule.vol_paced) << ","
      << "\"fail_exit\":" << b(t.rule.fail_exit) << ","
      << "\"fail_exit_short\":" << b(t.rule.fail_exit_short) << ","
      << "\"roi_trail\":" << b(t.rule.roi_trail) << ","
      << "\"roi_trail_start\":" << t.rule.roi_trail_start << ","
      << "\"roi_trail_back\":" << t.rule.roi_trail_back << ","
      << "\"once_per_bar\":" << b(t.rule.once_per_bar) << ","
      << "\"max_chase_atr\":" << t.rule.max_chase_atr << ","
      << "\"max_init_loss_pct\":" << t.rule.max_init_loss_pct << ","
      << "\"breakeven\":" << b(t.rule.breakeven) << ","
      << "\"breakeven_start\":" << t.rule.breakeven_start << ","
      << "\"expand_long\":" << b(t.rule.expand_long) << ","
      << "\"expand_long_min\":" << t.rule.expand_long_min << ","
      << "\"disaster_stop_buffer_pct\":" << t.disaster_stop_buffer_pct << ","
      << "\"signal_max_age_sec\":" << t.signal_max_age_sec
      << "}}";
    return o.str();
}

namespace {

bool num(simdjson::dom::object& o, const char* k, double& out) {
    simdjson::dom::element v;
    if (o[k].get(v) != simdjson::SUCCESS) return false;
    double d; int64_t i; uint64_t u;
    if (v.get(d) == simdjson::SUCCESS) { out = d; return true; }
    if (v.get(i) == simdjson::SUCCESS) { out = (double)i; return true; }
    if (v.get(u) == simdjson::SUCCESS) { out = (double)u; return true; }
    return false;
}
void get_num(simdjson::dom::object& o, const char* k, double& out) { num(o, k, out); }
void get_int(simdjson::dom::object& o, const char* k, int& out) {
    double d;
    if (num(o, k, d)) out = (int)std::llround(d);
}
void get_bool(simdjson::dom::object& o, const char* k, bool& out) {
    bool v;
    if (o[k].get(v) == simdjson::SUCCESS) out = v;
}

} // namespace

bool config_from_json(std::string_view s, Config& cfg, std::string* err) {
    simdjson::dom::parser p;
    simdjson::dom::object o;
    auto ps = simdjson::padded_string(s);
    if (p.parse(ps).get_object().get(o) != simdjson::SUCCESS) {
        if (err) *err = "智能模式配置不是合法的 JSON 对象";
        return false;
    }
    Config c = cfg;
    get_bool(o, "enabled", c.enabled);
    {
        simdjson::dom::element value;
        if (o["strategy"].get(value) == simdjson::SUCCESS) {
            std::string_view strategy;
            if (value.get(strategy) != simdjson::SUCCESS ||
                (strategy != "trend_following" && strategy != "turtle")) {
                if (err) *err = "智能模式仅支持趋势跟踪（strategy: trend_following）；旧裸K埋伏与波动顺势已移除";
                return false;
            }
        }
    }
    get_bool(o, "dir_long", c.dir_long);
    get_bool(o, "dir_short", c.dir_short);
    get_num(o, "min_quote_vol", c.min_quote_vol);
    get_int(o, "min_age_days", c.min_age_days);
    get_num(o, "atr_pct_min", c.atr_pct_min);
    get_num(o, "atr_pct_max", c.atr_pct_max);
    get_num(o, "max_abs_chg24", c.max_abs_chg24);
    get_num(o, "max_abs_funding", c.max_abs_funding);
    get_num(o, "max_tick_pct", c.max_tick_pct);
    {
        simdjson::dom::array arr;
        if (o["blacklist"].get_array().get(arr) == simdjson::SUCCESS) {
            c.blacklist.clear();
            for (auto el : arr) {
                std::string_view v;
                if (el.get(v) == simdjson::SUCCESS && !v.empty()) c.blacklist.insert(std::string(v));
            }
        }
    }
    get_int(o, "max_positions", c.max_positions);
    {
        simdjson::dom::element el;
        if (o["rotate_when_full"].get(el) == simdjson::SUCCESS &&
            el.get(c.rotate_when_full) != simdjson::SUCCESS) {
            if (err) *err = "智能模式：rotate_when_full 必须是布尔值";
            return false;
        }
    }
    get_num(o, "daily_loss_limit", c.daily_loss_limit);
    get_num(o, "float_loss_limit", c.float_loss_limit);
    get_bool(o, "pause_new", c.pause_new);
    {
        simdjson::dom::element el;
        if (o["max_total_risk_usdt"].get(el) == simdjson::SUCCESS) {
            double value{};
            if (el.get(value) != simdjson::SUCCESS) {
                if (err) *err = "智能模式：max_total_risk_usdt 类型或范围不合法";
                return false;
            }
            c.max_total_risk_usdt = value;
        }
    }
    {
        simdjson::dom::element el;
        if (o["max_direction_risk_usdt"].get(el) == simdjson::SUCCESS) {
            double value{};
            if (el.get(value) != simdjson::SUCCESS) {
                if (err) *err = "智能模式：max_direction_risk_usdt 类型或范围不合法";
                return false;
            }
            c.max_direction_risk_usdt = value;
        }
    }
    {
        simdjson::dom::element el;
        if (o["risk_cost_bps"].get(el) == simdjson::SUCCESS) {
            double value{};
            if (el.get(value) != simdjson::SUCCESS) {
                if (err) *err = "智能模式：risk_cost_bps 类型或范围不合法";
                return false;
            }
            c.risk_cost_bps = value;
        }
    }
    {
        simdjson::dom::element el;
        if (o["event_move_atr"].get(el) == simdjson::SUCCESS) {
            double value{};
            if (el.get(value) != simdjson::SUCCESS) {
                if (err) *err = "智能模式：event_move_atr 类型或范围不合法";
                return false;
            }
            c.event_move_atr = value;
        }
    }
    {
        simdjson::dom::element el;
        if (o["event_min_move_pct"].get(el) == simdjson::SUCCESS) {
            double value{};
            if (el.get(value) != simdjson::SUCCESS) {
                if (err) *err = "智能模式：event_min_move_pct 类型或范围不合法";
                return false;
            }
            c.event_min_move_pct = value;
        }
    }
    {
        simdjson::dom::element el;
        if (o["event_poll_sec"].get(el) == simdjson::SUCCESS) {
            int64_t value{};
            if (el.get(value) != simdjson::SUCCESS || value < 0 || value > 3600) {
                if (err) *err = "智能模式：event_poll_sec 类型或范围不合法";
                return false;
            }
            c.event_poll_sec = static_cast<int>(value);
        }
    }
    {
        simdjson::dom::element el;
        if (o["event_cooldown_sec"].get(el) == simdjson::SUCCESS) {
            int64_t value{};
            if (el.get(value) != simdjson::SUCCESS || value < 0 || value > 3600) {
                if (err) *err = "智能模式：event_cooldown_sec 类型或范围不合法";
                return false;
            }
            c.event_cooldown_sec = static_cast<int>(value);
        }
    }


    simdjson::dom::object t;
    if (o["template"].get_object().get(t) == simdjson::SUCCESS) {
        auto& tm = c.tmpl;
        if (t["tf_short_vol_ratio"].error() == simdjson::NO_SUCH_FIELD)
            get_num(o, "weak_vol_ratio", tm.rule.following.short_vol_ratio); // legacy input only

        {
            simdjson::dom::element el;
            if (t["tf_enabled"].get(el) == simdjson::SUCCESS) {
                bool value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_enabled 类型或范围不合法";
                    return false;
                }
                tm.rule.following.enabled = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_close_confirm"].get(el) == simdjson::SUCCESS) {
                bool value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_close_confirm 类型或范围不合法";
                    return false;
                }
                tm.rule.following.close_confirm = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_reversal_exit"].get(el) == simdjson::SUCCESS) {
                bool value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_reversal_exit 类型或范围不合法";
                    return false;
                }
                tm.rule.following.reversal_exit = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_fast_period"].get(el) == simdjson::SUCCESS) {
                int64_t value{};
                if (el.get(value) != simdjson::SUCCESS || value < 0 || value > 1000) {
                    if (err) *err = "智能模式：tf_fast_period 类型或范围不合法";
                    return false;
                }
                tm.rule.following.fast_period = static_cast<int>(value);
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_slow_period"].get(el) == simdjson::SUCCESS) {
                int64_t value{};
                if (el.get(value) != simdjson::SUCCESS || value < 0 || value > 1000) {
                    if (err) *err = "智能模式：tf_slow_period 类型或范围不合法";
                    return false;
                }
                tm.rule.following.slow_period = static_cast<int>(value);
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_slope_bars"].get(el) == simdjson::SUCCESS) {
                int64_t value{};
                if (el.get(value) != simdjson::SUCCESS || value < 0 || value > 1000) {
                    if (err) *err = "智能模式：tf_slope_bars 类型或范围不合法";
                    return false;
                }
                tm.rule.following.slope_bars = static_cast<int>(value);
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_min_slope_atr"].get(el) == simdjson::SUCCESS) {
                double value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_min_slope_atr 类型或范围不合法";
                    return false;
                }
                tm.rule.following.min_slope_atr = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_min_separation_atr"].get(el) == simdjson::SUCCESS) {
                double value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_min_separation_atr 类型或范围不合法";
                    return false;
                }
                tm.rule.following.min_separation_atr = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_breakout_buffer_atr"].get(el) == simdjson::SUCCESS) {
                double value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_breakout_buffer_atr 类型或范围不合法";
                    return false;
                }
                tm.rule.following.breakout_buffer_atr = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_min_close_location"].get(el) == simdjson::SUCCESS) {
                double value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_min_close_location 类型或范围不合法";
                    return false;
                }
                tm.rule.following.min_close_location = value;
            }
        }
        {
            simdjson::dom::element el;
            if (t["tf_short_vol_ratio"].get(el) == simdjson::SUCCESS) {
                double value{};
                if (el.get(value) != simdjson::SUCCESS) {
                    if (err) *err = "智能模式：tf_short_vol_ratio 类型或范围不合法";
                    return false;
                }
                tm.rule.following.short_vol_ratio = value;
            }
        }

        std::string_view sv;
        if (t["interval"].get(sv) == simdjson::SUCCESS) tm.interval = std::string(sv);
        get_num(t, "budget_usdt", tm.budget_usdt);
        get_int(t, "leverage", tm.leverage);
        if (t["size_mode"].get(sv) == simdjson::SUCCESS)
            tm.size_mode = (sv == "risk") ? TrendConfig::SizeMode::RiskBased
                                          : TrendConfig::SizeMode::Notional;
        get_num(t, "risk_usdt", tm.risk_usdt);
        get_int(t, "donchian_period", tm.rule.donchian_period);
        get_int(t, "atr_period", tm.rule.atr_period);
        get_num(t, "atr_mult", tm.rule.atr_mult);
        get_num(t, "squeeze_box_atr", tm.rule.squeeze_box_atr);
        get_num(t, "trend_er_min", tm.rule.trend_er_min);
        get_num(t, "breakout_vol_ratio", tm.rule.breakout_vol_ratio);
        get_bool(t, "lock_atr", tm.rule.lock_atr);
        get_bool(t, "accel_exit", tm.rule.accel_exit);
        get_bool(t, "vol_paced", tm.rule.vol_paced);
        get_bool(t, "fail_exit", tm.rule.fail_exit);
        get_bool(t, "fail_exit_short", tm.rule.fail_exit_short);   // 旧文件没有这项 → 默认不判空单
        get_bool(t, "roi_trail", tm.rule.roi_trail);
        get_num(t, "roi_trail_start", tm.rule.roi_trail_start);
        get_num(t, "roi_trail_back", tm.rule.roi_trail_back);
        get_bool(t, "once_per_bar", tm.rule.once_per_bar);   // 旧文件没有这项 → 默认开
        get_num(t, "max_chase_atr", tm.rule.max_chase_atr);  // 旧文件没有 → 默认 1.5
        get_num(t, "max_init_loss_pct", tm.rule.max_init_loss_pct);
        get_bool(t, "breakeven", tm.rule.breakeven);          // 旧文件没有 → 关
        get_num(t, "breakeven_start", tm.rule.breakeven_start);
        get_bool(t, "expand_long", tm.rule.expand_long);      // 旧文件没有 → 关
        get_num(t, "expand_long_min", tm.rule.expand_long_min);
        get_num(t, "disaster_stop_buffer_pct", tm.disaster_stop_buffer_pct);
        get_int(t, "signal_max_age_sec", tm.signal_max_age_sec);
    }

    // ── 校验：有一项不合法就整份拒绝，cfg 不动 ──
    auto bad = [&](const std::string& m) { if (err) *err = "智能模式：" + m; return false; };
    if (c.enabled && !c.dir_long && !c.dir_short) return bad("启用趋势跟踪至少要允许一个交易方向");
    if (!(c.min_quote_vol >= 0))                         return bad("min_quote_vol 不能为负");
    if (c.min_age_days < 0)                              return bad("min_age_days 不能为负");
    if (!(c.atr_pct_min >= 0 && c.atr_pct_max > c.atr_pct_min))
        return bad("ATR% 区间不合法（要求 0 ≤ 下限 < 上限）");
    if (!(c.max_abs_chg24 >= 0))                          return bad("max_abs_chg24 不能为负（0=关）");
    if (!(c.max_abs_funding > 0))                        return bad("max_abs_funding 必须大于 0");
    if (!(c.max_tick_pct > 0))                           return bad("max_tick_pct 必须大于 0");
    if (c.max_positions < 0)                             return bad("max_positions 不能为负");
    if (!(c.daily_loss_limit >= 0) || !(c.float_loss_limit >= 0))
        return bad("亏损限额不能为负（0 = 关）");
    if (!following::valid(c.tmpl.rule.following)) return bad("趋势因子参数范围不合法");
    if (!(std::isfinite(c.max_total_risk_usdt) && c.max_total_risk_usdt >= 0 &&
          std::isfinite(c.max_direction_risk_usdt) && c.max_direction_risk_usdt >= 0 &&
          std::isfinite(c.risk_cost_bps) && c.risk_cost_bps >= 0 && c.risk_cost_bps <= 1000))
        return bad("计划风险预算或成本参数不合法");
    if (c.event_poll_sec < 30 || c.event_poll_sec > 3600 || c.event_cooldown_sec < c.event_poll_sec ||
        c.event_cooldown_sec > 3600 || !(c.event_move_atr > 0) || !(c.event_min_move_pct > 0))
        return bad("异动轮询/冷却/阈值不合法");
    const auto& tm = c.tmpl;
    if (tm.interval != "15m" && tm.interval != "30m" && tm.interval != "1h" && tm.interval != "2h" && tm.interval != "4h")
        return bad("模板周期只支持 15m / 30m / 1h / 2h / 4h（扫描按 1h K线打分）");
    if (!(tm.budget_usdt > 0))                           return bad("模板 budget_usdt 必须大于 0");
    if (tm.leverage < 1 || tm.leverage > 125)            return bad("模板杠杆要在 1~125 之间");
    if (tm.size_mode == TrendConfig::SizeMode::RiskBased && !(tm.risk_usdt > 0))
        return bad("模板按风险定仓时 risk_usdt 必须大于 0");
    if (tm.rule.donchian_period < 2 || tm.rule.atr_period < 2)
        return bad("模板通道周期与 ATR 周期至少为 2");
    if (!(tm.rule.atr_mult > 0))                         return bad("模板 atr_mult 必须大于 0");
    if (!(tm.rule.squeeze_box_atr >= 0) || !(tm.rule.breakout_vol_ratio >= 0) ||
        !(tm.rule.trend_er_min >= 0 && tm.rule.trend_er_min <= 1))
        return bad("模板过滤阈值不合法");
    if (!(tm.rule.max_chase_atr >= 0))                   return bad("追高限度不能为负（0 = 关）");
    if (!(tm.rule.max_init_loss_pct >= 0 && tm.rule.max_init_loss_pct < 100))
        return bad("初始止损上限要在 0~100% 之间（0 = 关）");
    if (const char* e = trend::breakeven_config_error(tm.rule)) return bad(e);
    if (!(tm.rule.expand_long_min > 0))                  return bad("多单波动放大门槛必须大于 0");
    if (!(tm.disaster_stop_buffer_pct > 0))              return bad("模板硬止损缓冲必须大于 0");
    if (!(tm.rule.roi_trail_start > 0) || !(tm.rule.roi_trail_back > 0))
        return bad("追踪止盈的激活收益率与回撤点数必须大于 0");
    cfg = c;
    return true;
}

} // namespace ccbot::smart
