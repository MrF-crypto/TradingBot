#pragma once
#include "core/market_wake.h"
#include "core/journal.h"
#include "core/smart_mode.h"
#include "core/trend_engine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ccbot {

class TradingClient;
class ProfitManager;

// 智能模式的执行层：拉全市场数据 → 硬过滤 → 候选池决策 → 落到引擎上。
// GUI 与 headless 共用这一份，两边只负责"什么时候调"和"界面/日志怎么显示"。
//
// 线程：run_once 是阻塞的（几十秒的网络请求），调用方放到后台线程跑。
// 其余方法都可以在任意线程调用。

// 一次全市场扫描。pool_syms = 当前自动 bot 的品种（它们走放宽的门槛，
// 并且"这轮没拿到数据"时归为未知而不是不合格）
struct MarketScan {
    smart::ScanOutcome outcome;
    int universe = 0;        // USDT 永续总数
    int passed_gate = 0;     // 过了不需要K线的门槛
    int eligible = 0;        // 特征算得出且 ATR% 合格
    std::string error;       // outcome.ok=false 时的原因
    std::map<std::string, smart::OiStat> oi;   // 合格币的持仓量（数据记录层）；没拿到的不在里面
};
MarketScan scan_market(TradingClient& c, const smart::Config& cfg,
                       const std::set<std::string>& pool_syms, int64_t now_ms, int parallel = 3);

// 用成交历史算某个 UTC 日的已实现盈亏，扣估算手续费——口径与引擎的当日亏损一致
// （TrendEngine::book_day）。启动时状态文件里没有今天的数（升级后第一次运行、
// 文件丢了）就用它补，否则当日亏损限额会从 0 重新算
double sum_day_realized(const std::vector<TrendTrade>& trades, int64_t utc_day);

class SmartRunner {
public:
    using LogFn = std::function<void(const std::string& msg, bool warn)>;

    SmartRunner(std::shared_ptr<TradingClient> client, std::shared_ptr<TrendEngine> engine,
                std::string state_path, LogFn log);

    // 换配置：立刻把风控闸门落到引擎上；方向或模板变了的空仓自动 bot 下一轮换参数。
    // 启用 / 方向 / 门槛变化时，下一次 due() 立刻为真
    void set_config(const smart::Config& cfg);
    smart::Config config() const;

    // 到点没有（每小时整点后 1 分钟，等 1h K线收盘；启用或改配置后立即）
    bool due(int64_t now_ms) const;
    void force();

    struct Row {
        std::string symbol;
        double atr_pct = 0, close = 0;
        bool has_bot = false;
    };
    struct Report {
        bool ok = false;
        int64_t at_ms = 0;
        int universe = 0, passed_gate = 0, eligible = 0;
        int added = 0, removed = 0, monitored = 0, holding = 0;
        std::string summary;
        std::vector<Row> candidates;
    };
    static std::vector<Row> candidate_rows(const std::map<std::string, smart::Features>& candidates,
                                           const std::set<std::string>& with_bot);
    // 扫描 + 落地。阻塞。未启用时只做清理（见 housekeep）
    Report run_once(int64_t now_ms);
    Report last_report() const;

    // 不扫描的轻量维护，可以每分钟调一次：
    //   · 未启用时：空仓的自动 bot 移除（持仓中的等它走完）
    //   · 当日盈亏落盘（亏损限额跨重启要接得上）
    void housekeep(int64_t now_ms);
    void evaluate_profits(int64_t now_ms);
    void service_profits(int64_t now_ms);
    std::string profit_status() const;

    // 品种加入/移除时通知应用层（订阅/退订价格、刷新界面）
    std::function<void(const std::string&)> on_symbol_added;
    std::function<void(const std::string&)> on_symbol_removed;

    // 启动时调用：读池状态与当日盈亏
    void load();
    // 数据记录层：每轮扫描的名单快照、候选池的加入/移出/换方向写进 journal（可不设）
    void set_journal(std::shared_ptr<Journal> j) { journal_ = std::move(j); }
    // load 之后调用：引擎里今天的已实现还是 0（状态文件里没有今天的数）时，
    // 用成交历史补上
    void seed_day_from_history(const std::vector<TrendTrade>& trades);

    // 作废这个运行器：正在跑的那一轮扫描完成后【不落地】、不写状态文件。
    // GUI 断线重连会连引擎一起重建运行器，旧的那轮扫描若照常落地，会把结果写进
    // 已经不用的旧引擎，还和新运行器抢同一个状态文件
    void cancel();

private:
    void apply_engine_gates_locked();
    void save_locked() const;
    static bool same_params(const TrendConfig& a, const TrendConfig& b);
    // 持仓中的自动 bot 换上当前设置的出场参数（set_config 时、每分钟维护时）。返回改了几个
    int sync_exit_params(const smart::Config& cfg);
    std::shared_ptr<TradingClient> client_;
    std::shared_ptr<TrendEngine>   engine_;
    std::shared_ptr<Journal>       journal_;
    std::shared_ptr<ProfitManager> profits_;
    std::string                    state_path_;
    LogFn                          log_;

    mutable std::mutex mtx_;
    smart::Config      cfg_;
    smart::Pool        pool_;
    Report             last_;
    int64_t            next_due_ms_ = 0;   // 0 = 立刻
    smart::MarketWake market_wake_;
    std::map<std::string, double> wake_atr_pct_;
    int64_t next_poll_ms_ = 0, last_full_scan_ms_ = 0;
    std::mutex         run_mtx_;           // run_once 不重入
    std::atomic<bool>  cancelled_{false};
    // 连续扫描失败的轮数。只有第一轮失败走告警（webhook），之后只记日志，
    // 恢复时说一声——长时间断网不至于每小时推一条同样的话
    int                fail_streak_ = 0;
};

} // namespace ccbot
