// std::from_chars for double on every platform RayCAD builds for. Header-only, so any module may include it
// (by relative path) without a link dependency; it adds nothing to the public interface in include/ray.
//
// Apple's libc++ ships floating-point std::from_chars only in the libc++ of macOS 26 and later, and marks it
// unavailable when building for an earlier macOS (RayCAD's deployment target is Qt's minimum, 14.4). There this
// reads the same grammar itself and converts the characters it accepted with the standard stream in the classic
// locale; libc++ converts those with strtod_l, which rounds correctly, so the value is the one std::from_chars gives.
// Differences on that path only: "inf", "nan" and hexadecimal are refused (every caller refuses a non-finite value
// anyway), and a value that underflows to zero is reported as out of range. Everywhere else this is
// std::from_chars itself.
#pragma once

#include <charconv>
#include <system_error>

#if defined(__APPLE__) && defined(_LIBCPP_VERSION)
#include <locale>
#include <sstream>
#include <string>
#define RAY_FROM_CHARS_DOUBLE_FALLBACK 1
#endif

namespace ray::compat {

inline std::from_chars_result fromChars(const char* first, const char* last, double& value,
                                        std::chars_format fmt = std::chars_format::general)
{
#if defined(RAY_FROM_CHARS_DOUBLE_FALLBACK)
    const auto isDigit = [](char c) {
        return c >= '0' && c <= '9';
    };
    const char* p = first;
    if (p < last && *p == '-') {
        ++p;
    }
    int digits = 0;
    while (p < last && isDigit(*p)) {
        ++p;
        ++digits;
    }
    if (p < last && *p == '.') {
        ++p;
        while (p < last && isDigit(*p)) {
            ++p;
            ++digits;
        }
    }
    if (digits == 0) {
        return {first, std::errc::invalid_argument};
    }
    if (fmt != std::chars_format::fixed && p < last && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q < last && (*q == '+' || *q == '-')) {
            ++q;
        }
        if (q < last && isDigit(*q)) {
            while (q < last && isDigit(*q)) {
                ++q;
            }
            p = q; // an "e" without digits after it is not part of the number
        }
    }
    std::istringstream in(std::string(first, p));
    in.imbue(std::locale::classic());
    double v = 0;
    in >> v;
    if (in.fail()) {
        return {p, std::errc::result_out_of_range}; // the grammar was checked above: only the range can fail
    }
    value = v;
    return {p, std::errc()};
#else
    return std::from_chars(first, last, value, fmt);
#endif
}

} // namespace ray::compat
