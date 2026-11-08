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
// No network in record/evaluate. Network settlement is serialized by service_mutex_.
// Engine execution callbacks run outside its mutex. Snapshot reads may take the
// engine lock under mutex_; engine mutations/close dispatch always happen outside.
class ProfitManager {
public:
    ProfitManager(std::string path, std::string account);
    void configure(profit::Config cfg, bool smart_enabled, int64_t now);
    bool record(ExecutionEvent& event);
    void evaluate(TrendEngine& engine, int64_t now);
    void service(TradingClient& client, TrendEngine& engine, int64_t now);
    std::string status() const;
    std::vector<std::string> take_notices();
    void cancel() { cancelled_.store(true); }
private:
    struct Entry {
        ExecutionEvent event;
        bool settled = false;
        double net = 0;
        int64_t first_fill = 0, last_fill = 0;
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
    void totals_locked(double& shorts, double& overall, bool& complete) const;
    void finish_purchase_locked(const TradingClient::SpotPurchase& result);
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
    // Positions already open when tracking began. Engine bot IDs are rebuilt on restart;
    // durable ownership is per symbol, with signed quantities in one managed net position.
    std::map<std::string, double> inherited_;
    std::array<bool, 2> inventory_ok_{true, true};
    std::map<std::string, Entry> entries_;
    std::map<std::string, Funding> funding_;
    Purchase purchase_;
    std::string last_saved_;
    std::vector<std::string> notices_;
};
}
