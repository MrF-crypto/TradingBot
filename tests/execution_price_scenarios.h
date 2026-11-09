// Only intercepted HTTP; these scenarios cannot reach an account.
static void test_execution_price_scenarios() {
    for (const auto mode : {TradingClient::AccountMode::Futures, TradingClient::AccountMode::PortfolioMargin}) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            auto cfg = test_cfg(); cfg.account_mode = mode;
            TradingClient client(cfg);
            int posts = 0, queries = 0;
            std::string tag;
            client.set_test_hook([&](const auto& method, const auto& path, const auto& params, auto& reply) {
                if (path.find("exchangeInfo") != std::string::npos) reply.body = kExchangeInfo;
                else if (method == "POST") {
                    ++posts; tag = coid_of(params);
                    const std::string average = scenario == 4 ? "NaN" : scenario == 5 ? "100garbage" : "0";
                    reply.body = "{\"orderId\":91,\"status\":\"FILLED\",\"executedQty\":\"2\",\"avgPrice\":\"" + average + "\"" +
                        (scenario == 0 ? ",\"cumQuote\":\"202.5\"}" : "}");
                } else if (path.find("/order") != std::string::npos) {
                    ++queries;
                    check(params.find("origClientOrderId=" + tag) != std::string::npos,
                          "missing execution price queries the original client order ID");
                    if (scenario == 2) { reply.code = 504; reply.body.clear(); }
                    else if (scenario == 3) reply.body = R"({"orderId":91,"status":"FILLED","executedQty":"2","avgPrice":"0"})";
                    else reply.body = R"({"orderId":91,"status":"FILLED","executedQty":"2","avgPrice":"101.25"})";
                } else { reply.code = 400; reply.body = R"({"code":-9000,"msg":"unexpected offline endpoint"})"; }
                return true;
            });
            const auto r = client.place_market("BTCUSDT", "BUY", 2);
            check(posts == 1, "missing/malformed average never resubmits an opening order");
            if (scenario == 2 || scenario == 3)
                check(!r.ok && r.uncertain && r.executed_qty == 2 && !r.client_order_id.empty(),
                      "unpriced terminal fill preserves known quantity and uncertainty");
            else
                check(r.ok && !r.uncertain && std::fabs(r.avg_price - 101.25) < 1e-9 && r.executed_qty == 2,
                      "actual turnover/query average replaces missing price, never the quote");
            check(queries == (scenario == 0 ? 0 : (scenario == 2 || scenario == 3 ? 3 : 1)),
                  "execution recovery is bounded and unnecessary queries are avoided");
        }
        auto cfg = test_cfg(); cfg.account_mode = mode;
        TradingClient client(cfg);
        client.set_test_hook([](const auto&, const auto&, const auto&, auto& reply) {
            reply.body = R"({"orderId":91,"status":"FILLED","avgPrice":"100"})"; return true;
        });
        const auto r = client.query_order("BTCUSDT", "offline");
        check(!r.ok && r.uncertain, "missing executed quantity cannot masquerade as confirmed zero execution");
    }
}
