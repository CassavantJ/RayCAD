#include "json.h"
#include "../core/from_chars_double.h"

#include <ray/document.h>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>

namespace ray::api::json {

Value Value::boolean(bool b)
{
    Value v;
    v.v_ = b;
    return v;
}

Value Value::number(double d)
{
    Value v;
    v.v_ = Num{d, {}};
    return v;
}

Value Value::integer(std::int64_t i)
{
    Value v;
    v.v_ = Num{static_cast<double>(i), std::to_string(i)};
    return v;
}

Value Value::string(std::string s)
{
    Value v;
    v.v_ = std::move(s);
    return v;
}

Value Value::array(Array a)
{
    Value v;
    v.v_ = std::move(a);
    return v;
}

Value Value::object(Object o)
{
    Value v;
    v.v_ = std::move(o);
    return v;
}

bool Value::isInteger() const noexcept
{
    if (!isNumber()) {
        return false;
    }
    const double d = std::get<Num>(v_).value;
    return std::isfinite(d) && std::floor(d) == d;
}

const Value* Value::find(std::string_view key) const
{
    if (!isObject()) {
        return nullptr;
    }
    for (const auto& [k, v] : std::get<Object>(v_)) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

Value* Value::find(std::string_view key)
{
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

Value& Value::set(std::string key, Value value)
{
    if (Value* existing = find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    auto& members = asObject();
    members.emplace_back(std::move(key), std::move(value));
    return members.back().second;
}

bool Value::erase(std::string_view key)
{
    if (!isObject()) {
        return false;
    }
    auto& members = asObject();
    for (auto it = members.begin(); it != members.end(); ++it) {
        if (it->first == key) {
            members.erase(it);
            return true;
        }
    }
    return false;
}

std::string Value::stringOr(std::string_view key, std::string fallback) const
{
    const Value* v = find(key);
    return v && v->isString() ? v->asString() : fallback;
}

bool Value::boolOr(std::string_view key, bool fallback) const
{
    const Value* v = find(key);
    return v && v->isBool() ? v->asBool() : fallback;
}

double Value::numberOr(std::string_view key, double fallback) const
{
    const Value* v = find(key);
    return v && v->isNumber() ? v->asNumber() : fallback;
}

bool operator==(const Value& a, const Value& b)
{
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
    case Value::Type::Null:
        return true;
    case Value::Type::Bool:
        return a.asBool() == b.asBool();
    case Value::Type::Number:
        return a.asNumber() == b.asNumber();
    case Value::Type::String:
        return a.asString() == b.asString();
    case Value::Type::Array:
        return a.asArray() == b.asArray();
    case Value::Type::Object: {
        // Member order does not matter.
        const auto& x = a.asObject();
        const auto& y = b.asObject();
        if (x.size() != y.size()) {
            return false;
        }
        for (const auto& [key, value] : x) {
            const Value* other = b.find(key);
            if (!other || !(*other == value)) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

const char* typeName(Value::Type type)
{
    switch (type) {
    case Value::Type::Null:
        return "null";
    case Value::Type::Bool:
        return "boolean";
    case Value::Type::Number:
        return "number";
    case Value::Type::String:
        return "string";
    case Value::Type::Array:
        return "array";
    case Value::Type::Object:
        return "object";
    }
    return "value";
}

// ---------- Reader ----------
class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Result<Value> run()
    {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
        Value v;
        if (!value(v, 0)) {
            return fail();
        }
        skipWs();
        if (pos_ != s_.size()) {
            error_ = "unexpected text after the value";
            return fail();
        }
        return v;
    }

private:
    tl::unexpected<Error> fail() const
    {
        return tl::unexpected<Error>(
            Error{ErrorCode::FormatError, "Invalid JSON at byte " + std::to_string(pos_) + ": " + error_, {}});
    }

    void skipWs()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word)
    {
        if (s_.substr(pos_, word.size()) != word) {
            error_ = "unknown literal";
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool value(Value& out, int depth)
    {
        if (depth > 128) {
            error_ = "nested too deeply";
            return false;
        }
        skipWs();
        if (pos_ >= s_.size()) {
            error_ = "unexpected end of input";
            return false;
        }
        const char c = s_[pos_];
        switch (c) {
        case 'n':
            out = Value();
            return literal("null");
        case 't':
            out = Value::boolean(true);
            return literal("true");
        case 'f':
            out = Value::boolean(false);
            return literal("false");
        case '"': {
            std::string str;
            if (!string(str)) {
                return false;
            }
            out = Value::string(std::move(str));
            return true;
        }
        case '[':
            return array(out, depth);
        case '{':
            return object(out, depth);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return number(out);
            }
            error_ = "unexpected character";
            return false;
        }
    }

    bool array(Value& out, int depth)
    {
        ++pos_; // [
        Array items;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            out = Value::array(std::move(items));
            return true;
        }
        for (;;) {
            Value item;
            if (!value(item, depth + 1)) {
                return false;
            }
            items.push_back(std::move(item));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                out = Value::array(std::move(items));
                return true;
            }
            error_ = "expected ',' or ']'";
            return false;
        }
    }

    bool object(Value& out, int depth)
    {
        ++pos_; // {
        Value obj = Value::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            out = std::move(obj);
            return true;
        }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                error_ = "expected a member name";
                return false;
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            if (obj.find(key)) {
                error_ = "duplicate member name \"" + key + "\"";
                return false;
            }
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                error_ = "expected ':'";
                return false;
            }
            ++pos_;
            Value member;
            if (!value(member, depth + 1)) {
                return false;
            }
            obj.asObject().emplace_back(std::move(key), std::move(member));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                out = std::move(obj);
                return true;
            }
            error_ = "expected ',' or '}'";
            return false;
        }
    }

    static void appendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(std::uint32_t& out)
    {
        if (pos_ + 4 > s_.size()) {
            error_ = "truncated \\u escape";
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            out <<= 4;
            if (h >= '0' && h <= '9') {
                out |= static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                out |= static_cast<std::uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                out |= static_cast<std::uint32_t>(h - 'A' + 10);
            } else {
                error_ = "bad \\u escape";
                return false;
            }
        }
        return true;
    }

    bool string(std::string& out)
    {
        ++pos_; // opening quote
        for (;;) {
            if (pos_ >= s_.size()) {
                error_ = "unterminated string";
                return false;
            }
            const char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "control character in a string";
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                error_ = "unterminated string";
                return false;
            }
            const char e = s_[pos_++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                        error_ = "unpaired surrogate";
                        return false;
                    }
                    pos_ += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        error_ = "unpaired surrogate";
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    error_ = "unpaired surrogate";
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                error_ = "bad escape";
                return false;
            }
        }
    }

    bool number(Value& out)
    {
        const std::size_t start = pos_;
        if (s_[pos_] == '-') {
            ++pos_;
        }
        const auto digits = [this] {
            std::size_t n = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                ++n;
            }
            return n;
        };
        if (pos_ < s_.size() && s_[pos_] == '0') {
            ++pos_;
        } else if (digits() == 0) {
            error_ = "bad number";
            return false;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) {
                error_ = "bad number";
                return false;
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (digits() == 0) {
                error_ = "bad number";
                return false;
            }
        }
        const std::string text(s_.substr(start, pos_ - start));
        double d = 0;
        const auto res = compat::fromChars(text.data(), text.data() + text.size(), d);
        if (res.ec != std::errc() || !std::isfinite(d)) {
            error_ = "number out of range";
            return false;
        }
        out = Value::number(d);
        std::get<Value::Num>(out.v_).text = text;
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

Result<Value> parse(std::string_view text)
{
    try {
        return Parser(text).run();
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(Error{ErrorCode::Internal, std::string("JSON reader failed: ") + e.what(), {}});
    }
}

// ---------- Writer ----------
namespace {
void writeString(const std::string& s, std::string& out)
{
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}
} // namespace

void writeTo(const Value& v, std::string& out)
{
    switch (v.type()) {
    case Value::Type::Null:
        out += "null";
        return;
    case Value::Type::Bool:
        out += v.asBool() ? "true" : "false";
        return;
    case Value::Type::Number: {
        const auto& num = std::get<Value::Num>(v.v_);
        if (!num.text.empty()) {
            out += num.text;
            return;
        }
        if (!std::isfinite(num.value)) {
            out += "null";
            return;
        }
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof buf, num.value);
        out.append(buf, res.ptr);
        return;
    }
    case Value::Type::String:
        writeString(v.asString(), out);
        return;
    case Value::Type::Array: {
        out += '[';
        bool first = true;
        for (const Value& item : v.asArray()) {
            if (!first) {
                out += ',';
            }
            first = false;
            writeTo(item, out);
        }
        out += ']';
        return;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, member] : v.asObject()) {
            if (!first) {
                out += ',';
            }
            first = false;
            writeString(key, out);
            out += ':';
            writeTo(member, out);
        }
        out += '}';
        return;
    }
    }
}

std::string write(const Value& value)
{
    std::string out;
    writeTo(value, out);
    return out;
}

std::string canonical(const Value& value)
{
    return doc::canonicalJson(write(value));
}

} // namespace ray::api::json
