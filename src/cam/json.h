// A small JSON reader and the canonical JSON writer, private to src/cam (operation params and their schemas, T-035).
//
// A verbatim copy of src/lib/json.* and json_number.* (themselves copies of src/doc's, task T-008) with only the
// namespaces changed (ray::cam::json, ray::cam::detail), because src/cam may not use another module's private
// reader (PLAN.md 1.1). This is the third copy: src/lib's note says the copies then belong in src/core; that move
// touches core, doc and lib at once and is left to a task that owns them (T-035 report).
//
// Canonical form (the bytes every hash of JSON is taken over), following RFC 8785 (JSON Canonicalization Scheme):
//  - UTF-8, no byte order mark, no whitespace between tokens;
//  - object members sorted by key, comparing the keys as UTF-16 code units (equal to byte order for keys made of
//    characters below U+E000, which is every key RayCAD writes);
//  - strings: '"' and '\' escaped as \" and \\; U+0008, U+0009, U+000A, U+000C, U+000D as \b \t \n \f \r; other
//    characters below U+0020 as \u00xx with lower-case hex; everything else, '/' and non-ASCII included, as is;
//  - numbers: IEEE-754 doubles printed as ECMAScript's Number.prototype.toString does: the shortest decimal that
//    reads back to the same double, plain notation from 1e-6 up to below 1e21 (0.000001, 0.1, 100), exponent
//    notation outside it (1e-7, 1e+21, 1.5e+300); -0 is written as 0;
//  - literals true, false, null.
// The reader accepts RFC 8259 JSON with a leading UTF-8 byte order mark allowed and ignored. It rejects invalid
// UTF-8 (overlong forms, surrogates, code points above U+10FFFF), unpaired \u surrogate escapes, raw control
// characters in strings, duplicate keys in one object, numbers too large for a double, nesting deeper than 256 and
// anything after the value. It never throws: errors are ErrorCode::FormatError naming the byte offset.
#pragma once

#include <ray/core.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ray::cam::json {

struct Member;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::vector<Member>; // sorted by key in canonical order, keys unique

    Value() noexcept;
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();

    static Value boolean(bool);
    static Value number(double);
    static Value string(std::string);
    static Value array(Array);
    // Sorts the members into canonical order. If a key occurs more than once, the first occurrence is kept.
    static Value object(Object);

    Type type() const noexcept;
    bool isNull() const noexcept { return type() == Type::Null; }
    bool isBool() const noexcept { return type() == Type::Bool; }
    bool isNumber() const noexcept { return type() == Type::Number; }
    bool isString() const noexcept { return type() == Type::String; }
    bool isArray() const noexcept { return type() == Type::Array; }
    bool isObject() const noexcept { return type() == Type::Object; }

    // Each accessor requires the matching type.
    bool asBool() const;
    double asNumber() const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;
    // The number exactly as it was written in the parsed text; empty for a number made in code.
    const std::string& numberText() const;

    // Object member lookup; null if absent or not an object.
    const Value* find(std::string_view key) const;

private:
    struct NumberRep {
        double value;
        std::string text;
    };
    friend class Parser;
    std::variant<std::monostate, bool, NumberRep, std::string, Array, Object> v_;
};

struct Member {
    std::string key;
    Value value;
};

// Key order of the canonical form (UTF-16 code units).
bool keyLess(std::string_view a, std::string_view b) noexcept;

Result<Value> parse(std::string_view text);
// Writes the canonical form. Never fails; invalid UTF-8 in strings made in code is written as U+FFFD.
std::string write(const Value&);
// The canonical tokens laid out for people: members and items one per line, indented by two spaces, a space after
// each ':', and a final newline. Empty arrays and objects stay "[]" and "{}". Deterministic like write().
std::string writePretty(const Value&);

} // namespace ray::cam::json
