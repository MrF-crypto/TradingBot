// Included by test_profit.cpp. All account, fill and position data are fabricated.
enum class HistoryCase { Complete, ExchangeHolding, ReopenedDuringHistory, MissingFill, ForeignOpen, MissingFee, PositionError };

struct MissingCloseFixture {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("tradingbot-profit-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string checkpoint = (dir / "profit.json").string();
    const std::map<std::string, double> quantities = {
        {"ACTUSDT", 108898}, {"EVAAUSDT", 894.4}, {"MEGAUSDT", 20935}, {"QNTUSDT", 5.6},
        {"RAYSOLUSDT", 618.7}, {"TRXUSDT", 3158}, {"ZECUSDT", 1.017}
    };
    HistoryCase evidence = HistoryCase::Complete;
    std::string incomplete_symbol;
    bool later_cycle = false;
    bool hedge = false, mix_hedge_legs = false;
    std::function<void()> during_history;
    int64_t time = now;
    int unknown = 0, writes = 0, history_queries = 0, position_queries = 0;
    profit::Config cfg;
    std::shared_ptr<FakeClient> broker = std::make_shared<FakeClient>();
    EngineHost host;
    std::unique_ptr<TrendEngine> engine;
    std::unique_ptr<ProfitManager> manager;
    std::unique_ptr<TradingClient> exchange;

    std::string fill(const std::string& symbol, int id, int order, bool opening,
                     double qty, int64_t when, bool usdt) const {
        const int price = opening ? 100 : (order == 104 ? (symbol == "ZECUSDT" ? 103 : 98) : 99);
        std::ostringstream s;
        s << std::setprecision(14) << "{\"symbol\":" << profit::quote(symbol) << ",\"id\":" << id
          << ",\"orderId\":" << order << ",\"positionSide\":\"" << (hedge ? (mix_hedge_legs && id == 15 ? "LONG" : "SHORT") : "BOTH")
          << "\",\"side\":\"" << (opening ? "SELL" : "BUY")
          << "\",\"price\":\"" << price << "\",\"qty\":\"" << qty << "\",\"quoteQty\":\"" << price * qty
          << "\",\"time\":" << when << ",\"realizedPnl\":\"" << (opening ? 0 : qty * (100 - price))
          << "\",\"commission\":\"0.01\",\"commissionAsset\":\"" << (usdt ? "USDT" : "BNB") << "\"}";
        return s.str();
    }
    bool reply(const std::string& method, const std::string& endpoint, const std::string& params,
               TradingClient::FakeReply& result) {
        result.code = 200;
        if (method != "GET") ++writes;
        const auto symbol = param(params, "symbol");
        if (endpoint == "/fapi/v1/positionSide/dual") result.body = hedge ? R"({"dualSidePosition":true})" : R"({"dualSidePosition":false})";
        else if (endpoint == "/fapi/v1/income") result.body = "[]";
        else if (endpoint == "/fapi/v2/positionRisk" && quantities.count(symbol)) {
            ++position_queries;
            const auto pos = [&](const std::string& side, const std::string& amount) {
                return "{\"symbol\":" + profit::quote(symbol) + ",\"positionSide\":" + profit::quote(side) +
                    ",\"positionAmt\":" + profit::quote(amount) + "}";
            };
            const bool reopened = evidence == HistoryCase::ReopenedDuringHistory && history_queries > 0;
            const auto amount = evidence == HistoryCase::ExchangeHolding || reopened ? "-1" : "0";
            result.body = "[" + (hedge ? pos("LONG", "0") + "," + pos("SHORT", amount) : pos("BOTH", amount)) + "]";
            if (evidence == HistoryCase::PositionError) { result.code = 403; result.body = "{}"; }
        } else if (endpoint == "/fapi/v1/userTrades" && quantities.count(symbol)) {
            const double qty = quantities.at(symbol);
            const auto order = param(params, "orderId");
            const auto start = param(params, "startTime"), end = param(params, "endTime");
            if (order.empty()) ++history_queries;
            std::vector<std::string> rows;
            const auto add = [&](int id, int oid, bool opening, double amount, int64_t t, bool usdt = true) {
                if ((!order.empty() && std::stoi(order) != oid) || (!start.empty() && t < std::stoll(start)) ||
                    (!end.empty() && t > std::stoll(end))) return;
                rows.push_back(fill(symbol, id, oid, opening, amount, t, usdt));
            };
            add(10, 100, true, qty, now + 1000);
            if (evidence == HistoryCase::ForeignOpen) {
                add(11, 101, true, 1, now + 2000);
                add(12, 102, false, 1, now + 3000);
            }
            // Two partial fills in one order plus a second close order.
            add(13, 103, false, qty / 4, now + hour);
            add(14, 103, false, qty / 4, now + hour + 1);
            if (evidence != HistoryCase::MissingFill && symbol != incomplete_symbol)
                add(15, 104, false, qty / 2, now + 2 * hour, evidence != HistoryCase::MissingFee);
            if (later_cycle && symbol == "ACTUSDT") {
                add(20, 200, true, 7, now + 3 * hour);
                add(21, 201, false, 7, now + 4 * hour);
            }
            result.body = "[";
            for (const auto& row : rows) { if (result.body.size() > 1) result.body += ','; result.body += row; }
            result.body += ']';
            if (order.empty() && during_history) {
                auto callback = std::move(during_history);
                during_history = {};
                callback();
            }
        } else {
            ++unknown; result.code = 400; result.body = R"({"code":-9000,"msg":"unexpected offline endpoint"})";
        }
        return true;
    }
    explicit MissingCloseFixture(bool all, bool use_hedge = false, bool partial_booked = false) : hedge(use_hedge) {
        if (!std::filesystem::create_directory(dir)) throw std::runtime_error("cannot create isolated test directory");
        TradingClient::Config network;
        network.api_key = "offline"; network.api_secret = "offline"; network.testnet = false;
        exchange = std::make_unique<TradingClient>(network);
        exchange->set_test_hook([this](const auto& m, const auto& u, const auto& p, auto& out) { return reply(m,u,p,out); });
        exchange->set_wall_clock_for_test([this] { return time; });
        if (hedge) check(exchange->fetch_position_mode(), "mock exchange reports hedge mode");
        host.submit = [](auto f) { f(); };
        cfg.protect = true; cfg.accumulate = false;
        reload();
        for (const auto& [symbol, qty] : quantities) {
            if (!all && symbol != "ACTUSDT") continue;
            ExecutionEvent e;
            e.bot_id = "old-bot-" + symbol; e.symbol = symbol; e.is_auto = e.completed = e.opening = true;
            e.direction = -1; e.qty = qty; e.at_ms = now + 1000;
            e.result = {true, "100", "", 100, qty, false, ""};
            check(manager->record(e), "persist owned short with exchange receipt");
            if (symbol == "MEGAUSDT" || symbol == "EVAAUSDT") {
                auto rejected = e; rejected.key.clear(); rejected.opening = false; rejected.entry = 100;
                rejected.at_ms += hour; rejected.result = {};
                check(manager->record(rejected), "persist rejected close without crediting any execution");
            }
            if (partial_booked) {
                auto partial = e; partial.key.clear(); partial.opening = false; partial.entry = 100;
                partial.qty = qty / 2; partial.at_ms = now + hour + 1;
                partial.result = {true, "103", "", 99, partial.qty, false, ""};
                check(manager->record(partial), "persist already recorded partial close");
            }
            TrendBot b; b.is_auto = true; b.cfg.symbol = symbol; b.qty = partial_booked ? qty / 2 : qty;
            b.st.pos = trend::Pos::Short; b.st.entry_price = 100; b.st.stop = 110;
            engine->restore_bot(b);
        }
        time = now + hour + 5000;
        manager->service(*exchange, *engine, time); // Settle opening fees while positions exist.
        time = now + 9 * 24 * hour;
        reload(); // Exchange closures happened offline. Only the opening receipts survived locally.
        for (const auto& [symbol, qty] : quantities) {
            (void)qty;
            if (!all && symbol != "ACTUSDT") continue;
            TrendConfig c; c.symbol = symbol; c.use_disaster_stop = false;
            engine->add_bot(c, true);
        }
        engine->reconcile_positions({}, std::chrono::milliseconds(0));
        manager->evaluate(*engine, time);
    }
    void reload() {
        manager.reset();
        engine = std::make_unique<TrendEngine>(broker, host);
        manager = std::make_unique<ProfitManager>(checkpoint, exchange->account_fingerprint());
        manager->configure(cfg, true, time);
    }
    ~MissingCloseFixture() {
        manager.reset();
        std::error_code ec; std::filesystem::remove(checkpoint, ec); std::filesystem::remove(dir, ec);
    }
    void service() {
        time += hour;
        manager->evaluate(*engine, time);
        manager->service(*exchange, *engine, time);
        manager->evaluate(*engine, time);
    }
    bool history_pending() const {
        simdjson::dom::parser parser;
        const auto doc = parser.parse(simdjson::padded_string(read(checkpoint))).value();
        return doc["inventory_discrepancies"].get_array().value().size() > 0;
    }
    std::pair<size_t, double> settled_closes() const {
        simdjson::dom::parser parser;
        const auto root = parser.parse(simdjson::padded_string(read(checkpoint))).value();
        size_t count = 0; double net = 0;
        const auto entries = root["entries"].get_array().value();
        for (auto row : entries) {
            if (!profit::boolean(row, "opening") && profit::boolean(row, "ok") && profit::boolean(row, "settled")) {
                ++count; net += profit::number(row, "net");
            }
        }
        return {count, net};
    }
    void probe_short(const std::string& id, const std::string& symbol = "PROBEUSDT") {
        engine->update_signal(id, 2, 2, true, 110, 90, time / hour * hour);
        engine->tick(symbol, 100); // Automatic bots arm inside the channel before the breakout.
        engine->tick(symbol, 89);
    }
    // A later owned trade is anchored by its real order ID. History also
    // contains its close, which may or may not have reached the local ledger.
    void record_later_cycle(bool close_recorded) {
        later_cycle = true;
        ExecutionEvent e;
        e.bot_id = "new-bot-ACT"; e.symbol = "ACTUSDT"; e.is_auto = e.completed = e.opening = true;
        e.direction = -1; e.qty = 7; e.at_ms = now + 3 * hour;
        e.result = {true, "200", "", 100, 7, false, ""};
        check(manager->record(e), "later owned opening has an independent order identity");
        if (close_recorded) {
            e.key.clear(); e.opening = false; e.entry = 100; e.at_ms = now + 4 * hour;
            e.result = {true, "201", "", 99, 7, false, ""};
            check(manager->record(e), "existing later close receipt belongs to the later cycle");
        }
    }
};

void profit_recovery_tests() {
    {
        MissingCloseFixture f(true);
        TrendConfig probe; probe.symbol = "PROBEUSDT"; probe.use_disaster_stop = false;
        const auto id = f.engine->add_bot(probe, true);
        check(f.history_pending(), "restart detects the seven missing short close receipts");
        f.incomplete_symbol = "ZECUSDT";
        for (int i = 0; i < 7; ++i) f.service();
        f.engine->set_pause_new(true);
        f.probe_short(id);
        check(f.broker->opens == 0, "account pause still blocks a qualifying short while history is pending");
        f.engine->set_pause_new(false);
        f.probe_short(id);
        check(f.manager->recovered_trades().size() == 6 && f.history_pending() && f.broker->opens == 1,
              "an unrelated qualifying short enters while the seventh historical close is still missing");
        f.engine->close_bot(id);
        f.incomplete_symbol.clear();
        f.service();
        const auto [count, net] = f.settled_closes();
        double expected = 0;
        for (const auto& [symbol, quantity] : f.quantities)
            expected += quantity * (symbol == "ZECUSDT" ? -1 : 1.5) - .03;
        check(count == 14 && near(net, expected), "seven owned positions recover all split fills and their exchange commissions");
        check(!f.history_pending() && f.unknown == 0 && f.writes == 0, "read-only reconciliation clears the historical discrepancy");
        const auto recovered = f.manager->recovered_trades();
        std::set<std::string> symbols;
        bool correct = true;
        for (const auto& t : recovered) {
            const double close_price = t.symbol == "ZECUSDT" ? 101 : 98.5;
            correct = correct && symbols.insert(t.symbol).second && f.quantities.count(t.symbol) &&
                t.side == trend::Pos::Short && near(t.qty, f.quantities.at(t.symbol)) &&
                near(t.entry_price, 100) && near(t.exit_price, close_price) && near(t.pnl, (100 - close_price) * t.qty);
        }
        check(correct && symbols.size() == f.quantities.size(),
              "history exposes one complete weighted trade per original position, including the losing close");
        f.reload();
        f.service(); f.service();
        check(f.settled_closes() == std::make_pair(count, net) && f.manager->recovered_trades().size() == 7 && !f.history_pending(),
              "repeated service and restart do not duplicate recovered receipts or fees");
    }
    {
        MissingCloseFixture f(false, true);
        f.mix_hedge_legs = true;
        f.service();
        check(f.history_pending() && f.settled_closes().first == 0, "a LONG fill cannot complete a SHORT recovery");
        f.mix_hedge_legs = false;
        f.service();
        check(!f.history_pending() && f.settled_closes().first == 2 && f.writes == 0,
              "empty hedge legs and complete SHORT history can recover the missing short");
    }
    {
        MissingCloseFixture f(false, false, true);
        f.service();
        const auto history = f.manager->recovered_trades();
        check(!f.history_pending() && f.settled_closes().first == 2 && history.size() == 1 &&
              near(history[0].qty, f.quantities.at("ACTUSDT") / 2) && near(history[0].exit_price, 98),
              "already booked partial close retains its original receipt; history adds only the missing remainder");
    }
    for (const bool protect : {true, false}) {
        MissingCloseFixture f(false);
        f.cfg.protect = protect;
        f.manager->configure(f.cfg, true, f.time);
        f.manager->evaluate(*f.engine, f.time);
        const auto bots = f.engine->get_bots();
        const auto found = std::find_if(bots.begin(), bots.end(), [](const auto& b) { return b.cfg.symbol == "ACTUSDT"; });
        if (found == bots.end()) throw std::runtime_error("missing ACTUSDT fixture");
        const auto id = found->bot_id;
        f.during_history = [&] { f.engine->set_pending_for_test(id, true); };
        f.service();
        f.probe_short(id, "ACTUSDT");
        check(f.settled_closes().first == 0 && f.history_pending() && f.broker->opens == 0,
              "a pending order prevents same-symbol entry and invalidates the recovery snapshot");
        f.engine->set_pending_for_test(id, false);
        f.probe_short(id, "ACTUSDT");
        check(f.history_pending() && f.broker->opens == 1,
              "a verified flat symbol can enter with a historical gap regardless of the profit-protection switch");
        f.engine->close_bot(id);
        f.service();
        check(!f.history_pending() && f.settled_closes().first == 2, "fresh evidence after the pending order clears is accepted");
    }
    for (const bool later_close_recorded : {true, false}) {
        MissingCloseFixture f(false);
        f.record_later_cycle(later_close_recorded);
        f.reload();
        f.service();
        const auto history = f.manager->recovered_trades();
        const double qty = f.quantities.at("ACTUSDT");
        const auto old = std::find_if(history.begin(), history.end(), [qty](const auto& t) { return near(t.qty, qty); });
        const auto newer = std::find_if(history.begin(), history.end(), [](const auto& t) { return near(t.qty, 7); });
        check(!f.history_pending() && f.settled_closes().first == 3 && old != history.end() && near(old->exit_price, 98.5) &&
              near(old->pnl, qty * 1.5) && history.size() == (later_close_recorded ? 1U : 2U),
              "a later owned cycle cannot hide the older gap or take its fills");
        check(later_close_recorded ? newer == history.end() : (newer != history.end() && near(newer->pnl, 7)),
              "the later cycle retains its own close and is only backfilled when that close was missing");
        const auto balances = f.settled_closes();
        f.reload(); f.service();
        check(f.settled_closes() == balances && !f.history_pending(),
              "same-symbol recovered cycles stay reconciled after another restart");
    }
    for (const auto scenario : {HistoryCase::ExchangeHolding, HistoryCase::ReopenedDuringHistory, HistoryCase::MissingFill,
                                HistoryCase::ForeignOpen, HistoryCase::MissingFee, HistoryCase::PositionError}) {
        MissingCloseFixture f(false); f.evidence = scenario;
        f.service();
        check(f.position_queries > 0 && f.history_pending() && f.settled_closes().first == 0 && f.writes == 0,
              "non-flat, incomplete, mixed or unavailable evidence leaves the discrepancy open and all fills unbooked");
        if (scenario == HistoryCase::ReopenedDuringHistory)
            check(f.history_queries > 0 && f.position_queries >= 2,
                  "recheck exchange position after paging: an external re-entry invalidates the initial flat read");
        f.evidence = HistoryCase::Complete;
        for (int i = 0; i < 4 && f.history_pending(); ++i) f.service();
        check(!f.history_pending() && f.settled_closes().first == 2, "a later complete read can recover the same unresolved position");
    }
}
