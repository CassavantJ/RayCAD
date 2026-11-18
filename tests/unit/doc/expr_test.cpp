// Parameter expressions: ray::doc::evaluate and ray::doc::evaluateParameters.
#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;
using ray::ErrorCode;
using ray::doc::evaluate;
using ray::doc::evaluateParameters;
using ray::doc::Parameter;
using Kind = ray::doc::Parameter::Kind;

namespace {

constexpr double kPi = 3.141592653589793;
const std::map<std::string, double> kNoParams;

double eval(const std::string& text, Kind kind = Kind::Unitless,
            const std::map<std::string, double>& params = kNoParams)
{
    const auto r = evaluate(text, params, kind);
    INFO(text);
    if (!r) {
        INFO(r.error().message);
        REQUIRE(r.has_value());
    }
    return *r;
}

std::string evalError(const std::string& text, Kind kind = Kind::Unitless,
                      const std::map<std::string, double>& params = kNoParams)
{
    const auto r = evaluate(text, params, kind);
    INFO(text);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(r.error().message, ContainsSubstring("Expression '" + text + "'"));
    return r.error().message;
}

Parameter param(const std::string& name, const std::string& expression, Kind kind)
{
    Parameter p{};
    p.id = ray::ParamId{ray::Uuid::generate()};
    p.name = name;
    p.expression = expression;
    p.kind = kind;
    return p;
}

std::string paramsError(const std::vector<Parameter>& params)
{
    const auto r = evaluateParameters(params);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    return r.error().message;
}

} // namespace

TEST_CASE("numbers, precedence and associativity", "[doc][expr]")
{
    CHECK(eval("42") == 42);
    CHECK(eval("  .5 ") == 0.5);
    CHECK(eval("1.5e3") == 1500);
    CHECK(eval("007") == 7);
    CHECK(eval("2 + 3 * 4") == 14);
    CHECK(eval("(2 + 3) * 4") == 20);
    CHECK(eval("10 - 4 - 3") == 3);
    CHECK(eval("24 / 4 / 2") == 3);
    CHECK(eval("10 / 4") == 2.5);
    CHECK(eval("2 ^ 3 ^ 2") == 512);
    CHECK(eval("-2 ^ 2") == -4);
    CHECK(eval("(-2) ^ 2") == 4);
    CHECK(eval("2 ^ -1") == 0.5);
    CHECK(eval("2 * -3") == -6);
    CHECK(eval("--3") == 3);
    CHECK(eval("+3") == 3);
    CHECK(eval("1 - -1") == 2);
    CHECK(eval("2 ^ 10 - 24") == 1000);
    CHECK(eval("0.1 + 0.2") == 0.1 + 0.2);
}

TEST_CASE("units convert to mm and radians", "[doc][expr]")
{
    CHECK(eval("25.4 mm", Kind::Length) == 25.4);
    CHECK(eval("1 in", Kind::Length) == 25.4);
    CHECK(eval("0.5 in", Kind::Length) == 12.7);
    CHECK(eval("4in", Kind::Length) == 101.6);
    CHECK(eval("1 in + 1 mm", Kind::Length) == 26.4);
    CHECK(eval("1 in - 25.4 mm", Kind::Length) == 0);
    CHECK(eval("(1 + 1) in", Kind::Length) == 50.8);
    CHECK(eval("2 * 3 mm", Kind::Length) == 6);
    CHECK(eval("10 mm / 2", Kind::Length) == 5);
    CHECK(eval("1 in / 1 mm") == 25.4);                   // a ratio of lengths is a plain number
    CHECK(eval("6 mm * 2 mm / 3 mm", Kind::Length) == 4); // mm^2 / mm
    CHECK(eval("1.5 rad", Kind::Angle) == 1.5);
    CHECK_THAT(eval("180 deg", Kind::Angle), WithinULP(kPi, 1));
    CHECK_THAT(eval("90 deg", Kind::Angle), WithinULP(kPi / 2, 1));
    CHECK_THAT(eval("pi rad", Kind::Angle), WithinULP(kPi, 0));
    CHECK_THAT(eval("1 rad + 1 deg", Kind::Angle), WithinULP(1 + kPi / 180, 1));
}

TEST_CASE("functions", "[doc][expr]")
{
    CHECK(eval("sqrt(16)") == 4);
    CHECK(eval("sqrt(9 mm * 4 mm)", Kind::Length) == 6);
    CHECK(eval("abs(-3 mm)", Kind::Length) == 3);
    CHECK(eval("min(3 mm, 1 in, 2 mm)", Kind::Length) == 2);
    CHECK(eval("max(3 mm, 1 in, 2 mm)", Kind::Length) == 25.4);
    CHECK(eval("min(4)") == 4);
    CHECK(eval("floor(2.7)") == 2);
    CHECK(eval("floor(-2.5)") == -3);
    CHECK(eval("ceil(2.1)") == 3);
    CHECK(eval("round(2.5)") == 3);
    CHECK(eval("round(-2.5)") == -3);
    CHECK(eval("round(25.4 mm / 1 in)") == 1);
    CHECK(eval("sin(90 deg)") == 1);
    CHECK(eval("sin(0)") == 0);
    CHECK(eval("cos(0 deg)") == 1);
    CHECK(eval("cos(pi)") == -1);
    CHECK_THAT(eval("tan(45 deg)"), WithinULP(1.0, 1));
    CHECK(eval("pi") == kPi);
    CHECK(eval("pi()") == kPi);
    CHECK(eval("2 * pi") == 2 * kPi);
    CHECK_THAT(eval("atan2(1 mm, 1 mm)", Kind::Angle), WithinULP(kPi / 4, 1));
    CHECK_THAT(eval("asin(1)", Kind::Angle), WithinULP(kPi / 2, 1));
    CHECK(eval("acos(1)", Kind::Angle) == 0);
    CHECK(eval("max(1, 2) ^ 2 + sqrt(min(9, 16))") == 7);
}

TEST_CASE("names from a plain value map", "[doc][expr]")
{
    const std::map<std::string, double> params = {{"width", 10.0}, {"count", 4.0}, {"_a1", 2.0}};
    CHECK(eval("width * 2", Kind::Length, params) == 20);
    CHECK(eval("width / 2 + 1 mm", Kind::Length, params) == 6);
    CHECK(eval("count + 1", Kind::Count, params) == 5);
    CHECK(eval("_a1 ^ 3", Kind::Unitless, params) == 8);
    CHECK(eval("width mm", Kind::Length, params) == 10);
}

TEST_CASE("expression errors carry a message", "[doc][expr]")
{
    CHECK_THAT(evalError("foo + 1"), ContainsSubstring("unknown name 'foo'"));
    CHECK_THAT(evalError("foo(1)"), ContainsSubstring("unknown function 'foo'"));
    CHECK_THAT(evalError("1 / 0"), ContainsSubstring("division by zero"));
    CHECK_THAT(evalError("1 mm / (2 mm - 2 mm)", Kind::Length), ContainsSubstring("division by zero"));
    CHECK_THAT(evalError("0 ^ -1"), ContainsSubstring("division by zero"));
    CHECK_THAT(evalError("1 mm + 1 deg", Kind::Length),
               ContainsSubstring("unit mismatch: cannot add an angle to a length"));
    CHECK_THAT(evalError("1 mm - 1", Kind::Length), ContainsSubstring("unit mismatch"));
    CHECK_THAT(evalError("min(1 mm, 1 deg)", Kind::Length), ContainsSubstring("unit mismatch"));
    CHECK_THAT(evalError("30 deg", Kind::Length),
               ContainsSubstring("expected a length but the expression gives an angle"));
    CHECK_THAT(evalError("10", Kind::Length), ContainsSubstring("add a unit"));
    CHECK_THAT(evalError("45", Kind::Angle), ContainsSubstring("add a unit"));
    CHECK_THAT(evalError("2 mm * 3 mm", Kind::Length), ContainsSubstring("an area"));
    CHECK_THAT(evalError("2 mm", Kind::Unitless), ContainsSubstring("expected a plain number"));
    CHECK_THAT(evalError("2.5", Kind::Count), ContainsSubstring("whole number"));
    CHECK_THAT(evalError("2 ^ 0.5"), ContainsSubstring("whole-number exponents"));
    CHECK_THAT(evalError("2 ^ 1 mm"), ContainsSubstring("exponent cannot have a unit"));
    CHECK_THAT(evalError("sqrt(-1)"), ContainsSubstring("negative"));
    CHECK_THAT(evalError("sqrt(2 mm)", Kind::Length), ContainsSubstring("has no unit"));
    CHECK_THAT(evalError("floor(2 mm)", Kind::Length), ContainsSubstring("needs a plain number"));
    CHECK_THAT(evalError("sin(1 mm)"), ContainsSubstring("needs an angle"));
    CHECK_THAT(evalError("asin(2)", Kind::Angle), ContainsSubstring("from -1 to 1"));
    CHECK_THAT(evalError("atan2(1, 2, 3)", Kind::Angle), ContainsSubstring("takes 2 arguments, not 3"));
    CHECK_THAT(evalError("max()"), ContainsSubstring("at least 1"));
    CHECK_THAT(evalError("1e999"), ContainsSubstring("too large"));
    CHECK_THAT(evalError("1e200 * 1e200"), ContainsSubstring("too large"));
    CHECK_THAT(evalError("10 ^ 64 ^ 64"), ContainsSubstring("exponents"));
    CHECK_THAT(evalError("mm"), ContainsSubstring("must follow a number"));
    CHECK_THAT(evalError("sqrt 4"), ContainsSubstring("needs arguments"));
    CHECK_THAT(evalError(""), ContainsSubstring("empty"));
    CHECK_THAT(evalError("2 3"), ContainsSubstring("missing operator"));
    CHECK_THAT(evalError("2 inch", Kind::Length), ContainsSubstring("missing operator"));
    for (const char* bad :
         {"2 +", "(2", "2)", "2 ** 3", "*", "1,2", "min(1,", "min(1 2)", "4 $", "#", ". mm", "2e", "((((1))))))"}) {
        evalError(bad);
    }
    const std::string deep = std::string(500, '(') + "1" + std::string(500, ')');
    CHECK_THAT(evalError(deep), ContainsSubstring("nested too deeply"));
}

TEST_CASE("parameters refer to each other with unit checking", "[doc][expr]")
{
    const std::vector<Parameter> params = {
        param("height", "width / 2", Kind::Length), // refers to a parameter defined later
        param("width", "4 in", Kind::Length),
        param("count", "3", Kind::Count),
        param("pitch", "360 deg / count", Kind::Angle),
        param("ratio", "width / height", Kind::Unitless),
        param("diag", "sqrt(width ^ 2 + height ^ 2)", Kind::Length),
        param("half", "round(count / 2)", Kind::Count),
    };
    const auto values = evaluateParameters(params);
    REQUIRE(values.has_value());
    CHECK(values->size() == params.size());
    CHECK(values->at("width") == 101.6);
    CHECK(values->at("height") == 50.8);
    CHECK(values->at("count") == 3);
    CHECK_THAT(values->at("pitch"), WithinULP(2 * kPi / 3, 1));
    CHECK(values->at("ratio") == 2);
    CHECK_THAT(values->at("diag"), WithinRel(50.8 * 2.23606797749979, 1e-14));
    CHECK(values->at("half") == 2);
    CHECK(evaluateParameters({}).value().empty());
}

TEST_CASE("parameter errors name the parameter", "[doc][expr]")
{
    using Catch::Matchers::StartsWith;
    // Cycles.
    CHECK_THAT(paramsError({param("a", "b + 1 mm", Kind::Length), param("b", "c", Kind::Length),
                            param("c", "a * 2", Kind::Length)}),
               StartsWith("Parameter 'a'") && ContainsSubstring("depends on itself through a -> b -> c -> a"));
    CHECK_THAT(paramsError({param("x", "x * 2", Kind::Unitless)}),
               StartsWith("Parameter 'x'") && ContainsSubstring("x -> x"));
    CHECK_THAT(paramsError({param("ok", "1", Kind::Unitless), param("p", "q", Kind::Unitless),
                            param("q", "p", Kind::Unitless)}),
               StartsWith("Parameter 'p'") && ContainsSubstring("p -> q -> p"));
    // Unknown names, division by zero, unit mismatches.
    CHECK_THAT(paramsError({param("p", "q * 2", Kind::Unitless)}),
               StartsWith("Parameter 'p'") && ContainsSubstring("unknown name 'q'"));
    CHECK_THAT(paramsError({param("n", "0", Kind::Count), param("step", "10 mm / n", Kind::Length)}),
               StartsWith("Parameter 'step'") && ContainsSubstring("division by zero"));
    CHECK_THAT(paramsError({param("a", "b + 1 mm", Kind::Length), param("b", "30 deg", Kind::Angle)}),
               StartsWith("Parameter 'a'") && ContainsSubstring("unit mismatch"));
    // The error is reported for the parameter that is wrong, not the one that refers to it.
    CHECK_THAT(paramsError({param("a", "b + 1 mm", Kind::Length), param("b", "30 deg", Kind::Length)}),
               StartsWith("Parameter 'b'") && ContainsSubstring("expected a length but the expression gives an angle"));
    CHECK_THAT(paramsError({param("n", "2.5", Kind::Count)}),
               StartsWith("Parameter 'n'") && ContainsSubstring("whole"));
    CHECK_THAT(paramsError({param("a", "1 +", Kind::Unitless)}), StartsWith("Parameter 'a'"));
    // Names.
    CHECK_THAT(paramsError({param("a", "1", Kind::Unitless), param("a", "2", Kind::Unitless)}),
               StartsWith("Parameter 'a'") && ContainsSubstring("more than one"));
    CHECK_THAT(paramsError({param("mm", "1", Kind::Unitless)}), ContainsSubstring("reserved"));
    CHECK_THAT(paramsError({param("sqrt", "1", Kind::Unitless)}), ContainsSubstring("reserved"));
    CHECK_THAT(paramsError({param("pi", "1", Kind::Unitless)}), ContainsSubstring("reserved"));
    CHECK_THAT(paramsError({param("2x", "1", Kind::Unitless)}), ContainsSubstring("must start with a letter"));
    CHECK_THAT(paramsError({param("a-b", "1", Kind::Unitless)}), ContainsSubstring("must start with a letter"));
    CHECK_THAT(paramsError({param("", "1", Kind::Unitless)}), ContainsSubstring("must start with a letter"));
}

TEST_CASE("long parameter chains are evaluated without deep recursion", "[doc][expr]")
{
    std::vector<Parameter> chain;
    for (int i = 0; i < 5000; ++i) {
        chain.push_back(
            param("p" + std::to_string(i), i == 0 ? "1 mm" : "p" + std::to_string(i - 1) + " + 1 mm", Kind::Length));
    }
    auto values = evaluateParameters(chain);
    REQUIRE(values.has_value());
    CHECK(values->at("p4999") == 5000);
    std::reverse(chain.begin(), chain.end());
    values = evaluateParameters(chain);
    REQUIRE(values.has_value());
    CHECK(values->at("p4999") == 5000);
    chain.back().expression = "p4999 + 1 mm"; // p0 -> p4999 closes a cycle through all of them
    CHECK_THAT(paramsError(chain), ContainsSubstring("depends on itself through p4999 -> p4998"));
}
