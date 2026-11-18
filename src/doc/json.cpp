// JSON reader and canonical writer (see json.h for the rules).
#include "json.h"

#include "json_number.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <new>

namespace ray::doc::json {

// ---------- Value ----------

Value::Value() noexcept = default;
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

Value Value::boolean(bool b)
{
    Value out;
    out.v_ = b;
    return out;
}

Value Value::number(double d)
{
    Value out;
    out.v_ = NumberRep{d, {}};
    return out;
}

Value Value::string(std::string s)
{
    Value out;
    out.v_ = std::move(s);
    return out;
}

Value Value::array(Array a)
{
    Value out;
    out.v_ = std::move(a);
    return out;
}

Value Value::object(Object members)
{
    std::stable_sort(members.begin(), members.end(),
                     [](const Member& a, const Member& b) { return keyLess(a.key, b.key); });
    auto equal = [](const Member& a, const Member& b) {
        return a.key == b.key;
    };
    members.erase(std::unique(members.begin(), members.end(), equal), members.end());
    Value out;
    out.v_ = std::move(members);
    return out;
}

Value::Type Value::type() const noexcept
{
    return static_cast<Type>(v_.index());
}

bool Value::asBool() const
{
    return std::get<bool>(v_);
}

double Value::asNumber() const
{
    return std::get<NumberRep>(v_).value;
}

const std::string& Value::asString() const
{
    return std::get<std::string>(v_);
}

const Value::Array& Value::asArray() const
{
    return std::get<Array>(v_);
}

const Value::Object& Value::asObject() const
{
    return std::get<Object>(v_);
}

const std::string& Value::numberText() const
{
    return std::get<NumberRep>(v_).text;
}

const Value* Value::find(std::string_view key) const
{
    const Object* members = std::get_if<Object>(&v_);
    if (members == nullptr) {
        return nullptr;
    }
    auto it = std::lower_bound(members->begin(), members->end(), key,
                               [](const Member& m, std::string_view k) { return keyLess(m.key, k); });
    if (it != members->end() && it->key == key) {
        return &it->value;
    }
    return nullptr;
}

// ---------- UTF-8 ----------
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one code point at text[i]. Returns the code point and advances i, or returns nullopt for an invalid
// sequence (i then advances by one byte).
std::optional<char32_t> decodeUtf8(std::string_view text, std::size_t& i) noexcept
{
    const auto byte = [&](std::size_t k) {
        return static_cast<unsigned char>(text[k]);
    };
    const unsigned char b0 = byte(i);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int length = 0;
    char32_t cp = 0;
    char32_t minimum = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
        cp = b0 & 0x1Fu;
        minimum = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        cp = b0 & 0x0Fu;
        minimum = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        cp = b0 & 0x07u;
        minimum = 0x10000;
    } else {
        ++i;
        return std::nullopt;
    }
    if (i + static_cast<std::size_t>(length) > text.size()) {
        ++i;
        return std::nullopt;
    }
    for (int k = 1; k < length; ++k) {
        const unsigned char b = byte(i + static_cast<std::size_t>(k));
        if ((b & 0xC0u) != 0x80u) {
            ++i;
            return std::nullopt;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return std::nullopt;
    }
    i += static_cast<std::size_t>(length);
    return cp;
}

void encodeUtf8(char32_t cp, std::string& out)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// First UTF-16 code unit of a code point (the high surrogate for one above U+FFFF).
char32_t firstUnit(char32_t cp) noexcept
{
    return cp < 0x10000 ? cp : 0xD800 + ((cp - 0x10000) >> 10);
}

} // namespace

bool keyLess(std::string_view a, std::string_view b) noexcept
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const char32_t ca = decodeUtf8(a, i).value_or(kReplacement);
        const char32_t cb = decodeUtf8(b, j).value_or(kReplacement);
        if (ca == cb) {
            continue;
        }
        // Both in the same plane class: UTF-16 order equals code point order. Mixed: compare the first units.
        if ((ca < 0x10000) == (cb < 0x10000)) {
            return ca < cb;
        }
        return firstUnit(ca) < firstUnit(cb);
    }
    return i == a.size() && j < b.size();
}

// ---------- Reader ----------

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<Value> run()
    {
        if (text_.substr(0, 3) == "\xEF\xBB\xBF") {
            pos_ = 3;
        }
        skipSpace();
        Value value;
        if (!parseValue(value, 0)) {
            return fail();
        }
        skipSpace();
        if (pos_ != text_.size()) {
            error_ = "unexpected text after the JSON value";
            return fail();
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 256;

    tl::unexpected<Error> fail() const
    {
        return tl::unexpected(
            Error{ErrorCode::FormatError, "Invalid JSON at byte " + std::to_string(pos_) + ": " + error_, {}});
    }

    bool setError(const char* message)
    {
        error_ = message;
        return false;
    }

    void skipSpace() noexcept
    {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(pos_, word.size()) != word) {
            return setError("unknown literal");
        }
        pos_ += word.size();
        return true;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > kMaxDepth) {
            return setError("nesting is deeper than 256 levels");
        }
        if (pos_ >= text_.size()) {
            return setError("unexpected end of input");
        }
        switch (text_[pos_]) {
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
            std::string s;
            if (!parseString(s)) {
                return false;
            }
            out.v_ = std::move(s);
            return true;
        }
        case '[':
            return parseArray(out, depth);
        case '{':
            return parseObject(out, depth);
        default:
            return parseNumber(out);
        }
    }

    bool parseNumber(Value& out)
    {
        const std::string_view rest = text_.substr(pos_);
        const std::size_t length = detail::scanNumber(rest);
        if (length == 0) {
            return setError("expected a value");
        }
        const std::string_view lexeme = rest.substr(0, length);
        const auto value = detail::parseDouble(lexeme);
        if (!value) {
            return setError("number is too large for a double");
        }
        out.v_ = Value::NumberRep{*value, std::string(lexeme)};
        pos_ += length;
        return true;
    }

    bool hex4(std::uint32_t& out)
    {
        if (pos_ + 4 > text_.size()) {
            return setError("incomplete \\u escape");
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text_[pos_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                --pos_;
                return setError("bad hex digit in \\u escape");
            }
            out = out * 16 + digit;
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        ++pos_; // opening quote
        for (;;) {
            if (pos_ >= text_.size()) {
                return setError("unterminated string");
            }
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return setError("control character in string");
            }
            if (c == '\\') {
                if (!parseEscape(out)) {
                    return false;
                }
                continue;
            }
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            const std::size_t start = pos_;
            if (!decodeUtf8(text_, pos_)) {
                pos_ = start;
                return setError("invalid UTF-8");
            }
            out.append(text_.substr(start, pos_ - start));
        }
    }

    bool parseEscape(std::string& out)
    {
        ++pos_; // backslash
        if (pos_ >= text_.size()) {
            return setError("unterminated string");
        }
        const char c = text_[pos_++];
        switch (c) {
        case '"':
            out.push_back('"');
            return true;
        case '\\':
            out.push_back('\\');
            return true;
        case '/':
            out.push_back('/');
            return true;
        case 'b':
            out.push_back('\b');
            return true;
        case 'f':
            out.push_back('\f');
            return true;
        case 'n':
            out.push_back('\n');
            return true;
        case 'r':
            out.push_back('\r');
            return true;
        case 't':
            out.push_back('\t');
            return true;
        case 'u':
            break;
        default:
            --pos_;
            return setError("unknown escape");
        }
        std::uint32_t unit = 0;
        if (!hex4(unit)) {
            return false;
        }
        char32_t cp = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return setError("unpaired low surrogate escape");
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (text_.substr(pos_, 2) != "\\u") {
                return setError("unpaired high surrogate escape");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!hex4(low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return setError("unpaired high surrogate escape");
            }
            cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
        encodeUtf8(cp, out);
        return true;
    }

    bool parseArray(Value& out, int depth)
    {
        ++pos_; // [
        Value::Array items;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out.v_ = std::move(items);
            return true;
        }
        for (;;) {
            skipSpace();
            Value item;
            if (!parseValue(item, depth + 1)) {
                return false;
            }
            items.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) {
                return setError("unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out.v_ = std::move(items);
                return true;
            }
            return setError("expected ',' or ']'");
        }
    }

    bool parseObject(Value& out, int depth)
    {
        ++pos_; // {
        Value::Object members;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out.v_ = std::move(members);
            return true;
        }
        for (;;) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return setError("expected a string key");
            }
            Member member;
            if (!parseString(member.key)) {
                return false;
            }
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return setError("expected ':'");
            }
            ++pos_;
            skipSpace();
            if (!parseValue(member.value, depth + 1)) {
                return false;
            }
            members.push_back(std::move(member));
            skipSpace();
            if (pos_ >= text_.size()) {
                return setError("unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                break;
            }
            return setError("expected ',' or '}'");
        }
        std::stable_sort(members.begin(), members.end(),
                         [](const Member& a, const Member& b) { return keyLess(a.key, b.key); });
        for (std::size_t i = 1; i < members.size(); ++i) {
            if (members[i - 1].key == members[i].key) {
                return setError("duplicate key in object");
            }
        }
        out.v_ = std::move(members);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

Result<Value> parse(std::string_view text)
{
    try {
        return Parser(text).run();
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::FormatError, "Invalid JSON: out of memory while reading", {}});
    } catch (...) {
        return tl::unexpected(Error{ErrorCode::Internal, "Unexpected failure while reading JSON", {}});
    }
}

// ---------- Canonical writer ----------
namespace {

void writeString(std::string_view s, std::string& out)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const std::size_t start = i;
            if (decodeUtf8(s, i)) {
                out.append(s.substr(start, i - start));
            } else {
                encodeUtf8(kReplacement, out);
            }
            continue;
        }
        ++i;
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0x0F]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void writeValue(const Value& value, std::string& out)
{
    switch (value.type()) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += value.asBool() ? "true" : "false";
        break;
    case Value::Type::Number:
        out += detail::formatDouble(value.asNumber());
        break;
    case Value::Type::String:
        writeString(value.asString(), out);
        break;
    case Value::Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const Value& item : value.asArray()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            writeValue(item, out);
        }
        out.push_back(']');
        break;
    }
    case Value::Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const Member& member : value.asObject()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            writeString(member.key, out);
            out.push_back(':');
            writeValue(member.value, out);
        }
        out.push_back('}');
        break;
    }
    }
}

} // namespace

std::string write(const Value& value)
{
    try {
        std::string out;
        writeValue(value, out);
        return out;
    } catch (...) {
        // Only std::bad_alloc can get here. Canonical bytes feed hashes, so a partial result is never returned.
        std::terminate();
    }
}

} // namespace ray::doc::json

namespace ray::doc {

std::string canonicalJson(std::string_view anyJson)
{
    auto value = json::parse(anyJson);
    if (!value) {
        return {};
    }
    return json::write(*value);
}

} // namespace ray::doc
