#pragma once
#include <clocale>
#include <string>
#include <vector>

// Use only before starting workers and after joining them. The locale is process-wide.
// Windows .1252 and macOS UTF-8 exercise the original byte-wise toupper defect.
template<class F> void in_symbol_locales(F test) {
    const std::string original = std::setlocale(LC_CTYPE, nullptr);
    for (const char* name : {"C", "", "en_US.UTF-8", "zh_CN.UTF-8", ".1252"}) {
        if (std::setlocale(LC_CTYPE, name)) test();
    }
    std::setlocale(LC_CTYPE, original.c_str());
}

inline const std::vector<std::string> chinese_symbols = {"币安人生USDT", "牛来USDT", "龙虾USDT"};
