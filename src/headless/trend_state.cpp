#include "headless/trend_state.h"
#include "core/app_logic.h"
#include "core/monitor_state.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <stdexcept>
#include "core/smart_mode.h"

#include <simdjson.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <sstream>

namespace ccbot {

namespace {

std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n";  break;
        case '\r': break;
        default:   o << c;
        }
    }
    return o.str();
}

int64_t tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
std::chrono::system_clock::time_point ms_to_tp(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

double get_num(simdjson::dom::object& o, const char* key, double def) {
    simdjson::dom::element val;
    if (o[key].get(val) != simdjson::SUCCESS) return def;
    double d;
    if (val.get(d) == simdjson::SUCCESS) return d;
    int64_t i;
    if (val.get(i) == simdjson::SUCCESS) return (double)i;
    uint64_t u;
    if (val.get(u) == simdjson::SUCCESS) return (double)u;
    return def;
}
int64_t get_i64(simdjson::dom::object& o, const char* key, int64_t def) {
    int64_t i;
    if (o[key].get(i) == simdjson::SUCCESS) return i;
    return def;
}
std::string get_str(simdjson::dom::object& o, const char* key, const std::string& def) {
    std::string_view v;
    if (o[key].get(v) == simdjson::SUCCESS) return std::string(v);
    return def;
}

} // namespace

namespace {
std::mutex state_save_mutex;
SaveResult save_trend_state_locked(const std::string& path, const std::vector<TrendBot>& bots) {
    std::ostringstream ss;
    // ⚠ 必须设精度。ostringstream 默认 6 位有效数字，而这里存的是开仓价、
    //   止损线、持仓量。截断的后果在 SAR 上比 DCA 更直接：止损线【就是】出场价，
    //   94310.4912 存成 94310.5 等于每次重启都把出场价挪一下。
    //   max_digits10=17 是"任意 double 无损往返"的最小位数
    ss << std::setprecision(std::numeric_limits<double>::max_digits10);
    ss << "[";
    bool first = true;
    for (const auto& b : bots) {
        if (!first) ss << ",";
        first = false;
        smart::Config saved_template;
        saved_template.tmpl = b.cfg;
        saved_template.dir_long = b.cfg.rule.direction != trend::Direction::ShortOnly;
        saved_template.dir_short = b.cfg.rule.direction != trend::Direction::LongOnly;
        ss << "{"
           << "\"symbol\":\""    << json_escape(b.cfg.symbol) << "\","
           << "\"state\":"       << (int)b.state << ","
           // 智能模式管理的 bot 不在配置文件里，恢复时要靠这个标记 + 智能模式模板
           // 重建参数（见 load_trend_state）
           << "\"auto\":"        << (b.is_auto ? "true" : "false") << ","
           << (b.is_auto ? "\"auto_config\":" + smart::config_to_json(saved_template) + "," : "")
           << "\"saved_reverse\":" << static_cast<int>(b.cfg.rule.reverse) << ","
           << "\"saved_hard_stop\":" << (b.cfg.use_disaster_stop ? "true" : "false") << ","
           << "\"open_uncertain\":" << ((b.open_uncertain || (b.pending && !b.rotation_reserved && b.st.pos == trend::Pos::Flat)) ? "true" : "false") << ","
           << "\"uncertain_client_id\":\"" << json_escape(b.uncertain_client_id) << "\","
           << "\"uncertain_qty\":" << b.uncertain_qty << ","
           << "\"uncertain_stop\":" << b.uncertain_stop << ","
           << "\"pending_close_key\":\"" << json_escape(b.pending_close_key) << "\","
           << "\"pending_close_reason\":\"" << json_escape(b.pending_close_reason) << "\","
           << "\"pending_close_ms\":" << b.pending_close_ms << ","
           << "\"reserved_risk\":" << b.reserved_risk << ","
           << "\"reserved_margin\":" << b.reserved_margin << ","
           << "\"reserved_dir\":" << (int)b.reserved_dir << ","
           << "\"pos\":"         << (int)b.st.pos << ","
           << "\"entry_price\":" << b.st.entry_price << ","
           << "\"peak\":"        << b.st.peak << ","
           << "\"stop\":"        << b.st.stop << ","
           // ② 抛物线SAR 专用：AF 丢了的话重启后 SAR 会从 af_start 重新加速，
           //    止损线瞬间从"贴着价格"退回远处——等于白白让出已经锁住的利润
           << "\"af\":"          << b.st.af << ","
           // ① 趋势跟踪锁定 ATR / 加速收紧的基准。丢了的话重启后静默退回普通 Chandelier
           << "\"entry_atr\":"   << b.st.entry_atr << ","
           // 开仓时被突破的通道边沿。丢了的话"收盘跌回通道内就走"在重启后失效
           << "\"entry_edge\":"  << b.st.entry_edge << ","
           << "\"fail_bar_ms\":" << b.st.fail_bar_ms << ","   // 假突破从哪一根的收盘开始判
           // 数据记录层：开仓那一刻的名单信息与入场位置。⚠ NaN 写进 JSON 是非法的，
           // 没有值就不写这个键
           << "\"open_ms\":"     << b.open_ms << ","
           << (std::isfinite(b.open_dist_atr)
                   ? "\"open_dist_atr\":" + std::to_string(b.open_dist_atr) + ","
                   : std::string())
           // 浮盈亏最低（持仓期最差价）与 R 的分母（初始止损线）
           << "\"trough\":"      << b.trough << ","
           << "\"open_stop\":"   << b.open_stop << ","
           // 开仓时的量比 / 箱体 / 触发方式（记录层）；名单代码——智能裸K 的 bot
           // 重启后靠它认出来，换用 5m 的参数恢复（不能套 1h 趋势跟踪模板）
           << (std::isfinite(b.open_vol_ratio)
                   ? "\"open_vol_ratio\":" + std::to_string(b.open_vol_ratio) + ","
                   : std::string())
           << (std::isfinite(b.open_box_atr)
                   ? "\"open_box_atr\":" + std::to_string(b.open_box_atr) + ","
                   : std::string())
           << (std::isfinite(b.open_taker)
                   ? "\"open_taker\":" + std::to_string(b.open_taker) + ","
                   : std::string())

           // ③ 纯裸K 专用：once_per_bar 的判据。不存的话重启会让"这一根已开过"
           //    忘掉，盘中即时模式下同一根里会再开一次
           << "\"monitor_checkpoint\":" << monitor_checkpoint_json(b) << ","
           << "\"reentry_after_ms\":" << b.reentry_after_ms << ","
           << "\"last_entry_bar_ms\":" << b.st.last_entry_bar_ms << ","
           << "\"consec_reverses\":" << b.st.consec_reverses << ","
           // 单号必须跨重启存活：遗留一张触发价对不上的孤儿单会让
           // 下次挂新单被拒（closePosition 同方向只能有一张）
           << "\"disaster_stop_id\":\"" << b.disaster_stop_id << "\","
           << "\"ds_vanished_id\":\"" << json_escape(b.ds_vanished_id) << "\","
           << "\"disaster_stop_price\":" << b.disaster_stop_price << ","
           << "\"cooldown_left\":"   << b.st.cooldown_left << ","
           << "\"qty\":"          << b.qty << ","
           << "\"current_price\":" << b.current_price << ","
           << "\"realized_pnl\":" << b.realized_pnl << ","
           << "\"trade_count\":"  << b.trade_count << ","
           << "\"win_count\":"    << b.win_count << ","
           << "\"start_time_ms\":" << tp_to_ms(b.start_time)
           << "}";
    }
    ss << "]";

    return write_atomic_file(path, ss.str());
}
} // namespace

SaveResult save_trend_state(const std::string& path, const std::vector<TrendBot>& bots) {
    std::lock_guard<std::mutex> lock(state_save_mutex);
    return save_trend_state_locked(path, bots);
}

SaveResult save_trend_state(const std::string& path, TrendEngine& engine) {
    std::lock_guard<std::mutex> lock(state_save_mutex);
    const auto result = save_trend_state_locked(path, engine.get_bots());
    engine.set_persistence_error("趋势断点", result.error);
    return result;
}

std::vector<TrendTrade> load_trades_csv(const std::string& path) {
    std::vector<TrendTrade> out;
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f) return out;
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) { header = false; continue; }
        // 列：time,symbol,direction,reason,entry_price,exit_price,qty,pnl,layers
        std::vector<std::string> cols;
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == ',') { cols.push_back(line.substr(start, i - start)); start = i + 1; }
        if (cols.size() < 9) continue;
        const size_t n = cols.size();
        TrendTrade t;
        try {
            t.entry_price = std::stod(cols[n - 5]);
            t.exit_price  = std::stod(cols[n - 4]);
            t.qty         = std::stod(cols[n - 3]);
            t.pnl         = std::stod(cols[n - 2]);
        } catch (...) { continue; }
        std::tm tmv{};
        std::istringstream ts(cols[0]);
        ts >> std::get_time(&tmv, "%Y-%m-%d %H:%M:%S");
        if (ts.fail()) continue;
        tmv.tm_isdst = -1;   // 让 mktime 自己判断夏令时
        const std::time_t tt = std::mktime(&tmv);
        if (tt == (std::time_t)-1) continue;
        t.close_time = std::chrono::system_clock::from_time_t(tt);
        t.symbol = cols[1];
        t.side   = (cols[2].find("short") != std::string::npos) ? trend::Pos::Short : trend::Pos::Long;
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<TrendBot> load_trend_state(const std::string& path,
                                   const std::vector<TrendConfig>& cfgs,
                                   const TrendConfig* auto_tmpl) {
    std::vector<TrendBot> out;

    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f) return out;
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string raw = buf.str();
    if (raw.empty()) throw std::runtime_error("状态文件为空，保留原文件并停止恢复");

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(raw);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS)
        throw std::runtime_error("状态文件JSON损坏，保留原文件并停止恢复");

    for (auto elem : arr) {
        simdjson::dom::object o;
        if (elem.get(o) != simdjson::SUCCESS) throw std::runtime_error("状态条目格式无效，停止恢复");

        const std::string sym = app::normalize_symbol(get_str(o, "symbol", "")).symbol;
        if (sym.empty()) throw std::runtime_error("状态条目缺少品种，停止恢复");
        if (!symbol_text::intact(sym))
            throw std::runtime_error("品种名称损坏，保留原文件并停止恢复；请核对持仓及委托后按 docs/SYMBOL_RECOVERY.zh-CN.md 修复");

        bool is_auto = false;
        {
            bool v = false;
            if (o["auto"].get(v) == simdjson::SUCCESS) is_auto = v;
        }
        int64_t reentry_after = 0;
        const auto reentry_err = o["reentry_after_ms"].get(reentry_after);
        if ((reentry_err != simdjson::SUCCESS && reentry_err != simdjson::NO_SUCH_FIELD) || reentry_after < 0)
            throw std::runtime_error("reentry_after_ms 状态字段损坏，保留原文件并停止恢复");
        const int saved_pos = (int)get_num(o, "pos", (double)(int)trend::Pos::Flat);
        const char smart_list = (char)(int)get_num(o, "smart_list", 0);

        if (is_auto && app::removed_smart_strategy(smart_list)) {
            if (saved_pos != (int)trend::Pos::Flat || get_num(o, "qty", 0) > 0)
                throw std::runtime_error("状态文件包含已移除智能策略的持仓 " + sym +
                    "；请使用旧版管理至平仓后再升级。原状态文件保持不变。");
            continue;
        }

        // 配置里已经删掉的品种，落盘状态直接丢弃。
        // 自动 bot 例外：它本来就不在配置里，参数取智能模式模板
        const TrendConfig* cfg = nullptr;
        TrendConfig auto_cfg;
        bool saved_uncertain = false;
        const auto uncertain_err = o["open_uncertain"].get(saved_uncertain);
        if (uncertain_err != simdjson::SUCCESS && uncertain_err != simdjson::NO_SUCH_FIELD)
            throw std::runtime_error("open_uncertain 状态字段损坏，保留原文件并停止恢复");
        if (is_auto) {
            if (auto_tmpl) {
                auto_cfg = *auto_tmpl;
            } else if (saved_pos == (int)trend::Pos::Flat && !saved_uncertain && reentry_after == 0) {
                continue;
            }
            simdjson::dom::element saved_config;
            const auto saved_error = o["auto_config"].get(saved_config);
            if (saved_error == simdjson::SUCCESS) {
                smart::Config stored; std::string why;
                if (!smart::config_from_json(simdjson::minify(saved_config), stored, &why))
                    throw std::runtime_error("自动品种策略参数损坏：" + why);
                auto_cfg = stored.tmpl;
                auto_cfg.rule.direction = smart::bot_direction(stored);
                auto_cfg.rule.reverse = static_cast<trend::ReverseMode>(get_i64(o, "saved_reverse", static_cast<int>(trend::ReverseMode::None)));
                bool hard_stop = true;
                if (o["saved_hard_stop"].get(hard_stop) == simdjson::SUCCESS) auto_cfg.use_disaster_stop = hard_stop;
            } else if (saved_error != simdjson::NO_SUCH_FIELD) {
                throw std::runtime_error("自动品种策略参数格式无效");
            }
            // 没有模板但有持仓：默认参数的趋势跟踪接着管，至少止损线还在
            auto_cfg.symbol = sym;
            cfg = &auto_cfg;
        } else {
            for (const auto& c : cfgs) if (c.symbol == sym) { cfg = &c; break; }
        }
        if (!cfg) continue;

        TrendBot b;
        b.needs_reconcile = true; // Also protect migration from older state files.
        b.cfg   = *cfg; // Automatic bots retain their saved template; manual configs remain explicit.
        b.is_auto = is_auto;
        b.reentry_after_ms = reentry_after;
        bool uncertain = false;
        if (o["open_uncertain"].get(uncertain) == simdjson::SUCCESS) b.open_uncertain = uncertain;
        b.uncertain_client_id = get_str(o, "uncertain_client_id", "");
        b.uncertain_qty = get_num(o, "uncertain_qty", 0);
        b.uncertain_stop = get_num(o, "uncertain_stop", 0);
        b.pending_close_key = get_str(o, "pending_close_key", "");
        b.pending_close_reason = get_str(o, "pending_close_reason", "");
        b.pending_close_ms = static_cast<int64_t>(get_num(o, "pending_close_ms", 0));
        b.reserved_risk = get_num(o, "reserved_risk", 0);
        b.reserved_margin = get_num(o, "reserved_margin", 0);
        b.reserved_dir = (trend::Pos)(int)get_num(o, "reserved_dir", 0);
        b.state = (TrendBot::State)(int)get_num(o, "state", 0);

        const int pos = (int)get_num(o, "pos", (double)(int)trend::Pos::Flat);
        b.st.pos = (pos == (int)trend::Pos::Long)  ? trend::Pos::Long
                 : (pos == (int)trend::Pos::Short) ? trend::Pos::Short
                                                 : trend::Pos::Flat;
        b.st.entry_price     = get_num(o, "entry_price", 0);
        b.st.peak            = get_num(o, "peak", 0);
        b.st.stop            = get_num(o, "stop", 0);
        // 旧存档没有 af。兜底取 af_start 而不是 0：0 会让 SAR 完全不动
        // （SAR += 0×(EP−SAR)），止损线就此冻在重启那一刻的位置
        b.st.af              = get_num(o, "af", b.cfg.rule.af_start);
        b.st.entry_atr       = get_num(o, "entry_atr", 0);
        b.st.entry_edge      = get_num(o, "entry_edge", 0);
        b.st.fail_bar_ms     = get_i64(o, "fail_bar_ms", 0);
        b.open_ms            = get_i64(o, "open_ms", 0);
        b.open_dist_atr      = get_num(o, "open_dist_atr", std::numeric_limits<double>::quiet_NaN());
        b.trough             = get_num(o, "trough", 0);
        b.open_stop          = get_num(o, "open_stop", 0);
        b.open_vol_ratio     = get_num(o, "open_vol_ratio", std::numeric_limits<double>::quiet_NaN());
        b.open_box_atr       = get_num(o, "open_box_atr", std::numeric_limits<double>::quiet_NaN());
        b.open_taker         = get_num(o, "open_taker", std::numeric_limits<double>::quiet_NaN());

        b.st.last_entry_bar_ms = get_i64(o, "last_entry_bar_ms", 0);
        b.st.consec_reverses = (int)get_num(o, "consec_reverses", 0);
        b.disaster_stop_id    = get_str(o, "disaster_stop_id", "");
        b.ds_vanished_id      = get_str(o, "ds_vanished_id", "");
        b.disaster_stop_price = get_num(o, "disaster_stop_price", 0);
        b.st.cooldown_left   = (int)get_num(o, "cooldown_left", 0);
        b.qty           = get_num(o, "qty", 0);
        b.current_price = get_num(o, "current_price", 0);
        b.realized_pnl  = get_num(o, "realized_pnl", 0);
        b.trade_count   = (int)get_num(o, "trade_count", 0);
        b.win_count     = (int)get_num(o, "win_count", 0);
        b.start_time    = ms_to_tp(get_i64(o, "start_time_ms", 0));

        // 半截状态防御：有方向却没有开仓价或没有止损线，说明落盘写到一半或被
        // 人改过。这种状态比"空仓"危险得多——止损线为 0 时，多头的
        // `price <= stop` 永远不成立，仓位会一直裸着没人管。
        // 当成空仓丢弃，交给对账去发现交易所上那笔真实仓位并停掉 bot
        if (b.st.pos != trend::Pos::Flat &&
            (b.st.entry_price <= 0 || b.st.stop <= 0 || b.qty <= 0)) {
            b.st = trend::State{};
            b.qty = 0;
        }

        simdjson::dom::element checkpoint;
        if (o["monitor_checkpoint"].get(checkpoint) == simdjson::SUCCESS) {
            std::string error;
            if (!restore_monitor_checkpoint(simdjson::to_string(checkpoint), b, error)) throw std::runtime_error(error);
        }
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace ccbot
