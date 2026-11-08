#include "core/atomic_file.h"
#include "app/smart_runner.h"
#include "app/profit_manager.h"
#include "core/parallel.h"
#include "net/trading_client.h"

#include <simdjson.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace ccbot {

namespace {

constexpr int64_t kHourMs = 3600LL * 1000;

std::string fmt(double v, int prec) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}
// 选币只使用已收盘 K 线，排除最后一根未收盘数据。
std::vector<smart::Bar> closed_bars(const std::vector<TradingClient::Bar>& bars) {
    std::vector<smart::Bar> out;
    if (bars.size() < 2) return out;
    out.reserve(bars.size() - 1);
    for (size_t i = 0; i + 1 < bars.size(); ++i) {
        const auto& b = bars[i];
        out.push_back({b.open_ms, b.open, b.high, b.low, b.close, b.volume,
                       b.quote_volume, b.taker_buy_quote});
    }
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

} // namespace

std::vector<SmartRunner::Row> SmartRunner::candidate_rows(
        const std::map<std::string, smart::Features>& candidates,
        const std::set<std::string>& with_bot) {
    std::vector<Row> rows;
    for (const auto& [symbol, f] : candidates)
        rows.push_back({symbol, f.atr_pct, f.close, with_bot.count(symbol) > 0});
    return rows; // 按品种稳定显示；没有评分、方向分榜或候选数量截断
}

// ── 全市场扫描 ──────────────────────────────────────────────────────────────

namespace {

// 交易规则 + 24h 行情 + 资金费率 → 过硬门槛的趋势跟踪候选。
// 池里的币（pool_syms）走放宽门槛，没拿到数据 / 不合格 / 即将下架分别记进 out。
// 返回 false = 地基没拿到，整轮作废（r.error 已填）。
// 只接受加密币标的（COIN），排除黄金代币、股票、大宗商品与指数。
bool gate_universe(TradingClient& c, const smart::Config& cfg,
                   const std::set<std::string>& pool_syms, int64_t now_ms,
                   MarketScan& r, std::vector<std::string>& cand) {
    auto& out = r.outcome;

    // 交易规则与 24h 行情是整轮的地基，拿不到就整轮作废——绝不能把"没拿到"
    // 当成"池里的币都不合格"去删 bot
    bool ok_meta = false, ok_t24 = false, ok_fund = false;
    // 失败时带上网络层的原因（超时 / 连接中断 / 返回的不是 JSON……）：同一线程、紧跟着请求读
    auto net_cause = [&c]() {
        const auto e = c.last_error();
        return e.empty() ? std::string() : "：" + e;
    };
    const auto metas = c.fetch_exchange_symbols(&ok_meta);
    if (!ok_meta) { r.error = "交易规则拉取失败" + net_cause(); return false; }
    const auto t24 = c.fetch_all_24h(&ok_t24);
    if (!ok_t24) { r.error = "24h 行情拉取失败" + net_cause(); return false; }
    // 资金费率拿不到不致命：这一轮不按费率判，其余照常
    const auto fund = c.fetch_all_funding_pct(&ok_fund);

    std::set<std::string> seen;
    for (const auto& mm : metas) {
        smart::SymbolMeta m;
        m.symbol = mm.symbol; m.base = mm.base; m.quote = mm.quote;
        m.contract_type = mm.contract_type; m.status = mm.status;
        m.onboard_ms = mm.onboard_ms; m.delivery_ms = mm.delivery_ms; m.tick = mm.tick;
        m.underlying_type = mm.underlying_type;
        if (m.quote != "USDT" || m.contract_type != "PERPETUAL") continue;
        ++r.universe;
        seen.insert(m.symbol);
        const bool member = pool_syms.count(m.symbol) > 0;
        // 池里已有的（升级前选进来的）明确标为不合格，暂停新仓并在空仓时移出。
        if (((!m.underlying_type.empty() && m.underlying_type != "COIN") ||
                          smart::excluded_asset_base(m.base))) {
            if (member) out.hard_fail[m.symbol] = "非加密币标的（股票 / 大宗商品 / 指数）";
            continue;
        }
        if (member && smart::delisting(m, now_ms)) out.delisting.insert(m.symbol);

        const auto it = t24.find(m.symbol);
        if (it == t24.end()) {                 // 这一个没拿到：池里的归为未知
            if (member) out.unknown.insert(m.symbol);
            continue;
        }
        const smart::Ticker24 tv{it->second.quote_vol, it->second.chg_pct, it->second.last};
        const double* fp = nullptr;
        double fv = 0;
        if (ok_fund)
            if (auto f = fund.find(m.symbol); f != fund.end()) { fv = f->second; fp = &fv; }

        const auto why = smart::pre_gate(m, &tv, fp, cfg, now_ms, member);
        if (!why.empty()) {
            if (member) out.hard_fail[m.symbol] = why;
            continue;
        }
        ++r.passed_gate;
        cand.push_back(m.symbol);
    }
    // 交易规则这次【是拿到了的】，池里的币不在里面 = 合约确实没了
    for (const auto& s : pool_syms)
        if (!seen.count(s)) out.hard_fail[s] = "交易所已无此 USDT 永续合约";
    return true;
}

} // namespace

MarketScan scan_market(TradingClient& c, const smart::Config& cfg,
                       const std::set<std::string>& pool_syms, int64_t now_ms, int parallel) {
    MarketScan r;
    auto& out = r.outcome;
    std::vector<std::string> cand;
    if (!gate_universe(c, cfg, pool_syms, now_ms, r, cand)) return r;

    // 保留 200 根已收盘 1h K线用于 ATR14 的稳定预热。无需 BTC 基准或相对强弱。

    std::vector<smart::Features> feats(cand.size());
    std::vector<char> got(cand.size(), 0);
    parallel_for((int)cand.size(), parallel, [&](int i) {
        try {
            const auto bars = closed_bars(c.fetch_klines(cand[i], "1h", 201));
            if ((int)bars.size() < smart::kMinBars) return;
            feats[i] = smart::compute_features(bars);
            got[i] = 1;
        } catch (...) {}
    });

    int fetched = 0;
    std::map<std::string, smart::Features> eligible;
    for (size_t i = 0; i < cand.size(); ++i) {
        const auto& s = cand[i];
        const bool member = pool_syms.count(s) > 0;
        if (got[i]) ++fetched;
        if (!got[i] || !feats[i].ok) {
            if (member) out.unknown.insert(s);
            continue;
        }
        const auto why = smart::atr_gate(feats[i], cfg, member);
        if (!why.empty()) {
            if (member) out.hard_fail[s] = why;
            continue;
        }
        eligible[s] = feats[i];
    }
    // K线大面积拉不到：按数据故障处理，不据此批量移除已有候选。
    if (!cand.empty() && fetched * 10 < (int)cand.size() * 7) {
        r.error = "K线拉取失败过多（" + std::to_string(fetched) + "/" +
                  std::to_string(cand.size()) + "）";
        return r;
    }
    r.eligible = (int)eligible.size();
    // 成功拿到数据但无人通过过滤是合法的空候选池；明确不合格的旧 bot 仍要暂停新仓。
    out.candidates = std::move(eligible);
    out.ok = true;

    // 持仓量：只给合格的币拉，只写进扫描记录、不参与任何决策。
    // 拉不到的币就不在 r.oi 里（记录里留空），不影响这一轮
    std::vector<std::string> syms;
    for (const auto& kv : out.candidates) syms.push_back(kv.first);
    std::vector<smart::OiStat> oi(syms.size());
    parallel_for((int)syms.size(), parallel, [&](int i) {
        try {
            // 25 根 1h：最新一点 + 24 小时前那一点
            std::vector<smart::OiPoint> pts;
            for (const auto& p : c.fetch_oi_hist(syms[i], "1h", 25)) pts.push_back({p.ms, p.oi, p.value});
            oi[i] = smart::oi_stat(pts);
        } catch (...) {}
    });
    for (size_t i = 0; i < syms.size(); ++i)
        if (oi[i].ok) r.oi[syms[i]] = oi[i];
    return r;
}

double sum_day_realized(const std::vector<TrendTrade>& trades, int64_t utc_day) {
    double r = 0;
    for (const auto& t : trades) {
        if (t.close_time.time_since_epoch().count() == 0) continue;
        if (TrendEngine::utc_day(t.close_time) != utc_day) continue;
        const double fee = (std::fabs(t.entry_price) + std::fabs(t.exit_price)) *
                           std::fabs(t.qty) * TrendEngine::kEstFeeRate;
        if (std::isfinite(t.pnl) && std::isfinite(fee)) r += t.pnl - fee;
    }
    return r;
}

// ── SmartRunner ────────────────────────────────────────────────────────────

void SmartRunner::seed_day_from_history(const std::vector<TrendTrade>& trades) {
    if (!engine_) return;
    const auto d = engine_->day_pnl();
    if (d.realized != 0) return;   // 状态文件里有今天的数，以它为准
    const double r = sum_day_realized(trades, d.day);
    if (r != 0) {
        engine_->restore_day_pnl({d.day, r});
        if (log_) log_("智能模式：按成交历史补算今天（UTC）已实现 " + fmt(r, 2) + "U（含估算手续费）", false);
    }
}

void SmartRunner::cancel() { cancelled_.store(true); if (profits_) profits_->cancel(); }

SmartRunner::SmartRunner(std::shared_ptr<TradingClient> client, std::shared_ptr<TrendEngine> engine,
                         std::string state_path, LogFn log)
    : client_(std::move(client)), engine_(std::move(engine)),
      state_path_(std::move(state_path)), log_(std::move(log)) {
    if (client_ && engine_) {
        profits_ = std::make_shared<ProfitManager>(state_path_.empty() ? "" : state_path_ + ".profit", client_->account_fingerprint());
        engine_->set_execution_cb([p = profits_](ExecutionEvent& e) { return p->record(e); });
    }
}

void SmartRunner::evaluate_profits(int64_t now) {
    if (!profits_ || cancelled_) return;
    profits_->evaluate(*engine_, now);
    if (log_) for (const auto& notice : profits_->take_notices()) log_(notice, false);
}
void SmartRunner::service_profits(int64_t now) { if (profits_ && !cancelled_) profits_->service(*client_, *engine_, client_->market_time_ms()); }
std::string SmartRunner::profit_status() const { return profits_ ? profits_->status() : "尚未连接"; }

void SmartRunner::set_config(const smart::Config& cfg) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::string before = smart::config_to_json(cfg_);
        cfg_ = cfg;
        // 任何变化都立刻重扫一次：门槛、方向、模板变了，名单和 bot 参数都要跟上。
        if (smart::config_to_json(cfg_) != before) {
            next_due_ms_ = 0;
        }
        apply_engine_gates_locked();
    }
    sync_exit_params(cfg);
    if (profits_) {
        profits_->configure(cfg.profit, cfg.enabled, client_->market_time_ms());
        evaluate_profits(client_->market_time_ms());
    }
}

// 持仓中的自动 bot 跟着设置换出场参数（空仓的由各自的扫描整套换掉）。
// 按 bot 自己属于哪一套取那一套的设置：切了策略、还在走完的老仓位同样跟着改
int SmartRunner::sync_exit_params(const smart::Config& cfg) {
    if (!engine_) return 0;
    std::vector<std::string> names;
    for (const auto& b : engine_->get_bots()) {
        if (!b.is_auto || b.state == TrendBot::State::Stopped) continue;
        if (b.st.pos == trend::Pos::Flat && b.qty <= 0) continue;
        const auto want = smart::make_bot_config(cfg, b.cfg.symbol, b.cfg.rule.direction);
        if (engine_->update_exit_params(b.bot_id, want.rule)) names.push_back(b.cfg.symbol);
    }
    if (!names.empty() && log_)
        log_("智能模式：" + std::to_string(names.size()) + " 个持仓换上新的出场参数（" +
             join(names, "、") + "）", false);
    return (int)names.size();
}

smart::Config SmartRunner::config() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

void SmartRunner::apply_engine_gates_locked() {
    if (!engine_) return;
    // 暂停开新仓是全局应急开关，智能模式关着也照样生效
    engine_->set_pause_new(cfg_.pause_new);
    engine_->set_rotation_enabled(cfg_.enabled && cfg_.rotate_when_full);
    if (cfg_.enabled) {
        engine_->set_risk_limits(cfg_.max_total_risk_usdt, cfg_.max_direction_risk_usdt, cfg_.risk_cost_bps);
        engine_->set_max_open_positions(cfg_.max_positions);
        engine_->set_daily_loss_limit(cfg_.daily_loss_limit);
        engine_->set_float_loss_limit(cfg_.float_loss_limit);
    } else {
        // 关闭时撤掉智能模式的闸门。同时持仓上限交还应用层自己的设置
        // （应用层在 set_config 之后会重新设一次）
        engine_->set_risk_limits(0, 0, 0);
        engine_->set_daily_loss_limit(0);
        engine_->set_float_loss_limit(0);
    }
}

bool SmartRunner::due(int64_t now_ms) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_.enabled && (now_ms >= next_due_ms_ || now_ms >= next_poll_ms_);
}

void SmartRunner::force() {
    std::lock_guard<std::mutex> lk(mtx_);
    next_due_ms_ = 0;
}

SmartRunner::Report SmartRunner::last_report() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_;
}

bool SmartRunner::same_params(const TrendConfig& a, const TrendConfig& b) {
    const auto& x = a.rule;
    const auto& y = b.rule;
    return a.interval == b.interval && a.budget_usdt == b.budget_usdt &&
           a.leverage == b.leverage && a.size_mode == b.size_mode && a.risk_usdt == b.risk_usdt &&
           a.signal_max_age_sec == b.signal_max_age_sec &&
           a.use_disaster_stop == b.use_disaster_stop &&
           a.disaster_stop_buffer_pct == b.disaster_stop_buffer_pct &&
           x.following == y.following && x.strategy == y.strategy && x.direction == y.direction && x.reverse == y.reverse &&
           x.donchian_period == y.donchian_period && x.atr_period == y.atr_period &&
           x.atr_mult == y.atr_mult && x.squeeze_box_atr == y.squeeze_box_atr &&
           x.trend_er_min == y.trend_er_min && x.breakout_vol_ratio == y.breakout_vol_ratio &&
           x.lock_atr == y.lock_atr && x.accel_exit == y.accel_exit &&
           x.vol_paced == y.vol_paced && x.fail_exit == y.fail_exit &&
           x.fail_exit_short == y.fail_exit_short &&
           x.roi_trail == y.roi_trail && x.roi_trail_start == y.roi_trail_start &&
           x.roi_trail_back == y.roi_trail_back && x.once_per_bar == y.once_per_bar &&
           x.max_chase_atr == y.max_chase_atr && x.max_init_loss_pct == y.max_init_loss_pct &&
           x.breakeven == y.breakeven && x.breakeven_start == y.breakeven_start &&
           x.expand_long == y.expand_long && x.expand_long_min == y.expand_long_min &&
           x.max_consecutive_reverses == y.max_consecutive_reverses &&
           x.cooldown_bars == y.cooldown_bars;
}

SmartRunner::Report SmartRunner::run_once(int64_t now_ms) {
    std::unique_lock<std::mutex> rl(run_mtx_, std::try_to_lock);
    if (!rl.owns_lock()) return last_report();   // 上一轮还在跑

    const smart::Config cfg = config();
    Report rep;
    rep.at_ms = now_ms;
    if (cancelled_.load()) return rep;
    if (!cfg.enabled || !engine_ || !client_) { housekeep(now_ms); return rep; }
    bool regular_scan = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        regular_scan = now_ms >= next_due_ms_;
        next_poll_ms_ = now_ms + static_cast<int64_t>(cfg.event_poll_sec) * 1000;
    }
    if (!regular_scan) {
        bool ok = false;
        const auto tickers = client_->fetch_all_24h(&ok);
        bool wake = false;
        if (ok) {
            std::map<std::string, double> prices;
            for (const auto& [symbol, t] : tickers) prices[symbol] = t.last;
            wake = market_wake_.observe(prices, wake_atr_pct_, now_ms, cfg.event_poll_sec,
                                       cfg.event_cooldown_sec, cfg.event_move_atr, cfg.event_min_move_pct);
        }
        if (!wake || now_ms - last_full_scan_ms_ < static_cast<int64_t>(cfg.event_cooldown_sec) * 1000)
            return last_report();
        if (log_) log_("智能模式：检测到双向价格异动，合并重扫；入场仍需趋势信号和风险预算", false);
    }
    const auto bots = engine_->get_bots();
    std::set<std::string> pool_syms;
    std::vector<smart::BotView> views;
    views.reserve(bots.size());
    for (const auto& b : bots) {
        views.push_back({b.bot_id, b.cfg.symbol, b.is_auto,
                         b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending,
                         b.state == TrendBot::State::Stopped});
        if (b.is_auto) pool_syms.insert(b.cfg.symbol);
    }

    const auto scan = scan_market(*client_, cfg, pool_syms, now_ms);
    // 扫描要几十秒，期间这个运行器可能已经被作废（断线重连换了新的）：不落地
    if (cancelled_.load() || smart::config_to_json(config()) != smart::config_to_json(cfg)) return rep;
    {
        // 下一次：下一个整点后 1 分钟（等 1h K线收盘）
        std::lock_guard<std::mutex> lk(mtx_);
        if (regular_scan) next_due_ms_ = (now_ms / kHourMs + 1) * kHourMs + 60 * 1000;
        last_full_scan_ms_ = now_ms;
    }
    rep.universe    = scan.universe;
    rep.passed_gate = scan.passed_gate;
    rep.eligible    = scan.eligible;
    if (!scan.outcome.ok) {
        int streak = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            streak = ++fail_streak_;
        }
        rep.summary = "智能模式：本轮扫描失败（" + scan.error + "），候选池不动，5 分钟后重试" +
                      (streak > 1 ? "（已连续 " + std::to_string(streak) + " 轮）" : std::string());
        // 只有第一轮失败走告警（会推 webhook）；5 分钟一试，断网时会连着失败，之后每小时提一次
        if (log_ && (streak == 1 || streak % 12 == 0)) log_(rep.summary, streak == 1);
        std::lock_guard<std::mutex> lk(mtx_);
        // 扫描失败不等下个整点：经代理时多半是一次瞬时故障（实盘"交易规则拉取失败"），
        // 空等一小时池子就旧了一小时
        if (regular_scan) next_due_ms_ = now_ms + 5 * 60 * 1000;
        last_ = rep;
        return rep;
    }
    {
        int was = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            was = fail_streak_;
            fail_streak_ = 0;
        }
        if (was > 0 && log_)
            log_("智能模式：扫描已恢复（此前连续失败 " + std::to_string(was) + " 轮）", false);
    }

    wake_atr_pct_.clear();
    for (const auto& [sym, row] : scan.outcome.candidates) wake_atr_pct_[sym] = row.atr_pct;

    smart::Plan plan;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (regular_scan) plan = pool_.plan(scan.outcome, views, cfg, now_ms);
        else {
            auto scratch = pool_;
            plan = scratch.plan(scan.outcome, views, cfg, now_ms);
            plan.remove.clear(); // 异动重扫更新准入状态；移除操作留给常规扫描。
        }
    }

    // ── 移出 ──
    std::set<std::string> gone_ids;
    std::vector<std::string> rm_desc;
    for (const auto& r : plan.remove) {
        // 扫描期间它可能刚好开了仓：只删空仓的，开了仓的这一轮放过
        if (!engine_->remove_if_flat(r.bot_id)) continue;
        gone_ids.insert(r.bot_id);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pool_.on_removed(r.symbol);
        }
        ++rep.removed;
        rm_desc.push_back(r.symbol + "（" + r.why + "）");
        if (journal_) journal_->pool(now_ms, r.symbol, "移出", r.why);
        if (on_symbol_removed) on_symbol_removed(r.symbol);
    }

    // 明确不合格但仍持仓/在途/被批量保护保留的 bot 继续管理，只暂停新仓。
    // 未知数据不会出现在 meta 中，保持原状态；重新合格时解除这项暂停。
    std::set<std::string> eligible_ids;
    for (const auto& m : plan.meta) {
        if (gone_ids.count(m.bot_id)) continue;
        engine_->set_retiring(m.bot_id, !m.eligible);
        if (m.eligible) eligible_ids.insert(m.bot_id);
    }
    // 空仓候选更新方向权限与模板，实际多空仍由自身趋势和突破信号决定。
    int resynced = 0, redirected = 0;
    for (const auto& b : bots) {
        if (!b.is_auto || gone_ids.count(b.bot_id)) continue;
        if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending) continue;
        if (b.state == TrendBot::State::Stopped) continue;
        if (!eligible_ids.count(b.bot_id)) continue;
        const auto want = smart::make_bot_config(cfg, b.cfg.symbol);
        if (same_params(b.cfg, want)) continue;
        // ⚠ replace_bot 在 id 不存在时会【新建】——上面已排除本轮删掉的；
        //   它自己会拒绝有持仓/在途订单的
        if (engine_->replace_bot(b.bot_id, want).empty()) continue;
        ++resynced;
        if (b.cfg.rule.direction != want.rule.direction) {
            ++redirected;
            if (journal_)
                journal_->pool(now_ms, b.cfg.symbol, "方向权限变化",
                               std::string(trend::direction_name(b.cfg.rule.direction)) + " → " +
                               trend::direction_name(want.rule.direction));
        }
    }

    // ── 补位 ──
    std::vector<std::string> add_desc;
    for (const auto& a : plan.add) {
        const auto id = engine_->add_bot(smart::make_bot_config(cfg, a.symbol, a.dir), true);
        if (id.empty()) continue;   // 同品种已有 bot（扫描期间被手动加了）
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pool_.on_added(a.symbol, now_ms);
        }
        ++rep.added;
        add_desc.push_back(a.symbol);
        if (journal_) journal_->pool(now_ms, a.symbol, "加入", trend::direction_name(a.dir));
        if (on_symbol_added) on_symbol_added(a.symbol);
    }

    for (const auto& s : plan.alerts)
        if (log_) log_("⚠ " + s, true);

    // ── 汇总 ──
    std::set<std::string> with_bot;
    for (const auto& b : engine_->get_bots()) {
        with_bot.insert(b.cfg.symbol);
        if (!b.is_auto) continue;
        ++rep.monitored;
        if (b.qty > 0) ++rep.holding;
    }
    rep.candidates = candidate_rows(scan.outcome.candidates, with_bot);
    if (journal_) journal_->scan(now_ms, scan.outcome.candidates, with_bot, scan.oi);
    rep.ok = true;
    rep.summary = "智能模式：统一候选 " + std::to_string(rep.eligible) + "/" + std::to_string(rep.universe) +
                  "；+" + std::to_string(rep.added) + " −" + std::to_string(rep.removed) +
                  "，监控 " + std::to_string(rep.monitored) + "（持仓 " + std::to_string(rep.holding) + "）";
    if (log_) {
        log_(rep.summary, false);
        if (!add_desc.empty()) log_("  新增：" + join(add_desc, "、"), false);
        if (!rm_desc.empty())  log_("  移出：" + join(rm_desc, "、"), false);
        if (resynced > 0)
            log_("  " + std::to_string(resynced) + " 个空仓自动 bot 已换上新参数" +
                 (redirected > 0 ? "（其中 " + std::to_string(redirected) + " 个更新了方向权限）"
                                 : std::string()), false);
        const auto fs = engine_->take_filter_summary();
        if (!fs.empty())       log_("  上一轮以来自动 bot：" + fs, false);
    }

    std::lock_guard<std::mutex> lk(mtx_);
    last_ = rep;
    save_locked();
    return rep;
}

void SmartRunner::housekeep(int64_t now_ms) {
    if (!engine_ || cancelled_.load()) return;
    const bool enabled = config().enabled;
    // 重启恢复的持仓带的是落盘时的参数：每分钟对一次设置
    sync_exit_params(config());
    if (!enabled) {
        // 关闭后：普通空仓自动 bot 移除；持仓、在途和等待再次入场的记录保留并禁开。
        // 等待记录需留到重新启用时继续检查；已停止的（你或对账停的）不替你删。
        for (const auto& b : engine_->get_bots()) {
            if (!b.is_auto || b.state == TrendBot::State::Stopped) continue;
            if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending || b.waiting_reentry()) {
                if (!b.retiring) engine_->set_retiring(b.bot_id);
                continue;
            }
            if (!engine_->remove_if_flat(b.bot_id)) continue;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                pool_.on_removed(b.cfg.symbol);
            }
            if (on_symbol_removed) on_symbol_removed(b.cfg.symbol);
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    save_locked();
}

void SmartRunner::save_locked() const {
    if (state_path_.empty() || cancelled_.load()) return;   // 作废的不和新运行器抢文件
    std::ostringstream o;
    o << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto d = engine_ ? engine_->day_pnl() : TrendEngine::DayPnl{};
    o << "{\"pool\":" << pool_.to_json() << ",\"day\":{\"d\":" << d.day << ",\"r\":" << d.realized << "}}";
    const auto result = write_atomic_file(state_path_, o.str());
    if (engine_) engine_->set_persistence_error("智能状态", result.error);
}

void SmartRunner::load() {
    std::ifstream f(std::filesystem::path(std::u8string(state_path_.begin(), state_path_.end())), std::ios::binary);
    if (!f) return;
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string raw = buf.str();
    simdjson::dom::parser p;
    simdjson::dom::element root;
    auto ps = simdjson::padded_string(raw);
    if (p.parse(ps).get(root) != simdjson::SUCCESS) return;
    simdjson::dom::element pool;
    if (root["pool"].get(pool) == simdjson::SUCCESS) {
        std::lock_guard<std::mutex> lk(mtx_);
        pool_.from_json(simdjson::to_string(pool));
    }
    int64_t day = 0;
    double r = 0;
    if (root["day"]["d"].get(day) == simdjson::SUCCESS && engine_) {
        if (root["day"]["r"].get(r) != simdjson::SUCCESS) {
            int64_t ri = 0;
            if (root["day"]["r"].get(ri) == simdjson::SUCCESS) r = (double)ri;
        }
        engine_->restore_day_pnl({day, r});
    }
}

} // namespace ccbot
