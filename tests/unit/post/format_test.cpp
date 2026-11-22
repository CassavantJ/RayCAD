// formatNumber (post.h, src/post/format.cpp): snaps to integer nanometres, rounds half to even with integers, never
// "-0", no locale.
#include "format.h"

#include <ray/post.h>

#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <cstdint>
#include <limits>
#include <string>

using ray::post::formatNumber;
using ray::post::NumberFormat;

namespace {

constexpr NumberFormat kMm{3, true, true, true, 1.0};      // "X12.5", "X12."
constexpr NumberFormat kFixed{3, false, false, true, 1.0}; // "12.500"

// An integer-only reference: nm to 3 decimals of a millimetre, half to even, trailing zeros trimmed, point forced.
std::string reference(std::int64_t nm)
{
    std::int64_t q = nm / 1000;
    const std::int64_t r = nm % 1000;
    const std::int64_t a = r < 0 ? -r : r;
    if (2 * a > 1000 || (2 * a == 1000 && q % 2 != 0)) {
        q += nm < 0 ? -1 : 1;
    }
    std::string s = q < 0 ? "-" : "";
    const std::int64_t m = q < 0 ? -q : q;
    s += std::to_string(m / 1000) + ".";
    std::string frac = std::to_string(1000 + m % 1000).substr(1);
    while (!frac.empty() && frac.back() == '0') {
        frac.pop_back();
    }
    return s + frac;
}

} // namespace

TEST_CASE("formatNumber rounds half to even on the nanometre grid", "[post][format]")
{
    CHECK(formatNumber(12.3455, kMm) == "12.346"); // 12345.5 um: 12345 is odd, so up
    CHECK(formatNumber(12.3445, kMm) == "12.344"); // 12344.5 um: 12344 is even, so down
    CHECK(formatNumber(-12.3455, kMm) == "-12.346");
    CHECK(formatNumber(-12.3445, kMm) == "-12.344");
    CHECK(formatNumber(0.0005, kMm) == "0.");     // a tie onto 0 (even)
    CHECK(formatNumber(0.0015, kMm) == "0.002");
    // 2.675 is 2.67499999999999982236431605997495353221893310546875 as a double; printf("%.2f") gives 2.67. The
    // host snaps to 2675000 nm first, so the tie is exact and goes to even: 2.68.
    CHECK(formatNumber(2.675, NumberFormat{2, true, true, true, 1.0}) == "2.68");
    CHECK(formatNumber(1.0000004, kFixed) == "1.000");
}

TEST_CASE("formatNumber never writes minus zero", "[post][format]")
{
    CHECK(formatNumber(-0.0, kMm) == "0.");
    CHECK(formatNumber(-0.0004, kMm) == "0.");
    CHECK(formatNumber(-0.0005, kMm) == "0."); // tie onto 0
    CHECK(formatNumber(-0.0004, kFixed) == "0.000");
    CHECK(formatNumber(-1e-9, NumberFormat{0, true, false, true, 1.0}) == "0");
}

TEST_CASE("formatNumber options", "[post][format]")
{
    CHECK(formatNumber(12.5, kMm) == "12.5");
    CHECK(formatNumber(12.5, kFixed) == "12.500");
    CHECK(formatNumber(12.0, kMm) == "12.");
    CHECK(formatNumber(12.0, NumberFormat{3, true, false, true, 1.0}) == "12");
    CHECK(formatNumber(0.25, NumberFormat{3, true, true, false, 1.0}) == ".25");
    CHECK(formatNumber(-0.25, NumberFormat{3, true, true, false, 1.0}) == "-.25");
    CHECK(formatNumber(0.25, NumberFormat{3, true, true, true, 1.0}) == "0.25");
    CHECK(formatNumber(0.0, NumberFormat{3, true, false, false, 1.0}) == "0");
    CHECK(formatNumber(1234.5678, NumberFormat{0, true, true, true, 1.0}) == "1235.");
    CHECK(formatNumber(0.000001, NumberFormat{6, true, true, true, 1.0}) == "0.000001");
    CHECK(formatNumber(0.0000004, NumberFormat{6, true, true, true, 1.0}) == "0.");
    CHECK(formatNumber(1.5, NumberFormat{9, true, true, true, 1.0}) == "1.5");  // decimals clamp to 6
    CHECK(formatNumber(1.5, NumberFormat{-2, true, true, true, 1.0}) == "2."); // and to 0
    // scale: the value is multiplied first; 0 (a value-initialised format) means 1.
    CHECK(formatNumber(1.25, NumberFormat{3, true, true, true, 1000.0}) == "1250.");
    CHECK(formatNumber(1.25, NumberFormat{3, true, true, true, 0.0}) == "1.25");
    CHECK(formatNumber(9007199.254740, NumberFormat{6, true, true, true, 1.0}) == "9007199.25474");
}

TEST_CASE("formatNumber gives no text for values it cannot format", "[post][format]")
{
    CHECK(formatNumber(std::numeric_limits<double>::quiet_NaN(), kMm).empty());
    CHECK(formatNumber(std::numeric_limits<double>::infinity(), kMm).empty());
    CHECK(formatNumber(-std::numeric_limits<double>::infinity(), kMm).empty());
    CHECK(formatNumber(2e12, kMm).empty());
    CHECK(formatNumber(1e12, NumberFormat{0, true, false, true, 1.0}) == "1000000000000");
}

TEST_CASE("formatNumber equals an integer-only reference for every nanometre value it is given", "[post][format]")
{
    // A fixed-seed walk over values from 0 to 2 m, a quarter of them on a tie at 3 decimals and a quarter on whole
    // micrometres (spike S8's mix). The double the host would hand a script is nm / 1e6.
    std::uint64_t state = 0x5EED5EED5EED5EEDull;
    auto next = [&] {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    for (int i = 0; i < 200000; ++i) {
        std::int64_t nm = static_cast<std::int64_t>(next() % 2'000'000'000ull) - 1'000'000'000;
        switch (next() & 3) {
        case 0:
            nm = (nm / 1000) * 1000 + (nm < 0 ? -500 : 500);
            break;
        case 1:
            nm = (nm / 1000) * 1000;
            break;
        default:
            break;
        }
        const double mmValue = static_cast<double>(nm) / 1e6;
        const std::string got = formatNumber(mmValue, kMm);
        if (got != reference(nm)) {
            FAIL("nm " << nm << ": " << got << " != " << reference(nm));
        }
    }
}

TEST_CASE("formatNumber is not affected by the C locale", "[post][format]")
{
    const char* before = std::setlocale(LC_ALL, nullptr);
    const std::string saved = before ? before : "C";
    // A locale with a decimal comma, where one is installed; the result must not change either way.
    const bool switched = std::setlocale(LC_ALL, "de_DE.UTF-8") != nullptr || std::setlocale(LC_ALL, "German") != nullptr;
    CHECK(formatNumber(1234.5, kMm) == "1234.5");
    CHECK(formatNumber(-0.125, kFixed) == "-0.125");
    std::setlocale(LC_ALL, saved.c_str());
    INFO("decimal-comma locale available: " << switched);
}

TEST_CASE("divRoundHalfEven and appendFixed", "[post][format]")
{
    using namespace ray::post::fmt;
    CHECK(divRoundHalfEven(25, 10) == 2);
    CHECK(divRoundHalfEven(35, 10) == 4);
    CHECK(divRoundHalfEven(-25, 10) == -2);
    CHECK(divRoundHalfEven(-35, 10) == -4);
    CHECK(divRoundHalfEven(26, 10) == 3);
    CHECK(divRoundHalfEven(-24, 10) == -2);
    std::string s;
    appendFixed(s, std::numeric_limits<std::int64_t>::min(), 0, true, false, true);
    CHECK(s == "-9223372036854775808");
}
