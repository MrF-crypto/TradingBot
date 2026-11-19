#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace ccbot {

// Exchange-compatible ID shared by engine checkpoints and the actual market POST.
inline std::string make_client_order_id() {
    static const std::string run_tag = [] {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::random_device random;
        std::ostringstream tag;
        tag << std::hex << ms << '-' << static_cast<uint32_t>(random());
        return tag.str();
    }();
    static std::atomic<uint32_t> seq{0};
    return "cb-" + run_tag + "-" + std::to_string(seq.fetch_add(1));
}

} // namespace ccbot
