#include "net/url_encoding.h"
#include <mbedtls/md.h>
#include <iomanip>

static std::string test_signature(const std::string& payload) {
    unsigned char digest[32]{};
    const unsigned char key[] = {'s'}; // Fake test secret, never an account credential.
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 1,
        reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest);
    std::ostringstream out;
    for (unsigned char byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return out.str();
}

static void test_unicode_symbol_scenarios() {
    const std::string symbol = "龙虾USDT", encoded = "%E9%BE%99%E8%99%BEUSDT";
    check(url::encode(symbol) == encoded && url::encode("BTCUSDT-_.~") == "BTCUSDT-_.~" &&
          url::encode("a&b=+% /:") == "a%26b%3D%2B%25%20%2F%3A",
          "UTF-8 query values use RFC3986 encoding; ASCII symbols stay unchanged");
    for (auto mode : {TradingClient::AccountMode::Futures, TradingClient::AccountMode::PortfolioMargin}) {
        auto cfg = test_cfg(); cfg.account_mode = mode;
        TradingClient tc(cfg); int signed_calls = 0, public_calls = 0;
        tc.set_test_hook([&](const std::string& method, const std::string& path,
                            const std::string& params, TradingClient::FakeReply& out) {
            const auto request = path + "?" + params;
            check(request.find("symbol=" + encoded) != std::string::npos && request.find(symbol) == std::string::npos &&
                  request.find("%25E9") == std::string::npos, "every per-symbol request encodes Chinese once");
            if (!params.empty()) {
                ++signed_calls;
                const auto at = params.find("&signature=");
                check(at != std::string::npos && params.substr(at + 11) == test_signature(params.substr(0, at)),
                      "GET/POST/DELETE signature covers the exact percent-encoded wire payload");
                if (path.find("/order") != std::string::npos && method == "GET")
                    check(params.find("origClientOrderId=cb%2Ftest%3A1") != std::string::npos,
                          "order recovery encodes the original client ID as a value");
            } else ++public_calls;
            if (path.find("exchangeInfo") != std::string::npos)
                out.body = "{\"symbols\":[{\"symbol\":\"" + symbol + "\",\"filters\":[{\"filterType\":\"LOT_SIZE\",\"stepSize\":\"1\",\"minQty\":\"1\"},{\"filterType\":\"PRICE_FILTER\",\"tickSize\":\"0.01\"}]}]}";
            else if (path.find("klines") != std::string::npos) out.body = "[[1800000000000,\"100\",\"102\",\"99\",\"101\",\"100\"]]";
            else if (path.find("leverage") != std::string::npos) out.body = "{\"symbol\":\"" + symbol + "\",\"leverage\":3}";
            else if (path.find("algo") != std::string::npos || path.find("Algo") != std::string::npos) out.body = "{\"algoId\":123,\"triggerPrice\":\"90.00\"}";
            else if (path.find("/order") != std::string::npos) out.body = "{\"orderId\":1,\"status\":\"FILLED\",\"avgPrice\":\"100\",\"executedQty\":\"1\"}";
            else if (path.find("premiumIndex") != std::string::npos) out.body = "{\"markPrice\":\"100\",\"lastFundingRate\":\"0.0001\"}";
            else out.body = "[]";
            return true; // Never fall through to a real exchange, including unknown paths.
        });
        check(tc.fetch_klines(symbol, "1h", 1).size() == 1, "Chinese symbol candles decode normally");
        check(tc.set_leverage(symbol, 3), "Chinese symbol leverage uses encoded signed request");
        check(tc.place_market_order(symbol, "BUY", 1, false).ok, "Chinese symbol opening uses encoded signed request");
        check(tc.place_market_order(symbol, "SELL", 1, true).ok, "Chinese symbol closing uses encoded signed request");
        check(tc.opening_order_terminal(symbol, "cb/test:1"), "Chinese symbol uncertain-order recovery remains available");
        check(tc.place_disaster_stop(symbol, 90, "BUY", 1).ok(), "Chinese symbol protective stop uses encoded signed request");
        check(tc.cancel_disaster_stop(symbol, "123"), "Chinese symbol protective stop cancellation is encoded");
        (void)tc.fetch_oi_hist(symbol, "1h", 1);
        (void)tc.fetch_premium(symbol);
        (void)tc.fetch_funding_income(0, 0, symbol);
        check(signed_calls == 7 && public_calls == 4, "all affected endpoints are covered in both account modes");
    }
    TradingClient tc(test_cfg());
    tc.set_test_hook([](const std::string&, const std::string&, const std::string&, TradingClient::FakeReply& out) {
        out.code = 400; out.body = R"({"code":-1100,"msg":"Illegal characters found in parameter 'symbol'"})"; return true;
    });
    auto signal = tc.fetch_trend_signal(symbol, "1h", 20, 14);
    check(!signal.ok && signal.error.find("-1100") != std::string::npos,
          "exchange Kline error reaches the monitor instead of a generic expired label");
    tc.set_test_hook([](const std::string&, const std::string&, const std::string&, TradingClient::FakeReply& out) {
        out.code = 502; out.body = "<html>upstream unavailable</html>"; return true;
    });
    signal = tc.fetch_trend_signal(symbol, "1h", 20, 14);
    check(!signal.ok && signal.error.find("502") != std::string::npos,
          "gateway failure retains its actual diagnostic");
}
