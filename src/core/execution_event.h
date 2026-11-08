#pragma once
#include "core/itrading_client.h"
#include <cstdint>
#include <string>
namespace ccbot {
// Persisted before submission; a result with unknown status is queried, never blindly resent.
struct ExecutionEvent {
    std::string key, bot_id, symbol;
    bool opening = false, is_auto = false, completed = false;
    int direction = 0;
    int64_t at_ms = 0;
    double qty = 0, entry = 0;
    OrderOutcome result;
};
}
