// Tool queries and their expressions (see query.h).
#include "tool_query.h"

#include "codec.h"
#include "json.h"
#include "json_number.h"

#include <cctype>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace ray::lib::detail {
namespace {

tl::unexpected<Error> bad(std::string message)
{
    return tl::unexpected<Error>(Error{ErrorCode::InvalidArgument, std::move(message), {}});
}

class ExprParser {
public:
    ExprParser(std::string_view text, const std::map<std::string, double>& vars) : text_(text), vars_(vars) {}

    Result<double> run()
    {
        auto v = sum();
        if (!v) {
            return v;
        }
        skipSpace();
        if (pos_ != text_.size()) {
            return error("unexpected '" + std::string(1, text_[pos_]) + "'");
        }
        if (!std::isfinite(*v)) {
            return error("the result is not a finite number");
        }
        return v;
    }

private:
    tl::unexpected<Error> error(const std::string& what) const
    {
        return bad("Expression \"" + std::string(text_) + "\": " + what);
    }

    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    Result<double> sum()
    {
        auto left = product();
        while (left) {
            skipSpace();
            if (pos_ >= text_.size() || (text_[pos_] != '+' && text_[pos_] != '-')) {
                break;
            }
            const char op = text_[pos_++];
            auto right = product();
            if (!right) {
                return right;
            }
            left = op == '+' ? *left + *right : *left - *right;
        }
        return left;
    }

    Result<double> product()
    {
        auto left = unary();
        while (left) {
            skipSpace();
            if (pos_ >= text_.size() || (text_[pos_] != '*' && text_[pos_] != '/')) {
                break;
            }
            const char op = text_[pos_++];
            auto right = unary();
            if (!right) {
                return right;
            }
            if (op == '/' && *right == 0) {
                return error("division by zero");
            }
            left = op == '*' ? *left * *right : *left / *right;
        }
        return left;
    }

    Result<double> unary()
    {
        skipSpace();
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            const char op = text_[pos_++];
            auto v = unary();
            if (!v) {
                return v;
            }
            return op == '-' ? -*v : *v;
        }
        return primary();
    }

    Result<double> primary()
    {
        if (++depth_ > 64) {
            return error("nested too deeply");
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            return error("ends too early");
        }
        const char c = text_[pos_];
        if (c == '(') {
            ++pos_;
            auto v = sum();
            if (!v) {
                return v;
            }
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ')') {
                return error("a ')' is missing");
            }
            ++pos_;
            --depth_;
            return v;
        }
        if (c >= '0' && c <= '9') {
            const std::size_t n = scanNumber(text_.substr(pos_));
            const auto v = n ? parseDouble(text_.substr(pos_, n)) : std::nullopt;
            if (!v) {
                return error("bad number");
            }
            pos_ += n;
            --depth_;
            return *v;
        }
        std::size_t start = pos_;
        if (c == '$') {
            start = ++pos_;
        }
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) != 0 ||
                                       text_[pos_] == '_' || text_[pos_] == '.')) {
            ++pos_;
        }
        if (pos_ == start || std::isdigit(static_cast<unsigned char>(text_[start])) != 0) {
            return error("unexpected '" + std::string(1, c) + "'");
        }
        const std::string name(text_.substr(start, pos_ - start));
        const auto it = vars_.find(name);
        if (it == vars_.end()) {
            return error("unknown variable " + name);
        }
        --depth_;
        return it->second;
    }

    std::string_view text_;
    const std::map<std::string, double>& vars_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

Result<double> numberOf(const json::Value& v, const std::map<std::string, double>& vars)
{
    if (v.isNumber()) {
        return v.asNumber();
    }
    if (v.isString()) {
        return evaluate(v.asString(), vars);
    }
    return bad("a number or an expression was expected");
}

std::string fold(std::string_view s, bool dropSpaces)
{
    std::string out;
    for (const char c : s) {
        if (dropSpaces && (c == ' ' || c == '\t')) {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

using Field = double (*)(const Tool&);
std::optional<Field> numericField(std::string_view key)
{
    static const std::pair<std::string_view, Field> kFields[] = {
        {"diameter", [](const Tool& t) { return t.diameter; }},
        {"shankDiameter", [](const Tool& t) { return t.shankDiameter; }},
        {"fluteLength", [](const Tool& t) { return t.fluteLength; }},
        {"shoulderLength", [](const Tool& t) { return t.shoulderLength; }},
        {"overallLength", [](const Tool& t) { return t.overallLength; }},
        {"cornerRadius", [](const Tool& t) { return t.cornerRadius; }},
        {"tipAngle", [](const Tool& t) { return t.tipAngleDeg; }},
        {"tipDiameter", [](const Tool& t) { return t.tipDiameter; }},
        {"threadPitch", [](const Tool& t) { return t.threadPitch; }},
        {"flutes", [](const Tool& t) { return static_cast<double>(t.flutes); }},
    };
    for (const auto& [name, field] : kFields) {
        if (name == key) {
            return field;
        }
    }
    return std::nullopt;
}

bool nearlyEqual(double a, double b)
{
    const double scale = std::fabs(b) > 1 ? std::fabs(b) : 1;
    return std::fabs(a - b) <= 1e-6 * scale;
}

Result<std::string> plainString(const json::Value& v, std::string_view key)
{
    if (!v.isString()) {
        return bad("\"" + std::string(key) + "\" must be a string");
    }
    if (!v.asString().empty() && v.asString().front() == '$') {
        return bad("\"" + std::string(key) + "\" names the variable " + v.asString() +
                   "; query variables are numbers, so substitute it before querying");
    }
    return v.asString();
}

} // namespace

Result<double> evaluate(std::string_view expression, const std::map<std::string, double>& vars)
{
    try {
        return ExprParser(expression, vars).run();
    } catch (const std::exception& e) {
        return bad(std::string("Expression failed: ") + e.what());
    }
}

Result<ToolPredicate> compileToolQuery(std::string_view queryJson, const std::map<std::string, double>& vars)
{
    auto parsed = json::parse(queryJson.empty() ? std::string_view("{}") : queryJson);
    if (!parsed) {
        return bad("The tool query is not valid JSON: " + parsed.error().message);
    }
    if (!parsed->isObject()) {
        return bad("The tool query must be a JSON object");
    }
    std::vector<ToolPredicate> tests;
    std::optional<Uuid> material;
    std::optional<std::string> opClass;
    for (const json::Member& m : parsed->asObject()) {
        const std::string& key = m.key;
        const json::Value& v = m.value;
        if (key == "type") {
            std::vector<ToolType> types;
            const json::Value::Array single{v};
            for (const json::Value& name : v.isArray() ? v.asArray() : single) {
                const auto type = name.isString() ? toolTypeFromName(name.asString()) : std::nullopt;
                if (!type) {
                    return bad("\"type\": unknown tool type " + json::write(name));
                }
                types.push_back(*type);
            }
            tests.push_back([types](const Tool& t) {
                for (const ToolType type : types) {
                    if (t.type == type) {
                        return true;
                    }
                }
                return false;
            });
        } else if (const auto field = numericField(key)) {
            const Field get = *field;
            if (v.isObject()) {
                std::optional<double> lo;
                std::optional<double> hi;
                for (const json::Member& bound : v.asObject()) {
                    auto value = numberOf(bound.value, vars);
                    if (!value) {
                        return bad("\"" + key + "\"." + bound.key + ": " + value.error().message);
                    }
                    if (bound.key == "min") {
                        lo = *value;
                    } else if (bound.key == "max") {
                        hi = *value;
                    } else {
                        return bad("\"" + key + "\": only \"min\" and \"max\" are allowed, not \"" + bound.key + "\"");
                    }
                }
                tests.push_back([get, lo, hi](const Tool& t) {
                    const double x = get(t);
                    return (!lo || x >= *lo || nearlyEqual(x, *lo)) && (!hi || x <= *hi || nearlyEqual(x, *hi));
                });
            } else {
                auto value = numberOf(v, vars);
                if (!value) {
                    return bad("\"" + key + "\": " + value.error().message);
                }
                const double want = *value;
                tests.push_back([get, want](const Tool& t) { return nearlyEqual(get(t), want); });
            }
        } else if (key == "threadSpec" || key == "bodyMaterial") {
            auto text = plainString(v, key);
            if (!text) {
                return tl::unexpected<Error>(text.error());
            }
            const bool spec = key == "threadSpec";
            const std::string want = fold(*text, spec);
            tests.push_back(
                [want, spec](const Tool& t) { return fold(spec ? t.threadSpec : t.bodyMaterial, spec) == want; });
        } else if (key == "material") {
            auto text = plainString(v, key);
            if (!text) {
                return tl::unexpected<Error>(text.error());
            }
            material = Uuid::fromString(*text);
            if (*material == Uuid{}) {
                return bad("\"material\" must be a material id");
            }
        } else if (key == "opClass") {
            auto text = plainString(v, key);
            if (!text) {
                return tl::unexpected<Error>(text.error());
            }
            opClass = *text;
        } else {
            return bad("The tool query has an unknown key \"" + key + "\"");
        }
    }
    if (material || opClass) {
        tests.push_back([material, opClass](const Tool& t) {
            for (const ToolPreset& p : t.presets) {
                if ((!material || p.material.value == *material) && (!opClass || p.opClass == *opClass)) {
                    return true;
                }
            }
            return false;
        });
    }
    return ToolPredicate([tests = std::move(tests)](const Tool& t) {
        for (const ToolPredicate& test : tests) {
            if (!test(t)) {
                return false;
            }
        }
        return true;
    });
}

} // namespace ray::lib::detail
