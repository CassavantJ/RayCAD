#include "schema.h"

#include <cmath>
#include <exception>
#include <optional>
#include <regex>
#include <string>

namespace ray::api::schema {
namespace {

using json::Value;

std::size_t codePoints(const std::string& s)
{
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

std::string pointerText(const std::string& path)
{
    return path.empty() ? std::string("(the arguments)") : path;
}

tl::unexpected<Error> problem(const std::string& path, const std::string& what)
{
    return tl::unexpected<Error>(Error{ErrorCode::InvalidArgument, pointerText(path) + ": " + what, {path}});
}

bool typeMatches(const std::string& name, const Value& v)
{
    if (name == "integer") {
        return v.isInteger();
    }
    if (name == "number") {
        return v.isNumber();
    }
    if (name == "null") {
        return v.isNull();
    }
    if (name == "boolean") {
        return v.isBool();
    }
    if (name == "string") {
        return v.isString();
    }
    if (name == "array") {
        return v.isArray();
    }
    if (name == "object") {
        return v.isObject();
    }
    return false;
}

std::string shortText(const Value& v)
{
    std::string text = json::write(v);
    if (text.size() > 60) {
        text = text.substr(0, 57) + "...";
    }
    return text;
}

Status check(const Value& schema, const Value& v, const std::string& path, int depth);

Status checkNumber(const Value& schema, const Value& v, const std::string& path)
{
    const double d = v.asNumber();
    if (const Value* m = schema.find("minimum"); m && m->isNumber() && d < m->asNumber()) {
        return problem(path, "must be at least " + json::write(*m));
    }
    if (const Value* m = schema.find("maximum"); m && m->isNumber() && d > m->asNumber()) {
        return problem(path, "must be at most " + json::write(*m));
    }
    if (const Value* m = schema.find("exclusiveMinimum"); m && m->isNumber() && d <= m->asNumber()) {
        return problem(path, "must be greater than " + json::write(*m));
    }
    if (const Value* m = schema.find("exclusiveMaximum"); m && m->isNumber() && d >= m->asNumber()) {
        return problem(path, "must be less than " + json::write(*m));
    }
    return {};
}

Status checkString(const Value& schema, const Value& v, const std::string& path)
{
    const std::string& s = v.asString();
    const std::size_t n = codePoints(s);
    if (const Value* m = schema.find("minLength"); m && m->isNumber() && static_cast<double>(n) < m->asNumber()) {
        return problem(path, m->asNumber() == 1 ? "must not be empty"
                                                : "must be at least " + json::write(*m) + " characters long");
    }
    if (const Value* m = schema.find("maxLength"); m && m->isNumber() && static_cast<double>(n) > m->asNumber()) {
        return problem(path, "must be at most " + json::write(*m) + " characters long");
    }
    if (const Value* p = schema.find("pattern"); p && p->isString()) {
        try {
            const std::regex re(p->asString(), std::regex::ECMAScript);
            if (!std::regex_search(s, re)) {
                return problem(path, "does not match the pattern " + p->asString());
            }
        } catch (const std::exception&) {
            return tl::unexpected<Error>(
                Error{ErrorCode::Internal, "The schema has an invalid pattern: " + p->asString(), {path}});
        }
    }
    return {};
}

Status checkArray(const Value& schema, const Value& v, const std::string& path, int depth)
{
    const auto& items = v.asArray();
    if (const Value* m = schema.find("minItems");
        m && m->isNumber() && static_cast<double>(items.size()) < m->asNumber()) {
        return problem(path, "must have at least " + json::write(*m) + " items");
    }
    if (const Value* m = schema.find("maxItems");
        m && m->isNumber() && static_cast<double>(items.size()) > m->asNumber()) {
        return problem(path, "must have at most " + json::write(*m) + " items");
    }
    if (const Value* itemSchema = schema.find("items");
        itemSchema && (itemSchema->isObject() || itemSchema->isBool())) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (auto st = check(*itemSchema, items[i], pointerAppend(path, std::to_string(i)), depth + 1); !st) {
                return st;
            }
        }
    }
    return {};
}

Status checkObject(const Value& schema, const Value& v, const std::string& path, int depth)
{
    const auto& members = v.asObject();
    if (const Value* m = schema.find("minProperties");
        m && m->isNumber() && static_cast<double>(members.size()) < m->asNumber()) {
        return problem(path, "must have at least " + json::write(*m) + " properties");
    }
    if (const Value* m = schema.find("maxProperties");
        m && m->isNumber() && static_cast<double>(members.size()) > m->asNumber()) {
        return problem(path, "must have at most " + json::write(*m) + " properties");
    }
    const Value* properties = schema.find("properties");
    if (const Value* required = schema.find("required"); required && required->isArray()) {
        for (const Value& name : required->asArray()) {
            if (name.isString() && !v.find(name.asString())) {
                return problem(pointerAppend(path, name.asString()), "is required");
            }
        }
    }
    const Value* additional = schema.find("additionalProperties");
    for (const auto& [key, member] : members) {
        const std::string memberPath = pointerAppend(path, key);
        const Value* own = properties ? properties->find(key) : nullptr;
        if (own) {
            if (auto st = check(*own, member, memberPath, depth + 1); !st) {
                return st;
            }
        } else if (additional) {
            if (additional->isBool() && !additional->asBool()) {
                return problem(memberPath, "is not a known argument");
            }
            if (additional->isObject()) {
                if (auto st = check(*additional, member, memberPath, depth + 1); !st) {
                    return st;
                }
            }
        }
    }
    return {};
}

Status check(const Value& schema, const Value& v, const std::string& path, int depth)
{
    if (depth > 64) {
        return problem(path, "is nested too deeply");
    }
    if (schema.isBool()) {
        return schema.asBool() ? Status{} : problem(path, "is not allowed");
    }
    if (!schema.isObject()) {
        return {};
    }
    if (const Value* type = schema.find("type")) {
        bool ok = false;
        std::string expected;
        if (type->isString()) {
            ok = typeMatches(type->asString(), v);
            expected = type->asString();
        } else if (type->isArray()) {
            for (const Value& t : type->asArray()) {
                if (t.isString()) {
                    ok = ok || typeMatches(t.asString(), v);
                    expected += (expected.empty() ? "" : " or ") + t.asString();
                }
            }
        } else {
            ok = true;
        }
        if (!ok) {
            const char* article =
                (!expected.empty() && std::string("aeiou").find(expected[0]) != std::string::npos) ? "an " : "a ";
            std::string got = json::typeName(v.type());
            if (v.isNumber() && expected == "integer") {
                got = "a number with a fraction";
            }
            return problem(path, "must be " + std::string(article) + expected + ", not " + got);
        }
    }
    if (const Value* e = schema.find("enum"); e && e->isArray()) {
        bool found = false;
        std::string choices;
        for (const Value& option : e->asArray()) {
            found = found || option == v;
            choices += (choices.empty() ? "" : ", ") + json::write(option);
        }
        if (!found) {
            return problem(path, "must be one of " + choices + ", not " + shortText(v));
        }
    }
    if (const Value* c = schema.find("const"); c && !(*c == v)) {
        return problem(path, "must be " + json::write(*c));
    }
    if (v.isNumber()) {
        if (auto st = checkNumber(schema, v, path); !st) {
            return st;
        }
    } else if (v.isString()) {
        if (auto st = checkString(schema, v, path); !st) {
            return st;
        }
    } else if (v.isArray()) {
        if (auto st = checkArray(schema, v, path, depth); !st) {
            return st;
        }
    } else if (v.isObject()) {
        if (auto st = checkObject(schema, v, path, depth); !st) {
            return st;
        }
    }
    if (const Value* all = schema.find("allOf"); all && all->isArray()) {
        for (const Value& sub : all->asArray()) {
            if (auto st = check(sub, v, path, depth + 1); !st) {
                return st;
            }
        }
    }
    if (const Value* any = schema.find("anyOf"); any && any->isArray() && !any->asArray().empty()) {
        std::optional<Error> first;
        bool ok = false;
        for (const Value& sub : any->asArray()) {
            auto st = check(sub, v, path, depth + 1);
            if (st) {
                ok = true;
                break;
            }
            if (!first) {
                first = st.error();
            }
        }
        if (!ok) {
            return tl::unexpected<Error>(*first);
        }
    }
    if (const Value* one = schema.find("oneOf"); one && one->isArray() && !one->asArray().empty()) {
        int matches = 0;
        std::optional<Error> first;
        for (const Value& sub : one->asArray()) {
            auto st = check(sub, v, path, depth + 1);
            if (st) {
                ++matches;
            } else if (!first) {
                first = st.error();
            }
        }
        if (matches == 0) {
            return tl::unexpected<Error>(*first);
        }
        if (matches > 1) {
            return problem(path, "matches more than one of the allowed forms");
        }
    }
    if (const Value* n = schema.find("not"); n && (n->isObject() || n->isBool())) {
        if (check(*n, v, path, depth + 1)) {
            return problem(path, "has a form that is not allowed");
        }
    }
    return {};
}

} // namespace

std::string pointerAppend(const std::string& base, std::string_view token)
{
    std::string out = base;
    out += '/';
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

Status validate(const json::Value& schema, const json::Value& instance)
{
    try {
        return check(schema, instance, std::string(), 0);
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(Error{ErrorCode::Internal, std::string("Schema check failed: ") + e.what(), {}});
    }
}

void fillDefaults(const json::Value& schema, json::Value& instance)
{
    if (!instance.isObject()) {
        return;
    }
    const json::Value* properties = schema.find("properties");
    if (!properties || !properties->isObject()) {
        return;
    }
    for (const auto& [key, propertySchema] : properties->asObject()) {
        if (instance.find(key)) {
            continue;
        }
        if (const json::Value* d = propertySchema.find("default")) {
            instance.set(key, *d);
        }
    }
}

} // namespace ray::api::schema
