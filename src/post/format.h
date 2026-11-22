// Number formatting for G-code (PLAN.md section 8, "Number formatting"): integer arithmetic only, no printf, no
// locale. Internal to src/post; the public entry point is ray::post::formatNumber in post.h.
#pragma once
#include <ray/post.h>

#include <cstdint>
#include <string>

namespace ray::post::fmt {

// The largest magnitude formatNumber accepts. 1e12 output units is far beyond any machine travel, and
// 1e12 * 1e6 micro-units still fits an int64 with room to spare.
constexpr double kMaxAbsValue = 1.0e12;
// Digits after the decimal point that a format may ask for: one micro-unit (one nanometre in mm output).
constexpr int kMaxDecimals = 6;

// Rounds value / divisor to an integer, ties to even. divisor > 0.
std::int64_t divRoundHalfEven(std::int64_t value, std::int64_t divisor) noexcept;

// Snaps a script double to integer micro-units (1e-6 of the value's unit): llround(value * 1e6). For a length in
// millimetres that is the integer nanometre the host converted the double from (spike S8). False when the value is
// not finite or its magnitude exceeds kMaxAbsValue.
bool snapMicro(double value, std::int64_t& micro) noexcept;

// Rounds micro-units to `decimals` digits (half to even) and returns the result scaled by 10^decimals.
std::int64_t roundMicro(std::int64_t micro, int decimals) noexcept;

// Appends q / 10^decimals as text. trimZeros drops trailing zeros of the fraction; forcePoint keeps a decimal point
// when no fraction digit is left ("12."); leadingZero writes "0.5" rather than ".5". Never writes "-0".
void appendFixed(std::string& out, std::int64_t q, int decimals, bool trimZeros, bool forcePoint,
                 bool leadingZero);

// Appends micro-units formatted with `format` (its decimals clamped to 0..kMaxDecimals; scale is not applied here).
void appendMicro(std::string& out, std::int64_t micro, const NumberFormat& format);

} // namespace ray::post::fmt
