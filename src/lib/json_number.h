// Exact conversions between binary floating point and decimal text, private to src/lib (a copy of
// src/doc/json_number.*, see json.h).
//
// Written here rather than taken from <charconv> because the result must be the same text on every compiler and
// standard library (Apple's libc++ lacks floating-point from_chars), and from printf/strtod because those depend
// on the C locale. Both directions use exact big-integer arithmetic, so they are correctly rounded:
//  - formatDouble / formatFloat give the shortest decimal that reads back to the same value, and among those
//    the one closest to the value (the same digits as std::to_chars), laid out as ECMAScript's
//    Number.prototype.toString does (RFC 8785): 1e+21, 1e-7, 0.000001, 100, 0.1. -0 is written as 0.
//  - parseDouble / parseFloat read a JSON number (RFC 8259 grammar) and round half to even.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ray::lib::detail {

// Non-finite values have no JSON form; they are written as "null".
std::string formatDouble(double value);
std::string formatFloat(float value);

// The text must match the JSON number grammar exactly: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
// Returns nullopt if it does not, or if the value is too large for the type (it would be infinite).
// Values too small for the type become 0 with the sign kept.
std::optional<double> parseDouble(std::string_view text);
std::optional<float> parseFloat(std::string_view text);

// Length of the longest prefix of `text` that is a JSON number, or 0 if there is none.
std::size_t scanNumber(std::string_view text);

} // namespace ray::lib::detail
