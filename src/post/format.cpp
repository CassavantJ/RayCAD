// Number formatting for G-code: integers only from the snap on (see format.h).
#include "format.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numeric>

namespace ray::post {
namespace fmt {
namespace {

constexpr std::array<std::int64_t, kMaxDecimals + 1> kPow10 = {1, 10, 100, 1000, 10000, 100000, 1000000};

} // namespace

std::int64_t divRoundHalfEven(std::int64_t value, std::int64_t divisor) noexcept
{
    std::int64_t q = value / divisor;
    const std::int64_t r = value % divisor; // same sign as value
    const std::int64_t twiceAbs = 2 * (r < 0 ? -r : r);
    if (twiceAbs > divisor || (twiceAbs == divisor && (q % 2) != 0)) {
        q += value < 0 ? -1 : 1;
    }
    return q;
}

bool snapToGrid(double value, std::int64_t& n, std::int64_t grid) noexcept
{
    if (!std::isfinite(value) || std::fabs(value) > kMaxAbsValue) {
        return false;
    }
    // One multiplication and one round to nearest: llround is exact and the same on every platform. For a double
    // made by the host as nm / 1e6 (or nm / 25.4e6 for inch, on the inch grid) this recovers nm exactly.
    n = static_cast<std::int64_t>(std::llround(value * static_cast<double>(grid)));
    return true;
}

std::int64_t roundGrid(std::int64_t n, int decimals, std::int64_t grid) noexcept
{
    // n * 10^d / grid, rounded half to even, with the fraction reduced so that nothing overflows: on the micro grid
    // that is n / 10^(6 - d); on the inch grid n / (254e5 / 10^d) up to 5 decimals and n * 5 / 127 for 6.
    const int d = std::clamp(decimals, 0, kMaxDecimals);
    const std::int64_t scale = kPow10[static_cast<std::size_t>(d)];
    const std::int64_t g = std::gcd(scale, grid);
    return divRoundHalfEven(n * (scale / g), grid / g);
}

void appendFixed(std::string& out, std::int64_t q, int decimals, bool trimZeros, bool forcePoint, bool leadingZero)
{
    const int d = std::clamp(decimals, 0, kMaxDecimals);
    const std::uint64_t magnitude = q < 0 ? 0 - static_cast<std::uint64_t>(q) : static_cast<std::uint64_t>(q);
    const std::uint64_t scale = static_cast<std::uint64_t>(kPow10[static_cast<std::size_t>(d)]);
    const std::uint64_t whole = magnitude / scale;
    std::uint64_t fraction = magnitude % scale;

    std::array<char, kMaxDecimals> digits{};
    int count = d;
    for (int i = d - 1; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = static_cast<char>('0' + fraction % 10);
        fraction /= 10;
    }
    if (trimZeros) {
        while (count > 0 && digits[static_cast<std::size_t>(count - 1)] == '0') {
            --count;
        }
    }
    if (magnitude != 0 && q < 0) { // never "-0", also when the value rounded to zero
        out += '-';
    }
    if (whole != 0 || leadingZero || count == 0) {
        std::array<char, 24> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), whole);
        out.append(buffer.data(), result.ptr);
    }
    if (count > 0) {
        out += '.';
        out.append(digits.data(), static_cast<std::size_t>(count));
    } else if (forcePoint) {
        out += '.';
    }
}

void appendGrid(std::string& out, std::int64_t n, const NumberFormat& format, std::int64_t grid)
{
    const int d = std::clamp(format.decimals, 0, kMaxDecimals);
    appendFixed(out, roundGrid(n, d, grid), d, format.trimTrailingZeros, format.forceDecimalPoint, format.leadingZero);
}

} // namespace fmt

std::string formatNumber(double value, const NumberFormat& format)
{
    // scale 0 (a value-initialised NumberFormat) means 1. A scale other than 1 is one IEEE multiplication, the same
    // on every platform (src/post is compiled without contraction or fast math).
    const double scaled = format.scale == 0.0 || format.scale == 1.0 ? value : value * format.scale;
    std::int64_t micro = 0;
    if (!fmt::snapToGrid(scaled, micro, fmt::kMicroGrid)) {
        return {}; // not finite, or beyond fmt::kMaxAbsValue: no text rather than a wrong number
    }
    std::string text;
    fmt::appendGrid(text, micro, format, fmt::kMicroGrid);
    return text;
}

} // namespace ray::post
