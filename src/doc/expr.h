// Parameter expressions, private to src/doc (the public entry points are evaluate and evaluateParameters in
// <ray/document.h>).
//
// Grammar (whitespace is spaces and tabs):
//   expr    := term (('+' | '-') term)*
//   term    := unary (('*' | '/') unary)*
//   unary   := ('-' | '+') unary | power
//   power   := postfix ('^' unary)?                   right-associative: 2^3^2 = 2^9; -2^2 = -4
//   postfix := primary unit?                          unit: mm in deg rad, e.g. "4 in", "(a + b) mm", "pi rad"
//   primary := number | name | name '(' (expr (',' expr)*)? ')' | '(' expr ')'
//   number  := digits ('.' digits)? (('e' | 'E') ('+' | '-')? digits)?   also ".5"
//
// Values carry a dimension: a power of length and a power of angle. Internal units are mm and rad, so "1 in" is
// 25.4 and "180 deg" is pi. Rules:
//   + - min max atan2     operands must have the same dimension
//   * /                   dimensions add or subtract; dividing by zero is an error
//   ^                     the exponent must be a whole number from -64 to 64 with no unit (use sqrt for roots)
//   sqrt                  every power in the dimension must be even, e.g. sqrt(4 mm * 1 mm) = 2 mm
//   abs                   keeps the dimension
//   floor ceil round      need a plain number (round: halves away from zero)
//   sin cos tan           need an angle or a plain number (radians); give a plain number
//   asin acos atan2       give an angle
//   pi or pi()            3.141592653589793
// Every intermediate result must be finite. Unit and function names are reserved: no parameter may use them.
#pragma once

#include <ray/document.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ray::doc::detail {

struct Dimension {
    int length = 0;
    int angle = 0;
    auto operator<=>(const Dimension&) const = default;
};

struct Quantity {
    double value = 0;
    std::optional<Dimension> dim; // nullopt: unknown (a name from evaluate()'s plain map); matches anything
};

// Called for every name that is not a constant; returns the value or an error to report as is.
using NameLookup = std::function<Result<Quantity>(std::string_view name)>;

// Evaluates; error messages describe the problem without saying which parameter it is.
Result<Quantity> evaluateExpression(std::string_view expression, const NameLookup& lookup);

Dimension dimensionOf(Parameter::Kind kind);
std::string describe(const std::optional<Dimension>& dim);

bool isReservedName(std::string_view name);
bool isValidParameterName(std::string_view name);

} // namespace ray::doc::detail
