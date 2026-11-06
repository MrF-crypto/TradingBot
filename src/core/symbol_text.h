#pragma once
#include <string>
#include <string_view>

namespace ccbot::symbol_text {
// Exchange identifiers are UTF-8. C locale case conversion operates on bytes,
// and can turn Chinese lead bytes into unrelated Latin letters on macOS.
inline std::string upper_ascii(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}
inline bool ascii_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
// Reject broken UTF-8 and replacement characters left by older Qt restores.
// Never guess a symbol from lossy text: it may refer to an existing position.
inline bool intact(std::string_view s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i++]);
        if (lead < 0x80) {
            if (lead <= 0x20 || lead == 0x7f) return false;
            continue;
        }
        unsigned code = 0, minimum = 0;
        size_t count = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; code = lead & 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; code = lead & 0x0f; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; code = lead & 0x07; minimum = 0x10000; }
        else return false;
        if (count > s.size() - i) return false;
        while (count-- > 0) {
            const auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) || code == 0xfffd)
            return false;
    }
    return true;
}
} // namespace ccbot::symbol_text
