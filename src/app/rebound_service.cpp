#include "app/smart_runner.h"
#include "app/rebound_sampler.h"
#include "core/profit_json.h"

#include <iomanip>
#include <sstream>

namespace ccbot {
namespace {
int64_t close_boundary(int64_t now) {
    return now / rebound::kFiveMinutes * rebound::kFiveMinutes;
}

std::string reading_text(const rebound::Breadth& b) {
    std::ostringstream out;
    out << "有效样本 " << b.valid << '/' << b.pool;
    if (b.valid > 0)
        out << "，15分钟上涨 " << b.up << '/' << b.valid << "（" << std::fixed
            << std::setprecision(1) << b.up_fraction * 100 << "%），涨幅中位数 "
            << std::setprecision(3) << b.median_atr << " ATR";
    if (!b.covered()) out << "，覆盖不足";
    return out.str();
}

std::string symbols_json(const std::set<std::string>& symbols) {
    std::ostringstream out;
    out << '[';
    for (auto it = symbols.begin(); it != symbols.end(); ++it) {
        if (it != symbols.begin()) out << ',';
        out << profit::quote(*it);
    }
    out << ']';
    return out.str();
}

std::set<std::string> read_symbols(simdjson::dom::element root, const char* key) {
    simdjson::dom::array arr;
    if (root[key].get_array().get(arr) != simdjson::SUCCESS) throw std::runtime_error(key);
    std::set<std::string> result;
    for (auto item : arr) {
        std::string_view s;
        if (item.get(s) != simdjson::SUCCESS || s.empty() || !s.ends_with("USDT") ||
            !result.emplace(s).second) throw std::runtime_error(key);
    }
    return result;
}
} // namespace

void SmartRunner::stage_rebound_candidates_locked(const MarketScan& scan, int64_t now) {
    const auto current = close_boundary(now);
    // Preserve the list that was available before this close, even if a new scan
    // finishes before the background worker gets to sample it.
    if (!rebound_state_.paused && rebound_next_close_ms_ && rebound_next_close_ms_ <= current)
        rebound_pool_ = rebound_next_pool_;
    const auto& previous = rebound_next_close_ms_ ? rebound_next_pool_ : rebound_pool_;
    std::set<std::string> next;
    for (const auto& sym : scan.outcome.unknown)
        if (previous.contains(sym)) next.insert(sym);
    for (const auto& [sym, features] : scan.outcome.candidates) next.insert(sym);
    rebound_next_pool_ = std::move(next);
    rebound_next_close_ms_ = current + rebound::kFiveMinutes;
    engine_->set_rebound_breakouts(scan.rebound_short_breakouts);
}

bool SmartRunner::rebound_due_locked(int64_t now) const {
    const auto close = close_boundary(now);
    if (!rebound_sampler_ || cancelled_ || close <= rebound_state_.last_close_ms ||
        now < rebound_retry_ms_) return false;
    if ((!rebound_state_.paused || rebound_pool_.empty()) && rebound_next_close_ms_ && rebound_next_close_ms_ <= close)
        return !rebound_next_pool_.empty();
    return !rebound_pool_.empty();
}

bool SmartRunner::rebound_due(int64_t now) const {
    std::lock_guard lock(mtx_);
    return rebound_due_locked(now);
}

void SmartRunner::refresh_rebound() {
    std::unique_lock fetch(rebound_fetch_mtx_, std::try_to_lock);
    if (!fetch || !client_ || !engine_) return;
    const auto now = client_->market_time_ms();
    const auto close = close_boundary(now);
    std::set<std::string> symbols;
    std::shared_ptr<rebound::Sampler> sampler;
    uint64_t generation;
    {
        std::lock_guard lock(mtx_);
        if (!rebound_due_locked(now)) return;
        if ((!rebound_state_.paused || rebound_pool_.empty()) && rebound_next_close_ms_ && rebound_next_close_ms_ <= close) {
            rebound_pool_ = std::move(rebound_next_pool_);
            rebound_next_close_ms_ = 0;
        }
        sampler = rebound_sampler_;
        symbols = rebound_pool_;
        generation = rebound_generation_.load();
        rebound_retry_ms_ = std::min(close + rebound::kFiveMinutes, now + 30'000);
    }
    const auto obsolete = [&] { return cancelled_ || generation != rebound_generation_; };
    const auto b = sampler->sample(*client_, symbols, close, obsolete);
    std::string notice;
    {
        std::lock_guard lock(mtx_);
        const auto finished = client_->market_time_ms();
        if (obsolete() || close_boundary(finished) != close) return;
        const bool was_missing = rebound_reading_ms_ && !rebound_reading_.covered();
        rebound_reading_ = b;
        rebound_reading_ms_ = close;
        if (!b.covered()) {
            // Retry just missing symbols. An outage cannot count as a recovery close.
            rebound_retry_ms_ = std::min(close + rebound::kFiveMinutes, finished + 30'000);
            if (!was_missing) notice = "急反弹保护：本轮数据不足，保留当前状态";
        } else {
            const auto change = rebound::advance(rebound_state_, close, b);
            if (change == rebound::Transition::Paused) {
                rebound_pool_ = symbols;
                notice = "急反弹保护：暂停新增智能空仓";
            } else if (change == rebound::Transition::Released) {
                notice = "急反弹保护：连续两根5分钟收盘上涨占比低于50%，等待新的1h收盘向下突破";
            } else if (was_missing) notice = "急反弹保护：行情覆盖已恢复";
            if (change != rebound::Transition::None)
                engine_->set_rebound_gate(rebound_state_.paused, rebound_state_.released_ms);
            save_locked();
        }
        if (!notice.empty()) notice += "，" + reading_text(b);
    }
    if (!notice.empty() && log_) log_(notice, false);
}

std::string SmartRunner::rebound_status() const {
    std::lock_guard lock(mtx_);
    if (!rebound_sampler_) return "急反弹保护已关闭";
    std::string text = "急反弹保护：";
    if (rebound_state_.paused) {
        text += "暂停新增智能空仓";
        if (rebound_state_.low_streak) text += "，恢复确认 1/2";
    } else if (rebound_state_.released_ms) text += "监测中，各品种恢复后首笔开空等待新的1h收盘向下突破";
    else text += "监测中";
    if (rebound_reading_ms_)
        text += "，" + Journal::utc_time(rebound_reading_ms_) + " UTC 收盘，" + reading_text(rebound_reading_);
    else if (rebound_state_.last_close_ms)
        text += "，已接续 " + Journal::utc_time(rebound_state_.last_close_ms) + " UTC 收盘断点";
    else text += "，等待候选池及5分钟收盘数据";
    return text;
}

std::string SmartRunner::rebound_checkpoint_locked() const {
    std::ostringstream out;
    out << "{\"paused\":" << std::boolalpha << rebound_state_.paused
        << ",\"last\":" << rebound_state_.last_close_ms << ",\"released\":" << rebound_state_.released_ms
        << ",\"low_streak\":" << rebound_state_.low_streak << ",\"pool\":" << symbols_json(rebound_pool_)
        << ",\"next\":" << symbols_json(rebound_next_pool_) << ",\"next_at\":" << rebound_next_close_ms_
        << ",\"account\":" << profit::quote(client_ ? client_->account_fingerprint() : "") << '}';
    return out.str();
}

void SmartRunner::restore_rebound_checkpoint(const std::string& json) {
    std::lock_guard lock(mtx_);
    try {
        simdjson::dom::parser parser;
        auto root = parser.parse(simdjson::padded_string(json)).value();
        if (!client_ || profit::string(root,"account") != client_->account_fingerprint()) return;
        rebound::State s;
        s.paused = profit::boolean(root,"paused");
        s.last_close_ms = profit::integer(root,"last");
        s.released_ms = profit::integer(root,"released");
        const auto streak = profit::integer(root,"low_streak");
        const auto next_at = profit::integer(root,"next_at");
        auto pool = read_symbols(root,"pool");
        auto next = read_symbols(root,"next");
        if (streak > (s.paused ? 1 : 0) || s.released_ms > s.last_close_ms ||
            s.last_close_ms % rebound::kFiveMinutes || s.released_ms % rebound::kFiveMinutes ||
            next_at % rebound::kFiveMinutes || s.last_close_ms > close_boundary(client_->market_time_ms()) ||
            (s.paused && pool.empty()) || (!next_at && !next.empty()))
            throw std::runtime_error("checkpoint");
        s.low_streak = static_cast<int>(streak);
        rebound_state_ = s;
        rebound_pool_ = std::move(pool); rebound_next_pool_ = std::move(next);
        rebound_next_close_ms_ = next_at;
    } catch (const std::exception&) {
        // A damaged checkpoint may represent an active pause. Re-establish breadth
        // from the next scan and use the ordinary two-close release rule.
        rebound_state_ = {};
        rebound_state_.paused = true;
        rebound_pool_.clear(); rebound_next_pool_.clear(); rebound_next_close_ms_ = 0;
        if (log_) log_("急反弹保护：断点无效，启用后等待重新核实市场广度", false);
    }
}
} // namespace ccbot
