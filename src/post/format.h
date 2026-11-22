// Number formatting for G-code (PLAN.md section 8, "Number formatting"): integer arithmetic only, no printf, no
// locale. Internal to src/post; the public entry point is ray::post::formatNumber in post.h.
#pragma once
#include <ray/post.h>

#include <cstdint>
#include <string>

namespace ray::post::fmt {

// The largest magnitude a number may have. 1e10 output units is far beyond any machine travel, and 1e10 on the inch
// grid, times 5 for 6 decimals, still fits an int64.
constexpr double kMaxAbsValue = 1.0e10;

// Integer grids numbers are snapped to, in steps per output unit. Millimetres and unitless numbers: 1e6 (one
// nanometre per mm). Inch: 25.4e6, one nanometre per step, so that inch values made from integer nanometres come
// back to exactly those nanometres. (A micro-inch grid rounds twice: 5 mm = 0.19685039 in, snapped to 196850
// micro-inch, then rounds down to 0.1968 at 4 decimals instead of up to 0.1969.)
constexpr std::int64_t kMicroGrid = 1'000'000;
constexpr std::int64_t kInchGrid = 25'400'000;
// Digits after the decimal point that a format may ask for: one nanometre in mm output.
constexpr int kMaxDecimals = 6;

// Rounds value / divisor to an integer, ties to even. divisor > 0.
std::int64_t divRoundHalfEven(std::int64_t value, std::int64_t divisor) noexcept;

// Snaps a script double to the grid: llround(value * grid). For a length the host converted from integer nanometres
// that is those nanometres again (spike S8). False when the value is not finite or its magnitude exceeds
// kMaxAbsValue.
bool snapToGrid(double value, std::int64_t& n, std::int64_t grid) noexcept;

// Rounds a grid value to `decimals` digits (half to even, integers only) and returns the result scaled by
// 10^decimals. grid is kMicroGrid or kInchGrid.
std::int64_t roundGrid(std::int64_t n, int decimals, std::int64_t grid) noexcept;

// Appends q / 10^decimals as text. trimZeros drops trailing zeros of the fraction; forcePoint keeps a decimal point
// when no fraction digit is left ("12."); leadingZero writes "0.5" rather than ".5". Never writes "-0".
void appendFixed(std::string& out, std::int64_t q, int decimals, bool trimZeros, bool forcePoint,
                 bool leadingZero);

// Appends a grid value formatted with `format` (decimals clamped to 0..kMaxDecimals; scale is not applied here).
void appendGrid(std::string& out, std::int64_t n, const NumberFormat& format, std::int64_t grid);

} // namespace ray::post::fmt
