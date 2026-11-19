#pragma once
#include "core/profit_policy.h"
#include "core/execution_event.h"
#include "core/trend_engine.h"
#include "net/trading_client.h"
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace ccbot {
// Settlement is serialized by service_mutex_. An opening during an unresolved
// futures transfer checks the available margin outside the engine and profit locks.
// Engine execution callbacks run outside its mutex. Snapshot reads may take the
// engine lock under mutex_; engine mutations/close dispatch always happen outside.
class ProfitManager {
public:
    ProfitManager(std::string path, std::string account);
    void configure(profit::Config cfg, bool smart_enabled, int64_t now, const TrendConfig& tmpl = {});
    bool record(ExecutionEvent& event, TradingClient* client = nullptr);
    void evaluate(TrendEngine& engine, int64_t now);
    void service(TradingClient& client, TrendEngine& engine, int64_t now);
    bool needs_breadth_quotes() const;
    void refresh_breadth(TradingClient& client);
    void update_breadth_market(const std::set<std::string>& listed, const std::set<std::string>& settled,
                               const std::set<std::string>& candidates, int64_t now);
    std::string status() const;
    std::vector<std::string> take_notices();
    std::vector<TrendTrade> recovered_trades() const;
    void cancel() { cancelled_.store(true); }
private:
    struct Entry {
        ExecutionEvent event;
        bool settled = false;
        double net = 0;
        int64_t first_fill = 0, last_fill = 0;
        // Only historical closes carry a link to their settled opening receipt.
        std::string recovery_open_key;
        double recovery_gross = 0, recovery_fee = 0;
    };
    struct Funding { double net = 0; int64_t checked = 0; };
    struct Purchase {
        // 0 idle; 1 transfer submitted/unknown; 2 spot order submitted/unknown.
        int phase = 0;
        int64_t sent = 0;
        std::string tag, transfer_id;
        double reserved = 0;
    };
    std::string serialize_locked() const;
    void load();
    bool save_locked();
    bool record_locked(ExecutionEvent& event);
    double pending_open_margin_locked() const;
    void totals_locked(double& shorts, double& overall, bool& complete) const;
    void finish_purchase_locked(const TradingClient::SpotPurchase& result);
    void recover_external_close(TradingClient& client, TrendEngine& engine, int64_t now);
    std::string path_, account_, error_, message_;
    mutable std::mutex mutex_;
    std::mutex service_mutex_;
    std::atomic<bool> cancelled_{false};
    profit::Config cfg_;
    bool smart_enabled_ = false, tracking_ = false, damaged_ = false;
    int64_t started_ = 0, accumulation_since_ = 0, sequence_ = 0;
    double capital_floor_ = 0, used_ = 0, btc_qty_ = 0, spot_cash_ = 0;
    double btc_price_ = 0;
    std::array<profit::Guard, 2> guards_;
    std::array<std::set<std::string>, 2> cohorts_;
    struct BreadthQuote {
        int64_t bar = 0, attempted = 0;
        following::Regime regime = following::Regime::Unknown;
        std::string error;
    };
    struct Coverage { size_t ready = 0, missing_live = 0, missing_observer = 0, unavailable = 0; std::string error; };
    std::array<Coverage, 2> coverage_;
    std::map<std::string, BreadthQuote> observer_quotes_;
    std::map<std::string, int64_t> missing_from_market_;
    std::set<std::string> unavailable_;
    uint64_t observation_epoch_ = 0;
    std::string breadth_signature_;
    following::Config breadth_model_;
    int breadth_channel_ = 0, breadth_atr_ = 0;
    // Positions already open when tracking began. Engine bot IDs are rebuilt on restart;
    // durable ownership is per symbol, with signed quantities in one managed net position.
    std::map<std::string, double> inherited_;
    std::array<bool, 2> inventory_ok_{true, true};
    std::map<std::string, std::array<bool, 2>> inventory_discrepancies_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, uint64_t> receipt_revision_;
    struct RecoveryCheck { int64_t last = 0, next = 0; };
    std::map<std::string, RecoveryCheck> recovery_checks_;
    std::map<std::string, std::string> recovery_wait_;
    std::map<std::string, Funding> funding_;
    Purchase purchase_;
    // In-process opening intents admitted while a transfer was unresolved.
    // Include any admissions during a balance read even if their fills arrived meanwhile.
    std::map<std::pair<std::string, int64_t>, double> submitting_margins_;
    double admitted_margin_ = 0;
    uint64_t execution_revision_ = 0;
    std::string last_saved_;
    std::vector<std::string> notices_;
};
}
