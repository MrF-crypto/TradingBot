#pragma once
#include <string>
#include <string_view>

namespace ccbot::url {
// Encode a raw UTF-8 parameter value, never a whole query string or an already encoded value.
// Callers construct the final wire payload before HMAC signing.
inline std::string encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
}
