static void test_audit_order_scenarios() {
    for (const auto mode : {TradingClient::AccountMode::Futures, TradingClient::AccountMode::PortfolioMargin}) {
        for (int code : {-1006, -1007, -1000}) {
          for (int status = 0; status < 3; ++status) {
            auto cfg = test_cfg(); cfg.account_mode = mode;
            TradingClient client(cfg); int posts = 0, queries = 0;
            client.set_test_hook([&](const std::string& method, const std::string& path,
                                    const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) out.body = kExchangeInfo;
                else if (method == "POST") {
                    ++posts; out.code = 503;
                    out.body = "{\"code\":"+std::to_string(code)+",\"msg\":\"Unknown error, please check your request or try again later.\"}";
                } else if (path.find("/order") != std::string::npos) {
                    ++queries;
                    if (status == 2) out.body = "";
                    else out.body = "{\"orderId\":42,\"status\":\"" +
                        std::string(status == 0 ? "FILLED" : "PARTIALLY_FILLED") +
                        "\",\"avgPrice\":\"100\",\"executedQty\":\"0.015\"}";
                } else out.body = R"({"code":-1100,"msg":"Unexpected mocked endpoint"})";
                return true;
            });
            const auto result = client.place_market("BTCUSDT","BUY",.015,false);
            if (status == 0)
                check(result.ok && !result.uncertain && posts == 1 && queries == 1 && result.executed_qty == .015,
                      "JSON unknown execution is queried by client ID, never resubmitted as failed");
            else
                check(!result.ok && result.uncertain && posts == 1 && queries == (status == 1 ? 1 : 3) &&
                      !result.client_order_id.empty(),
                      "nonterminal or failed query retains uncertainty and original order ID without reposting");
          }
        }
        for (int scenario = 0; scenario < 6; ++scenario) {
            auto cfg = test_cfg(); cfg.account_mode = mode; TradingClient client(cfg);
            int algo_queries = 0, order_queries = 0;
            client.set_test_hook([&](const std::string&, const std::string& path,
                                    const std::string& params, TradingClient::FakeReply& out) {
                if (path.find("positionRisk") != std::string::npos) {
                    if (scenario == 0) out.body = "";
                    else if (scenario == 1) out.body = R"([{"symbol":"BTCUSDT","positionAmt":"1"}])";
                    else if (scenario == 5) out.body = R"([{"symbol":"BTCUSDT","positionAmt":"NaN"}])";
                    else out.body = R"([{"symbol":"BTCUSDT","positionAmt":"-2"}])"; // opposite hedge leg
                } else if (path.find("algo") != std::string::npos) {
                    ++algo_queries;
                    check(path == (mode == TradingClient::AccountMode::Futures ? "/fapi/v1/algoOrder" : "/papi/v1/um/algo/algoOrder"),
                          "protective execution lookup uses account-specific query endpoint");
                    out.body = "{\"algoId\":77,\"symbol\":\"BTCUSDT\",\"side\":\"SELL\",\"algoStatus\":\"" +
                        std::string(scenario == 2 ? "CANCELED" : "FINISHED") + "\",\"actualOrderId\":\"88\"}";
                } else if (path.find("/order") != std::string::npos) {
                    ++order_queries;
                    check(params.find("orderId=88") != std::string::npos,"look up actual order ID from protective order");
                    out.body = "{\"orderId\":88,\"symbol\":\"BTCUSDT\",\"side\":\"SELL\",\"positionSide\":\"LONG\",\"status\":\"" +
                        std::string(scenario == 3 ? "PARTIALLY_FILLED" : "FILLED") + "\",\"avgPrice\":\"90\",\"executedQty\":\"1\"}";
                } else out.body = "{}";
                return true;
            });
            const auto result = client.verify_close("BTCUSDT","BUY","77");
            check(result.position_known == (scenario != 0 && scenario != 5),"failed or malformed position data cannot prove flat");
            check(result.protective_fill.ok == (scenario == 4),"only a verified full execution proves a protective stop filled");
            if (scenario == 1) check(result.remaining_qty == 1 && algo_queries == 0,"still held: do not infer close from stop state");
            if (scenario == 2) check(order_queries == 0,"canceled protective order is not a filled stop");
        }
    }
}
