// Xform::identity, nanometre quantisation, and length formatting and parsing for display.
#include <ray/core.h>

#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>

namespace ray {

Xform Xform::identity()
{
    return Xform{{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0}};
}

// ---------- Nanometres ----------
namespace {

// Dekker's exact product: a * b == p + error, exactly, when nothing overflows or underflows. Needs strict
// double arithmetic without fused multiply-add, which ray_core is compiled with (PLAN.md section 8).
double productError(double a, double b, double p) noexcept
{
    constexpr double kSplit = 134217729.0; // 2^27 + 1
    const double ca = kSplit * a;
    const double ah = ca - (ca - a);
    const double al = a - ah;
    const double cb = kSplit * b;
    const double bh = cb - (cb - b);
    const double bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

} // namespace

// Rounds the exact value mm * 1e6 (not the double nearest to it) to the nearest integer, ties to even.
// NaN gives 0; values outside the Nm range saturate to its ends.
Nm toNm(double mm)
{
    constexpr Nm kMax = std::numeric_limits<Nm>::max();
    constexpr Nm kMin = std::numeric_limits<Nm>::min();
    constexpr double kTwo63 = 9223372036854775808.0;
    if (std::isnan(mm)) {
        return 0;
    }
    // Beyond 1e13 mm the product exceeds 2^63 nm; this also keeps the product error free of overflow.
    if (!(std::fabs(mm) < 1.0e13)) {
        return mm > 0 ? kMax : kMin;
    }
    const double p = mm * kNmPerMm;
    const double error = productError(mm, kNmPerMm, p);
    const bool negative = p < 0;
    const double magnitude = negative ? -p : p;
    const double magnitudeError = negative ? -error : error; // exact value of |mm * 1e6| is magnitude + this
    if (magnitude >= kTwo63) {
        return negative ? kMin : kMax;
    }
    if (magnitude >= 4503599627370496.0) {
        // From 2^52 on, magnitude is an integer and the error term may hold whole nanometres too (up to half an
        // ulp, at most 512 below 2^63): round the error and add it in integers.
        const double errorWhole = std::floor(magnitudeError);
        const double errorFraction = magnitudeError - errorWhole; // exact: |error| < 2^10
        Nm n = static_cast<Nm>(magnitude) + static_cast<Nm>(errorWhole);
        if (errorFraction > 0.5 || (errorFraction == 0.5 && n % 2 != 0)) {
            ++n;
        }
        return negative ? -n : n;
    }
    double whole = std::floor(magnitude);
    const double fraction = magnitude - whole; // exact: whole <= magnitude < 2 * whole, or whole == 0
    bool up = fraction > 0.5;
    if (fraction == 0.5) {
        up = magnitudeError > 0 || (magnitudeError == 0 && std::fmod(whole, 2.0) != 0.0);
    }
    if (up) {
        whole += 1.0;
    }
    const Nm n = static_cast<Nm>(whole);
    return negative ? -n : n;
}

double toMm(Nm nm)
{
    return static_cast<double>(nm) / kNmPerMm;
}

// ---------- Display formatting ----------
namespace {

constexpr double kMmPerInch = 25.4;

} // namespace

// Fixed-point with `decimals` digits (clamped to 0..15) and a unit suffix: "12.700 mm", "0.5000 in".
// Independent of the C and C++ locale. A value that rounds to zero is shown without a minus sign.
std::string formatLength(double mm, UnitFormat format)
{
    try {
        const bool inch = format.system == UnitSystem::Inch;
        const double value = inch ? mm / kMmPerInch : mm;
        const int decimals = format.decimals < 0 ? 0 : (format.decimals > 15 ? 15 : format.decimals);
        char buffer[400]; // the longest fixed form of a double: 309 digits, sign, point and 15 decimals
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, decimals);
        if (result.ec != std::errc()) {
            return {};
        }
        std::string text(buffer, result.ptr);
        if (!text.empty() && text.front() == '-' && text.find_first_not_of("0.", 1) == std::string::npos) {
            text.erase(0, 1);
        }
        text += inch ? " in" : " mm";
        return text;
    } catch (...) {
        return {};
    }
}

// ---------- Parsing ----------
namespace {

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool isLetter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

struct Cursor {
    explicit Cursor(std::string_view input) noexcept : text(input) {}

    std::string_view text;
    std::size_t pos = 0;

    bool atEnd() const noexcept { return pos >= text.size(); }
    char peek() const noexcept { return atEnd() ? '\0' : text[pos]; }
    // Returns the number of characters skipped.
    std::size_t skipSpace() noexcept
    {
        const std::size_t start = pos;
        while (!atEnd() && isSpace(text[pos])) {
            ++pos;
        }
        return pos - start;
    }
};

struct Number {
    double value = 0.0;
    bool integer = true; // no decimal point and no exponent
};

// Decimal digits to double. Exact inputs of up to 15 significant digits with a small exponent, which is every
// length a person types, take the fast path: one correctly rounded multiply or divide of two exact doubles.
// Anything else goes through the stream parser in the classic locale.
std::optional<double> decimalToDouble(std::string_view token)
{
    static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    std::uint64_t mantissa = 0;
    int digits = 0;
    int significant = 0;  // all significant digits, including those beyond the 19 kept in mantissa
    int exponent = 0;     // of the last digit kept in mantissa
    int pointShift = 0;   // minus the number of digits after the point, from the first significant digit on
    int exponentPart = 0; // the value written after "e"
    bool seenPoint = false;
    std::size_t i = 0;
    for (; i < token.size() && (isDigit(token[i]) || token[i] == '.'); ++i) {
        if (token[i] == '.') {
            seenPoint = true;
            continue;
        }
        if (mantissa == 0 && token[i] == '0') {
            if (seenPoint) {
                --exponent;
                --pointShift;
            }
            continue; // leading zeros carry no significant digit
        }
        ++significant;
        if (seenPoint) {
            --pointShift;
        }
        if (digits < 19) {
            mantissa = mantissa * 10 + static_cast<std::uint64_t>(token[i] - '0');
            ++digits;
            if (seenPoint) {
                --exponent;
            }
        } else {
            digits = 99; // too many digits for the fast path
        }
    }
    if (i < token.size()) { // exponent part, already validated by the caller
        ++i;
        bool negative = false;
        if (token[i] == '+' || token[i] == '-') {
            negative = token[i] == '-';
            ++i;
        }
        int value = 0;
        for (; i < token.size(); ++i) {
            if (value < 100000) {
                value = value * 10 + (token[i] - '0');
            }
        }
        exponentPart = negative ? -value : value;
        exponent += exponentPart;
    }
    if (mantissa == 0) {
        return 0.0;
    }
    // The value lies in [10^order, 10^(order + 1)). Decide overflow and underflow here rather than trusting
    // how each standard library's stream reports them.
    const int order = significant - 1 + pointShift + exponentPart;
    if (order > 308) {
        return std::nullopt;
    }
    if (order < -330) {
        return 0.0;
    }
    if (digits <= 15 && exponent >= -22 && exponent <= 22) {
        const double m = static_cast<double>(mantissa);
        return exponent >= 0 ? m * kPow10[exponent] : m / kPow10[-exponent];
    }
    std::istringstream stream{std::string(token)};
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (stream.fail()) {
        return std::nullopt;
    }
    return value;
}

// number := digits [ "." digits ] | "." digits, then optionally [eE] [+-] digits
std::optional<Number> parseNumber(Cursor& cursor)
{
    const std::size_t start = cursor.pos;
    std::size_t digits = 0;
    bool point = false;
    while (!cursor.atEnd()) {
        const char c = cursor.peek();
        if (isDigit(c)) {
            ++digits;
        } else if (c == '.' && !point) {
            point = true;
        } else {
            break;
        }
        ++cursor.pos;
    }
    if (digits == 0) {
        cursor.pos = start;
        return std::nullopt;
    }
    bool exponent = false;
    if (cursor.peek() == 'e' || cursor.peek() == 'E') {
        std::size_t look = cursor.pos + 1;
        if (look < cursor.text.size() && (cursor.text[look] == '+' || cursor.text[look] == '-')) {
            ++look;
        }
        if (look < cursor.text.size() && isDigit(cursor.text[look])) {
            exponent = true;
            cursor.pos = look;
            while (!cursor.atEnd() && isDigit(cursor.peek())) {
                ++cursor.pos;
            }
        }
    }
    const std::optional<double> value = decimalToDouble(cursor.text.substr(start, cursor.pos - start));
    if (!value) {
        return std::nullopt;
    }
    return Number{*value, !point && !exponent};
}

Error lengthError(std::string message, std::string_view text)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {"parseLength: \"" + std::string(text) + "\""}};
}

std::optional<double> unitScale(std::string_view unit)
{
    struct Unit {
        std::string_view name;
        double mm;
    };
    static constexpr Unit kUnits[] = {{"mm", 1.0},        {"cm", 10.0},         {"m", 1000.0},
                                      {"in", kMmPerInch}, {"inch", kMmPerInch}, {"inches", kMmPerInch},
                                      {"\"", kMmPerInch}};
    for (const Unit& u : kUnits) {
        if (u.name.size() != unit.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < unit.size(); ++i) {
            same = same && lower(unit[i]) == u.name[i];
        }
        if (same) {
            return u.mm;
        }
    }
    return std::nullopt;
}

Result<double> parseLengthImpl(std::string_view text, UnitSystem defaultSystem)
{
    Cursor cursor{text};
    cursor.skipSpace();
    if (cursor.atEnd()) {
        return tl::unexpected(lengthError("Enter a length.", text));
    }
    bool negative = false;
    if (cursor.peek() == '+' || cursor.peek() == '-') {
        negative = cursor.peek() == '-';
        ++cursor.pos;
    }
    const std::optional<Number> first = parseNumber(cursor);
    if (!first) {
        return tl::unexpected(lengthError("Not a length.", text));
    }
    double value = first->value;

    // Optional fraction "a/b", or mixed number "w n/d" (whole part and fraction made of integers).
    const std::size_t afterFirst = cursor.pos;
    const std::size_t gap = cursor.skipSpace();
    if (cursor.peek() == '/') {
        ++cursor.pos;
        cursor.skipSpace();
        const std::optional<Number> denominator = parseNumber(cursor);
        if (!denominator) {
            return tl::unexpected(lengthError("A fraction needs a number after \"/\".", text));
        }
        if (denominator->value == 0.0) {
            return tl::unexpected(lengthError("Division by zero.", text));
        }
        value /= denominator->value;
    } else if (gap > 0 && first->integer && isDigit(cursor.peek())) {
        const std::optional<Number> numerator = parseNumber(cursor);
        cursor.skipSpace();
        if (!numerator || !numerator->integer || cursor.peek() != '/') {
            return tl::unexpected(lengthError("Unexpected text after the number.", text));
        }
        ++cursor.pos;
        cursor.skipSpace();
        const std::optional<Number> denominator = parseNumber(cursor);
        if (!denominator || !denominator->integer) {
            return tl::unexpected(lengthError("A fraction needs a whole number after \"/\".", text));
        }
        if (denominator->value == 0.0) {
            return tl::unexpected(lengthError("Division by zero.", text));
        }
        value += numerator->value / denominator->value;
    } else {
        cursor.pos = afterFirst;
    }

    // Optional unit.
    cursor.skipSpace();
    const std::size_t unitStart = cursor.pos;
    if (cursor.peek() == '"') {
        ++cursor.pos;
    } else {
        while (!cursor.atEnd() && isLetter(cursor.peek())) {
            ++cursor.pos;
        }
    }
    double scale = defaultSystem == UnitSystem::Inch ? kMmPerInch : 1.0;
    if (cursor.pos > unitStart) {
        const std::optional<double> unit = unitScale(text.substr(unitStart, cursor.pos - unitStart));
        if (!unit) {
            return tl::unexpected(lengthError("Unknown unit; use mm, cm, m or in.", text));
        }
        scale = *unit;
    }
    cursor.skipSpace();
    if (!cursor.atEnd()) {
        return tl::unexpected(lengthError("Unexpected text after the length.", text));
    }

    const double mm = (negative ? -value : value) * scale;
    if (!std::isfinite(mm)) {
        return tl::unexpected(lengthError("The length is too large.", text));
    }
    return mm;
}

} // namespace

// Grammar, with any whitespace (spaces, tabs, newlines) between the parts:
//   [+|-] number [ "/" number | integer "/" integer ] [unit]
// number is a decimal such as 12, 0.5, .5 or 1.5e2; "1/2" is a fraction and "1 1/2" a mixed number.
// unit is mm, cm, m, in, inch, inches or " (any letter case); without one, defaultSystem's unit applies.
// Returns millimetres, or InvalidArgument with a message for the user.
Result<double> parseLength(std::string_view text, UnitSystem defaultSystem)
{
    try {
        return parseLengthImpl(text, defaultSystem);
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory.", {"parseLength"}});
    }
}

} // namespace ray
