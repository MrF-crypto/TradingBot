// Complete signed GETs with anonymized, offline exchange replies.
static void test_position_history_scenarios() {
    const std::string flat = R"({"symbol":"XUSDT","positionSide":"BOTH","positionAmt":"0"})";
    const std::string hedge = R"({"symbol":"XUSDT","positionSide":"LONG","positionAmt":"0"})";
    const std::string fill = R"({"symbol":"XUSDT","positionSide":"BOTH","side":"BUY","id":11,"orderId":12,"time":1000,"qty":"4","price":"2","quoteQty":"8","realizedPnl":"1.2","commission":"0.01","commissionAsset":"USDT"})";
    std::string full_page = "[";
    for (int i = 0; i < 1000; ++i) full_page += (i ? ",0" : "0");
    full_page += "]";
    for (auto mode : {TradingClient::AccountMode::Futures, TradingClient::AccountMode::PortfolioMargin}) {
        auto cfg = test_cfg(); cfg.account_mode = mode;
        TradingClient client(cfg);
        client.set_wall_clock_for_test([] { return int64_t{2000}; });
        const bool pm = mode == TradingClient::AccountMode::PortfolioMargin;
        std::string error, body = "[" + flat + "]";
        bool timeout = false;
        client.set_test_hook([&](const auto& method, const auto& path, const auto& params, auto& reply) {
            if (method == "GET" && path == (pm ? "/papi/v1/um/positionSide/dual" : "/fapi/v1/positionSide/dual")) {
                reply.body = R"({"dualSidePosition":true})";
                return true;
            }
            check(method == "GET" && params.find("symbol=XUSDT&") != std::string::npos &&
                  path == (pm ? "/papi/v1/um/positionRisk" : "/fapi/v2/positionRisk"), "signed read-only position confirmation");
            reply.body = body; reply.timed_out = timeout; return true;
        });
        check(client.profit_confirm_flat("XUSDT", error), "zero BOTH position confirms flat");
        for (const auto& malformed : std::vector<std::string>{
                 "[]", "[" + hedge + "]", "[" + flat + "," + flat + "]",
                 R"([{"symbol":"XUSDT","positionSide":"BOTH"}])",
                 R"([{"symbol":"ZUSDT","positionSide":"BOTH","positionAmt":"0"}])",
                 R"([{"symbol":"XUSDT","positionSide":"BOTH","positionAmt":"0.00000001"}])"}) {
            body = malformed;
            check(!client.profit_confirm_flat("XUSDT", error), "partial, malformed or nonflat snapshot cannot release a hold");
        }
        body = "[" + (pm ? flat + "," : "") + hedge + R"(,{"symbol":"XUSDT","positionSide":"SHORT","positionAmt":"0"}])";
        bool retry = true;
        check(!client.profit_confirm_flat("XUSDT", error, &retry) && !retry,
              "hedge positions cannot certify a one-way account as flat");
        bool ok = false;
        check(client.fetch_position_mode(&ok) && ok, "exchange mode read-back enables hedge-mode fixture");
        check(client.profit_confirm_flat("XUSDT", error), "both hedge legs explicitly flat");
        body = "[" + flat + "]";
        check(!client.profit_confirm_flat("XUSDT", error, &retry) && !retry,
              "one-way position cannot certify a hedge account as flat");
        timeout = true;
        check(!client.profit_confirm_flat("XUSDT", error, &retry) && retry,
              "transport failure with apparent zero position waits for a fresh read");

        int calls = 0;
        std::string last_page = "[]";
        client.set_test_hook([&](const auto& method, const auto& path, const auto& params, auto& reply) {
            ++calls;
            check(method == "GET" && path == (pm ? "/papi/v1/um/userTrades" : "/fapi/v1/userTrades") &&
                  params.find("fromId") == std::string::npos, "account trade windows use the correct signed read endpoint");
            if (params.find("startTime=1000&endTime=1001&") != std::string::npos) reply.body = full_page;
            else if (params.find("startTime=1000&endTime=1000&") != std::string::npos) reply.body = "[" + fill + "]";
            else if (params.find("startTime=1001&endTime=1001&") != std::string::npos) reply.body = last_page;
            else check(false, "unexpected history window");
            return true;
        });
        std::vector<TradingClient::HistoryFill> got;
        check(client.profit_history("XUSDT", 1000, 1001, got, error) && got.size() == 1 && calls == 3 &&
              got[0].id == "11" && got[0].quote_qty == 8 && got[0].pnl == 1.2 && got[0].fee == 0.01,
              "saturated result is re-read as disjoint complete windows with actual fill economics");
        for (const auto& incomplete : std::vector<std::string>{full_page, "[" + fill + "]", "[{}]"}) {
            last_page = incomplete; got.clear();
            check(!client.profit_history("XUSDT", 1000, 1001, got, error) && got.empty(),
                  "no partial evidence when the second window is saturated, misattributed or malformed");
        }
    }
}
