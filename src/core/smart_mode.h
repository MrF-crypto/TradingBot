#pragma once
#include "core/trend_engine.h"

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 智能模式：硬过滤 → 统一候选池 → 自身趋势与突破信号决定方向。
// 所有合格品种都监控；候选次序仅供稳定显示，不作为交易优先级。
// 数据未知保留现有 bot；持仓、在途订单和手动停止的 bot 不自动移除。
namespace ccbot::smart {

struct Bar {
    int64_t open_ms = 0;
    double  open = 0, high = 0, low = 0, close = 0, volume = 0;
    double  quote_vol = 0;   // 成交额（U）；0 = 没有，用 volume × close 兜底
    double  taker_quote = 0; // 主动买入成交额（U）；0 = 没有
};

struct Config {
    bool enabled   = false;
    bool dir_long  = true;
    bool dir_short = true;
    // ── 硬门槛 ──
    double min_quote_vol   = 5e6;    // 24h 成交额（U）
    int    min_age_days    = 14;     // 上线天数
    // Candidate volatility bounds; research defaults, not empirically proven optima.
    double atr_pct_min     = 0.1;
    double atr_pct_max     = 8.0;
    double max_abs_chg24   = 0;     // 24h 涨跌幅上限（%，0=关）；实际入场仍有 ATR 追价限制
    double max_abs_funding = 0.1;    // 资金费率绝对值上限（% / 8h）：太拥挤或被操纵
    double max_tick_pct    = 0.1;    // 最小价格变动 ÷ 价格（%）：精度太粗止损和 ATR 都失真
    std::set<std::string> blacklist; // 用户拉黑

    // ── 风控（落到引擎的账户级闸门上）──
    // Planned loss budgets in USDT; both directions share total, each direction shares a cap.
    // Conservative common-risk group for all coins; does not assume independent altcoins.
    double max_total_risk_usdt = 6, max_direction_risk_usdt = 4;
    double risk_cost_bps = 20; // round-trip fee + slippage allowance, not a loss guarantee
    int    event_poll_sec = 60, event_cooldown_sec = 300;
    double event_move_atr = 2, event_min_move_pct = 1;
    int    max_positions    = 20;    // 同时持仓上限（全部 bot）
    bool   rotate_when_full = false; // 满仓时，以收盘突破替换持续失去趋势许可的自动持仓
    double daily_loss_limit = 0;     // 当日亏损限额（U，0=关）
    double float_loss_limit = 0;     // 持仓浮亏限额（U，0=关）
    bool   pause_new        = false; // 应急：暂停全部 bot 开新仓（不删 bot、不动已有仓位）

    // ── bot 模板（趋势跟踪）。symbol 留空，方向/反手/硬止损由 make_bot_config 强制 ──
    TrendConfig tmpl = default_template();

    static TrendConfig default_template();
};

// 两个方向都允许时，同一候选按自身趋势与突破信号决定多空。
trend::Direction bot_direction(const Config& cfg);
TrendConfig make_bot_config(const Config& cfg, const std::string& symbol);
TrendConfig make_bot_config(const Config& cfg, const std::string& symbol, trend::Direction dir);
// 排除黄金代币等不适合趋势选币的标的。
bool excluded_asset_base(const std::string& base);
// ── 市场数据（net 层拉好后喂进来）────────────────────────────────────────────
struct SymbolMeta {
    std::string symbol, base, quote, contract_type, status;
    std::string underlying_type;   // COIN / INDEX / 大宗商品、股票……；空 = 交易所没给
    int64_t onboard_ms  = 0;
    int64_t delivery_ms = 0;     // 永续是 2100 年；公告下架后会被改到近期
    double  tick = 0;            // 最小价格变动
};
struct Ticker24 {
    double quote_vol = 0;        // 24h 成交额（U）
    double chg_pct   = 0;        // 24h 涨跌幅（%）
    double last      = 0;
};

// 内置排除：BTC、ETH、稳定币
bool builtin_excluded(const std::string& base);

// 不需要K线的硬门槛。返回空串 = 通过，否则是原因。
// relax = 已经在池里的币：成交额按门槛的 80% 判（滞回），免得在门槛边上来回进出。
// t / funding 为 nullptr = 这轮没拿到，不据此判不合格（由调用方归为"未知"）
std::string pre_gate(const SymbolMeta& m, const Ticker24* t, const double* funding_pct,
                     const Config& cfg, int64_t now_ms, bool relax);
// 下架判定单独拿出来：持仓中的 bot 碰到它要告警
bool delisting(const SymbolMeta& m, int64_t now_ms);

// ── 特征 ──────────────────────────────────────────────────────────────────
struct Features {
    bool ok = false;
    std::string why;
    double atr_pct = 0;  // 已收盘 1h ATR14 / 收盘价，供硬过滤及异动唤醒
    double close = 0;
};
inline constexpr int kMinBars = 15;
Features compute_features(const std::vector<Bar>& bars);
// ── 持仓量（OI）：只采集记录，不参与决策 ─────────────────────────────────
struct OiPoint {
    int64_t ms = 0;
    double  oi = 0;      // 持仓量（张/币）
    double  value = 0;   // 持仓价值（U）
};
// 变化率按持仓量（不按价值：价值会跟着价格动）。NaN = 那个时刻的点没拿到
struct OiStat {
    bool    ok = false;  // 至少有最新一个点
    int64_t ms = 0;      // 最新一个点的时间
    double  value = 0;   // 最新持仓价值（U）
    double  chg1  = std::numeric_limits<double>::quiet_NaN();   // 1h 变化 %
    double  chg4  = std::numeric_limits<double>::quiet_NaN();   // 4h
    double  chg24 = std::numeric_limits<double>::quiet_NaN();   // 24h
};
// pts：1h 周期、时间正序。按时间戳找 1 / 4 / 24 小时前的点，缺了就是 NaN（不拿相邻的凑）
OiStat oi_stat(const std::vector<OiPoint>& pts);

// ATR% 区间（要K线，所以和 pre_gate 分开）。relax 时区间各放宽 20%
std::string atr_gate(const Features& f, const Config& cfg, bool relax);

// ── 候选池 ────────────────────────────────────────────────────────────────
struct BotView {
    std::string bot_id, symbol;
    bool is_auto = false;
    bool has_pos = false;
    bool stopped = false;
};

struct ScanOutcome {
    bool ok = false;                           // 整轮可信。false 时什么都不做
    std::map<std::string, Features> candidates; // 全部通过硬过滤的品种，不设榜单名额
    std::map<std::string, std::string> hard_fail;   // 明确不合格 → 原因
    std::set<std::string> unknown;             // 这轮没拿到数据（绝不当成不合格）
    std::set<std::string> delisting;           // 即将下架
};

struct Plan {
    struct Add { std::string symbol; trend::Direction dir = trend::Direction::Both; };
    struct Remove { std::string bot_id, symbol, why; };
    struct Meta { std::string bot_id; bool eligible = false; };
    std::vector<Add> add;
    std::vector<Remove> remove;
    std::vector<Meta> meta;
    std::vector<std::string> alerts;
};

class Pool {
public:
    struct Entry { int64_t added_ms = 0; };
    Plan plan(const ScanOutcome& scan, const std::vector<BotView>& bots,
              const Config& cfg, int64_t now_ms);
    void on_added(const std::string& symbol, int64_t now_ms);
    void on_removed(const std::string& symbol);
    const std::map<std::string, Entry>& entries() const { return entries_; }
    std::string to_json() const;
    bool from_json(std::string_view s);
private:
    std::map<std::string, Entry> entries_;
};

// ── 配置的 JSON 往返（GUI 存独立文件，headless 读主配置里的 "smart_mode" 段）──
std::string config_to_json(const Config& cfg);
// 缺的键保持 cfg 里原有的值（调用方先放默认值）。err 非空 = 有非法值，cfg 不动
bool config_from_json(std::string_view s, Config& cfg, std::string* err);

} // namespace ccbot::smart
