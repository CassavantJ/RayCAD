// A minimal JSON reader for the property harness (task T-058): reads an operation type's parameter schema
// (IOperationType::paramsSchemaJson) so random parameters can be drawn from its ranges. RFC 8259 values; no
// validation beyond what parsing needs. Test-only.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::cam::invariants::json {

// An object member is a plain struct, not a std::pair<std::string, Value>: instantiating that pair while Value is
// still incomplete fails to compile with libstdc++ 15 under Clang (the pair's default constructor asks whether Value
// is implicitly default-constructible, which needs Value's destructor, which needs the pair). src/cam/json.h does
// the same.
struct Member;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<Member> object;

    const Value* find(std::string_view key) const;
};

struct Member {
    std::string key;
    Value value;
};

inline const Value* Value::find(std::string_view key) const
{
    for (const auto& [k, v] : object) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    std::optional<Value> read()
    {
        Value v;
        if (!value(v)) {
            return std::nullopt;
        }
        space();
        if (p_ != s_.size()) {
            return std::nullopt;
        }
        return v;
    }

private:
    void space()
    {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) {
            ++p_;
        }
    }
    bool literal(std::string_view w)
    {
        if (s_.substr(p_, w.size()) != w) {
            return false;
        }
        p_ += w.size();
        return true;
    }
    bool str(std::string& out)
    {
        if (p_ >= s_.size() || s_[p_] != '"') {
            return false;
        }
        ++p_;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) {
                const char e = s_[p_++];
                switch (e) {
                case 'n':
                    c = '\n';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'u': {
                    // Kept as a '?' (the schema's texts are not read).
                    p_ = std::min(s_.size(), p_ + 4);
                    c = '?';
                    break;
                }
                default:
                    c = e;
                    break;
                }
            }
            out.push_back(c);
        }
        if (p_ >= s_.size()) {
            return false;
        }
        ++p_;
        return true;
    }
    bool value(Value& v)
    {
        space();
        if (p_ >= s_.size()) {
            return false;
        }
        const char c = s_[p_];
        if (c == '{') {
            ++p_;
            v.type = Value::Type::Object;
            space();
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            while (true) {
                space();
                std::string key;
                if (!str(key)) {
                    return false;
                }
                space();
                if (p_ >= s_.size() || s_[p_] != ':') {
                    return false;
                }
                ++p_;
                Value item;
                if (!value(item)) {
                    return false;
                }
                v.object.push_back(Member{std::move(key), std::move(item)});
                space();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            v.type = Value::Type::Array;
            space();
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            while (true) {
                Value item;
                if (!value(item)) {
                    return false;
                }
                v.array.push_back(std::move(item));
                space();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return str(v.string);
        }
        if (literal("true")) {
            v.type = Value::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (literal("false")) {
            v.type = Value::Type::Bool;
            return true;
        }
        if (literal("null")) {
            return true;
        }
        const std::size_t start = p_;
        while (p_ < s_.size() && (std::string_view("+-.eE0123456789").find(s_[p_]) != std::string_view::npos)) {
            ++p_;
        }
        if (p_ == start) {
            return false;
        }
        v.type = Value::Type::Number;
        v.number = std::strtod(std::string(s_.substr(start, p_ - start)).c_str(), nullptr);
        return true;
    }

    std::string_view s_;
    std::size_t p_ = 0;
};

inline std::optional<Value> parse(std::string_view text)
{
    return Reader(text).read();
}

} // namespace ray::cam::invariants::json
