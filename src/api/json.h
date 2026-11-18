// A small JSON value, reader and writer, private to src/api: action arguments, schemas, results and the recent-files
// list. RFC 8259 JSON; the reader rejects duplicate keys, invalid UTF-8 escapes, nesting deeper than 128 and anything
// after the value, and never throws. The writer is compact; pass its output through doc::canonicalJson where the
// canonical form matters (it keeps object members in insertion order).
#pragma once

#include <ray/core.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ray::api::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b);
    static Value number(double d);
    static Value integer(std::int64_t i);
    static Value string(std::string s);
    static Value array(Array a = {});
    static Value object(Object o = {});

    Type type() const noexcept { return static_cast<Type>(v_.index()); }
    bool isNull() const noexcept { return type() == Type::Null; }
    bool isBool() const noexcept { return type() == Type::Bool; }
    bool isNumber() const noexcept { return type() == Type::Number; }
    bool isString() const noexcept { return type() == Type::String; }
    bool isArray() const noexcept { return type() == Type::Array; }
    bool isObject() const noexcept { return type() == Type::Object; }
    // True for a number with no fractional part (JSON Schema's "integer").
    bool isInteger() const noexcept;

    // Each accessor requires the matching type.
    bool asBool() const { return std::get<bool>(v_); }
    double asNumber() const { return std::get<Num>(v_).value; }
    const std::string& asString() const { return std::get<std::string>(v_); }
    const Array& asArray() const { return std::get<Array>(v_); }
    Array& asArray() { return std::get<Array>(v_); }
    const Object& asObject() const { return std::get<Object>(v_); }
    Object& asObject() { return std::get<Object>(v_); }

    // Object members: null if absent or not an object.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    // Sets (replaces or appends) an object member; the value must be an object.
    Value& set(std::string key, Value value);
    bool erase(std::string_view key);
    // Appends to an array.
    void push(Value value) { asArray().push_back(std::move(value)); }

    // Convenience lookups with a fallback.
    std::string stringOr(std::string_view key, std::string fallback) const;
    bool boolOr(std::string_view key, bool fallback) const;
    double numberOr(std::string_view key, double fallback) const;

    friend bool operator==(const Value& a, const Value& b);

private:
    struct Num {
        double value;
        std::string text; // as written in parsed input; empty when made in code
    };
    friend class Parser;
    friend void writeTo(const Value&, std::string&);
    std::variant<std::monostate, bool, Num, std::string, Array, Object> v_;
};

// FormatError naming the byte offset for anything that is not one JSON value.
Result<Value> parse(std::string_view text);
std::string write(const Value& value);
// write() then doc::canonicalJson().
std::string canonical(const Value& value);

// "string", "number", ... for messages.
const char* typeName(Value::Type type);

} // namespace ray::api::json
