#include "net/trading_client.h"
#include "core/profit_json.h"
#include "net/url_encoding.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace ccbot {
namespace {
double decimal(simdjson::dom::element o, const char* key) {
    double d; if (o[key].get(d) == simdjson::SUCCESS && std::isfinite(d)) return d;
    const auto s = profit::string(o, key); size_t consumed = 0; d = std::stod(s, &consumed);
    if (consumed != s.size() || !std::isfinite(d)) throw std::runtime_error(key);
    return d;
}
std::string identifier(simdjson::dom::element o, const char* key) {
    int64_t n; if (o[key].get(n) == simdjson::SUCCESS && n > 0) return std::to_string(n);
    const auto text = profit::string(o, key);
    if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos || std::stoll(text) <= 0)
        throw std::runtime_error(key);
    return text;
}
std::string fixed(double d) { std::ostringstream s; s << std::fixed << std::setprecision(8) << d; return s.str(); }
bool rejected(simdjson::dom::element o) {
    int64_t code; return o["code"].get(code) == simdjson::SUCCESS && code < 0 &&
        code != -1000 && code != -1001 && code != -1006 && code != -1007;
}
}
std::string TradingClient::account_fingerprint() const { return sign("TradingBot-profit-v1:" + cfg_.api_key + ":" + base_); }

bool TradingClient::profit_fills(const std::string& symbol, const std::string& order_id, double expected,
                                std::vector<ProfitFill>& fills, std::string& error) {
    try {
        if (order_id.empty() || !std::isfinite(expected) || expected <= 0) throw std::runtime_error("成交数量／订单号未确认");
        std::vector<ProfitFill> got; std::set<std::string> ids; double quantity = 0;
        int64_t from = 0;
        for (int page = 0; page < 100; ++page) {
            auto query = "symbol=" + url::encode(symbol) + "&orderId=" + url::encode(order_id) + "&limit=1000&recvWindow=5000";
            if (from > 0) query += "&fromId=" + std::to_string(from);
            simdjson::dom::parser p; auto doc = p.parse(simdjson::padded_string(http_get(is_pm() ? "/papi/v1/um/userTrades" : "/fapi/v1/userTrades", query))).value();
            auto rows = doc.get_array().value(); int64_t next = from;
            for (auto row : rows) {
                if (identifier(row, "orderId") != order_id || profit::string(row, "symbol") != symbol) throw std::runtime_error("成交归属不一致");
                ProfitFill f; f.id = identifier(row, "id"); f.order_id = order_id;
                f.time = profit::integer(row, "time"); f.qty = decimal(row, "qty"); f.pnl = decimal(row, "realizedPnl");
                f.fee = decimal(row, "commission"); f.side = profit::string(row, "side");
                if (profit::string(row, "commissionAsset") != "USDT" && f.fee != 0) throw std::runtime_error("非 USDT 手续费待换算，暂停分配");
                if (f.qty <= 0 || f.fee < 0 || !ids.insert(f.id).second) throw std::runtime_error("成交明细重复或不完整");
                quantity += f.qty; next = std::max<int64_t>(next, std::stoll(f.id) + 1LL); got.push_back(f);
            }
            if (rows.size() < 1000) break;
            if (next <= from || page == 99) throw std::runtime_error("成交分页未完成");
            from = next;
        }
        if (std::fabs(quantity - expected) > std::max(1e-9, expected * 1e-8)) throw std::runtime_error("成交明细数量未对齐");
        fills = std::move(got); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool TradingClient::profit_funding(const std::string& symbol, int64_t start, int64_t end, double& amount, std::string& error) {
    try {
        double total = 0; std::set<std::string> ids;
        if (start <= 0 || end < start || end - start > 90LL * 86400000) throw std::runtime_error("资金费查询时间范围不完整");
        for (int64_t begin = start; begin <= end; begin += 86400000) {
            const auto finish = std::min(end, begin + 86400000 - 1);
            auto query = "symbol=" + url::encode(symbol) + "&incomeType=FUNDING_FEE&limit=1000&startTime=" + std::to_string(begin) + "&endTime=" + std::to_string(finish) + "&recvWindow=5000";
            simdjson::dom::parser p; auto rows = p.parse(simdjson::padded_string(http_get(ep(Ep::Income), query))).get_array().value();
            if (rows.size() >= 1000) throw std::runtime_error("资金费分页未完成");
            for (auto row : rows) {
                const auto when = profit::integer(row, "time");
                if (profit::string(row, "symbol") != symbol || profit::string(row, "asset") != "USDT" ||
                    profit::string(row, "incomeType") != "FUNDING_FEE" || when < begin || when > finish) throw std::runtime_error("资金费归属不一致");
                if (!ids.insert(identifier(row, "tranId")).second) throw std::runtime_error("资金费记录重复");
                total += decimal(row, "income");
            }
        }
        amount = total; error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool TradingClient::profit_owned_window(const std::string& symbol, int64_t start, int64_t end,
                                       const std::set<std::string>& owned, std::string& error) {
    try {
        std::set<std::string> seen;
        if (start <= 0 || end < start || end - start > 90LL * 86400000) throw std::runtime_error("持仓成交窗口不完整");
        for (int64_t begin = start; begin <= end; begin += 86400000) {
            const auto finish = std::min(end, begin + 86400000 - 1);
            auto query = "symbol=" + url::encode(symbol) + "&startTime=" + std::to_string(begin) + "&endTime=" + std::to_string(finish) + "&limit=1000&recvWindow=5000";
            simdjson::dom::parser p; auto rows = p.parse(simdjson::padded_string(http_get(is_pm() ? "/papi/v1/um/userTrades" : "/fapi/v1/userTrades", query))).get_array().value();
            if (rows.size() >= 1000) throw std::runtime_error("持仓期间成交明细达到分页上限，暂停分配");
            for (auto row : rows) {
                const auto time = profit::integer(row, "time");
                const auto id = identifier(row, "orderId");
                if (profit::string(row, "symbol") != symbol || time < begin || time > finish || !owned.count(id))
                    throw std::runtime_error("持仓期间存在未归属成交，不能提取该笔利润");
                seen.insert(id);
            }
        }
        if (owned.empty() || seen != owned) throw std::runtime_error("持仓期间成交窗口尚未覆盖全部已知订单");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
TradingClient::ProfitCash TradingClient::profit_cash() {
    ProfitCash cash;
    try {
        if (is_pm() || is_dual_mode() || cfg_.testnet) throw std::runtime_error("BTC 利润积累首版支持普通单向 USDT 主网账户");
        simdjson::dom::parser mode_parser;
        auto mode = mode_parser.parse(simdjson::padded_string(http_get("/fapi/v1/multiAssetsMargin", "recvWindow=5000"))).value();
        if (profit::boolean(mode, "multiAssetsMargin")) throw std::runtime_error("联合保证金模式尚未支持自动利润划转");
        simdjson::dom::parser p;
        auto rows = p.parse(simdjson::padded_string(http_get("/fapi/v3/balance", "recvWindow=5000"))).get_array().value();
        for (auto row : rows) if (profit::string(row, "asset") == "USDT") {
            cash.available = decimal(row, "availableBalance"); cash.transferable = decimal(row, "maxWithdrawAmount");
            // Include isolated-position floating losses too; the cross-only balance is insufficient.
            simdjson::dom::parser account_parser;
            const auto account = account_parser.parse(simdjson::padded_string(http_get(ep(Ep::Account), "recvWindow=5000"))).value();
            cash.equity = decimal(account, "totalMarginBalance");
            cash.ok = cash.equity >= 0 && cash.available >= 0 && cash.transferable >= 0; return cash;
        }
        throw std::runtime_error("USDT 余额缺失");
    } catch (const std::exception& e) { cash.error = e.what(); return cash; }
}
TradingClient::ProfitTransfer TradingClient::transfer_profit(double usdt) {
    ProfitTransfer result;
    try {
        if (cfg_.testnet || is_pm() || !(std::isfinite(usdt) && usdt > 0)) { result.rejected = true; throw std::runtime_error("利润划转账户／金额不适用"); }
        long code = 0;
        const auto body = http_post("/sapi/v1/asset/transfer", "type=UMFUTURE_MAIN&asset=USDT&amount=" + fixed(usdt) + "&recvWindow=5000", &code);
        simdjson::dom::parser p; auto doc = p.parse(simdjson::padded_string(body)).value();
        if (code >= 400 && code < 500 && rejected(doc)) { result.rejected = true; throw std::runtime_error(profit::string(doc, "msg")); }
        result.id = identifier(doc, "tranId"); result.ok = code >= 200 && code < 300;
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}
TradingClient::ProfitTransfer TradingClient::query_profit_transfer(double usdt, int64_t sent, const std::string& id) {
    ProfitTransfer result;
    try {
        // Never widen an unknown request's window on each restart: a later manual
        // transfer of the same amount must not be mistaken for this request.
        const auto end = sent + 60000;
        auto params = "type=UMFUTURE_MAIN&startTime=" + std::to_string(sent - 5000) + "&endTime=" + std::to_string(end) + "&size=100&recvWindow=5000";
        simdjson::dom::parser p; auto doc = p.parse(simdjson::padded_string(http_get("/sapi/v1/asset/transfer", params))).value();
        auto rows = doc["rows"].get_array().value();
        if (profit::integer(doc, "total") > 100) throw std::runtime_error("划转记录过多，待核对");
        int matches = 0;
        for (auto row : rows) {
            auto tid = identifier(row, "tranId");
            const auto when = profit::integer(row, "timestamp");
            if ((!id.empty() && tid != id) || profit::string(row, "asset") != "USDT" ||
                profit::string(row, "type") != "UMFUTURE_MAIN" || when < sent - 5000 || when > end ||
                std::fabs(decimal(row, "amount") - usdt) > 1e-7) continue;
            ++matches; result.id = tid;
            result.ok = profit::string(row, "status") == "CONFIRMED";
        }
        if (matches != 1 || !result.ok) throw std::runtime_error("划转结果未唯一确认，保留预留额度，不重复划转");
    } catch (const std::exception& e) { result.ok = false; result.error = e.what(); }
    return result;
}
TradingClient::SpotQuote TradingClient::btc_spot_quote() {
    SpotQuote q;
    try {
        simdjson::dom::parser p, p2;
        auto book = p.parse(simdjson::padded_string(http_get_public("/api/v3/ticker/bookTicker?symbol=BTCUSDT"))).value();
        if (profit::string(book, "symbol") != "BTCUSDT") throw std::runtime_error("BTC 报价品种不匹配");
        q.ask = decimal(book, "askPrice");
        auto rows = p2.parse(simdjson::padded_string(http_get_public("/api/v3/exchangeInfo?symbol=BTCUSDT")))["symbols"].get_array().value();
        for (auto row : rows) {
            if (profit::string(row, "symbol") != "BTCUSDT" || profit::string(row, "status") != "TRADING") continue;
            const auto filters = row["filters"].get_array().value();
            for (auto f : filters) {
                auto type = profit::string(f, "filterType");
                if (type == "PRICE_FILTER") q.tick = decimal(f, "tickSize");
                if (type == "LOT_SIZE") { q.step = decimal(f, "stepSize"); q.min_qty = decimal(f, "minQty"); }
                if (type == "MIN_NOTIONAL" || type == "NOTIONAL") q.min_notional = std::max(q.min_notional, decimal(f, "minNotional"));
            }
        }
        q.ok = q.ask > 0 && q.tick > 0 && q.step > 0 && q.min_qty > 0 && q.min_notional > 0;
        if (!q.ok) throw std::runtime_error("BTC 现货规则不完整");
    } catch (const std::exception& e) { q.ok = false; q.error = e.what(); } return q;
}
TradingClient::SpotPurchase TradingClient::buy_btc_profit(double qty, double price, const std::string& tag) {
    SpotPurchase result;
    try {
        if (cfg_.testnet || is_pm() || tag.empty() || qty <= 0 || price <= 0 || !std::isfinite(qty) || !std::isfinite(price)) {
            result.rejected = true; throw std::runtime_error("利润现货订单参数不合法");
        }
        long code = 0;
        auto body = http_post("/api/v3/order", "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=IOC&quantity=" + fixed(qty) + "&price=" + fixed(price) + "&newClientOrderId=" + url::encode(tag) + "&newOrderRespType=FULL&recvWindow=5000", &code);
        simdjson::dom::parser p; auto doc = p.parse(simdjson::padded_string(body)).value();
        if (code >= 400 && code < 500 && rejected(doc)) { result.rejected = true; throw std::runtime_error(profit::string(doc, "msg")); }
        // Always reconcile fills, including fees; never trust the submitted quantity.
        return query_btc_profit(tag);
    } catch (const std::exception& e) { result.error = e.what(); } return result;
}
TradingClient::SpotPurchase TradingClient::query_btc_profit(const std::string& tag) {
    SpotPurchase result;
    try {
        simdjson::dom::parser p, p2;
        auto doc = p.parse(simdjson::padded_string(http_get("/api/v3/order", "symbol=BTCUSDT&origClientOrderId=" + url::encode(tag) + "&recvWindow=5000"))).value();
        if (profit::string(doc, "symbol") != "BTCUSDT" || profit::string(doc, "side") != "BUY" || profit::string(doc, "clientOrderId") != tag)
            throw std::runtime_error("BTC 订单归属不匹配");
        auto status = profit::string(doc, "status");
        result.terminal = status == "FILLED" || status == "EXPIRED" || status == "CANCELED" || status == "REJECTED" || status == "EXPIRED_IN_MATCH";
        if (!result.terminal) throw std::runtime_error("BTC 订单尚未终结");
        const auto qty = decimal(doc, "executedQty"), quote_qty = decimal(doc, "cummulativeQuoteQty");
        if (qty == 0 && quote_qty == 0) { result.ok = true; return result; }
        auto id = identifier(doc, "orderId");
        auto rows = p2.parse(simdjson::padded_string(http_get("/api/v3/myTrades", "symbol=BTCUSDT&orderId=" + id + "&limit=1000&recvWindow=5000"))).get_array().value();
        if (rows.size() >= 1000) throw std::runtime_error("BTC 成交分页未完成，保留订单待核对");
        double filled = 0, spent = 0, btc_fee = 0, usdt_fee = 0; std::set<std::string> ids;
        for (auto row : rows) {
            if (identifier(row, "orderId") != id || profit::string(row, "symbol") != "BTCUSDT" || !profit::boolean(row, "isBuyer") ||
                !ids.insert(identifier(row, "id")).second) throw std::runtime_error("BTC 成交归属不一致");
            filled += decimal(row, "qty"); spent += decimal(row, "quoteQty");
            const auto fee = decimal(row, "commission"); const auto asset = profit::string(row, "commissionAsset");
            if (fee < 0) throw std::runtime_error("BTC 手续费不合法");
            if (asset == "BTC") btc_fee += fee;
            else if (asset == "USDT") usdt_fee += fee;
            else if (fee != 0) throw std::runtime_error("BTC 非 USDT/BTC 手续费待换算，保留已购订单");
        }
        if (std::fabs(filled - qty) > std::max(1e-10, qty * 1e-8) || std::fabs(spent - quote_qty) > 1e-5 || btc_fee > filled)
            throw std::runtime_error("BTC 成交数量／金额未对齐");
        result.qty = filled - btc_fee; result.cost = spent + usdt_fee; result.ok = true;
    } catch (const std::exception& e) { result.error = e.what(); } return result;
}
} // namespace ccbot
