// The script's `out` object (post_api.d.ts, Writer): thin JavaScript bindings over Writer (writer.h). Every number
// a script passes is snapped to integer micro-units first (format.h); nothing here formats a double.
#include "engine_host.h"
#include "format.h"

#include <array>
#include <cmath>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace ray::post::detail {
namespace {

// The C strings of a call's arguments, freed on scope exit. The first 24 need no allocation (this runs several
// times per move).
class CStrings {
public:
    explicit CStrings(JSContext* ctx) : ctx_(ctx) {}
    ~CStrings()
    {
        for (std::size_t i = 0; i < count_ && i < small_.size(); ++i) {
            JS_FreeCString(ctx_, small_[i]);
        }
        for (const char* p : more_) {
            JS_FreeCString(ctx_, p);
        }
    }
    CStrings(const CStrings&) = delete;
    CStrings& operator=(const CStrings&) = delete;
    // False on a script exception (a toString that throws).
    bool add(JSValueConst v, std::string_view& out)
    {
        std::size_t n = 0;
        const char* p = JS_ToCStringLen(ctx_, &n, v);
        if (!p) {
            return false;
        }
        if (count_ < small_.size()) {
            small_[count_] = p;
        } else {
            more_.push_back(p);
        }
        ++count_;
        out = std::string_view(p, n);
        return true;
    }

private:
    JSContext* ctx_;
    std::array<const char*, 24> small_{};
    std::size_t count_ = 0;
    std::vector<const char*> more_;
};

std::string outputLimitMessage(Session& s)
{
    return "the post output exceeded its limit of " + std::to_string(s.writer().outputLimit()) + " bytes";
}

JSValue outputLimit(JSContext* ctx)
{
    Session& s = sessionOf(ctx);
    return s.throwStop(Stop::Output, outputLimitMessage(s));
}

// A number argument snapped to the writer's grid (format.h); throws (and returns false) for anything else.
bool numberArg(JSContext* ctx, JSValueConst v, const char* fn, double scale, std::int64_t& n)
{
    double d = 0;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "out.%s: the value must be a number", fn);
        return false;
    }
    JS_ToFloat64(ctx, &d, v);
    if (scale != 1.0) {
        d *= scale;
    }
    if (!fmt::snapToGrid(d, n, sessionOf(ctx).writer().grid())) {
        JS_ThrowRangeError(ctx, "out.%s: %s is not a finite number within 1e10", fn,
                           std::isnan(d) ? "NaN" : "the value");
        return false;
    }
    return true;
}

// An axis letter, A to Z (lower case is made upper case).
bool letterArg(JSContext* ctx, JSValueConst v, const char* fn, char& letter)
{
    CStrings strings(ctx);
    std::string_view s;
    if (!JS_IsString(v) || !strings.add(v, s) || s.size() != 1 ||
        !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        if (!JS_HasException(ctx)) {
            JS_ThrowTypeError(ctx, "out.%s: the letter must be one of A to Z", fn);
        }
        return false;
    }
    letter = s[0] >= 'a' ? static_cast<char>(s[0] - 'a' + 'A') : s[0];
    return true;
}

JSValue newString(JSContext* ctx, const std::string& s)
{
    return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue jsBlock(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Session& s = sessionOf(ctx);
    CStrings strings(ctx);
    std::array<std::string_view, 24> small{};
    std::vector<std::string_view> large;
    std::string_view* words = small.data();
    if (argc > static_cast<int>(small.size())) {
        large.resize(static_cast<std::size_t>(argc));
        words = large.data();
    }
    int count = 0;
    for (int i = 0; i < argc; ++i) {
        if (JS_IsNull(argv[i]) || JS_IsUndefined(argv[i])) {
            continue;
        }
        if (!strings.add(argv[i], words[count])) {
            return JS_EXCEPTION;
        }
        ++count;
    }
    if (!s.writer().block(std::span<const std::string_view>(words, static_cast<std::size_t>(count)))) {
        return outputLimit(ctx);
    }
    return JS_UNDEFINED;
}

JSValue jsComment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    CStrings strings(ctx);
    std::string_view text;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_UNDEFINED;
    }
    if (!strings.add(argv[0], text)) {
        return JS_EXCEPTION;
    }
    if (!sessionOf(ctx).writer().comment(text)) {
        return outputLimit(ctx);
    }
    return JS_UNDEFINED;
}

JSValue jsRaw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    CStrings strings(ctx);
    std::string_view text;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "out.raw: a line of text is needed");
    }
    if (!strings.add(argv[0], text)) {
        return JS_EXCEPTION;
    }
    if (!sessionOf(ctx).writer().raw(text)) {
        return outputLimit(ctx);
    }
    return JS_UNDEFINED;
}

JSValue jsModal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "out.modal(group, word): two strings are needed");
    }
    CStrings strings(ctx);
    std::string_view group;
    std::string_view word;
    if (!strings.add(argv[0], group) || !strings.add(argv[1], word)) {
        return JS_EXCEPTION;
    }
    return sessionOf(ctx).writer().modal(group, word) ? JS_DupValue(ctx, argv[1]) : JS_NULL;
}

JSValue jsAxis(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "out.axis(letter, value): two arguments are needed");
    }
    char letter = 0;
    std::int64_t micro = 0;
    if (!letterArg(ctx, argv[0], "axis", letter) || !numberArg(ctx, argv[1], "axis", 1.0, micro)) {
        return JS_EXCEPTION;
    }
    std::string word;
    if (!sessionOf(ctx).writer().axis(letter, micro, word)) {
        return JS_NULL;
    }
    return newString(ctx, word);
}

JSValue jsFeed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::int64_t micro = 0;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "out.feed(value): a feed is needed");
    }
    if (!numberArg(ctx, argv[0], "feed", 1.0, micro)) {
        return JS_EXCEPTION;
    }
    int decimals = -1;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        double d = 0;
        if (!JS_IsNumber(argv[1]) || JS_ToFloat64(ctx, &d, argv[1]) || std::floor(d) != d || d < 0 ||
            d > fmt::kMaxDecimals) {
            return JS_ThrowRangeError(ctx, "out.feed(value, decimals): decimals must be a whole number from 0 to 6");
        }
        decimals = static_cast<int>(d);
    }
    std::string word;
    if (!sessionOf(ctx).writer().feed(micro, word, decimals)) {
        return JS_NULL;
    }
    return newString(ctx, word);
}

// FormatSpec: decimals (0 to 6) is required; the other fields default to the coordinate format's.
bool formatArg(JSContext* ctx, JSValueConst spec, NumberFormat& f)
{
    if (!JS_IsObject(spec)) {
        JS_ThrowTypeError(ctx, "out.word: the format must be an object such as { decimals: 3 }");
        return false;
    }
    auto number = [&](const char* key, double& value, bool required) {
        JSValue v = JS_GetPropertyStr(ctx, spec, key);
        if (JS_IsException(v)) {
            return false;
        }
        if (JS_IsUndefined(v)) {
            if (required) {
                JS_ThrowTypeError(ctx, "out.word: format.%s is missing", key);
                return false;
            }
            return true;
        }
        const bool ok = JS_IsNumber(v) && !JS_ToFloat64(ctx, &value, v) && std::isfinite(value);
        JS_FreeValue(ctx, v);
        if (!ok) {
            JS_ThrowTypeError(ctx, "out.word: format.%s must be a finite number", key);
        }
        return ok;
    };
    auto flag = [&](const char* key, bool& value) {
        JSValue v = JS_GetPropertyStr(ctx, spec, key);
        if (JS_IsException(v)) {
            return false;
        }
        if (!JS_IsUndefined(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "out.word: format.%s must be true or false", key);
                return false;
            }
            value = JS_ToBool(ctx, v) != 0;
        }
        JS_FreeValue(ctx, v);
        return true;
    };
    double decimals = 0;
    double scale = 1.0;
    if (!number("decimals", decimals, true) || !number("scale", scale, false) ||
        !flag("trimZeros", f.trimTrailingZeros) || !flag("forceDecimalPoint", f.forceDecimalPoint) ||
        !flag("leadingZero", f.leadingZero)) {
        return false;
    }
    if (decimals < 0 || decimals > fmt::kMaxDecimals || std::floor(decimals) != decimals) {
        JS_ThrowRangeError(ctx, "out.word: format.decimals must be a whole number from 0 to 6");
        return false;
    }
    f.decimals = static_cast<int>(decimals);
    f.scale = scale;
    return true;
}

JSValue jsWord(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Session& s = sessionOf(ctx);
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "out.word(letter, value, format?): a letter and a number are needed");
    }
    CStrings strings(ctx);
    std::string_view prefix;
    if (!strings.add(argv[0], prefix)) {
        return JS_EXCEPTION;
    }
    if (prefix.size() > 8 || prefix.find_first_of(" \t\r\n") != std::string_view::npos) {
        return JS_ThrowTypeError(ctx, "out.word: the letter must be at most 8 characters, without spaces");
    }
    NumberFormat format = s.writer().coordinateFormat();
    const bool ownFormat = argc > 2 && !JS_IsUndefined(argv[2]);
    if (ownFormat && !formatArg(ctx, argv[2], format)) {
        return JS_EXCEPTION;
    }
    std::int64_t micro = 0;
    if (!numberArg(ctx, argv[1], "word", format.scale, micro)) {
        return JS_EXCEPTION;
    }
    std::string word;
    // Without a format of its own the word is a coordinate: the output's trailing-zero style applies (setFormat).
    s.writer().word(prefix, micro, format, word, !ownFormat);
    return newString(ctx, word);
}

// out.setFormat({ axisDecimals, feedDecimals, trailingZeros, commentMaxLength }): every key optional, the others keep
// their value. Forgets the axes and the feed (writer.h).
JSValue jsSetFormat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Writer& w = sessionOf(ctx).writer();
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "out.setFormat: an object such as { axisDecimals: 3 } is needed");
    }
    Writer::Format f = w.format();
    auto whole = [&](const char* key, double lo, double hi, auto& target) -> bool {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
        if (JS_IsException(v)) {
            return false;
        }
        if (JS_IsUndefined(v)) {
            return true;
        }
        double d = 0;
        const bool ok = JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) && std::floor(d) == d && d >= lo && d <= hi;
        JS_FreeValue(ctx, v);
        if (!ok) {
            JS_ThrowRangeError(ctx, "out.setFormat: %s must be a whole number from %d to %d", key,
                               static_cast<int>(lo), static_cast<int>(hi));
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(d);
        return true;
    };
    if (!whole("axisDecimals", 0, fmt::kMaxDecimals, f.axisDecimals) ||
        !whole("feedDecimals", 0, fmt::kMaxDecimals, f.feedDecimals) ||
        !whole("commentMaxLength", 0, 1000, f.commentMaxLength)) {
        return JS_EXCEPTION;
    }
    JSValue t = JS_GetPropertyStr(ctx, argv[0], "trailingZeros");
    if (JS_IsException(t)) {
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(t)) {
        CStrings strings(ctx);
        std::string_view name;
        const bool isString = JS_IsString(t) && strings.add(t, name);
        JS_FreeValue(ctx, t);
        if (isString && name == "trim") {
            f.trailing = Writer::Trailing::Trim;
        } else if (isString && name == "one") {
            f.trailing = Writer::Trailing::One;
        } else if (isString && name == "all") {
            f.trailing = Writer::Trailing::All;
        } else {
            if (!JS_HasException(ctx)) {
                JS_ThrowTypeError(ctx, "out.setFormat: trailingZeros must be \"trim\", \"one\" or \"all\"");
            }
            return JS_EXCEPTION;
        }
    }
    w.setFormat(f);
    return JS_UNDEFINED;
}

JSValue jsResetModal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Writer& w = sessionOf(ctx).writer();
    if (argc == 0) {
        w.resetAll();
        return JS_UNDEFINED;
    }
    CStrings strings(ctx);
    for (int i = 0; i < argc; ++i) {
        std::string_view name;
        if (!JS_IsString(argv[i])) {
            return JS_ThrowTypeError(ctx, "out.resetModal: group names must be strings");
        }
        if (!strings.add(argv[i], name)) {
            return JS_EXCEPTION;
        }
        w.reset(name);
    }
    return JS_UNDEFINED;
}

JSValue jsWarn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Session& s = sessionOf(ctx);
    CStrings strings(ctx);
    std::string_view text = "warning";
    if (argc > 0 && !strings.add(argv[0], text)) {
        return JS_EXCEPTION;
    }
    if (!s.writer().warn(text)) {
        return s.throwStop(Stop::Warnings,
                           "the post issued more than " + std::to_string(s.limits().maxWarnings) + " warnings");
    }
    return JS_UNDEFINED;
}

JSValue jsFail(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Session& s = sessionOf(ctx);
    CStrings strings(ctx);
    std::string_view text = "the post stopped (out.fail)";
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !strings.add(argv[0], text)) {
        return JS_EXCEPTION;
    }
    return s.throwStop(Stop::Fail, std::string(text));
}

// out.sequence: accessors over Writer::Sequence. magic 0 enabled, 1 start, 2 increment, 3 onlyOnToolChange, 4 max
// (the wrap limit, writer.h sequenceMax; T-040).
JSValue jsSequenceGet(JSContext* ctx, JSValueConst, int, JSValueConst*, int magic)
{
    const Writer& w = sessionOf(ctx).writer();
    const Writer::Sequence& seq = w.sequence();
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, seq.enabled);
    case 1:
        return JS_NewFloat64(ctx, static_cast<double>(seq.start));
    case 2:
        return JS_NewFloat64(ctx, static_cast<double>(seq.increment));
    case 4:
        return JS_NewFloat64(ctx, static_cast<double>(w.sequenceMax()));
    default:
        return JS_NewBool(ctx, seq.onlyOnToolChange);
    }
}

JSValue jsSequenceSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic)
{
    Writer& w = sessionOf(ctx).writer();
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    if (magic == 0 || magic == 3) {
        if (!JS_IsBool(argv[0])) {
            return JS_ThrowTypeError(ctx, "out.sequence: enabled and onlyOnToolChange must be true or false");
        }
        const bool on = JS_ToBool(ctx, argv[0]) != 0;
        if (magic == 0) {
            w.setSequenceEnabled(on);
        } else {
            w.setSequenceOnlyOnToolChange(on);
        }
        return JS_UNDEFINED;
    }
    double d = 0;
    const double lowest = magic == 1 || magic == 4 ? 0.0 : 1.0;
    if (!JS_IsNumber(argv[0]) || JS_ToFloat64(ctx, &d, argv[0]) || std::floor(d) != d || d < lowest || d > 1.0e9) {
        return JS_ThrowRangeError(ctx, "out.sequence: start and max (0 or more) and increment (1 or more) must be "
                                       "whole numbers up to 1e9");
    }
    if (magic == 1) {
        w.setSequenceStart(static_cast<std::int64_t>(d));
    } else if (magic == 4) {
        w.setSequenceMax(static_cast<std::int64_t>(d));
    } else {
        w.setSequenceIncrement(static_cast<std::int64_t>(d));
    }
    return JS_UNDEFINED;
}

// No C++ exception may unwind through the engine's C frames: a host function that runs out of memory (the output
// text is the one large allocation) stops the script instead.
template <JSCFunction* F>
JSValue guarded(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv)
{
    try {
        return F(ctx, self, argc, argv);
    } catch (const std::bad_alloc&) {
        return sessionOf(ctx).throwStop(Stop::Output, "the post host ran out of memory for the output");
    }
}

void addFunction(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn, int length)
{
    JS_DefinePropertyValueStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, length), JS_PROP_C_W_E);
}

} // namespace

void installWriterFunctions(JSContext* ctx, JSValueConst out)
{
    addFunction(ctx, out, "block", &guarded<&jsBlock>, 0);
    addFunction(ctx, out, "comment", &guarded<&jsComment>, 1);
    addFunction(ctx, out, "raw", &guarded<&jsRaw>, 1);
    addFunction(ctx, out, "modal", &guarded<&jsModal>, 2);
    addFunction(ctx, out, "axis", &guarded<&jsAxis>, 2);
    addFunction(ctx, out, "word", &guarded<&jsWord>, 3);
    addFunction(ctx, out, "feed", &guarded<&jsFeed>, 1);
    addFunction(ctx, out, "resetModal", &guarded<&jsResetModal>, 0);
    addFunction(ctx, out, "warn", &guarded<&jsWarn>, 1);
    addFunction(ctx, out, "fail", &guarded<&jsFail>, 1);
    addFunction(ctx, out, "setFormat", &guarded<&jsSetFormat>, 1);
    JSValue seq = JS_NewObject(ctx);
    static constexpr std::array<const char*, 5> kFields = {"enabled", "start", "increment", "onlyOnToolChange", "max"};
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        const JSAtom a = JS_NewAtom(ctx, kFields[i]);
        const int magic = static_cast<int>(i);
        JS_DefinePropertyGetSet(ctx, seq, a,
                                JS_NewCFunctionMagic(ctx, &jsSequenceGet, kFields[i], 0, JS_CFUNC_generic_magic, magic),
                                JS_NewCFunctionMagic(ctx, &jsSequenceSet, kFields[i], 1, JS_CFUNC_generic_magic, magic),
                                JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_DefinePropertyValueStr(ctx, out, "sequence", seq, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
}

} // namespace ray::post::detail
