// toNm / toMm, Xform::identity, formatLength and parseLength.
#include <catch2/catch_test_macros.hpp>

#include <ray/core.h>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {

// Independent reference for toNm: the exact decimal expansion of the double (std::to_chars with enough digits
// prints it exactly), shifted six places, rounded half to even on the digits.
ray::Nm referenceToNm(double mm)
{
    static char buffer[1200];
    const auto printed = std::to_chars(buffer, buffer + sizeof buffer, std::fabs(mm), std::chars_format::fixed, 1080);
    REQUIRE(printed.ec == std::errc());
    const std::string text(buffer, printed.ptr);
    const std::size_t point = text.find('.');
    REQUIRE(point != std::string::npos);
    std::string whole = text.substr(0, point) + text.substr(point + 1, 6);
    const std::string rest = text.substr(point + 7);
    bool up = false;
    if (rest[0] > '5') {
        up = true;
    } else if (rest[0] == '5') {
        const bool aboveHalf = rest.find_first_not_of('0', 1) != std::string::npos;
        up = aboveHalf || (whole.back() - '0') % 2 == 1;
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(whole.data(), whole.data() + whole.size(), value);
    REQUIRE(parsed.ec == std::errc());
    if (up) {
        ++value;
    }
    return mm < 0 ? -value : value;
}

std::uint64_t g_state = 0x746f4e6d74657374ULL;

std::uint64_t next()
{
    std::uint64_t z = (g_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

double parsed(std::string_view text, ray::UnitSystem system = ray::UnitSystem::Metric)
{
    const ray::Result<double> result = ray::parseLength(text, system);
    INFO("parseLength(\"" << std::string(text) << "\")");
    REQUIRE(result.has_value());
    return *result;
}

bool rejected(std::string_view text, ray::UnitSystem system = ray::UnitSystem::Metric)
{
    const ray::Result<double> result = ray::parseLength(text, system);
    if (result.has_value()) {
        return false;
    }
    return result.error().code == ray::ErrorCode::InvalidArgument && !result.error().message.empty();
}

} // namespace

TEST_CASE("toNm rounds half to even", "[units]")
{
    // m / 128 mm with m odd is exactly (m * 7812.5) nm: a true tie.
    CHECK(ray::toNm(1.0 / 128) == 7812);   // 7812.5 -> even below
    CHECK(ray::toNm(3.0 / 128) == 23438);  // 23437.5 -> even above
    CHECK(ray::toNm(5.0 / 128) == 39062);  // 39062.5 -> even below
    CHECK(ray::toNm(7.0 / 128) == 54688);  // 54687.5 -> even above
    CHECK(ray::toNm(-1.0 / 128) == -7812); // negatives mirror the positives
    CHECK(ray::toNm(-3.0 / 128) == -23438);
    CHECK(ray::toNm(1000000000.0078125) == 1000000000007812); // 1e15 + 7812.5 nm
    CHECK(ray::toNm(1000000000.0234375) == 1000000000023438);

    CHECK(ray::toNm(0.0) == 0);
    CHECK(ray::toNm(-0.0) == 0);
    CHECK(ray::toNm(1.0) == 1000000);
    CHECK(ray::toNm(-12.7) == -12700000);
    CHECK(ray::toNm(0.000000499) == 0);
    CHECK(ray::toNm(-0.000000499) == 0);
    CHECK(ray::toNm(0.000000501) == 1);

    // Near a tie the exact product decides, not the rounded double product: 2.5e-6 mm is not exactly
    // 2.5 nm, though 2.5e-6 * 1e6 rounds to exactly 2.5 in double.
    CHECK(ray::toNm(2.5e-6) == referenceToNm(2.5e-6));
    CHECK(ray::toNm(0.0000125) == referenceToNm(0.0000125));

    // Saturation and NaN.
    CHECK(ray::toNm(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(ray::toNm(1e300) == std::numeric_limits<ray::Nm>::max());
    CHECK(ray::toNm(-1e300) == std::numeric_limits<ray::Nm>::min());
    CHECK(ray::toNm(std::numeric_limits<double>::infinity()) == std::numeric_limits<ray::Nm>::max());
    CHECK(ray::toNm(-std::numeric_limits<double>::infinity()) == std::numeric_limits<ray::Nm>::min());
    CHECK(ray::toNm(9.3e12) == std::numeric_limits<ray::Nm>::max());
}

TEST_CASE("toNm agrees with an exact decimal reference", "[units]")
{
    int checked = 0;
    for (int i = 0; i < 6000; ++i) {
        double mm = 0.0;
        switch (i % 3) {
        case 0: { // log-uniform magnitude in [2^-30, 2^40), random mantissa and sign
            const std::uint64_t exponent = 1023 - 30 + next() % 70;
            mm = std::bit_cast<double>((next() & 0x800FFFFFFFFFFFFFULL) | (exponent << 52));
            break;
        }
        case 1: { // exact ties: odd m / 128
            const std::int64_t m = static_cast<std::int64_t>(next() % (1ULL << 40)) | 1;
            mm = static_cast<double>(m) / 128.0 * ((next() & 1) ? 1.0 : -1.0);
            break;
        }
        default: { // the double nearest (n + 0.5) nm, and its neighbours
            const double n = static_cast<double>(next() % (1ULL << 40));
            mm = (n + 0.5) / 1e6;
            const std::int64_t nudge = static_cast<std::int64_t>(next() % 5) - 2;
            mm = std::bit_cast<double>(static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(mm) + nudge));
            break;
        }
        }
        INFO("mm = " << mm);
        CHECK(ray::toNm(mm) == referenceToNm(mm));
        ++checked;
    }
    CHECK(checked == 6000);
}

TEST_CASE("toMm and Xform::identity", "[units]")
{
    CHECK(ray::toMm(1000000) == 1.0);
    CHECK(ray::toMm(-12700000) == -12.7);
    CHECK(ray::toMm(1) == 1e-6);
    CHECK(ray::toNm(ray::toMm(123456789)) == 123456789);

    const ray::Xform id = ray::Xform::identity();
    const double expected[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; ++i) {
        CHECK(id.m[i] == expected[i]);
    }
}

TEST_CASE("parseLength accepts numbers, fractions and units", "[units]")
{
    using ray::UnitSystem;
    CHECK(parsed("0.5") == 0.5);
    CHECK(parsed("0.5", UnitSystem::Inch) == 12.7);
    CHECK(parsed("1/2") == 0.5);
    CHECK(parsed("1/2", UnitSystem::Inch) == 12.7);
    CHECK(parsed("12.7mm") == 12.7);
    CHECK(parsed("12.7mm", UnitSystem::Inch) == 12.7);
    CHECK(parsed("0.5in") == 12.7);
    CHECK(parsed("0.5in", UnitSystem::Inch) == 12.7);

    // Mixed whitespace around and between the parts.
    CHECK(parsed(" \t12.7 \t mm \r\n") == 12.7);
    CHECK(parsed("\t1 / 2\tin ") == 12.7);
    CHECK(parsed("  0.5\t", UnitSystem::Inch) == 12.7);

    // Other forms.
    CHECK(parsed("1 1/2 in") == 1.5 * 25.4);
    CHECK(parsed("1 1/2\"") == 1.5 * 25.4);
    CHECK(parsed("3/4\"") == 0.75 * 25.4);
    CHECK(parsed("-3mm") == -3.0);
    CHECK(parsed("+3") == 3.0);
    CHECK(parsed("-1/2", UnitSystem::Inch) == -12.7);
    CHECK(parsed("2.5cm") == 25.0);
    CHECK(parsed("1m") == 1000.0);
    CHECK(parsed("12.7 MM") == 12.7);
    CHECK(parsed("1 Inch") == 25.4);
    CHECK(parsed("2 inches") == 50.8);
    CHECK(parsed(".5") == 0.5);
    CHECK(parsed("5.") == 5.0);
    CHECK(parsed("1.5e2 mm") == 150.0);
    CHECK(parsed("1E-3") == 0.001);
    CHECK(parsed("0") == 0.0);
    CHECK(parsed("000012.50") == 12.5);
    CHECK(parsed("1.5/2") == 0.75);
    CHECK(parsed("3.14159265358979323846264338327950288") == 3.141592653589793);
    CHECK(parsed("0.1") == 0.1);
    CHECK(parsed("123456.789") == 123456.789);
}

TEST_CASE("parseLength rejects bad input", "[units]")
{
    const char* const bad[] = {
        "",         "   ",     "\t\n",    "abc",        "mm",       "in",      "-",       "+",
        ".",        "1..2",    "--1",     "- 1",        "1/0",      "0/0",     "1 / 0in", "1 1/0",
        "1/",       "/2",      "1/2/3",   "1 2",        "1 2 in",   "5 1/2.5", "1.5 1/2", "12.7mm junk",
        "12.7 mmm", "12.7 ft", "12.7mm1", "12.7 mm mm", "1e",       "1e+",     "5em",     "1e999",
        "inf",      "nan",     "0x10",    "1,5",        "12.7\"\"", "1 in\"",  "\"1",     "1 1/2 1/2",
    };
    for (const char* text : bad) {
        INFO("parseLength(\"" << text << "\")");
        CHECK(rejected(text));
        CHECK(rejected(text, ray::UnitSystem::Inch));
    }
}

TEST_CASE("formatLength", "[units]")
{
    using ray::UnitFormat;
    using ray::UnitSystem;
    CHECK(ray::formatLength(12.7, UnitFormat{UnitSystem::Metric, 3}) == "12.700 mm");
    CHECK(ray::formatLength(12.7, UnitFormat{UnitSystem::Inch, 4}) == "0.5000 in");
    CHECK(ray::formatLength(-25.4, UnitFormat{UnitSystem::Inch, 2}) == "-1.00 in");
    CHECK(ray::formatLength(2.6, UnitFormat{UnitSystem::Metric, 0}) == "3 mm");
    CHECK(ray::formatLength(2.6, UnitFormat{UnitSystem::Metric, -4}) == "3 mm");
    CHECK(ray::formatLength(-0.0001, UnitFormat{UnitSystem::Metric, 2}) == "0.00 mm");
    CHECK(ray::formatLength(-0.0, UnitFormat{UnitSystem::Metric, 1}) == "0.0 mm");
    CHECK(ray::formatLength(1.0 / 3.0, UnitFormat{UnitSystem::Metric, 40}) == "0.333333333333333 mm");

    // What formatLength writes, parseLength reads back.
    for (const double mm : {0.0, 1.0, -3.25, 12.7, 1234.5678, 0.001}) {
        for (const UnitSystem system : {UnitSystem::Metric, UnitSystem::Inch}) {
            const std::string text = ray::formatLength(mm, UnitFormat{system, 6});
            const double back = parsed(text, system == UnitSystem::Metric ? UnitSystem::Inch : UnitSystem::Metric);
            CHECK(std::fabs(back - mm) < 1e-4);
        }
    }
}
