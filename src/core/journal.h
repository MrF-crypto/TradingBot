#pragma once
#include "core/smart_mode.h"
#include "core/trend_engine.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

// 数据记录层：把实盘里"事后要拿来分析"的东西写成 CSV，用来校准智能模式与过滤规则。
// 只写不读、不参与任何交易决策；写失败就丢掉这一行，绝不影响交易。
//
// CSV 记录交易、候选变更、扫描快照及被过滤的突破；表头改变时自动归档旧文件。
// 扫描保留 ATR、收盘价与 OI 观察值，不包含候选排名或分数。
namespace ccbot {

class Journal {
public:
    explicit Journal(std::string dir, int keep_days = 60);

    void trade (const TrendTrade& t);
    void filter(const FilterEvent& e);
    void pool  (int64_t ms, const std::string& symbol, const std::string& event,
                const std::string& reason);
    // oi：合格币的持仓量（没拿到的不在里面，那几列留空）
    void scan  (int64_t ms, const std::map<std::string, smart::Features>& candidates,
                const std::set<std::string>& with_bot,
                const std::map<std::string, smart::OiStat>& oi = {});
    const std::string& dir() const { return dir_; }

    // 工具（测试也用）
    static std::string csv_field(const std::string& s);   // 含逗号/引号/换行时加引号
    static std::string utc_time(int64_t ms);              // "2026-10-10 08:00:00"
    static std::string utc_date(int64_t ms);              // "20261010"

private:
    void append(const std::string& file, const std::string& header, const std::string& line);
    void prune_locked(int64_t now_ms);

    std::string dir_;
    int         keep_days_;
    std::mutex  mtx_;
    std::string pruned_day_;   // 今天清理过没有
    std::set<std::string> header_checked_;   // 本次运行已核对过表头的文件
};

} // namespace ccbot
