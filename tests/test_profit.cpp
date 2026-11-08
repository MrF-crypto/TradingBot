#include "app/profit_manager.h"
#include "core/profit_json.h"
#include "core/smart_mode.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace ccbot;
namespace {
int failures = 0;
void check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name); if (!ok) ++failures; }
bool near(double a, double b) { return std::fabs(a - b) < 1e-7; }
constexpr int64_t hour = 3600000, now = 1800000000000LL;
std::string read(const std::string& path) { std::ifstream f(path); return {(std::istreambuf_iterator<char>(f)), {}}; }
std::string param(const std::string& query, const std::string& key) {
    const auto pos = query.find(key + "="); if (pos == std::string::npos) return {};
    auto begin = pos + key.size() + 1, end = query.find('&', begin); return query.substr(begin, end - begin);
}
struct FakeClient : ITradingClient {
    int closes = 0, opens = 0;
    OrderOutcome place_market_order(const std::string&, const std::string&, double qty, bool reduce) override {
        if (reduce) ++closes; else ++opens;
        OrderOutcome r; r.ok = true; r.order_id = "1"; r.executed_qty = qty; r.avg_price = 100; return r;
    }
    double round_qty(const std::string&, double qty) override { return qty; }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }
};
struct Fixture {
    std::string path;
    std::shared_ptr<TradingClient> client;
    std::shared_ptr<FakeClient> fake = std::make_shared<FakeClient>();
    EngineHost host;
    std::shared_ptr<TrendEngine> engine;
    std::shared_ptr<ProfitManager> manager;
    profit::Config cfg;
    double equity = 1000, transfer_amount = 0, order_qty = 0, order_cost = 0;
    int transfers = 0, purchases = 0, unknown = 0;
    int64_t transfer_time = 0;
    bool unrelated_transfer = false;
    bool lost_transfer = false, lost_purchase = false, non_usdt_fee = false, foreign_fill = false, half_fill = false;
    std::string order_tag;
    explicit Fixture(const std::string& name) {
        path = "profit-test-" + name + ".json"; std::filesystem::remove(path);
        TradingClient::Config config; config.api_key = "offline-test-key"; config.api_secret = "offline-test-secret"; config.testnet = false;
        client = std::make_shared<TradingClient>(config);
        client->set_test_hook([this](const auto& method, const auto& endpoint, const auto& params, auto& reply) {
            reply.code = 200;
            if (endpoint == "/fapi/v1/multiAssetsMargin") reply.body = R"({"multiAssetsMargin":false})";
            else if (endpoint == "/fapi/v2/account" || endpoint == "/fapi/v3/account") reply.body = "{\"totalMarginBalance\":\"" + std::to_string(equity) + "\"}";
            else if (endpoint == "/fapi/v3/balance") reply.body = "[{\"asset\":\"USDT\",\"balance\":\"" + std::to_string(equity) + "\",\"crossUnPnl\":\"0\",\"availableBalance\":\"900\",\"maxWithdrawAmount\":\"900\"}]";
            else if (endpoint == "/fapi/v3/positionRisk" || endpoint == "/fapi/v2/positionRisk") reply.body = "[]";
            else if (endpoint == "/fapi/v1/order") { reply.code = 400; reply.body = R"({"code":-2013,"msg":"Order does not exist"})"; }
            else if (endpoint == "/fapi/v1/userTrades") {
                const auto id = param(params, "orderId"); const bool open = id == "11";
                if (id.empty()) {
                    reply.body = foreign_fill ? "[{\"symbol\":\"ETHUSDT\",\"orderId\":999,\"time\":" + std::to_string(now + 1500) + "}]" :
                        "[{\"symbol\":\"ETHUSDT\",\"orderId\":11,\"time\":" + std::to_string(now + 1000) + "},{\"symbol\":\"ETHUSDT\",\"orderId\":12,\"time\":" + std::to_string(now + 2000) + "}]";
                    return true;
                }
                reply.body = "[{\"symbol\":\"ETHUSDT\",\"id\":" + std::string(open ? "101" : "102") + ",\"orderId\":" + id +
                    ",\"time\":" + std::to_string(open ? now + 1000 : now + 2000) + ",\"qty\":\"10\",\"realizedPnl\":\"" + (open ? "0" : "100") +
                    "\",\"commission\":\"0.5\",\"commissionAsset\":\"" + (non_usdt_fee ? "BNB" : "USDT") + "\",\"side\":\"" + (open ? "SELL" : "BUY") + "\"}]";
            } else if (endpoint == "/fapi/v1/income") reply.body = "[]";
            else if (endpoint == "/sapi/v1/asset/transfer" && method == "POST") {
                ++transfers; transfer_amount = std::stod(param(params, "amount"));
                transfer_time = client->market_time_ms();
                reply.body = lost_transfer ? "" : R"({"tranId":44})"; if (lost_transfer) reply.code = 504;
            } else if (endpoint == "/sapi/v1/asset/transfer") reply.body = "{\"total\":1,\"rows\":[{\"asset\":\"USDT\",\"amount\":\"" + std::to_string(transfer_amount) + "\",\"tranId\":44,\"status\":\"CONFIRMED\",\"type\":\"UMFUTURE_MAIN\",\"timestamp\":" + std::to_string(transfer_time + (unrelated_transfer ? 86400000 : 0)) + "}]}";
            else if (endpoint == "/api/v3/ticker/bookTicker?symbol=BTCUSDT") reply.body = R"({"symbol":"BTCUSDT","askPrice":"50000"})";
            else if (endpoint == "/api/v3/exchangeInfo?symbol=BTCUSDT") reply.body = R"({"symbols":[{"symbol":"BTCUSDT","status":"TRADING","filters":[{"filterType":"PRICE_FILTER","tickSize":"0.01"},{"filterType":"LOT_SIZE","stepSize":"0.00001","minQty":"0.00001"},{"filterType":"NOTIONAL","minNotional":"5"}]}]})";
            else if (endpoint == "/api/v3/order" && method == "POST") {
                ++purchases; order_tag = param(params, "newClientOrderId"); order_qty = std::stod(param(params, "quantity")) * (half_fill ? .5 : 1); order_cost = order_qty * 50000;
                reply.body = lost_purchase ? "" : R"({"orderId":55})"; if (lost_purchase) reply.code = 504;
            } else if (endpoint == "/api/v3/order") {
                reply.body = "{\"symbol\":\"BTCUSDT\",\"side\":\"BUY\",\"clientOrderId\":\"" + order_tag + "\",\"orderId\":55,\"status\":\"FILLED\",\"executedQty\":\"" + std::to_string(order_qty) + "\",\"cummulativeQuoteQty\":\"" + std::to_string(order_cost) + "\"}";
            } else if (endpoint == "/api/v3/myTrades") reply.body = "[{\"symbol\":\"BTCUSDT\",\"isBuyer\":true,\"orderId\":55,\"id\":77,\"qty\":\"" + std::to_string(order_qty) + "\",\"quoteQty\":\"" + std::to_string(order_cost) + "\",\"commission\":\"0.0000001\",\"commissionAsset\":\"BTC\"}]";
            else { ++unknown; reply.code = 400; reply.body = R"({"code":-9000,"msg":"unhandled offline fixture"})"; }
            return true; // Never fall through to live networking.
        });
        host.submit = [](auto fn) { fn(); };
        engine = std::make_shared<TrendEngine>(fake, host);
        cfg.accumulate = true; cfg.wait_btc_up = false; cfg.batch_usdt = 5;
        manager = std::make_shared<ProfitManager>(path, client->account_fingerprint());
        manager->configure(cfg, true, now);
        manager->service(*client, *engine, now); // Capture the initial capital reserve before any gain.
    }
    ~Fixture() { manager.reset(); std::filesystem::remove(path); }
    void closed_short(bool late_receipt = false) {
        ExecutionEvent open; open.bot_id = "bot"; open.symbol = "ETHUSDT"; open.is_auto = true; open.direction = -1;
        open.opening = true; open.qty = 10; open.at_ms = now + 1000;
        check(manager->record(open), "opening intent persisted before submission");
        if (late_receipt) manager->service(*client, *engine, now + 1500);
        open.completed = true; open.result.ok = true; open.result.order_id = "11"; open.result.executed_qty = 10; open.result.avg_price = 100;
        manager->record(open);
        auto close = open; close.key.clear(); close.opening = false; close.entry = 100; close.at_ms = now + 2000;
        close.bot_id = "restored-bot"; // The engine assigns fresh IDs after a process restart.
        close.result.order_id = "12"; close.result.avg_price = 90; manager->record(close);
        equity = 1100;
    }
};

void policy_tests() {
    profit::Config c; c.protect = true;
    check(profit::valid(c), "default policy valid and accumulation disabled");
    check(near(profit::entitlement(100, 100, 0, 0, .3), 30), "profit allocation uses cumulative budget");
    check(profit::entitlement(40, 40, 30, 0, .3) == 0, "loss after allocation blocks another purchase");
    check(near(profit::entitlement(140, 140, 30, 0, .3), 12), "only newly recovered profit is allocatable");
    check(profit::entitlement(100, -50, 0, 0, .3) == 0, "overall strategy losses block short harvesting");
    check(near(profit::entitlement(100, 100, 0, 20, .3), 10), "in-flight reservation cannot be spent twice");
    profit::Guard g; g.active = true;
    g.update(c, hour, 200, true, true, hour, .8, true);
    g.update(c, 2 * hour, 180, true, true, 2 * hour, .4, true);
    for (int n = 0; n < 20; ++n) g.update(c, 2 * hour + n, 180, true, true, 2 * hour, .4, true);
    check(g.weak == 1 && g.phase == profit::Phase::Watching, "same candle cannot accumulate weakness confirmations");
    g.update(c, 3 * hour, 170, true, true, 3 * hour, .4, true);
    check(g.phase == profit::Phase::Caution, "weak breadth freezes additions before soft exit");
    g.update(c, 3 * hour + 100, 150, true, true, 3 * hour, .4, true);
    check(g.phase == profit::Phase::Exiting, "soft drawdown exits while still fully exposed");
    g.update(c, 3 * hour + 200, 150, true, false, 3 * hour, .8, true);
    check(g.phase == profit::Phase::Waiting, "exit goes into recovery wait");
    g.update(c, 4 * hour, 150, true, false, 4 * hour, .7, true);
    check(g.phase == profit::Phase::Waiting, "one new recovery bar cannot reopen");
    g.update(c, 5 * hour, 150, true, false, 5 * hour, .7, true);
    check(g.phase == profit::Phase::Watching && g.fresh_after == 5 * hour, "two recovery bars require a later fresh breakout");
    profit::Guard h; h.active = true; h.update(c, hour, 200, true, true, hour, .8, true);
    h.update(c, 2 * hour, 0, false, true, 2 * hour, 0, false);
    check(h.phase == profit::Phase::Watching && near(h.peak, 200), "stale prices cannot fabricate drawdown");
    h.update(c, 2 * hour + 1, 120, true, true, 2 * hour, 0, false);
    check(h.phase == profit::Phase::Exiting, "hard drawdown works even without breadth data");
    profit::Guard missing; missing.active = true; missing.update(c, hour, 200, true, true, hour, .5, true);
    missing.update(c, 2 * hour, 190, true, true, 2 * hour, .4, true, .5);
    check(missing.weak == 0, "missing supporters cannot be counted as weakness");
    profit::Guard natural; natural.active = true;
    natural.update(c, hour, 200, true, true, hour, .8, true);
    natural.update(c, 2 * hour, 195, true, true, 2 * hour, .4, true);
    natural.update(c, 3 * hour, 190, true, true, 3 * hour, .4, true);
    natural.update(c, 3 * hour + 100, 190, true, false, 3 * hour, .4, true);
    check(natural.phase == profit::Phase::Waiting && natural.active, "individual exits during caution continue into recoverable waiting state");
    natural.update(c, 4 * hour, 190, true, false, 4 * hour, .7, true);
    natural.update(c, 5 * hour, 190, true, false, 5 * hour, .7, true);
    check(natural.phase == profit::Phase::Watching, "natural flat exit is not permanently blocked");
    profit::Guard flat; flat.active = true;
    flat.update(c, hour, 200, true, true, hour, .8, true);
    flat.update(c, hour + 100, 200, true, false, hour, .8, true);
    check(!flat.active && flat.breadth_peak >= flat.breadth, "normal flat reset preserves valid checkpoint invariants");
}
void integration_tests() {
    {
        smart::Config input, output; input.profit.protect = input.profit.accumulate = true; input.profit.allocation = .42;
        std::string error;
        check(smart::config_from_json(smart::config_to_json(input), output, &error) && output.profit.protect && output.profit.accumulate && near(output.profit.allocation, .42), "profit options round-trip through shared GUI/headless config");
        check(!smart::config_from_json(R"({"profit":{"protect":"true"}})", output, &error), "invalid safety switch type is rejected");
        check(!smart::config_from_json(R"({"profit":{"soft_drawdown":0.4,"hard_drawdown":0.2}})", output, &error), "inverted drawdown thresholds are rejected");
        check(!smart::config_from_json(R"({"profit":{"protect":true},"template":{"interval":"15m"}})", output, &error), "incompatible breadth timeframe is rejected");
    }
    {
        Fixture f("success"); f.closed_short();
        f.manager->service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 1 && near(f.transfer_amount, 29.7), "fees and complete cycles survive bot-ID reassignment before profit transfer");
        f.manager->service(*f.client, *f.engine, now + 430000);
        check(f.purchases == 1, "eligible transferred profit buys capped BTC IOC");
        check(f.manager->status().find("0.00057990") != std::string::npos, "BTC holdings deduct actual BTC commission");
        f.manager.reset(); f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
        f.manager->configure(f.cfg, true, now + 450000);
        f.manager->service(*f.client, *f.engine, now + 460000);
        check(f.transfers == 1 && f.purchases == 1, "restart preserves used budget and avoids duplicate purchases");
        check(f.unknown == 0, "end-to-end uses only expected offline endpoints");
    }
    {
        Fixture f("late-receipt"); f.closed_short(true);
        f.manager->service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 1 && near(f.transfer_amount, 29.7),
              "late execution receipt invalidates an early not-found settlement and recomputes actual fees");
    }
    {
        Fixture f("transfer-timeout"); f.lost_transfer = true; f.closed_short();
        f.manager->service(*f.client, *f.engine, now + 400000);
        f.manager.reset(); f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint()); f.manager->configure(f.cfg, true, now + 410000);
        f.manager->service(*f.client, *f.engine, now + 430000);
        f.manager->service(*f.client, *f.engine, now + 460000);
        check(f.transfers == 1 && f.purchases == 1, "transfer timeout resumes by history query without another transfer");
    }
    {
        Fixture f("unrelated-transfer"); f.lost_transfer = true; f.unrelated_transfer = true; f.closed_short();
        f.manager->service(*f.client, *f.engine, now + 400000);
        f.manager->service(*f.client, *f.engine, now + 430000);
        f.manager->service(*f.client, *f.engine, now + 460000);
        check(f.transfers == 1 && f.purchases == 0, "later same-amount transfer cannot settle an earlier unknown request");
    }
    {
        Fixture f("order-timeout"); f.lost_purchase = true; f.closed_short();
        f.manager->service(*f.client, *f.engine, now + 400000); f.manager->service(*f.client, *f.engine, now + 430000);
        f.cfg.accumulate = false; f.manager->configure(f.cfg, true, now + 440000);
        f.manager->service(*f.client, *f.engine, now + 460000);
        check(f.purchases == 1 && f.manager->status().find("0.00057990") != std::string::npos, "disable still reconciles an uncertain filled order without resubmission");
    }
    {
        Fixture f("fees"); f.non_usdt_fee = true; f.closed_short(); f.manager->service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 0 && f.purchases == 0, "unknown fee currency blocks spending estimated profit");
    }
    {
        Fixture f("foreign"); f.foreign_fill = true; f.closed_short(); f.manager->service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 0, "manual fills in an owned position cycle block profit attribution");
    }
    {
        Fixture f("partial"); f.half_fill = true; f.closed_short();
        f.manager->service(*f.client, *f.engine, now + 400000); f.manager->service(*f.client, *f.engine, now + 430000);
        simdjson::dom::parser p; const auto state = p.parse(simdjson::padded_string(read(f.path))).value();
        check(near(profit::number(state, "used"), 14.5) && near(profit::number(state, "spot_cash"), 15.2), "partial spot fills spend only actual cost and retain unused transferred cash");
    }
    {
        Fixture f("floor"); f.closed_short(); f.equity = 990; f.manager->service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 0, "floating/account losses below retained capital block profit transfer");
    }
    {
        Fixture f("guard-restart"); f.cfg.accumulate = false; f.cfg.protect = true; f.cfg.arm_usdt = 1; f.cfg.soft_drawdown = .1; f.cfg.hard_drawdown = .2;
        f.manager->configure(f.cfg, true, now);
        TrendBot bot; bot.bot_id = "guard"; bot.cfg.symbol = "ETHUSDT"; bot.is_auto = true;
        bot.st.pos = trend::Pos::Short; bot.st.entry_price = 110; bot.st.stop = 120; bot.qty = 1;
        f.engine->restore_bot(bot); f.engine->reconcile_positions({{"ETHUSDT", -1, 1, 110}}, std::chrono::milliseconds(0));
        f.engine->tick("ETHUSDT", 100); f.manager->evaluate(*f.engine, now + 1000);
        const auto before = read(f.path);
        f.manager.reset(); f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
        f.manager->configure(f.cfg, true, now + 2000);
        check(read(f.path) == before, "restart/configure does not reset protection peak or progress");
        f.engine->tick("ETHUSDT", 105); f.manager->evaluate(*f.engine, now + 3000);
        check(f.fake->closes == 1, "persisted peak still triggers exit after restart without waiting for breadth");
    }
    {
        Fixture f("external-adjustment"); f.cfg.accumulate = false; f.cfg.protect = true; f.cfg.arm_usdt = 1;
        f.manager->configure(f.cfg, true, now);
        TrendBot bot; bot.cfg.symbol = "ETHUSDT"; bot.is_auto = true;
        bot.st.pos = trend::Pos::Short; bot.st.entry_price = 110; bot.st.stop = 120; bot.qty = 1;
        f.engine->restore_bot(bot); bot.cfg.symbol = "SOLUSDT"; f.engine->restore_bot(bot);
        f.engine->reconcile_positions({{"ETHUSDT", -1, 1, 110}, {"SOLUSDT", -1, 1, 110}}, std::chrono::milliseconds(0));
        f.engine->tick("ETHUSDT", 100); f.engine->tick("SOLUSDT", 100);
        f.manager->evaluate(*f.engine, now + 1000);
        // The exchange confirms a manual close; no attributable receipt exists in this ledger.
        f.engine->reconcile_positions({{"SOLUSDT", -1, 1, 110}}, std::chrono::milliseconds(0));
        f.engine->reconcile_positions({{"SOLUSDT", -1, 1, 110}}, std::chrono::milliseconds(0));
        f.manager->evaluate(*f.engine, now + 2000);
        check(f.fake->closes == 0 && f.manager->status().find("缺少匹配成交") != std::string::npos,
              "external close cannot fabricate drawdown and liquidate the remaining position");
        f.manager.reset(); f.manager = std::make_shared<ProfitManager>(f.path, f.client->account_fingerprint());
        f.manager->configure(f.cfg, true, now + 3000); f.manager->evaluate(*f.engine, now + 3000);
        check(f.fake->closes == 0 && f.manager->status().find("缺少匹配成交") != std::string::npos,
              "restart retains inherited inventory and unresolved external changes");
    }
    {
        Fixture f("invalid-receipt"); const auto before = read(f.path);
        ExecutionEvent e; e.is_auto = true; e.bot_id = "bot"; e.symbol = "ETHUSDT";
        e.direction = -1; e.at_ms = now + 1; e.qty = 1; e.completed = true;
        e.result.avg_price = std::nan("");
        check(!f.manager->record(e) && read(f.path) == before, "non-finite receipt cannot corrupt a recoverable checkpoint");
    }
    {
        Fixture f("binding"); f.closed_short();
        ProfitManager other(f.path, "different-account"); other.configure(f.cfg, true, now + 1); other.service(*f.client, *f.engine, now + 400000);
        check(f.transfers == 0 && other.status().find("不匹配") != std::string::npos, "different account cannot spend another account ledger");
        const auto original = read(f.path); check(original.find("different-account") == std::string::npos, "account mismatch preserves original checkpoint");
    }
    {
        Fixture f("corrupt"); { std::ofstream s(f.path); s << "{broken"; }
        ProfitManager damaged(f.path, f.client->account_fingerprint()); damaged.configure(f.cfg, true, now + 1); damaged.evaluate(*f.engine, now + 1);
        check(read(f.path) == "{broken", "damaged checkpoint is not silently replaced");
    }
    {
        Fixture f("corrupt-exit");
        auto json = read(f.path); const auto at = json.find("\"guards\":[{\"phase\":0");
        json.replace(at, std::string("\"guards\":[{\"phase\":0").size(), "\"guards\":[{\"phase\":2");
        { std::ofstream file(f.path); file << json; }
        TrendBot bot; bot.bot_id = "held"; bot.cfg.symbol = "ETHUSDT"; bot.is_auto = true;
        bot.st.pos = trend::Pos::Long; bot.st.entry_price = 100; bot.st.stop = 90; bot.qty = 1;
        f.engine->restore_bot(bot); f.engine->reconcile_positions({{"ETHUSDT", 1, 1, 100}}, std::chrono::milliseconds(0));
        ProfitManager damaged(f.path, f.client->account_fingerprint()); f.cfg.protect = true;
        damaged.configure(f.cfg, true, now + 1); damaged.evaluate(*f.engine, now + 1);
        check(f.fake->closes == 0 && read(f.path) == json, "partially parsed corrupt exit state cannot liquidate holdings or overwrite source");
    }
    {
        auto fake = std::make_shared<FakeClient>(); EngineHost host; host.submit = [](auto fn) { fn(); };
        TrendEngine engine(fake, host);
        TrendBot auto_bot; auto_bot.bot_id = "auto"; auto_bot.cfg.symbol = "ETHUSDT"; auto_bot.is_auto = true;
        auto_bot.st.pos = trend::Pos::Short; auto_bot.st.entry_price = 110; auto_bot.st.stop = 120; auto_bot.qty = 1;
        engine.restore_bot(auto_bot);
        auto manual = auto_bot; manual.bot_id = "manual"; manual.cfg.symbol = "SOLUSDT"; manual.is_auto = false; engine.restore_bot(manual);
        const auto ids = engine.get_bots();
        std::vector<TrendEngine::ExchangePos> positions; for (const auto& b : ids) positions.push_back({b.cfg.symbol, -1, 1, 110});
        engine.reconcile_positions(positions, std::chrono::milliseconds(0));
        engine.close_auto_direction(trend::Pos::Short, "profit protection");
        check(fake->closes == 1, "direction protection only closes owned automatic positions");
    }
}
}
int main() {
    try { policy_tests(); integration_tests(); }
    catch (const std::exception& e) { std::printf("EXCEPTION %s\n", e.what()); ++failures; }
    return failures ? 1 : 0;
}
