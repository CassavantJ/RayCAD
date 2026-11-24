// Number formatting and reading for canonical JSON (src/doc/json_number.h).
#include "json_number.h"

#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <system_error>

using ray::doc::detail::formatDouble;
using ray::doc::detail::formatFloat;
using ray::doc::detail::parseDouble;
using ray::doc::detail::parseFloat;

namespace {

double fromBits(std::uint64_t bits)
{
    return std::bit_cast<double>(bits);
}

// The canonical text of one number, through the public entry point.
std::string canonicalNumber(const std::string& text)
{
    const std::string out = ray::doc::canonicalJson("[" + text + "]");
    REQUIRE(out.size() >= 2);
    return out.substr(1, out.size() - 2);
}

} // namespace

TEST_CASE("pinned number formats", "[doc][number]")
{
    CHECK(formatDouble(0.0) == "0");
    CHECK(formatDouble(-0.0) == "0");
    CHECK(formatDouble(1.0) == "1");
    CHECK(formatDouble(-1.0) == "-1");
    CHECK(formatDouble(100.0) == "100");
    CHECK(formatDouble(123456789.0) == "123456789");
    CHECK(formatDouble(4.0) == "4");
    CHECK(formatDouble(0.1) == "0.1");
    CHECK(formatDouble(0.5) == "0.5");
    CHECK(formatDouble(-1.5) == "-1.5");
    CHECK(formatDouble(25.4) == "25.4");
    CHECK(formatDouble(4.35) == "4.35");
    CHECK(formatDouble(0.1 + 0.2) == "0.30000000000000004");
    CHECK(formatDouble(1.0 / 3.0) == "0.3333333333333333");
    CHECK(formatDouble(2.0 / 3.0) == "0.6666666666666666");
    CHECK(formatDouble(1e-6) == "0.000001");
    CHECK(formatDouble(1e-7) == "1e-7");
    CHECK(formatDouble(1.5e-7) == "1.5e-7");
    CHECK(formatDouble(-1e-7) == "-1e-7");
    CHECK(formatDouble(1e20) == "100000000000000000000");
    CHECK(formatDouble(123e18) == "123000000000000000000");
    CHECK(formatDouble(1e21) == "1e+21");
    CHECK(formatDouble(1.5e21) == "1.5e+21");
    CHECK(formatDouble(1e23) == "1e+23");
    CHECK(formatDouble(1e300) == "1e+300");
    CHECK(formatDouble(9007199254740992.0) == "9007199254740992");
    CHECK(formatDouble(std::numeric_limits<double>::max()) == "1.7976931348623157e+308");
    CHECK(formatDouble(std::numeric_limits<double>::min()) == "2.2250738585072014e-308");
    CHECK(formatDouble(std::numeric_limits<double>::denorm_min()) == "5e-324");
    CHECK(formatDouble(-std::numeric_limits<double>::denorm_min()) == "-5e-324");
    CHECK(formatDouble(std::numeric_limits<double>::infinity()) == "null");
    CHECK(formatDouble(std::numeric_limits<double>::quiet_NaN()) == "null");
}

TEST_CASE("RFC 8785 number samples", "[doc][number]")
{
    // RFC 8785 appendix B: IEEE-754 bit patterns and their canonical text.
    const std::pair<std::uint64_t, const char*> samples[] = {
        {0x0000000000000000, "0"},
        {0x8000000000000000, "0"},
        {0x0000000000000001, "5e-324"},
        {0x8000000000000001, "-5e-324"},
        {0x7fefffffffffffff, "1.7976931348623157e+308"},
        {0xffefffffffffffff, "-1.7976931348623157e+308"},
        {0x4340000000000000, "9007199254740992"},
        {0xc340000000000000, "-9007199254740992"},
        {0x4430000000000000, "295147905179352830000"},
        {0x44b52d02c7e14af5, "9.999999999999997e+22"},
        {0x44b52d02c7e14af6, "1e+23"},
        {0x44b52d02c7e14af7, "1.0000000000000001e+23"},
        {0x444b1ae4d6e2ef4e, "999999999999999700000"},
        {0x444b1ae4d6e2ef4f, "999999999999999900000"},
        {0x444b1ae4d6e2ef50, "1e+21"},
        {0x3eb0c6f7a0b5ed8c, "9.999999999999997e-7"},
        {0x3eb0c6f7a0b5ed8d, "0.000001"},
        {0x41b3de4355555553, "333333333.3333332"},
        {0x41b3de4355555554, "333333333.33333325"},
        {0x41b3de4355555555, "333333333.3333333"},
        {0x41b3de4355555556, "333333333.3333334"},
        {0x41b3de4355555557, "333333333.33333343"},
        {0xbecbf647612f3696, "-0.0000033333333333333333"},
        {0x43143ff3c1cb0959, "1424953923781206.2"},
    };
    for (const auto& [bits, text] : samples) {
        INFO(text);
        CHECK(formatDouble(fromBits(bits)) == text);
        const auto back = parseDouble(text);
        REQUIRE(back.has_value());
        CHECK(std::bit_cast<std::uint64_t>(*back) == (bits == 0x8000000000000000 ? 0 : bits));
    }
}

TEST_CASE("canonical JSON prints numbers in the canonical form", "[doc][number]")
{
    CHECK(canonicalNumber("0") == "0");
    CHECK(canonicalNumber("-0") == "0");
    CHECK(canonicalNumber("-0.0e5") == "0");
    CHECK(canonicalNumber("1.0") == "1");
    CHECK(canonicalNumber("1E2") == "100");
    CHECK(canonicalNumber("1e+2") == "100");
    CHECK(canonicalNumber("0.1") == "0.1");
    CHECK(canonicalNumber("0.10000000000000000555") == "0.1");
    CHECK(canonicalNumber("1e-7") == "1e-7");
    CHECK(canonicalNumber("0.0000001") == "1e-7");
    CHECK(canonicalNumber("1e21") == "1e+21");
    CHECK(canonicalNumber("1000000000000000000000") == "1e+21");
    CHECK(canonicalNumber("123456789012345678901234567890") == "1.2345678901234568e+29");
    CHECK(canonicalNumber("1e-400") == "0");
    CHECK(canonicalNumber("-1e-400") == "0");
    CHECK(canonicalNumber("9007199254740993") == "9007199254740992"); // tie, rounds to even
    CHECK(canonicalNumber("9007199254740995") == "9007199254740996");
    CHECK(canonicalNumber("2.4703282292062327e-324") == "0");      // just below half the smallest subnormal
    CHECK(canonicalNumber("2.4703282292062328e-324") == "5e-324"); // just above
    CHECK(canonicalNumber("1.7976931348623158e308") == "1.7976931348623157e+308");
    CHECK(ray::doc::canonicalJson("[1.7976931348623159e308]").empty()); // rounds to infinity
    CHECK(ray::doc::canonicalJson("[1e400]").empty());
}

TEST_CASE("reading is correctly rounded", "[doc][number]")
{
    CHECK(parseDouble("0.1") == 0.1);
    CHECK(parseDouble("25.4") == 25.4);
    CHECK(parseDouble("-2.5e-3") == -2.5e-3);
    CHECK(parseDouble("2.2250738585072011e-308") == 2.2250738585072011e-308);
    CHECK(parseDouble("4.9406564584124654e-324") == std::numeric_limits<double>::denorm_min());
    // 800 significant digits and more: the digits after the cut still decide the rounding.
    const std::string half = "9007199254740993" + std::string(1000, '0'); // 2^53 + 1 exactly halfway, times 10^1000
    CHECK(!parseDouble(half).has_value());                                // too large
    // 1 + 2^-53 is exactly halfway between 1 and the next double.
    const std::string tie = "1.00000000000000011102230246251565404236316680908203125" + std::string(900, '0');
    const std::string above = tie + "1";
    CHECK(parseDouble(tie) == 1.0);
    CHECK(parseDouble(above) == 1.0000000000000002);
    // Not JSON numbers.
    for (const char* bad : {"", "-", "+1", "01", "1.", ".5", "1e", "1e+", "0x10", "1 ", " 1", "NaN", "Infinity"}) {
        INFO(bad);
        CHECK_FALSE(parseDouble(bad).has_value());
    }
}

TEST_CASE("floats print short and read back exactly", "[doc][number]")
{
    CHECK(formatFloat(0.72f) == "0.72");
    CHECK(formatFloat(1.0f) == "1");
    CHECK(formatFloat(0.1f) == "0.1");
    CHECK(formatFloat(-0.0f) == "0");
    CHECK(formatFloat(std::numeric_limits<float>::max()) == "3.4028235e+38");
    CHECK(formatFloat(std::numeric_limits<float>::denorm_min()) == "1e-45");
    CHECK(parseFloat("0.72") == 0.72f);
    CHECK_FALSE(parseFloat("3.5e38").has_value());

    std::mt19937 rng(20261118u);
    for (int i = 0; i < 20000; ++i) {
        const auto bits = static_cast<std::uint32_t>(rng());
        const float f = std::bit_cast<float>(bits);
        if (!(f == f) || f - f != 0.0f) {
            continue; // NaN or infinity
        }
        const std::string text = formatFloat(f);
        const auto back = parseFloat(text);
        REQUIRE(back.has_value());
        INFO(text);
        CHECK(std::bit_cast<std::uint32_t>(*back) == (f == 0.0f ? 0u : bits));
        // Written through a double (as Body colours are), the canonical text is the float's text.
        const auto asDouble = parseDouble(text);
        REQUIRE(asDouble.has_value());
        CHECK(formatDouble(*asDouble) == text);
    }
}

TEST_CASE("random doubles round-trip through the shortest text", "[doc][number]")
{
    std::mt19937_64 rng(0x5241594341440001ULL);
    for (int i = 0; i < 20000; ++i) {
        const std::uint64_t bits = rng();
        const double d = fromBits(bits);
        if (!(d == d) || d - d != 0.0) {
            continue;
        }
        const std::string text = formatDouble(d);
        const auto back = parseDouble(text);
        REQUIRE(back.has_value());
        INFO(text);
        CHECK(std::bit_cast<std::uint64_t>(*back) == (d == 0.0 ? 0u : bits));
        CHECK(text.size() <= 25); // e.g. -0.0000012676439047293855
    }
}

// Not with Apple's libc++ for macOS below 26: its floating-point std::from_chars is unavailable there.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !(defined(__APPLE__) && defined(_LIBCPP_VERSION))
// The standard library's shortest round-trip conversion picks the same digits (closest, ties to even), so where
// it exists it is an independent check of ours.
namespace {

// ECMAScript layout of shortest digits, written independently of the code under test, for the cross-checks.
std::string esLayout(bool negative, const std::string& digits, int n)
{
    const int k = static_cast<int>(digits.size());
    std::string out = negative ? "-" : "";
    if (k <= n && n <= 21) {
        return out + digits + std::string(static_cast<std::size_t>(n - k), '0');
    }
    if (0 < n && n <= 21) {
        return out + digits.substr(0, static_cast<std::size_t>(n)) + "." + digits.substr(static_cast<std::size_t>(n));
    }
    if (-6 < n && n <= 0) {
        return out + "0." + std::string(static_cast<std::size_t>(-n), '0') + digits;
    }
    out += digits.substr(0, 1);
    if (k > 1) {
        out += "." + digits.substr(1);
    }
    const int e = n - 1;
    return out + "e" + (e < 0 ? "-" : "+") + std::to_string(e < 0 ? -e : e);
}

template <class T>
std::string viaToChars(T value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    REQUIRE(result.ec == std::errc());
    const std::string s(buffer, result.ptr);
    // "-d.ddde+XX"
    const bool negative = s[0] == '-';
    const std::size_t e = s.find('e');
    std::string digits;
    for (std::size_t i = negative ? 1 : 0; i < e; ++i) {
        if (s[i] != '.') {
            digits.push_back(s[i]);
        }
    }
    const int exponent = std::stoi(s.substr(e + 1));
    if (digits == "0") {
        return "0";
    }
    return esLayout(negative, digits, exponent + 1);
}

} // namespace

TEST_CASE("formatting agrees with std::to_chars", "[doc][number]")
{
    std::mt19937_64 rng(0x5241594341440002ULL);
    for (int i = 0; i < 50000; ++i) {
        const std::uint64_t bits = rng();
        // Half the samples from random bit patterns, half from short decimals (where ties and short outputs live).
        double d = fromBits(bits);
        if (i % 2 == 1) {
            d = static_cast<double>(static_cast<std::int64_t>(bits % 2000001) - 1000000) / 1000.0;
        }
        if (!(d == d) || d - d != 0.0) {
            continue;
        }
        INFO(bits);
        CHECK(formatDouble(d) == viaToChars(d));
        const auto f = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        if (f == f && f - f == 0.0f) {
            CHECK(formatFloat(f) == viaToChars(f));
        }
    }
    for (std::uint64_t bits = 1; bits < 2000; ++bits) {
        CHECK(formatDouble(fromBits(bits)) == viaToChars(fromBits(bits))); // subnormals
    }
    for (int e = 0; e < 2046; ++e) {
        const double d = fromBits(static_cast<std::uint64_t>(e + 1) << 52); // powers of two: unequal gaps
        CHECK(formatDouble(d) == viaToChars(d));
    }
}

TEST_CASE("reading agrees with std::from_chars", "[doc][number]")
{
    std::mt19937_64 rng(0x5241594341440003ULL);
    for (int i = 0; i < 50000; ++i) {
        std::string text = (rng() % 2 == 0) ? "-" : "";
        const int digits = 1 + static_cast<int>(rng() % 25);
        text.push_back(static_cast<char>('1' + rng() % 9));
        if (digits > 1) {
            text.push_back('.');
            for (int k = 1; k < digits; ++k) {
                text.push_back(static_cast<char>('0' + rng() % 10));
            }
        }
        text += "e" + std::to_string(static_cast<int>(rng() % 660) - 340);
        double expected = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), expected);
        INFO(text);
        const auto ours = parseDouble(text);
        if (result.ec == std::errc::result_out_of_range) {
            // Overflow is an error for us; underflow to zero gives a (signed) zero.
            if (ours.has_value()) {
                CHECK(*ours == 0.0);
            }
            continue;
        }
        REQUIRE(result.ec == std::errc());
        REQUIRE(ours.has_value());
        CHECK(std::bit_cast<std::uint64_t>(*ours) == std::bit_cast<std::uint64_t>(expected));
    }
}

namespace {

// Reads `text` with std::from_chars and with parseDouble; both must give the same bits (or both report overflow).
void readsLikeTheStandard(const std::string& text)
{
    INFO(text);
    double expected = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), expected);
    const auto ours = parseDouble(text);
    if (result.ec == std::errc::result_out_of_range) {
        if (ours.has_value()) {
            CHECK(*ours == 0.0); // underflow: a signed zero; overflow is an error for us
        }
        return;
    }
    REQUIRE(result.ec == std::errc());
    REQUIRE(result.ptr == text.data() + text.size());
    REQUIRE(ours.has_value());
    CHECK(std::bit_cast<std::uint64_t>(*ours) == std::bit_cast<std::uint64_t>(expected));
}

// formatDouble agrees with std::to_chars and reads back to the same bits.
void roundTrips(double d)
{
    INFO(std::bit_cast<std::uint64_t>(d));
    const std::string text = formatDouble(d);
    CHECK(text == viaToChars(d));
    const auto back = parseDouble(text);
    REQUIRE(back.has_value());
    CHECK(std::bit_cast<std::uint64_t>(*back) == std::bit_cast<std::uint64_t>(d == 0.0 ? 0.0 : d));
}

} // namespace

TEST_CASE("extreme and long numbers convert exactly in both directions", "[doc][number]")
{
    using limits = std::numeric_limits<double>;
    // The value a GCC debug run was reading when a stack-protector abort hit the process (it is read correctly; the
    // abort did not come from here).
    readsLikeTheStandard("7.08204361320760e-99");
    roundTrips(7.08204361320760e-99);
    for (const double d : {limits::min(), limits::denorm_min(), limits::max(), limits::lowest(), -limits::denorm_min(),
                           limits::epsilon(), 1e-99, 1e-308, 2.2250738585072009e-308 /* largest subnormal */,
                           2.2250738585072014e-308, 4.9406564584124654e-324, 1.7976931348623157e308, 1e308, 1e22, 1e23,
                           9007199254740993.0, 0.1, 5e-324, 2.4703282292062328e-324}) {
        roundTrips(d);
        roundTrips(-d);
    }
    // Neighbours of the boundaries.
    for (const double d : {limits::min(), limits::max(), 1e-99, 1e-308}) {
        roundTrips(std::nextafter(d, 0.0));
        if (d != limits::max()) {
            roundTrips(std::nextafter(d, limits::infinity())); // above DBL_MAX is infinity, which JSON cannot hold
        }
    }
    // Every bit pattern around 1e-99 and the bottom of the subnormals.
    const std::uint64_t around = std::bit_cast<std::uint64_t>(7.08204361320760e-99);
    for (std::uint64_t b = around - 500; b < around + 500; ++b) {
        roundTrips(fromBits(b));
    }
    for (std::uint64_t b = 1; b < 300; ++b) {
        roundTrips(fromBits(b));
    }

    // Texts: 17 and more significant digits, the exact (767-digit) expansion of the smallest subnormal, long runs of
    // leading and trailing zeros, long exponents, halfway cases.
    for (const char* text : {"7.08204361320760e-99",
                             "4.9406564584124654e-324",
                             "2.4703282292062327e-324",
                             "2.4703282292062328e-324",
                             "2.2250738585072011e-308",
                             "2.2250738585072012e-308",
                             "1.7976931348623157e308",
                             "1.7976931348623158e308",
                             "1.7976931348623159e308",
                             "9007199254740993",
                             "9007199254740992.5000000000000000000000001",
                             "0.1000000000000000055511151231257827021181583404541015625",
                             "1e-400",
                             "1e400",
                             "1e-324",
                             "1e-325",
                             "123456789012345678901234567890e-300",
                             "0.000000000000000000000000000000000000000000000001e-260",
                             "1e0000000000000000000000000000000000000000000000000000000000001",
                             "1e-0000000000000000000000000000000099",
                             "1000000000000000000000000000000000000000000000000000000000000e-61",
                             "-0",
                             "-0.0e-999999999999999"}) {
        readsLikeTheStandard(text);
    }
    std::string exact = "0.";
    exact.append(323, '0');
    exact += "49406564584124654417656879286822137236505980"; // DBL_TRUE_MIN, first digits ...
    for (int k = 0; k < 800; ++k) {
        exact.push_back(static_cast<char>('0' + k % 10)); // ... then far more digits than any double needs
    }
    readsLikeTheStandard(exact);
    std::string zeros = "0." + std::string(5000, '0') + "1e5000";
    readsLikeTheStandard(zeros);
    std::string ones(2000, '1');
    readsLikeTheStandard(ones + "e-2100");
    readsLikeTheStandard(ones + std::string(3000, '0') + "e-4700");
}

// Not part of the regular run (about a minute in a debug build): run with "[.number-fuzz]" under ASan or a hardened
// GCC build.
TEST_CASE("number fuzz: 300,000 random bit patterns and texts", "[.number-fuzz]")
{
    std::mt19937_64 rng(0x5241594341440009ULL);
    for (int i = 0; i < 300000; ++i) {
        const double d = fromBits(rng());
        if (!(d == d) || d - d != 0.0) {
            continue;
        }
        const std::string text = formatDouble(d);
        const auto back = parseDouble(text);
        REQUIRE(back.has_value());
        REQUIRE(std::bit_cast<std::uint64_t>(*back) == std::bit_cast<std::uint64_t>(d == 0.0 ? 0.0 : d));
        // A random JSON number: 2 to 41 significant digits (JSON needs a digit after the point), any exponent.
        std::string t = (rng() % 2 == 0) ? "-" : "";
        t.push_back(static_cast<char>('1' + rng() % 9));
        t.push_back('.');
        for (int k = 1 + static_cast<int>(rng() % 40); k > 0; --k) {
            t.push_back(static_cast<char>('0' + rng() % 10));
        }
        t += "e" + std::to_string(static_cast<int>(rng() % 700) - 350);
        INFO(t);
        double expected = 0;
        const auto result = std::from_chars(t.data(), t.data() + t.size(), expected);
        const auto ours = parseDouble(t);
        if (result.ec == std::errc()) {
            REQUIRE(ours.has_value());
            REQUIRE(std::bit_cast<std::uint64_t>(*ours) == std::bit_cast<std::uint64_t>(expected));
        }
    }
}
#endif
