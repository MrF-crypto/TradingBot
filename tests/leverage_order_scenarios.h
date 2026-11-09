// All responses are intercepted, including unexpected paths: never reach an account.
static void test_leverage_order_scenarios() {
    for (int error : {-1000, -1001, -1006, -1007}) {
        TradingClient client(test_cfg()); int posts = 0, gets = 0;
        client.set_test_hook([&](const auto& method, const auto&, const auto&, auto& reply) {
            if (method == "POST") { ++posts; reply.code = 503; reply.body = "{\"code\":" + std::to_string(error) + ",\"msg\":\"unknown result\"}"; }
            else { ++gets; reply.body = R"([{"symbol":"ACTUSDT","leverage":"2"}])"; }
            return true;
        });
        check(client.set_leverage("ACTUSDT", 2) && posts == 1 && gets == 1,
              "ambiguous JSON errors read back leverage before retrying a setting");
    }
    for (auto mode : {TradingClient::AccountMode::Futures, TradingClient::AccountMode::PortfolioMargin}) {
        for (const std::string& sym : {std::string("ACTUSDT"), std::string("龙虾USDT")}) {
            for (int scenario = 0; scenario < 14; ++scenario) {
                auto cfg = test_cfg(); cfg.account_mode = mode;
                TradingClient client(cfg);
                int posts = 0, gets = 0;
                std::string sequence;
                const std::string target = "{\"symbol\":\"" + sym + "\",\"leverage\":2}";
                const std::string row = "{\"symbol\":\"" + sym + "\",\"positionAmt\":\"0\",\"leverage\":\"2\"}";
                client.set_test_hook([&](const auto& method, const auto& path, const auto& params, auto& reply) {
                    const std::string expected_symbol = sym == "ACTUSDT" ? sym : "%E9%BE%99%E8%99%BEUSDT";
                    check(params.find("symbol=" + expected_symbol + "&") != std::string::npos &&
                          params.find("signature=") != std::string::npos, "leverage recovery signs/encodes the exact symbol");
                    if (method == "POST" && path.find("/leverage") != std::string::npos) {
                        ++posts; sequence += 'P';
                        check(path == (mode == TradingClient::AccountMode::Futures ? "/fapi/v1/leverage" : "/papi/v1/um/leverage"),
                              "leverage setting uses the account-specific endpoint");
                        reply.code = 0; reply.timed_out = true;
                        if (scenario == 1 || ((scenario == 2 || scenario == 13) && posts == 2)) {
                            reply.code = 200; reply.timed_out = false; reply.body = target;
                        }
                        if (scenario == 4) {
                            reply.code = 400; reply.timed_out = false;
                            reply.body = R"({"code":-4028,"msg":"invalid leverage"})";
                        }
                        if (scenario == 5) { reply.code = 200; reply.timed_out = false; reply.body = R"({"symbol":"OTHERUSDT","leverage":2})"; }
                        if (scenario == 7) { reply.code = 503; reply.timed_out = false; reply.body = R"({"code":-1007,"msg":"Timeout: execution status unknown"})"; }
                        if (scenario == 8) { reply.code = 502; reply.timed_out = false; reply.body = "gateway unavailable"; }
                        if (scenario == 13 && posts == 1) { reply.code = 200; reply.body = target; } // timeout with partial/old body
                    } else if (method == "GET" && path.find("/positionRisk") != std::string::npos) {
                        ++gets; sequence += 'Q';
                        check(path == (mode == TradingClient::AccountMode::Futures ? "/fapi/v2/positionRisk" : "/papi/v1/um/positionRisk"),
                              "leverage query uses an endpoint with leverage, including flat positions");
                        reply.body = "[" + row + "]";
                        if (scenario == 0) reply.body = "[" + row + "," + row + "]"; // agreeing hedge-side rows
                        if (scenario == 2 || scenario == 13) reply.body = "[{\"symbol\":\"" + sym + "\",\"leverage\":3}]";
                        if (scenario == 3 || (scenario == 12 && gets == 1)) { reply.code = 0; reply.timed_out = true; reply.body.clear(); }
                        if (scenario == 6) { reply.code = 403; reply.body = "forbidden"; }
                        if (scenario == 9) reply.body = "[" + row + ",{\"symbol\":\"" + sym + "\",\"leverage\":3}]";
                        if (scenario == 10) reply.body = "[]";
                        if (scenario == 11) { reply.timed_out = true; } // even a matching body cannot prove a timed-out query
                    } else {
                        check(false, "unexpected leverage recovery endpoint"); reply.code = 400; reply.body = "offline";
                    }
                    return true;
                });
                const bool expected = scenario == 0 || scenario == 1 || scenario == 2 || scenario == 7 ||
                                      scenario == 8 || scenario == 12 || scenario == 13;
                check(client.set_leverage(sym, 2) == expected, "leverage timeout recovery scenario " + std::to_string(scenario));
                const int want_posts = (scenario == 2 || scenario == 3 || scenario >= 9) ? 2 : 1;
                const int want_gets = (scenario == 1 || scenario == 4 || scenario == 5) ? 0 :
                                     (scenario == 3 || (scenario >= 9 && scenario <= 12)) ? 2 : 1;
                check(posts == want_posts && gets == want_gets && sequence.find("PP") == std::string::npos,
                      "query precedes every retry; at most two settings and two queries");
                if (expected) check(client.last_error().empty(), "successful confirmation clears stale timeout errors");
                if (scenario == 3) check(client.last_error().find("有限重试已用尽") != std::string::npos,
                                          "exhausted recovery retains a useful diagnostic");
                if (scenario == 4) check(client.last_error().find("-4028") != std::string::npos,
                                          "explicit leverage rejection preserves the exchange code");
            }
        }
    }
    for (const std::string bad : {"[]", "{}", "broken", "[{\"symbol\":\"OTHERUSDT\",\"leverage\":2}]",
          "[{\"symbol\":\"ACTUSDT\"}]", "[{\"symbol\":\"ACTUSDT\",\"leverage\":\"2x\"}]",
          "[{\"symbol\":\"ACTUSDT\",\"leverage\":2.1}]", "[{\"symbol\":\"ACTUSDT\",\"leverage\":null}]"}) {
        TradingClient client(test_cfg()); int posts = 0, gets = 0;
        client.set_test_hook([&](const auto& method, const auto&, const auto&, auto& reply) {
            if (method == "POST") { ++posts; reply.code = 0; reply.timed_out = true; }
            else { ++gets; reply.body = bad; }
            return true;
        });
        check(!client.set_leverage("ACTUSDT", 2) && posts == 2 && gets == 2,
              "malformed/missing/foreign leverage never permits an entry");
    }
    for (bool rejection_on_query : {false, true}) for (long status : {401L, 418L, 429L}) {
        TradingClient client(test_cfg()); int posts = 0, gets = 0;
        client.set_test_hook([&](const auto& method, const auto&, const auto&, auto& reply) {
            if (method == "POST") ++posts; else ++gets;
            if (rejection_on_query && method == "POST") { reply.code = 0; reply.timed_out = true; }
            else { reply.code = status; reply.body = "rejected"; }
            return true;
        });
        check(!client.set_leverage("ACTUSDT", 2) && posts == 1 && gets == (rejection_on_query ? 1 : 0),
              "authentication failure or rate ban never triggers another immediate setting");
    }
}
