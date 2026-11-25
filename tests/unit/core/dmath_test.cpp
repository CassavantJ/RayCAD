// ray::dmath against the stored reference table (tools/gen_dmath_table.py), bit for bit, and against the
// platform libm to within 1 ulp (Apple's tan: 3).
#include <catch2/catch_test_macros.hpp>

#include <ray/core.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

struct Record {
    int function = 0;
    double a = 0.0;
    double b = 0.0;
    double expected = 0.0;
};

const char* const kNames[] = {"sin", "cos", "tan", "asin", "acos", "atan2", "sqrt", "hypot"};

std::uint32_t readU32(const std::vector<unsigned char>& data, std::size_t at)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | data[at + static_cast<std::size_t>(i)];
    }
    return value;
}

double readF64(const std::vector<unsigned char>& data, std::size_t at)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | data[at + static_cast<std::size_t>(i)];
    }
    return std::bit_cast<double>(value);
}

std::vector<Record> loadTable()
{
    std::ifstream file(RAY_DMATH_TABLE, std::ios::binary);
    REQUIRE(file.good());
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(data.size() >= 16);
    REQUIRE(std::memcmp(data.data(), "RAYDMATH", 8) == 0);
    REQUIRE(readU32(data, 8) == 1);
    const std::uint32_t groups = readU32(data, 12);
    REQUIRE(groups == 8);
    std::size_t at = 16 + 16 * static_cast<std::size_t>(groups);
    REQUIRE(data.size() >= at);
    std::vector<Record> records;
    for (std::uint32_t g = 0; g < groups; ++g) {
        const std::size_t header = 16 + 16 * static_cast<std::size_t>(g);
        const int function = static_cast<int>(readU32(data, header));
        const std::uint32_t arity = readU32(data, header + 4);
        const std::uint32_t count = readU32(data, header + 8);
        REQUIRE(function >= 0);
        REQUIRE(function < 8);
        REQUIRE(arity == ((function == 5 || function == 7) ? 2u : 1u));
        REQUIRE(data.size() >= at + static_cast<std::size_t>(count) * (arity + 1) * 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            Record r;
            r.function = function;
            r.a = readF64(data, at);
            at += 8;
            if (arity == 2) {
                r.b = readF64(data, at);
                at += 8;
            }
            r.expected = readF64(data, at);
            at += 8;
            records.push_back(r);
        }
    }
    REQUIRE(at == data.size());
    return records;
}

double dmathCall(int function, double a, double b)
{
    switch (function) {
    case 0:
        return ray::dmath::sin(a);
    case 1:
        return ray::dmath::cos(a);
    case 2:
        return ray::dmath::tan(a);
    case 3:
        return ray::dmath::asin(a);
    case 4:
        return ray::dmath::acos(a);
    case 5:
        return ray::dmath::atan2(a, b);
    case 6:
        return ray::dmath::sqrt(a);
    default:
        return ray::dmath::hypot(a, b);
    }
}

double platformCall(int function, double a, double b)
{
    switch (function) {
    case 0:
        return std::sin(a);
    case 1:
        return std::cos(a);
    case 2:
        return std::tan(a);
    case 3:
        return std::asin(a);
    case 4:
        return std::acos(a);
    case 5:
        return std::atan2(a, b);
    case 6:
        return std::sqrt(a);
    default:
        return std::hypot(a, b);
    }
}

// Distance in units in the last place, counting across zero (+0 and -0 are 0 apart).
std::uint64_t ulpDistance(double x, double y)
{
    const auto ordered = [](double d) {
        const auto bits = std::bit_cast<std::int64_t>(d);
        return bits < 0 ? std::numeric_limits<std::int64_t>::min() - bits : bits;
    };
    const std::int64_t a = ordered(x);
    const std::int64_t b = ordered(y);
    return a > b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                 : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
}

std::string describe(const Record& r, double got)
{
    char text[200];
    std::snprintf(text, sizeof text, "%s(%a, %a): expected %a, got %a", kNames[r.function], r.a, r.b, r.expected, got);
    return text;
}

} // namespace

TEST_CASE("dmath matches the stored table bit for bit", "[dmath]")
{
    const std::vector<Record> records = loadTable();
    REQUIRE(records.size() == 100000);
    std::size_t mismatches = 0;
    std::size_t nans = 0;
    for (const Record& r : records) {
        const double got = dmathCall(r.function, r.a, r.b);
        // A NaN result is compared as "is NaN": the sign and payload of a NaN made by an invalid operation
        // differ between x86-64 and ARM64 hardware, and no caller may depend on them.
        bool same = false;
        if (std::isnan(r.expected)) {
            same = std::isnan(got);
            ++nans;
        } else {
            same = std::bit_cast<std::uint64_t>(got) == std::bit_cast<std::uint64_t>(r.expected);
        }
        if (!same) {
            if (mismatches < 20) {
                UNSCOPED_INFO(describe(r, got));
            }
            ++mismatches;
        }
    }
    INFO("NaN results: " << nans);
    CHECK(mismatches == 0);
}

// How far the platform libm may be from dmath, in ulp. 1 everywhere, except Apple's tan: up to 3 ulp at 84 of the
// table's arguments on macOS ARM64 (CI run 45576522199; sin, cos, asin, acos, atan2 and hypot within 1 there). This
// compares with a library dmath does not use; what dmath gives is the bit-for-bit test above.
std::uint64_t platformBound(int function)
{
#if defined(__APPLE__)
    if (std::strcmp(kNames[function], "tan") == 0) {
        return 3;
    }
#endif
    (void)function;
    return 1;
}

TEST_CASE("dmath is within 1 ulp of the platform libm (Apple's tan: 3)", "[dmath]")
{
    const std::vector<Record> records = loadTable();
    std::size_t compared = 0;
    std::size_t over = 0;
    std::uint64_t worst = 0;
    std::map<std::string, std::size_t> overBy; // function name: values past the function's bound
    std::string examples;                      // the first 40, reported with the check
    for (const Record& r : records) {
        const double ours = dmathCall(r.function, r.a, r.b);
        const double theirs = platformCall(r.function, r.a, r.b);
        if (std::isnan(ours) || std::isnan(theirs)) {
            CHECK(std::isnan(ours) == std::isnan(theirs));
            continue;
        }
        ++compared;
        const std::uint64_t distance = ulpDistance(ours, theirs);
        worst = distance > worst ? distance : worst;
        if (distance > platformBound(r.function)) {
            if (over < 40) {
                examples += "\n  " + describe(r, theirs) + " (platform; ours " + describe(r, ours) + "), " +
                            std::to_string(distance) + " ulp";
            }
            ++overBy[kNames[r.function]];
            ++over;
        }
    }
    std::string byFunction;
    for (const auto& [name, n] : overBy)
        byFunction += " " + name + " " + std::to_string(n);
    INFO("compared " << compared << ", worst " << worst << " ulp; past the bound by function:" << byFunction
                     << examples);
    CHECK(over == 0);
    CHECK(compared > 90000);
}

TEST_CASE("dmath special values", "[dmath]")
{
    namespace dm = ray::dmath;
    CHECK(std::bit_cast<std::uint64_t>(dm::sin(-0.0)) == std::bit_cast<std::uint64_t>(-0.0));
    CHECK(dm::sin(0.0) == 0.0);
    CHECK(dm::cos(0.0) == 1.0);
    CHECK(dm::tan(0.0) == 0.0);
    CHECK(dm::asin(1.0) == 0x1.921fb54442d18p+0);
    CHECK(dm::acos(-1.0) == 0x1.921fb54442d18p+1);
    CHECK(dm::atan2(1.0, 1.0) == 0x1.921fb54442d18p-1);
    CHECK(dm::sqrt(4.0) == 2.0);
    CHECK(dm::hypot(3.0, 4.0) == 5.0);
    CHECK(std::isnan(dm::sin(std::numeric_limits<double>::infinity())));
    CHECK(std::isnan(dm::asin(1.5)));
    CHECK(dm::hypot(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()) ==
          std::numeric_limits<double>::infinity());
}
