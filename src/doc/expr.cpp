// Parameter expressions (grammar and unit rules in expr.h). Deterministic: arithmetic is IEEE-754 double under
// the module's strict floating-point flags, trigonometry goes through ray::dmath, numbers are read by our own
// exact decimal reader, and nothing depends on the locale.
#include "expr.h"

#include "json_number.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <map>
#include <vector>

namespace ray::doc {
namespace detail {
namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kMmPerInch = 25.4;
constexpr double kRadPerDeg = kPi / 180.0;
constexpr int kMaxNesting = 64;
constexpr int kMaxExponent = 64;
constexpr int kMaxDimensionPower = 1000;

struct Unit {
    std::string_view name;
    double factor;
    Dimension dim;
};
constexpr std::array<Unit, 4> kUnits = {{
    {"mm", 1.0, {1, 0}},
    {"in", kMmPerInch, {1, 0}},
    {"deg", kRadPerDeg, {0, 1}},
    {"rad", 1.0, {0, 1}},
}};

struct Function {
    std::string_view name;
    int minArgs;
    int maxArgs;
};
constexpr std::array<Function, 14> kFunctions = {{
    {"sqrt", 1, 1},
    {"abs", 1, 1},
    {"floor", 1, 1},
    {"ceil", 1, 1},
    {"round", 1, 1},
    {"sin", 1, 1},
    {"cos", 1, 1},
    {"tan", 1, 1},
    {"asin", 1, 1},
    {"acos", 1, 1},
    {"atan2", 2, 2},
    {"min", 1, 64},
    {"max", 1, 64},
    {"pi", 0, 0},
}};

const Unit* findUnit(std::string_view name)
{
    for (const Unit& unit : kUnits) {
        if (unit.name == name) {
            return &unit;
        }
    }
    return nullptr;
}

const Function* findFunction(std::string_view name)
{
    for (const Function& fn : kFunctions) {
        if (fn.name == name) {
            return &fn;
        }
    }
    return nullptr;
}

bool isIdentStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

tl::unexpected<Error> exprError(std::string message)
{
    return tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(message), {}});
}

bool sameDimension(const std::optional<Dimension>& a, const std::optional<Dimension>& b)
{
    return !a || !b || *a == *b;
}

// The dimension of a + b or min(a, b): the known one, if any.
std::optional<Dimension> commonDimension(const std::optional<Dimension>& a, const std::optional<Dimension>& b)
{
    return a ? a : b;
}

bool isPlain(const std::optional<Dimension>& dim)
{
    return !dim || *dim == Dimension{};
}

class Evaluator {
public:
    Evaluator(std::string_view text, const NameLookup& lookup) : text_(text), lookup_(lookup) {}

    Result<Quantity> run()
    {
        skipSpace();
        if (pos_ == text_.size()) {
            return exprError("the expression is empty");
        }
        auto value = parseExpr();
        if (!value) {
            return value;
        }
        skipSpace();
        if (pos_ != text_.size()) {
            return syntaxError("unexpected '" + std::string(1, text_[pos_]) + "' (missing operator?)");
        }
        return value;
    }

private:
    tl::unexpected<Error> syntaxError(const std::string& what) const
    {
        return exprError(what + " at character " + std::to_string(pos_ + 1));
    }

    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool peek(char c)
    {
        skipSpace();
        return pos_ < text_.size() && text_[pos_] == c;
    }

    std::string_view identifierAt(std::size_t at) const
    {
        std::size_t end = at;
        if (end < text_.size() && isIdentStart(text_[end])) {
            ++end;
            while (end < text_.size() && isIdentChar(text_[end])) {
                ++end;
            }
        }
        return text_.substr(at, end - at);
    }

    static Result<Quantity> finite(double value, std::optional<Dimension> dim)
    {
        if (!std::isfinite(value)) {
            return exprError("the result is too large");
        }
        return Quantity{value, dim};
    }

    static Result<std::optional<Dimension>> scaleDimension(const std::optional<Dimension>& dim, int factor)
    {
        if (!dim) {
            return std::optional<Dimension>{};
        }
        const Dimension out{dim->length * factor, dim->angle * factor};
        if (out.length > kMaxDimensionPower || out.length < -kMaxDimensionPower || out.angle > kMaxDimensionPower ||
            out.angle < -kMaxDimensionPower) {
            return exprError("the unit power is too large");
        }
        return std::optional<Dimension>{out};
    }

    Result<Quantity> parseExpr()
    {
        if (++depth_ > kMaxNesting) {
            return exprError("the expression is nested too deeply");
        }
        auto left = parseTerm();
        while (left) {
            const bool plus = peek('+');
            if (!plus && !peek('-')) {
                break;
            }
            ++pos_;
            auto right = parseTerm();
            if (!right) {
                return right;
            }
            if (!sameDimension(left->dim, right->dim)) {
                return exprError(std::string("unit mismatch: cannot ") + (plus ? "add " : "subtract ") +
                                 describe(right->dim) + (plus ? " to " : " from ") + describe(left->dim));
            }
            left = finite(plus ? left->value + right->value : left->value - right->value,
                          commonDimension(left->dim, right->dim));
        }
        --depth_;
        return left;
    }

    Result<Quantity> parseTerm()
    {
        auto left = parseUnary();
        while (left) {
            const bool times = peek('*');
            if (!times && !peek('/')) {
                break;
            }
            ++pos_;
            auto right = parseUnary();
            if (!right) {
                return right;
            }
            std::optional<Dimension> dim;
            if (left->dim && right->dim) {
                dim = times ? Dimension{left->dim->length + right->dim->length, left->dim->angle + right->dim->angle}
                            : Dimension{left->dim->length - right->dim->length, left->dim->angle - right->dim->angle};
                auto checked = scaleDimension(dim, 1);
                if (!checked) {
                    return tl::unexpected(checked.error());
                }
            }
            if (!times && right->value == 0.0) {
                return exprError("division by zero");
            }
            left = finite(times ? left->value * right->value : left->value / right->value, dim);
        }
        return left;
    }

    Result<Quantity> parseUnary()
    {
        if (peek('-') || peek('+')) {
            const bool negative = text_[pos_] == '-';
            ++pos_;
            if (++depth_ > kMaxNesting) {
                return exprError("the expression is nested too deeply");
            }
            auto operand = parseUnary();
            --depth_;
            if (operand && negative) {
                operand->value = -operand->value;
            }
            return operand;
        }
        return parsePower();
    }

    Result<Quantity> parsePower()
    {
        auto base = parsePostfix();
        if (!base || !peek('^')) {
            return base;
        }
        ++pos_;
        if (++depth_ > kMaxNesting) {
            return exprError("the expression is nested too deeply");
        }
        auto exponent = parseUnary();
        --depth_;
        if (!exponent) {
            return exponent;
        }
        if (!isPlain(exponent->dim)) {
            return exprError("an exponent cannot have a unit, but it is " + describe(exponent->dim));
        }
        const double e = exponent->value;
        if (e != std::floor(e) || e < -kMaxExponent || e > kMaxExponent) {
            return exprError("only whole-number exponents from -64 to 64 are supported (use sqrt for square roots)");
        }
        const int n = static_cast<int>(e);
        auto dim = scaleDimension(base->dim, n);
        if (!dim) {
            return tl::unexpected(dim.error());
        }
        if (n < 0 && base->value == 0.0) {
            return exprError("division by zero (zero to a negative power)");
        }
        // Exponentiation by squaring: a fixed sequence of IEEE multiplications, so the same result everywhere.
        double result = 1.0;
        double factor = base->value;
        for (int k = n < 0 ? -n : n; k != 0; k >>= 1) {
            if ((k & 1) != 0) {
                result *= factor;
            }
            factor *= factor;
        }
        if (n < 0) {
            result = 1.0 / result;
        }
        return finite(result, *dim);
    }

    Result<Quantity> parsePostfix()
    {
        auto value = parsePrimary();
        if (!value) {
            return value;
        }
        skipSpace();
        const std::string_view word = identifierAt(pos_);
        if (word.empty()) {
            return value;
        }
        const Unit* unit = findUnit(word);
        if (unit == nullptr) {
            return value; // run() or the caller reports what follows
        }
        pos_ += word.size();
        std::optional<Dimension> dim;
        if (value->dim) {
            dim = Dimension{value->dim->length + unit->dim.length, value->dim->angle + unit->dim.angle};
        }
        return finite(value->value * unit->factor, dim);
    }

    Result<Quantity> parseNumber()
    {
        // Normalise to the JSON number grammar for the exact reader: no leading zeros, digits on both sides of '.'.
        std::string integer;
        std::string fraction;
        std::string exponent;
        while (pos_ < text_.size() && isDigit(text_[pos_])) {
            integer.push_back(text_[pos_++]);
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && isDigit(text_[pos_])) {
                fraction.push_back(text_[pos_++]);
            }
        }
        if (integer.empty() && fraction.empty()) {
            return syntaxError("expected a number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            std::size_t at = pos_ + 1;
            std::string sign;
            if (at < text_.size() && (text_[at] == '+' || text_[at] == '-')) {
                sign.push_back(text_[at++]);
            }
            if (at < text_.size() && isDigit(text_[at])) {
                exponent = "e" + sign;
                while (at < text_.size() && isDigit(text_[at])) {
                    exponent.push_back(text_[at++]);
                }
                pos_ = at;
            }
        }
        const std::size_t firstNonZero = integer.find_first_not_of('0');
        integer = firstNonZero == std::string::npos ? "0" : integer.substr(firstNonZero);
        std::string normal = integer;
        if (!fraction.empty()) {
            normal += "." + fraction;
        }
        normal += exponent;
        const auto value = parseDouble(normal);
        if (!value) {
            return exprError("the number " + normal + " is too large");
        }
        return Quantity{*value, Dimension{}};
    }

    Result<Quantity> parsePrimary()
    {
        skipSpace();
        if (pos_ >= text_.size()) {
            return syntaxError("unexpected end of expression");
        }
        const char c = text_[pos_];
        if (isDigit(c) || c == '.') {
            return parseNumber();
        }
        if (c == '(') {
            ++pos_;
            auto inner = parseExpr();
            if (!inner) {
                return inner;
            }
            if (!peek(')')) {
                return syntaxError("expected ')'");
            }
            ++pos_;
            return inner;
        }
        if (!isIdentStart(c)) {
            return syntaxError("unexpected '" + std::string(1, c) + "'");
        }
        const std::string_view name = identifierAt(pos_);
        pos_ += name.size();
        if (findUnit(name) != nullptr) {
            return exprError("the unit '" + std::string(name) + "' must follow a number");
        }
        const Function* fn = findFunction(name);
        const bool call = peek('(');
        if (fn == nullptr) {
            if (call) {
                return exprError("unknown function '" + std::string(name) + "'");
            }
            return lookup_(name);
        }
        if (!call) {
            if (fn->name == "pi") {
                return Quantity{kPi, Dimension{}};
            }
            return exprError("the function '" + std::string(name) + "' needs arguments in parentheses");
        }
        ++pos_; // (
        std::vector<Quantity> args;
        if (!peek(')')) {
            for (;;) {
                auto arg = parseExpr();
                if (!arg) {
                    return arg;
                }
                args.push_back(*arg);
                if (peek(',')) {
                    ++pos_;
                    continue;
                }
                break;
            }
        }
        if (!peek(')')) {
            return syntaxError("expected ',' or ')'");
        }
        ++pos_;
        const int count = static_cast<int>(args.size());
        if (count < fn->minArgs || count > fn->maxArgs) {
            const std::string expected =
                fn->minArgs == fn->maxArgs ? std::to_string(fn->minArgs) : "at least " + std::to_string(fn->minArgs);
            return exprError("the function '" + std::string(name) + "' takes " + expected + " argument" +
                             (expected == "1" ? "" : "s") + ", not " + std::to_string(count));
        }
        return callFunction(fn->name, args);
    }

    static Result<Quantity> callFunction(std::string_view name, const std::vector<Quantity>& args)
    {
        const std::string fn(name);
        if (name == "pi") {
            return Quantity{kPi, Dimension{}};
        }
        if (name == "min" || name == "max") {
            Quantity best = args[0];
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (!sameDimension(best.dim, args[i].dim)) {
                    return exprError("unit mismatch in " + fn + "(): " + describe(best.dim) + " and " +
                                     describe(args[i].dim));
                }
                const bool take = name == "min" ? args[i].value < best.value : args[i].value > best.value;
                best = Quantity{take ? args[i].value : best.value, commonDimension(best.dim, args[i].dim)};
            }
            return best;
        }
        if (name == "atan2") {
            if (!sameDimension(args[0].dim, args[1].dim)) {
                return exprError("unit mismatch in atan2(): " + describe(args[0].dim) + " and " +
                                 describe(args[1].dim));
            }
            return finite(dmath::atan2(args[0].value, args[1].value), Dimension{0, 1});
        }
        const Quantity& x = args[0];
        if (name == "abs") {
            return Quantity{x.value < 0 ? -x.value : x.value, x.dim};
        }
        if (name == "sqrt") {
            if (x.value < 0) {
                return exprError("sqrt() of a negative number");
            }
            std::optional<Dimension> dim;
            if (x.dim) {
                if (x.dim->length % 2 != 0 || x.dim->angle % 2 != 0) {
                    return exprError("sqrt() of " + describe(x.dim) + " has no unit");
                }
                dim = Dimension{x.dim->length / 2, x.dim->angle / 2};
            }
            return finite(dmath::sqrt(x.value), dim);
        }
        if (name == "floor" || name == "ceil" || name == "round") {
            if (!isPlain(x.dim)) {
                return exprError(fn + "() needs a plain number, but it got " + describe(x.dim) +
                                 " (divide by a unit first, e.g. " + fn + "(width / 1 mm) * 1 mm)");
            }
            const double r = name == "floor"  ? std::floor(x.value)
                             : name == "ceil" ? std::ceil(x.value)
                                              : std::round(x.value);
            return Quantity{r, x.dim};
        }
        if (name == "sin" || name == "cos" || name == "tan") {
            if (x.dim && *x.dim != Dimension{} && *x.dim != Dimension{0, 1}) {
                return exprError(fn + "() needs an angle, but it got " + describe(x.dim));
            }
            const double r = name == "sin"   ? dmath::sin(x.value)
                             : name == "cos" ? dmath::cos(x.value)
                                             : dmath::tan(x.value);
            return finite(r, Dimension{});
        }
        // asin, acos
        if (!isPlain(x.dim)) {
            return exprError(fn + "() needs a plain number, but it got " + describe(x.dim));
        }
        if (x.value < -1.0 || x.value > 1.0) {
            return exprError(fn + "() needs a value from -1 to 1");
        }
        return finite(name == "asin" ? dmath::asin(x.value) : dmath::acos(x.value), Dimension{0, 1});
    }

    std::string_view text_;
    const NameLookup& lookup_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

std::string powerText(std::string_view unit, int power)
{
    std::string out(unit);
    if (power != 1) {
        out += "^" + std::to_string(power);
    }
    return out;
}

// Checks a final value against the kind a parameter or field expects.
Result<double> checkKind(const Quantity& q, Parameter::Kind kind)
{
    const Dimension want = dimensionOf(kind);
    if (q.dim && *q.dim != want) {
        std::string message = "expected " + describe(want) + " but the expression gives " + describe(q.dim);
        if (*q.dim == Dimension{} && want == Dimension{1, 0}) {
            message += "; add a unit, e.g. '10 mm' or '0.5 in'";
        } else if (*q.dim == Dimension{} && want == Dimension{0, 1}) {
            message += "; add a unit, e.g. '30 deg'";
        }
        return exprError(message);
    }
    if (kind == Parameter::Kind::Count) {
        const double nearest = std::round(q.value);
        const double slack = 1e-9 * (nearest < 0 ? -nearest : nearest > 1 ? nearest : 1.0);
        if (q.value - nearest > slack || nearest - q.value > slack) {
            return exprError("a count must be a whole number, but the expression gives " + formatDouble(q.value));
        }
        return nearest;
    }
    return q.value;
}

const char* kindName(Parameter::Kind kind)
{
    switch (kind) {
    case Parameter::Kind::Length:
        return "length";
    case Parameter::Kind::Angle:
        return "angle";
    case Parameter::Kind::Count:
        return "count";
    case Parameter::Kind::Unitless:
        return "unitless";
    }
    return "unknown";
}

} // namespace

Result<Quantity> evaluateExpression(std::string_view expression, const NameLookup& lookup)
{
    try {
        return Evaluator(expression, lookup).run();
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while evaluating an expression", {}});
    }
}

Dimension dimensionOf(Parameter::Kind kind)
{
    switch (kind) {
    case Parameter::Kind::Length:
        return {1, 0};
    case Parameter::Kind::Angle:
        return {0, 1};
    case Parameter::Kind::Count:
    case Parameter::Kind::Unitless:
        break;
    }
    return {};
}

std::string describe(const std::optional<Dimension>& dim)
{
    if (!dim) {
        return "a value of unknown unit";
    }
    if (*dim == Dimension{}) {
        return "a plain number";
    }
    if (*dim == Dimension{1, 0}) {
        return "a length";
    }
    if (*dim == Dimension{0, 1}) {
        return "an angle";
    }
    if (*dim == Dimension{2, 0}) {
        return "an area (mm^2)";
    }
    std::string units;
    if (dim->length != 0) {
        units = powerText("mm", dim->length);
    }
    if (dim->angle != 0) {
        units += (units.empty() ? "" : "*") + powerText("rad", dim->angle);
    }
    return "a value in " + units;
}

bool isReservedName(std::string_view name)
{
    return findUnit(name) != nullptr || findFunction(name) != nullptr;
}

bool isValidParameterName(std::string_view name)
{
    if (name.empty() || !isIdentStart(name[0])) {
        return false;
    }
    for (const char c : name) {
        if (!isIdentChar(c)) {
            return false;
        }
    }
    return !isReservedName(name);
}

namespace {

// The names an expression refers to (identifiers that are not units, functions or constants), in order of first
// appearance, without duplicates. Only a lexical scan: evaluation reports any syntax error.
std::vector<std::string> referencedNames(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (isDigit(c) || c == '.') {
            while (i < text.size() && (isDigit(text[i]) || text[i] == '.')) {
                ++i;
            }
            // An exponent belongs to the number: "1e5" refers to nothing.
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
                    ++j;
                }
                if (j < text.size() && isDigit(text[j])) {
                    i = j;
                    while (i < text.size() && isDigit(text[i])) {
                        ++i;
                    }
                }
            }
            continue;
        }
        if (!isIdentStart(c)) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && isIdentChar(text[i])) {
            ++i;
        }
        const std::string name(text.substr(start, i - start));
        if (!isReservedName(name) && std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out;
}

// Evaluates a parameter list in dependency order. The order comes from a depth-first search with an explicit
// stack (no recursion, so a long chain of references cannot overflow the call stack), which also finds cycles.
// Parameters are visited in list order, so the first error found does not depend on anything but the list.
class ParameterEvaluator {
public:
    explicit ParameterEvaluator(const std::vector<Parameter>& params) : params_(params) {}

    Result<std::map<std::string, double>> run()
    {
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < params_.size(); ++i) {
            const std::string& name = params_[i].name;
            if (!isValidParameterName(name)) {
                return paramError(name, isReservedName(name)
                                            ? "the name is reserved for a unit or function"
                                            : "a name must start with a letter or '_' and contain only letters, "
                                              "digits and '_'");
            }
            if (!index.emplace(name, i).second) {
                return paramError(name, "the name is used by more than one parameter");
            }
        }
        // Edges to other parameters; names that are not parameters are left for evaluation to report.
        std::vector<std::vector<std::size_t>> deps(params_.size());
        for (std::size_t i = 0; i < params_.size(); ++i) {
            for (const std::string& name : referencedNames(params_[i].expression)) {
                auto it = index.find(name);
                if (it != index.end()) {
                    deps[i].push_back(it->second);
                }
            }
        }
        enum class Mark { New, OnStack, Done };
        std::vector<Mark> mark(params_.size(), Mark::New);
        std::vector<std::optional<Quantity>> values(params_.size());
        struct Frame {
            std::size_t param;
            std::size_t next; // next dependency to visit
        };
        std::vector<Frame> stack;
        for (std::size_t root = 0; root < params_.size(); ++root) {
            if (mark[root] != Mark::New) {
                continue;
            }
            stack.push_back({root, 0});
            mark[root] = Mark::OnStack;
            while (!stack.empty()) {
                Frame& top = stack.back();
                if (top.next < deps[top.param].size()) {
                    const std::size_t dep = deps[top.param][top.next++];
                    if (mark[dep] == Mark::OnStack) {
                        std::string path;
                        bool inCycle = false;
                        for (const Frame& f : stack) {
                            inCycle = inCycle || f.param == dep;
                            if (inCycle) {
                                path += params_[f.param].name + " -> ";
                            }
                        }
                        path += params_[dep].name;
                        return paramError(params_[dep].name, "it depends on itself through " + path);
                    }
                    if (mark[dep] == Mark::New) {
                        mark[dep] = Mark::OnStack;
                        stack.push_back({dep, 0});
                    }
                    continue;
                }
                // Every dependency is done: evaluate this one.
                const std::size_t i = top.param;
                stack.pop_back();
                auto value = evaluateOne(i, index, values);
                if (!value) {
                    return tl::unexpected(value.error());
                }
                values[i] = Quantity{*value, dimensionOf(params_[i].kind)};
                mark[i] = Mark::Done;
            }
        }
        std::map<std::string, double> out;
        for (std::size_t i = 0; i < params_.size(); ++i) {
            out.emplace(params_[i].name, values[i]->value);
        }
        return out;
    }

private:
    static tl::unexpected<Error> paramError(const std::string& name, const std::string& message)
    {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Parameter '" + name + "': " + message, {}});
    }

    Result<double> evaluateOne(std::size_t i, const std::map<std::string, std::size_t>& index,
                               const std::vector<std::optional<Quantity>>& values) const
    {
        const Parameter& param = params_[i];
        NameLookup lookup = [&](std::string_view name) -> Result<Quantity> {
            auto it = index.find(std::string(name));
            if (it == index.end()) {
                return exprError("unknown name '" + std::string(name) + "'");
            }
            if (!values[it->second]) {
                // Dependencies are evaluated first; reaching this means the lexical scan missed a name.
                return tl::unexpected(Error{
                    ErrorCode::Internal, "parameter '" + std::string(name) + "' was used before it was evaluated", {}});
            }
            return *values[it->second];
        };
        auto q = evaluateExpression(param.expression, lookup);
        if (!q) {
            Error error = q.error();
            error.message = "Parameter '" + param.name + "': " + error.message;
            return tl::unexpected(error);
        }
        auto value = checkKind(*q, param.kind);
        if (!value) {
            return paramError(param.name,
                              std::string("it is a ") + kindName(param.kind) + " parameter: " + value.error().message);
        }
        return value;
    }

    const std::vector<Parameter>& params_;
};

} // namespace
} // namespace detail

Result<double> evaluate(std::string_view expression, const std::map<std::string, double>& params, Parameter::Kind kind)
{
    try {
        detail::NameLookup lookup = [&params](std::string_view name) -> Result<detail::Quantity> {
            auto it = params.find(std::string(name));
            if (it == params.end()) {
                return detail::exprError("unknown name '" + std::string(name) + "'");
            }
            return detail::Quantity{it->second, std::nullopt};
        };
        auto q = detail::evaluateExpression(expression, lookup);
        if (q) {
            auto value = detail::checkKind(*q, kind);
            if (value) {
                return value;
            }
            q = tl::unexpected(value.error());
        }
        Error error = q.error();
        error.message = "Expression '" + std::string(expression) + "': " + error.message;
        return tl::unexpected(error);
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while evaluating an expression", {}});
    }
}

Result<std::map<std::string, double>> evaluateParameters(const std::vector<Parameter>& params)
{
    try {
        return detail::ParameterEvaluator(params).run();
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while evaluating parameters", {}});
    }
}

} // namespace ray::doc
