// compat::fromCharsFallback (src/core/from_chars_double.h), the reader of doubles used where Apple's libc++ has no
// floating-point std::from_chars, against std::from_chars where it exists, and compat::fromChars on fixed values
// everywhere.
#include <catch2/catch_test_macros.hpp>

#include "from_chars_double.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <system_error>

// Apple's libc++ for macOS below 26 has no floating-point std::from_chars to compare with.
#if !(defined(__APPLE__) && defined(_LIBCPP_VERSION))
namespace {

struct Read {
    std::errc ec;
    std::ptrdiff_t used;
    std::uint64_t bits;
};

Read viaStd(const std::string& s, std::chars_format fmt)
{
    double v = -7.0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, fmt);
    return {r.ec, r.ptr - s.data(), std::bit_cast<std::uint64_t>(v)};
}

Read viaFallback(const std::string& s, std::chars_format fmt)
{
    double v = -7.0;
    const auto r = ray::compat::fromCharsFallback(s.data(), s.data() + s.size(), v, fmt);
    return {r.ec, r.ptr - s.data(), std::bit_cast<std::uint64_t>(v)};
}

void same(const std::string& s, std::chars_format fmt = std::chars_format::general)
{
    INFO("'" << s << "' fixed " << (fmt == std::chars_format::fixed));
    const Read a = viaStd(s, fmt);
    const Read b = viaFallback(s, fmt);
    CHECK(b.ec == a.ec);
    CHECK(b.used == a.used);
    if (a.ec == std::errc()) {
        CHECK(b.bits == a.bits);
    }
}

} // namespace

TEST_CASE("fromCharsFallback reads what std::from_chars reads", "[core][from_chars]")
{
    for (const char* s : {"0",
                          "-0",
                          "1",
                          "-1",
                          "0.5",
                          ".5",
                          "5.",
                          "-.25",
                          "12.75mm",
                          "3e2",
                          "3E-2",
                          "1e",
                          "1e+",
                          "2.5e+3x",
                          "007",
                          "1.0000000000000002",
                          "0.1",
                          "123456789012345678901234567890",
                          "2.2250738585072014e-308",
                          "1.7976931348623157e308",
                          "  1",
                          "+1",
                          "",
                          "-",
                          ".",
                          "e5",
                          "x",
                          "1,5",
                          "12 34",
                          "-0.000001"})
        same(s);
    for (const char* s : {"1e5", "2.5E3", "12.5", "-3.", ".75 rest"})
        same(s, std::chars_format::fixed);
}

TEST_CASE("fromCharsFallback rounds as std::from_chars on random decimals", "[core][from_chars]")
{
    std::mt19937_64 rng(0x46524f4d43484152ULL);
    for (int i = 0; i < 20000; ++i) {
        std::string s = (rng() % 2 == 0) ? "-" : "";
        const int intDigits = static_cast<int>(rng() % 8);
        const int fracDigits = static_cast<int>(rng() % 19);
        for (int k = 0; k < intDigits; ++k)
            s.push_back(static_cast<char>('0' + rng() % 10));
        if (fracDigits > 0 || intDigits == 0) {
            s.push_back('.');
            for (int k = 0; k < std::max(fracDigits, 1); ++k)
                s.push_back(static_cast<char>('0' + rng() % 10));
        }
        if (rng() % 3 == 0)
            s += "e" + std::to_string(static_cast<int>(rng() % 80) - 40);
        same(s);
    }
}

#endif

TEST_CASE("fromCharsFallback reports a number out of range", "[core][from_chars]")
{
    double v = 3.0;
    const std::string big = "1e400";
    const auto r = ray::compat::fromCharsFallback(big.data(), big.data() + big.size(), v);
    CHECK(r.ec == std::errc::result_out_of_range);
    CHECK(r.ptr == big.data() + big.size());
    CHECK(v == 3.0);
}

TEST_CASE("compat::fromChars reads fixed values", "[core][from_chars]")
{
    struct Case {
        const char* text;
        std::chars_format fmt;
        double value;
        std::ptrdiff_t used;
    };
    for (const Case& c :
         {Case{"0.1", std::chars_format::general, 0.1, 3}, Case{"12.75mm", std::chars_format::general, 12.75, 5},
          Case{"-.25", std::chars_format::general, -0.25, 4}, Case{"2.5e+3x", std::chars_format::general, 2500.0, 6},
          Case{"1e5", std::chars_format::fixed, 1.0, 1}, Case{"123.456 7", std::chars_format::fixed, 123.456, 7}}) {
        INFO(c.text);
        const std::string s = c.text;
        double v = 0;
        const auto r = ray::compat::fromChars(s.data(), s.data() + s.size(), v, c.fmt);
        CHECK(r.ec == std::errc());
        CHECK(r.ptr - s.data() == c.used);
        CHECK(v == c.value);
    }
    double v = 4.0;
    const std::string plus = "+1";
    CHECK(ray::compat::fromChars(plus.data(), plus.data() + plus.size(), v).ec == std::errc::invalid_argument);
    CHECK(v == 4.0);
}
