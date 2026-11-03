// 智能模式执行层测试：假交易所（HTTP 钩子）→ 全市场扫描 → 候选池决策落到引擎。
// 重点是"没拿到 ≠ 不合格"：交易规则/行情/K线拉不到时绝不删 bot。
#include "app/smart_runner.h"
#include "net/trading_client.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static constexpr int64_t kDay = 24LL * 3600 * 1000;
static constexpr int64_t kNow = 1'800'000'000'000LL;

// ── 假交易所 ────────────────────────────────────────────────────────────────
struct FakeMarket {
    struct Sym {
        double  slope = 0;          // 每根K线的涨跌
        double  quote_vol = 20e6;
        int64_t onboard_ms = kNow - 100 * kDay;
        std::string status = "TRADING";
        int64_t delivery_ms = 4133404800000LL;
        // 第 140 根起价格整体乘 (1+pump)：一根K线里暴涨。
        // ⚠ 放在最后：上面几个字段有用例按位置花括号初始化
        double last = 100;
        double  pump = 0;
        std::string utype = "";     // underlyingType；空 = 不发这个字段（带初值：clang 的花括号初始化不报缺字段）
    };
    std::map<std::string, Sym> syms;
    bool fail_info = false, fail_t24 = false;
    std::set<std::string> fail_klines;
    std::set<std::string> fail_oi;         // 这些币的持仓量拉不到
    bool oi_429 = false;                   // 持仓量接口整组被限流（它有自己的独立配额）
    std::atomic<int> oi_calls{0};
    std::atomic<int> btc_kline_calls{0};

    // 持仓量：25 个整点，每小时 +10，持仓价值固定 5e6
    static std::string oi_hist(const std::string& sym) {
        std::string b = "[";
        const int64_t last = kNow / 3600000LL * 3600000LL - 3600000LL;
        for (int i = 0; i < 25; ++i)
            b += std::string(i ? "," : "") + "{\"symbol\":\"" + sym + "\",\"sumOpenInterest\":\"" +
                 std::to_string(1000 + i * 10) + "\",\"sumOpenInterestValue\":\"5000000\",\"timestamp\":" +
                 std::to_string(last - (24 - i) * 3600000LL) + "}";
        return b + "]";
    }

    // 一根K线（8 列：开盘时间、开高低收、量、收盘时间、成交额）

    std::string info() const {
        std::string b = "{\"symbols\":[";
        bool first = true;
        for (const auto& kv : syms) {
            const std::string base = kv.first.substr(0, kv.first.size() - 4);
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"baseAsset\":\"" + base +
                 "\",\"quoteAsset\":\"USDT\",\"contractType\":\"PERPETUAL\"," +
                 (kv.second.utype.empty() ? "" : "\"underlyingType\":\"" + kv.second.utype + "\",") +
                 "\"status\":\"" + kv.second.status +
                 "\",\"onboardDate\":" + std::to_string(kv.second.onboard_ms) +
                 ",\"deliveryDate\":" + std::to_string(kv.second.delivery_ms) +
                 ",\"filters\":[{\"filterType\":\"PRICE_FILTER\",\"tickSize\":\"0.0001\"}]}";
            first = false;
        }
        return b + "]}";
    }
    std::string t24() const {
        std::string b = "[";
        bool first = true;
        for (const auto& kv : syms) {
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"quoteVolume\":\"" +
                 std::to_string(kv.second.quote_vol) + "\",\"priceChangePercent\":\"2.0\",\"lastPrice\":\"" + std::to_string(kv.second.last) + "\"}";
            first = false;
        }
        return b + "]";
    }
    std::string funding() const {
        std::string b = "[";
        bool first = true;
        for (const auto& kv : syms) {
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"lastFundingRate\":\"0.0001\"}";
            first = false;
        }
        return b + "]";
    }
    // 201 根 1h：一条带斜率的锯齿线，高低各 ±1（ATR 约 2%）
    std::string klines(const std::string& sym) const {
        const auto it = syms.find(sym);
        const double slope = (it == syms.end()) ? 0 : it->second.slope;
        const double pump  = (it == syms.end()) ? 0 : it->second.pump;
        std::string b = "[";
        double prev = 100;
        for (int i = 0; i < 201; ++i) {
            double c = 100 + slope * (i - 200) + ((i % 2) ? 0.6 : -0.6);
            if (i >= 140) c *= (1.0 + pump);
            // 最后 6 根已收盘的（194~199；200 是未收盘那根）
            const double h = std::max(prev, c) + 1, l = std::min(prev, c) - 1;
            b += std::string(i ? "," : "") + "[" + std::to_string(kNow - (201 - i) * 3600000LL) + ",\"" +
                 std::to_string(prev) + "\",\"" + std::to_string(h) + "\",\"" + std::to_string(l) + "\",\"" +
                 std::to_string(c) + "\",\"1000\"]";
            prev = c;
        }
        return b + "]";
    }
    void hook(TradingClient& tc) {
        tc.set_test_hook([this](const std::string&, const std::string& path, const std::string&,
                                TradingClient::FakeReply& out) {
            if (path == "/fapi/v1/exchangeInfo") { out.body = fail_info ? "" : info(); return true; }
            if (path == "/fapi/v1/ticker/24hr")  { out.body = fail_t24 ? "<html>502</html>" : t24(); return true; }
            if (path == "/fapi/v1/premiumIndex") { out.body = funding(); return true; }
            if (path.rfind("/fapi/v1/klines?symbol=", 0) == 0) {
                const auto s = path.substr(23, path.find('&') - 23);
                if (s == "BTCUSDT") ++btc_kline_calls;
                out.body = fail_klines.count(s) ? "" : klines(s);
                return true;
            }
            if (path.rfind("/futures/data/openInterestHist?symbol=", 0) == 0) {
                const auto s = path.substr(38, path.find('&') - 38);
                ++oi_calls;
                if (oi_429) {
                    out.code = 429;
                    out.body = "{\"code\":-1003,\"msg\":\"Too many requests\"}";
                    return true;
                }
                out.body = fail_oi.count(s) ? "" : oi_hist(s);
                return true;
            }
            // 测试钩子兜住所有未知请求，避免意外访问真实交易所。
            out.code = 400;
            out.body = "{}";
            return true;
        });
    }
};

struct Rig {
    FakeMarket mk;
    std::shared_ptr<TradingClient> tc;
    std::shared_ptr<TrendEngine> eng;
    std::unique_ptr<SmartRunner> run;
    std::vector<std::string> logs;
    std::set<std::string> subs;
    std::string path;

    explicit Rig(const std::string& p) : path(p) {
        TradingClient::Config c; c.api_key = "k"; c.api_secret = "s"; c.testnet = true;
        tc = std::make_shared<TradingClient>(c);
        mk.hook(*tc);
        EngineHost h;
        h.submit = [](std::function<void()> fn) { fn(); };
        eng = std::make_shared<TrendEngine>(tc, h);
        std::filesystem::remove(path);
        // 告警（会推 webhook）带 "W:" 前缀，方便断言去重
        run = std::make_unique<SmartRunner>(tc, eng, path,
                                            [this](const std::string& m, bool warn) {
                                                logs.push_back((warn ? "W:" : "I:") + m);
                                            });
        run->on_symbol_added   = [this](const std::string& s) { subs.insert(s); };
        run->on_symbol_removed = [this](const std::string& s) { subs.erase(s); };
    }
    // 通过硬过滤的全部品种进入统一候选池。
    smart::Config cfg() {
        smart::Config c;
        c.enabled = true;
        // 假行情的 1h ATR 约 2%（贴着默认上限）。这里测的是候选池机制，不是 ATR 门槛：放宽
        c.atr_pct_min = 0.8;
        c.atr_pct_max = 6.0;
        return c;
    }
    TrendBot bot(const std::string& sym) {
        for (const auto& b : eng->get_bots()) if (b.cfg.symbol == sym) return b;
        return {};
    }
    bool has_bot(const std::string& sym) {
        for (const auto& b : eng->get_bots()) if (b.cfg.symbol == sym) return true;
        return false;
    }
    std::string all_logs() const {
        std::string s;
        for (const auto& l : logs) s += l + "\n";
        return s;
    }
};

static void basic_market(FakeMarket& m) {
    m.syms["UPAUSDT"].slope = 0.05;
    m.syms["UPBUSDT"].slope = 0.03;
    m.syms["DNAUSDT"].slope = -0.05;
    m.syms["DNBUSDT"].slope = -0.03;
    m.syms["FLATUSDT"].slope = 0;
    m.syms["BTCUSDT"].slope = 0;
    m.syms["ETHUSDT"].slope = 0.04;
    m.syms["THINUSDT"] = {0.05, 1e6};                       // 成交额太小
    m.syms["NEWUSDT"]  = {0.05, 20e6, kNow - 3 * kDay};     // 上线太短
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dir = (std::filesystem::temp_directory_path() / "ccg_smart_runner_test").string();
    std::filesystem::create_directories(dir);

    // ── 基本流程：扫描 → 全部合格币 → 自动建 bot ─────────────────────
    {
        Rig r(dir + "/a.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        check(r.run->due(kNow), "启用后立刻到点");
        const auto rep = r.run->run_once(kNow);
        check(rep.ok, "扫描成功");
        check(rep.added == 5 && rep.candidates.size() == 5, "全部 5 个合格币进入统一候选，实际 " + std::to_string(rep.added));
        check(r.has_bot("UPAUSDT") && r.has_bot("DNAUSDT"), "  上涨币和下跌币都在");
        check(!r.has_bot("BTCUSDT") && !r.has_bot("ETHUSDT"), "  BTC/ETH 内置排除");
        check(r.mk.btc_kline_calls == 0, "  统一候选不拉 BTC 基准计算相对评分");
        check(!r.has_bot("THINUSDT") && !r.has_bot("NEWUSDT"), "  成交额不足 / 上线不满 14 天的不加");
        const auto b = r.bot("UPAUSDT");
        check(b.is_auto && !b.armed && b.cfg.interval == "1h" &&
              b.cfg.rule.direction == trend::Direction::Both && b.cfg.use_disaster_stop &&
              b.cfg.rule.lock_atr && !b.cfg.rule.accel_exit && b.cfg.rule.following.enabled,
              "  自动 bot：未上膛、1h 趋势跟踪、多空、硬止损、出场优化");
        check(r.subs.count("UPAUSDT") == 1, "  通知应用层订阅价格");
        check(r.run->due(kNow + 60000), "扫完之后每分钟有轻量异动观察");
        check(r.run->due(kNow / 3600000 * 3600000 + 3600000 + 61000), "  下个整点后 1 分钟到点");
        check(has(r.all_logs(), "新增："), "日志：新增一行");
    }

    // ── 整轮拉取失败：一个 bot 都不动 ─────────────────────────────────────────
    {
        Rig r(dir + "/b.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        const auto n0 = r.eng->get_bots().size();
        r.mk.fail_info = true;
        r.run->force();
        auto rep = r.run->run_once(kNow + 3600000 * 30LL);
        check(!rep.ok && r.eng->get_bots().size() == n0, "交易规则拉不到：整轮作废，bot 一个不动");
        check(has(rep.summary, "失败"), "  日志说明失败原因");
        r.mk.fail_info = false;
        r.mk.fail_t24 = true;
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 * 31LL);
        check(!rep.ok && r.eng->get_bots().size() == n0, "24h 行情拉不到（返回 HTML）：同样不动");

        // 数据正常但全部跌破硬门槛：合法空候选池，批量保护保留 bot 但暂停全部新仓。
        r.mk.fail_t24 = false;
        for (auto& kv : r.mk.syms) kv.second.quote_vol = 1;
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 * 32LL);
        bool all_retiring = true;
        for (const auto& b : r.eng->get_bots()) all_retiring &= b.retiring;
        check(rep.ok && rep.candidates.empty() && all_retiring && r.eng->get_bots().size() == n0,
              "全市场合格为 0：空候选池，批量保护保留已有 bot 并暂停新仓");
        for (auto& kv : r.mk.syms) kv.second.quote_vol = 20e6;
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 * 33LL);
        bool any_retiring = false;
        for (const auto& b : r.eng->get_bots()) any_retiring |= b.retiring;
        check(rep.ok && !rep.candidates.empty() && !any_retiring,
              "硬门槛恢复后重新允许候选等待突破");
    }

    // ── 单个币的K线拉不到：它是"未知"，不淘汰；成交额掉了：淘汰 ──────────────
    {
        Rig r(dir + "/c.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        r.mk.fail_klines.insert("UPAUSDT");
        for (int i = 1; i <= 3; ++i) {
            r.run->force();
            r.run->run_once(kNow + i * 30 * 3600000LL);
        }
        check(r.has_bot("UPAUSDT"), "K线连续 3 轮拉不到：仍然保留（没拿到 ≠ 不合格）");
        r.mk.fail_klines.clear();

        r.mk.syms["DNAUSDT"].quote_vol = 1e6;
        r.run->force();
        r.run->run_once(kNow + 5 * 30 * 3600000LL);
        check(!r.has_bot("DNAUSDT"), "成交额跌破门槛（1M < 4M）：空仓立即移除");
        check(r.subs.count("DNAUSDT") == 0, "  通知应用层退订");
        check(has(r.all_logs(), "移出：DNAUSDT"), "  日志写明移出原因");

        // 币安 TradFi 永续（股票 / 大宗商品）：趋势跟踪也只做币。池里已有的空仓移出，不再加回
        r.mk.syms["UPAUSDT"].utype = "EQUITY";
        r.mk.syms["UPBUSDT"].utype = "COIN";
        r.run->force();
        r.run->run_once(kNow + 6 * 30 * 3600000LL);
        check(!r.has_bot("UPAUSDT") && has(r.all_logs(), "非加密币标的"),
              "标的不是币（股票 / 大宗商品）：空仓移出，日志写明");
        check(r.has_bot("UPBUSDT"), "  标的类型是 COIN 的照常");
        r.run->force();
        r.run->run_once(kNow + 7 * 30 * 3600000LL);
        check(!r.has_bot("UPAUSDT"), "  下一轮也不会加回来");
    }

    // ── 手动 bot 占着的品种不加；方向改成只做多，空仓的自动 bot 换参数 ─────────
    {
        Rig r(dir + "/d.json");
        basic_market(r.mk);
        TrendConfig manual;
        manual.symbol = "UPAUSDT";
        r.eng->add_bot(manual);
        auto c = r.cfg();
        r.run->set_config(c);
        r.run->run_once(kNow);
        int autos_on_upa = 0;
        for (const auto& b : r.eng->get_bots())
            if (b.cfg.symbol == "UPAUSDT" && b.is_auto) ++autos_on_upa;
        check(autos_on_upa == 0, "手动 bot 占着的品种：智能模式不加");

        c.dir_short = false;
        r.run->set_config(c);
        check(r.run->due(kNow + 60000), "改了配置：立刻重扫");
        r.run->run_once(kNow + 60000);
        bool all_long = true;
        int autos = 0;
        for (const auto& b : r.eng->get_bots())
            if (b.is_auto) { ++autos; all_long &= (b.cfg.rule.direction == trend::Direction::LongOnly); }
        check(autos > 0 && all_long, "只勾做多：空仓的自动 bot 都换成仅多");
    }

    // ── 关闭智能模式：空仓的自动 bot 移除，手动 bot 不动 ──────────────────────
    {
        Rig r(dir + "/e.json");
        basic_market(r.mk);
        TrendConfig manual;
        manual.symbol = "FLATUSDT";
        r.eng->add_bot(manual);
        auto c = r.cfg();
        r.run->set_config(c);
        r.run->run_once(kNow);
        c.enabled = false;
        r.run->set_config(c);
        check(!r.run->due(kNow + 3600000 * 5LL), "关闭后不再到点");
        r.run->housekeep(kNow);
        int autos = 0;
        for (const auto& b : r.eng->get_bots()) if (b.is_auto) ++autos;
        check(autos == 0 && r.has_bot("FLATUSDT"), "关闭：空仓自动 bot 全部移除，手动 bot 保留");
    }

    // ── 统一候选不以历史形态预定方向 ─────────────────────
    {
        Rig r(dir + "/m.json");
        auto& m = r.mk.syms;
        m["UPAUSDT"].slope  = 0.05;    // 历史上涨
        m["UPBUSDT"].slope  = 0.02;    // 较慢上涨
        m["DNAUSDT"].slope  = -0.05;   // 历史下跌
        m["DNBUSDT"].slope  = -0.03;
        m["FLATUSDT"].slope = 0;       // 横盘
        m["FLAT2USDT"].slope = 0;
        m["PUMPUSDT"].pump  = 0.9;     // 历史一根K线涨 90%
        m["BTCUSDT"].slope  = 0;
        auto c = r.cfg();
        c.tmpl.rule.following.short_vol_ratio = 1.0;
        r.run->set_config(c);
        const auto rep = r.run->run_once(kNow);
        check(rep.ok && rep.candidates.size() == 7, "七个合格品种全部进入统一候选");
        for (const auto& [symbol, sym] : m) {
            if (symbol == "BTCUSDT") continue;
            check(r.has_bot(symbol) && r.bot(symbol).cfg.rule.direction == trend::Direction::Both,
                  "历史上涨、下跌、横盘、暴涨均不锁定候选方向：" + symbol);
        }
        check(r.bot("PUMPUSDT").cfg.rule.breakout_vol_ratio == c.tmpl.rule.breakout_vol_ratio &&
              r.bot("DNAUSDT").cfg.rule.following.short_vol_ratio == 1.0, "不同历史形态使用同一入场模板");

        // 数据记录层：扫描快照与候选池事件
        const auto jdir = std::filesystem::path(dir) / "journal_m";
        std::filesystem::remove_all(jdir);
        Rig rj(dir + "/mj.json");
        rj.mk.syms = m;
        rj.mk.fail_oi = {"FLATUSDT"};
        rj.run->set_journal(std::make_shared<Journal>(jdir.string()));
        rj.run->set_config(c);
        const auto rpj = rj.run->run_once(kNow);
        bool scan_file = false;
        std::string dna_line, flat_line;
        for (const auto& e : std::filesystem::directory_iterator(jdir)) {
            if (e.path().filename().string().rfind("scan-", 0) != 0) continue;
            scan_file = true;
            std::ifstream f(e.path(), std::ios::binary);
            for (std::string l; std::getline(f, l);) {
                if (has(l, "DNAUSDT"))  dna_line = l;
                if (has(l, "FLATUSDT")) flat_line = l;
            }
        }
        check(scan_file && std::filesystem::exists(jdir / "pool.csv"),
              "数据记录层：每轮写扫描快照，加入的币记进 pool.csv");
        // 最新 1240，1h 前 1230，24h 前 1000
        check(has(dna_line, ",5000000,0.813,3.333,24"),
              "  持仓量：价值与 1/4/24h 变化写进扫描快照");
        check(!flat_line.empty() && !has(flat_line, "5000000"),
              "  持仓量拉不到的币：那几列留空，照常加入候选");
        check(rpj.ok, "  持仓量不影响这一轮");

        // 持仓量接口被限流：它有独立配额，不能把全局闸门（下单、止损也走它）冻住；
        // 本组冷却，这一轮剩下的币不再去撞
        {
            Rig rl(dir + "/ml.json");
            rl.mk.syms = m;
            rl.mk.oi_429 = true;
            rl.run->set_config(c);
            const auto rpl = rl.run->run_once(kNow);
            check(rpl.ok && !rl.tc->rate_status().banned, "持仓量 429：这一轮照常，全局闸门不封");
            check(rl.mk.oi_calls.load() <= 4,
                  "  429 之后本组冷却，不再逐个去撞（并发下最多几个已在路上）");
        }

        // K线解析：币安的 12 列（第 10 列是主动买入成交额）
        {
            TradingClient::Config tcfg; tcfg.api_key = "k"; tcfg.api_secret = "s"; tcfg.testnet = true;
            TradingClient tk(tcfg);
            tk.set_test_hook([](const std::string&, const std::string& p, const std::string&,
                                TradingClient::FakeReply& out) {
                if (p.rfind("/fapi/v1/klines?", 0) != 0) return false;
                out.body = "[[1000,\"1\",\"2\",\"0.5\",\"1.5\",\"10\",1999,\"15\",42,\"6\",\"9.75\",\"0\"],"
                           "[2000,\"1\",\"2\",\"0.5\",\"1.5\",\"10\",2999,\"15\"]]";
                return true;
            });
            const auto kb = tk.fetch_klines("XUSDT", "1h", 2);
            check(kb.size() == 2 && std::fabs(kb[0].quote_volume - 15) < 1e-9 &&
                      std::fabs(kb[0].taker_buy_quote - 9.75) < 1e-9 && kb[1].taker_buy_quote == 0,
                  "K线解析：主动买入成交额取第 10 列；列不够的留 0，不算坏行");
        }

        // 只勾做多：弱势、过热不加；中间段只做多
        Rig r2(dir + "/m2.json");
        r2.mk.syms = m;
        auto c2 = c; c2.dir_short = false;
        r2.run->set_config(c2);
        r2.run->run_once(kNow);
        check(r2.has_bot("DNAUSDT") && r2.has_bot("PUMPUSDT") && r2.bot("PUMPUSDT").cfg.rule.direction == trend::Direction::LongOnly, "只勾做多：各段可观察，但实际只允许趋势多头");
        check(r2.bot("FLATUSDT").cfg.rule.direction == trend::Direction::LongOnly,
              "  中间段只做多（与勾选取交集）");
    }

    // ── 扫描连续失败：只有第一轮告警（推 webhook），之后只记日志，恢复时说一声 ──
    {
        Rig r(dir + "/g.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.mk.fail_info = true;
        r.run->run_once(kNow);
        check(!r.run->due(kNow + 30000) && r.run->due(kNow + 5 * 60000),
              "扫描失败：5 分钟后重试（不空等到下个整点）");
        // 5 分钟一试：连续 12 轮（一小时）
        for (int i = 1; i < 12; ++i) r.run->run_once(kNow + i * 5 * 60000LL);
        int warns = 0, infos = 0;
        for (const auto& l : r.logs) {
            if (l.rfind("W:", 0) == 0 && has(l, "扫描失败")) ++warns;
            if (l.rfind("I:", 0) == 0 && has(l, "扫描失败")) ++infos;
        }
        check(warns == 1 && infos == 1, "连续失败 12 轮：第一轮告警，之后每小时只记一行（实际 告警 " +
                                        std::to_string(warns) + " / 日志 " + std::to_string(infos) + "）");
        check(has(r.all_logs(), "已连续 12 轮") && has(r.all_logs(), "5 分钟后重试"), "  日志里写明连续几轮、何时重试");
        r.mk.fail_info = false;
        r.run->run_once(kNow + 12 * 5 * 60000LL);
        check(has(r.all_logs(), "扫描已恢复（此前连续失败 12 轮）"), "  恢复时说一声");
    }

    // ── 作废的运行器：正在跑的那轮不落地、不写文件 ────────────────────────────
    {
        Rig r(dir + "/h.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->cancel();
        r.run->run_once(kNow);
        int autos = 0;
        for (const auto& b : r.eng->get_bots()) if (b.is_auto) ++autos;
        check(autos == 0, "作废后：扫描结果不落地（断线重连时旧运行器不往旧引擎里加 bot）");
        r.run->housekeep(kNow);
        check(!std::filesystem::exists(r.path), "  也不写状态文件（不和新运行器抢）");
    }

    // ── 当日已实现：状态文件里没有就用成交历史补 ───────────────────────────────
    {
        const auto now = std::chrono::system_clock::now();
        const int64_t today = TrendEngine::utc_day(now);
        std::vector<TrendTrade> tr(3);
        tr[0].close_time = now;                         tr[0].pnl = -10; tr[0].entry_price = 100; tr[0].exit_price = 95; tr[0].qty = 2;
        tr[1].close_time = now;                         tr[1].pnl = 4;   tr[1].entry_price = 50;  tr[1].exit_price = 52; tr[1].qty = 1;
        tr[2].close_time = now - std::chrono::hours(48); tr[2].pnl = -99; tr[2].entry_price = 1;  tr[2].exit_price = 1;  tr[2].qty = 1;
        const double want = (-10 - (100 + 95) * 2 * TrendEngine::kEstFeeRate) +
                            (4 - (50 + 52) * 1 * TrendEngine::kEstFeeRate);
        check(std::fabs(sum_day_realized(tr, today) - want) < 1e-9,
              "按成交历史算今天的已实现：只算今天的，扣估算手续费（与引擎同口径）");

        Rig r(dir + "/i.json");
        r.run->seed_day_from_history(tr);
        check(std::fabs(r.eng->day_pnl().realized - want) < 1e-9, "状态里没有今天的数：用历史补上");
        r.eng->restore_day_pnl({today, -1});
        r.run->seed_day_from_history(tr);
        check(r.eng->day_pnl().realized == -1, "状态里已有今天的数：以状态为准，不覆盖");
    }

    // 重扫、模板替换与停启智能模式都不得绕过平仓后的等待。
    {
        Rig r(dir + "/reentry.json");
        auto c = r.cfg();
        TrendBot waiting;
        waiting.cfg = smart::make_bot_config(c, "WAITUSDT");
        waiting.is_auto = true; waiting.reentry_after_ms = kNow + 7200000;
        r.eng->restore_bot(waiting);
        r.mk.syms["WAITUSDT"].slope = 0;
        r.mk.syms["OTHERUSDT"].slope = 0;
        c.tmpl.rule.donchian_period = 20;
        r.run->set_config(c); r.run->run_once(kNow);
        check(r.bot("WAITUSDT").reentry_after_ms == waiting.reentry_after_ms &&
              r.bot("WAITUSDT").cfg.rule.donchian_period == 20,
              "候选模板重建后继续保留平仓等待");
        r.mk.syms["WAITUSDT"].quote_vol = 1;
        r.run->force(); r.run->run_once(kNow + 3600000);
        check(r.has_bot("WAITUSDT") && r.bot("WAITUSDT").retiring && r.bot("WAITUSDT").waiting_reentry(),
              "过滤不合格时暂停新仓但保留等待记录，防止删后重加绕过");
        c.enabled = false; r.run->set_config(c); r.run->housekeep(kNow + 3600000);
        check(r.has_bot("WAITUSDT") && r.bot("WAITUSDT").retiring,
              "关闭智能模式保留等待记录且禁止新仓");
        c.enabled = true; r.mk.syms["WAITUSDT"].quote_vol = 20e6;
        r.run->set_config(c); r.run->run_once(kNow + 3600000);
        check(!r.bot("WAITUSDT").retiring && r.bot("WAITUSDT").reentry_after_ms == waiting.reentry_after_ms,
              "重新通过过滤只解除候选暂停，仍须等待新的收盘突破");
    }

    // 持仓中的趋势跟踪更新出场设置；不改周期、方向、杠杆及已有止损。
    {
        Rig r(dir + "/hold_exit.json");
        auto c = r.cfg();
        TrendBot held;
        held.cfg = smart::make_bot_config(c, "HELDUSDT", trend::Direction::LongOnly);
        held.is_auto = true;
        held.st.pos = trend::Pos::Long;
        held.st.entry_price = 100; held.st.peak = 106; held.st.stop = 99; held.qty = 1;
        r.eng->restore_bot(held);
        check(r.bot("HELDUSDT").retiring, "恢复自动 bot 后先等新一轮候选过滤，持仓管理保留");
        TrendBot manual = held;
        manual.cfg.symbol = "MANUUSDT"; manual.is_auto = false;
        r.eng->restore_bot(manual);
        c.tmpl.rule.roi_trail = true; c.tmpl.rule.roi_trail_start = 12;
        c.tmpl.rule.roi_trail_back = 3; c.tmpl.rule.breakeven = true;
        c.tmpl.rule.breakeven_start = 6; c.tmpl.rule.atr_mult = 2.5;
        c.tmpl.leverage = 5; c.tmpl.interval = "4h";
        r.run->set_config(c);
        const auto b = r.bot("HELDUSDT");
        check(b.cfg.rule.roi_trail && b.cfg.rule.breakeven &&
              std::fabs(b.cfg.rule.atr_mult - 2.5) < 1e-9,
              "趋势跟踪持仓即时更新出场参数");
        check(b.cfg.interval == "1h" && b.cfg.leverage != 5 &&
              b.cfg.rule.direction == trend::Direction::LongOnly &&
              b.st.pos == trend::Pos::Long && b.qty == 1 && b.st.stop == 99 && b.st.entry_price == 100,
              "已有持仓、止损、方向、杠杆和周期保留");
        check(!r.bot("MANUUSDT").cfg.rule.breakeven, "手动 bot 不被智能配置覆盖");
        check(has(r.all_logs(), "1 个持仓换上新的出场参数"), "更新记录一条汇总");
        const auto n = r.logs.size();
        r.run->housekeep(kNow);
        check(r.logs.size() == n, "配置未变时不重复记录");
        c.enabled = false;
        r.run->set_config(c);
        r.run->housekeep(kNow);
        check(r.bot("HELDUSDT").retiring && r.bot("HELDUSDT").qty == 1,
              "关闭智能模式保留持仓并标为待移出");
        c.enabled = true;
        r.run->set_config(c);
        check(r.bot("HELDUSDT").retiring && r.bot("HELDUSDT").st.stop == 99,
              "重新启用等待候选过滤确认，不直接解除待移出");
        r.mk.syms["HELDUSDT"].slope = 0;
        r.run->run_once(kNow);
        check(!r.bot("HELDUSDT").retiring && r.bot("HELDUSDT").qty == 1,
              "通过新扫描后接回候选，持仓保持");
        r.mk.syms["HELDUSDT"].quote_vol = 1;
        r.mk.syms["OTHERUSDT"].slope = 0;
        r.run->force(); r.run->run_once(kNow + 3600000);
        check(r.bot("HELDUSDT").retiring && r.bot("HELDUSDT").qty == 1 && r.bot("HELDUSDT").st.stop == 99,
              "持仓不合格时只禁止新仓，不丢弃仓位及止损");
    }

    // Event observations share the production scheduler but do not repeatedly scan candles.
    {
        Rig r(dir + "/wake.json"); basic_market(r.mk);
        auto c = r.cfg(); c.event_move_atr = 1; c.event_min_move_pct = 1;
        r.run->set_config(c);
        const int64_t t = kNow / 3600000 * 3600000 + 120000;
        const auto first = r.run->run_once(t);
        for (int n = 1; n <= 5; ++n) {
            const auto idle = r.run->run_once(t + n * 60000);
            check(idle.at_ms == first.at_ms, "lightweight unchanged polls preserve last full-scan report");
        }
        r.mk.syms["UPAUSDT"].last = 110;
        r.mk.syms["DNAUSDT"].last = 90;
        const auto event = r.run->run_once(t + 360000);
        check(event.ok && event.at_ms == t + 360000 && has(r.all_logs(), "双向价格异动"),
              "simultaneous directional events cause one real merged rescan");
        r.mk.syms["UPAUSDT"].last = 130;
        const auto cooled = r.run->run_once(t + 420000);
        check(cooled.at_ms == event.at_ms, "additional event during cooldown does not repeat full scan");
    }

    // ── 池状态与当日盈亏落盘 ─────────────────────────────────────────────────
    {
        Rig r(dir + "/f.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        r.eng->restore_day_pnl({TrendEngine::utc_day(std::chrono::system_clock::now()), -12.5});
        r.run->housekeep(kNow);

        auto eng2 = std::make_shared<TrendEngine>(r.tc, EngineHost{});
        SmartRunner r2(r.tc, eng2, r.path, nullptr);
        r2.load();
        check(std::fabs(eng2->day_pnl().realized + 12.5) < 1e-9, "当日盈亏跨重启接得上（亏损限额不因重启清零）");
    }

    std::filesystem::remove_all(dir);
    if (g_fail == 0) {
        std::printf("OK: 智能模式执行层测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
