#include "core/journal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace ccbot {

namespace {

namespace fs = std::filesystem;

constexpr int64_t kDayMs = 24LL * 3600 * 1000;

std::string num(double v, int prec = 6) {
    if (!std::isfinite(v)) return {};
    std::ostringstream o;
    o << std::setprecision(prec) << v;
    return o.str();
}

int64_t ms_of(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

std::string side_name(trend::Pos p) {
    return p == trend::Pos::Short ? "空" : "多";
}

} // namespace

Journal::Journal(std::string dir, int keep_days) : dir_(std::move(dir)), keep_days_(keep_days) {}

std::string Journal::csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    return out + "\"";
}

std::string Journal::utc_time(int64_t ms) {
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::ostringstream o;
    o << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return o.str();
}

std::string Journal::utc_date(int64_t ms) {
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::ostringstream o;
    o << std::put_time(&tmv, "%Y%m%d");
    return o.str();
}

void Journal::append(const std::string& file, const std::string& header, const std::string& lines) {
    // 写失败就丢掉：数据记录层不能反过来影响交易
    try {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        const fs::path p = fs::path(dir_) / file;
        // 新版本改了列：老文件的表头对不上，接着写会错列。每个文件每次运行核对一次，
        // 对不上就把老文件改名（xxx-old-时间.csv）留着，另起一份
        if (header_checked_.insert(file).second && fs::exists(p, ec)) {
            std::string first;
            {
                std::ifstream in(p, std::ios::binary);
                std::getline(in, first);
            }
            if (!first.empty() && first.back() == '\r') first.pop_back();
            if (first != "\xEF\xBB\xBF" + header) {
                const auto now = std::chrono::system_clock::now();
                const std::string stamp = utc_date(ms_of(now)) + "T" +
                                          utc_time(ms_of(now)).substr(11, 8);
                std::string old = file.substr(0, file.rfind('.')) + "-old-" + stamp + ".csv";
                for (char& ch : old) if (ch == ':') ch = '-';
                fs::rename(p, fs::path(dir_) / old, ec);
                // 改名失败（多半是 Excel 开着这个文件）：这一行不写进错列的老文件，下次再试
                if (ec) { header_checked_.erase(file); return; }
            }
        }
        const bool fresh = !fs::exists(p, ec);
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (!f) return;
        if (fresh) f << "\xEF\xBB\xBF" << header << "\n";   // BOM：Excel 直接打开不乱码
        f << lines;
    } catch (...) {}
}

void Journal::prune_locked(int64_t now_ms) {
    const std::string today = utc_date(now_ms);
    if (pruned_day_ == today) return;
    pruned_day_ = today;
    const std::string cutoff = utc_date(now_ms - (int64_t)keep_days_ * kDayMs);
    try {
        std::error_code ec;
        if (!fs::exists(dir_, ec)) return;
        for (const auto& e : fs::directory_iterator(dir_, ec)) {
            const std::string name = e.path().filename().string();
            std::string date;
            if (name.rfind("scan-", 0) == 0)        date = name.substr(5, 8);
            else if (name.rfind("filter-", 0) == 0) date = name.substr(7, 8);
            else continue;
            if (date.size() == 8 && date < cutoff) fs::remove(e.path(), ec);
        }
    } catch (...) {}
}

void Journal::trade(const TrendTrade& t) {
    const int64_t close_ms = ms_of(t.close_time);
    const int64_t open_ms  = t.open_time.time_since_epoch().count() ? ms_of(t.open_time) : 0;
    const double  fee = (std::fabs(t.entry_price) + std::fabs(t.exit_price)) *
                        std::fabs(t.qty) * TrendEngine::kEstFeeRate;
    std::ostringstream l;
    l << utc_time(close_ms) << ","
      << (open_ms ? utc_time(open_ms) : std::string()) << ","
      << (open_ms ? num((close_ms - open_ms) / 3600000.0, 4) : std::string()) << ","
      << csv_field(t.symbol) << "," << side_name(t.side) << ","
      << trend::strategy_name(t.strategy) << "," << csv_field(t.interval) << ","
      << (t.is_auto ? "自动" : "手动") << ","
      << num(t.entry_price, 10) << "," << num(t.exit_price, 10) << "," << num(t.qty, 10) << ","
      << num(t.pnl, 8) << "," << num(fee, 6) << "," << num(t.pnl - fee, 8) << ","
      << num(t.entry_dist_atr, 4) << ","
      << num(t.mfe_usd, 8) << "," << num(t.mae_usd, 8) << ","
      << num(t.mfe_pct, 4) << "," << num(t.mae_pct, 4) << ","
      << num(t.mfe_atr, 4) << "," << num(t.mae_atr, 4) << ","
      << num(t.mfe_r, 4)   << "," << num(t.mae_r, 4)   << ","
      << num(t.entry_vol_ratio, 4) << ","
      << num(t.entry_box_atr, 4) << "," << num(t.entry_taker, 4) << ","
      << csv_field(t.reason) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    append("trades.csv",
           "平仓时间UTC,开仓时间UTC,持仓小时,品种,方向,策略,周期,手动自动,开仓价,平仓价,数量,盈亏U,估算手续费U,净盈亏U,入场离通道边沿ATR,"
           "浮盈亏最高U,浮盈亏最低U,浮盈亏最高%,浮盈亏最低%,浮盈亏最高ATR,浮盈亏最低ATR,"
           "浮盈亏最高R,浮盈亏最低R,入场量比,入场箱体ATR,入场主动买入占比,原因",
           l.str());
}

void Journal::filter(const FilterEvent& e) {
    const int64_t ms = ms_of(e.time);
    std::ostringstream l;
    l << utc_time(ms) << "," << csv_field(e.symbol) << "," << (e.is_long ? "向上" : "向下") << ","
      << (e.is_auto ? "自动" : "手动") << "," << csv_field(e.category) << ","
      << num(e.price, 10) << "," << csv_field(e.detail) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    prune_locked(ms);
    append("filter-" + utc_date(ms) + ".csv", "时间UTC,品种,突破方向,手动自动,卡在,价格,说明", l.str());
}

void Journal::pool(int64_t ms, const std::string& symbol, const std::string& event,
                   const std::string& reason) {
    std::ostringstream l;
    l << utc_time(ms) << "," << csv_field(symbol) << "," << csv_field(event) << "," << csv_field(reason) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    append("pool.csv", "时间UTC,品种,事件,原因", l.str());
}

void Journal::scan(int64_t ms, const std::map<std::string, smart::Features>& candidates,
                   const std::set<std::string>& with_bot,
                   const std::map<std::string, smart::OiStat>& oi) {
    std::ostringstream l;
    for (const auto& [symbol, f] : candidates) {
        l << utc_time(ms) << "," << csv_field(symbol) << "," << num(f.atr_pct, 4) << ","
          << num(f.close, 10) << "," << (with_bot.count(symbol) ? "是" : "");
        if (auto it = oi.find(symbol); it != oi.end()) {
            const auto& stat = it->second;
            l << "," << utc_time(stat.ms) << "," << num(stat.value, 10) << ","
              << num(stat.chg1, 4) << "," << num(stat.chg4, 4) << "," << num(stat.chg24, 4);
        } else l << ",,,,,";
        l << "\n";
    }
    std::lock_guard<std::mutex> lk(mtx_);
    prune_locked(ms);
    append("scan-" + utc_date(ms) + ".csv",
           "时间UTC,品种,ATR%,收盘价,已有bot,持仓量时间UTC,持仓价值U,持仓量变化1h%,持仓量变化4h%,持仓量变化24h%",
           l.str());
}

} // namespace ccbot
