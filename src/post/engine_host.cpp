// One post run's QuickJS-NG runtime: allocator, sandbox, interrupt handler, two-context compile, declaration and
// error reading (engine_host.h, engine.h).
#include "engine_host.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ray::post::detail {
namespace {

// ---------- Counting allocator ----------
constexpr std::size_t kHeader = 16; // keeps the 16-byte alignment of malloc

void* blockOf(void* p)
{
    return static_cast<unsigned char*>(p) - kHeader;
}
std::size_t sizeOf(const void* p)
{
    std::size_t size = 0;
    std::memcpy(&size, static_cast<const unsigned char*>(p) - kHeader, sizeof size);
    return size;
}
void* finish(AllocatorStats& stats, void* block, std::size_t size)
{
    std::memcpy(block, &size, sizeof size);
    stats.live += size;
    stats.peak = std::max(stats.peak, stats.live);
    return static_cast<unsigned char*>(block) + kHeader;
}

void* allocMalloc(void* opaque, std::size_t size)
{
    if (size > std::numeric_limits<std::size_t>::max() - kHeader) {
        return nullptr;
    }
    void* block = std::malloc(size + kHeader);
    return block ? finish(*static_cast<AllocatorStats*>(opaque), block, size) : nullptr;
}
void* allocCalloc(void* opaque, std::size_t count, std::size_t size)
{
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max() - kHeader) / size) {
        return nullptr;
    }
    const std::size_t total = count * size;
    void* block = std::calloc(1, total + kHeader);
    return block ? finish(*static_cast<AllocatorStats*>(opaque), block, total) : nullptr;
}
void allocFree(void* opaque, void* p)
{
    if (!p) {
        return;
    }
    static_cast<AllocatorStats*>(opaque)->live -= sizeOf(p);
    std::free(blockOf(p));
}
void* allocRealloc(void* opaque, void* p, std::size_t size)
{
    if (!p) {
        return allocMalloc(opaque, size);
    }
    if (size == 0) {
        allocFree(opaque, p);
        return nullptr;
    }
    if (size > std::numeric_limits<std::size_t>::max() - kHeader) {
        return nullptr;
    }
    auto& stats = *static_cast<AllocatorStats*>(opaque);
    const std::size_t old = sizeOf(p);
    void* block = std::realloc(blockOf(p), size + kHeader);
    if (!block) {
        return nullptr; // the old block is untouched and still counted
    }
    stats.live -= old;
    return finish(stats, block, size);
}
std::size_t allocUsable(const void* p)
{
    return p ? sizeOf(p) : 0;
}

const JSMallocFunctions kAllocator = {allocCalloc, allocMalloc, allocFree, allocRealloc, allocUsable};

// ---------- Small helpers ----------
std::string toStdString(JSContext* ctx, JSValueConst v, bool& ok)
{
    std::size_t n = 0;
    const char* p = JS_ToCStringLen(ctx, &n, v);
    if (!p) {
        ok = false;
        return {};
    }
    std::string s(p, n);
    JS_FreeCString(ctx, p);
    ok = true;
    return s;
}

void deleteProperty(JSContext* ctx, JSValueConst obj, const char* name)
{
    const JSAtom a = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, obj, a, 0);
    JS_FreeAtom(ctx, a);
}

// Math replacements on ray::dmath (identical on every platform). magic: 0 sin, 1 cos, 2 tan, 3 asin, 4 acos, 5 atan.
JSValue jsMathUnary(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic)
{
    double x = std::numeric_limits<double>::quiet_NaN();
    if (argc > 0 && JS_ToFloat64(ctx, &x, argv[0])) {
        return JS_EXCEPTION;
    }
    double r = 0;
    switch (magic) {
    case 0:
        r = dmath::sin(x);
        break;
    case 1:
        r = dmath::cos(x);
        break;
    case 2:
        r = dmath::tan(x);
        break;
    case 3:
        r = dmath::asin(x);
        break;
    case 4:
        r = dmath::acos(x);
        break;
    default:
        r = dmath::atan2(x, 1.0); // musl's atan2 with x == 1 is atan(y)
        break;
    }
    return JS_NewFloat64(ctx, r);
}
JSValue jsMathAtan2(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    double y = std::numeric_limits<double>::quiet_NaN();
    double x = y;
    if ((argc > 0 && JS_ToFloat64(ctx, &y, argv[0])) || (argc > 1 && JS_ToFloat64(ctx, &x, argv[1]))) {
        return JS_EXCEPTION;
    }
    return JS_NewFloat64(ctx, dmath::atan2(y, x));
}
JSValue jsMathHypot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::vector<double> v(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (JS_ToFloat64(ctx, &v[static_cast<std::size_t>(i)], argv[i])) {
            return JS_EXCEPTION;
        }
    }
    bool nan = false;
    double largest = 0;
    for (const double d : v) {
        if (std::isinf(d)) {
            return JS_NewFloat64(ctx, std::numeric_limits<double>::infinity());
        }
        nan = nan || std::isnan(d);
        largest = std::max(largest, std::fabs(d));
    }
    if (nan) {
        return JS_NewFloat64(ctx, std::numeric_limits<double>::quiet_NaN());
    }
    if (v.size() == 2) {
        return JS_NewFloat64(ctx, dmath::hypot(v[0], v[1]));
    }
    if (largest == 0) {
        return JS_NewFloat64(ctx, 0.0);
    }
    double sum = 0;
    for (const double d : v) {
        const double s = d / largest;
        sum += s * s;
    }
    return JS_NewFloat64(ctx, largest * dmath::sqrt(sum));
}

std::string megabytes(std::size_t bytes)
{
    return std::to_string(bytes >> 20) + " MB";
}

std::string seconds(double s)
{
    // Whole tenths, written with integers (no locale): "10.5 s".
    const auto tenths = static_cast<long long>(std::llround(s * 10.0));
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
}

} // namespace

const JSMallocFunctions& countingAllocator()
{
    return kAllocator;
}

std::string_view handlerName(Handler h)
{
    static constexpr std::array<std::string_view, kHandlerCount> kNames = {
        "onProgramStart", "onSectionStart", "onRapid", "onLinear",     "onArc",
        "onCycle",        "onCycleEnd",     "onDwell", "onSectionEnd", "onProgramEnd"};
    return kNames[h];
}

Session& sessionOf(JSContext* ctx)
{
    return *static_cast<Session*>(JS_GetContextOpaque(ctx));
}

int lineFromStack(std::string_view stack, std::string_view scriptName)
{
    if (scriptName.empty()) {
        return -1;
    }
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = stack.find(scriptName, from);
        if (at == std::string_view::npos) {
            return -1;
        }
        std::size_t p = at + scriptName.size();
        const bool framed = at == 0 || stack[at - 1] == '(' || stack[at - 1] == ' ';
        if (framed && p < stack.size() && stack[p] == ':' && p + 1 < stack.size() && stack[p + 1] >= '0' &&
            stack[p + 1] <= '9') {
            int line = 0;
            for (++p; p < stack.size() && stack[p] >= '0' && stack[p] <= '9' && line < 100'000'000; ++p) {
                line = line * 10 + (stack[p] - '0');
            }
            return line;
        }
        from = at + 1;
    }
}

// ---------- definePost ----------
JSValue jsDefinePost(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    Session& s = sessionOf(ctx);
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "definePost: expected one object, the post definition");
    }
    if (++s.defineCalls_ > 1) {
        return JS_ThrowTypeError(ctx, "definePost: called more than once");
    }
    s.post_ = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

// ---------- Session ----------
Session::Session(std::string scriptName, const EngineLimits& limits, double cpuLimitSeconds, const CancelToken* cancel)
    : scriptName_(std::move(scriptName)), limits_(limits), cpuLimit_(cpuLimitSeconds), cpuStart_(threadCpuSeconds()),
      softHeap_(limits.heapBytes - limits.heapBytes / 8), cancel_(cancel)
{
    handlers_.fill(JS_UNDEFINED);
    roles_.fill(JS_UNDEFINED);
    planes_.fill(JS_UNDEFINED);
    cycles_.fill(JS_UNDEFINED);
    returnLevels_.fill(JS_UNDEFINED);
    atoms_.fill(JS_ATOM_NULL);
    rt_ = JS_NewRuntime2(&kAllocator, &alloc_);
    JS_SetMemoryLimit(rt_, limits_.heapBytes);
    JS_SetMaxStackSize(rt_, limits_.jsStackBytes);
    JS_SetInterruptHandler(rt_, &Session::interruptHandler, this);
    installSandbox();
}

Session::~Session()
{
    freeValues();
    JS_FreeContext(ctx_);
    JS_FreeRuntime(rt_);
    if (report_) {
        report_->heapPeakBytes = alloc_.peak;
        report_->heapAfterFreeBytes = alloc_.live;
        report_->interruptPolls = polls_;
    }
}

void Session::freeValues()
{
    writer_.reset();
    JS_FreeValue(ctx_, post_);
    post_ = JS_UNDEFINED;
    JS_FreeValue(ctx_, out_);
    out_ = JS_UNDEFINED;
    for (JSValue& v : handlers_) {
        JS_FreeValue(ctx_, v);
        v = JS_UNDEFINED;
    }
    for (JSValue& v : roles_) {
        JS_FreeValue(ctx_, v);
    }
    for (JSValue& v : planes_) {
        JS_FreeValue(ctx_, v);
    }
    for (JSValue& v : cycles_) {
        JS_FreeValue(ctx_, v);
    }
    for (JSValue& v : returnLevels_) {
        JS_FreeValue(ctx_, v);
        v = JS_UNDEFINED;
    }
    for (JSAtom a : atoms_) {
        if (a != JS_ATOM_NULL) {
            JS_FreeAtom(ctx_, a);
        }
    }
    atoms_.fill(JS_ATOM_NULL);
}

// The interrupt handler. It reads a monotonic clock only to decide how often to ask for the thread's CPU time (a
// system call); neither ever reaches the output.
int Session::interruptHandler(JSContext*, void* opaque)
{
    Session& s = *static_cast<Session*>(opaque);
    ++s.polls_;
    if (!s.armed_) {
        return 0;
    }
    if (s.cancel_ && s.cancel_->cancelled()) {
        s.stop_ = Stop::Cancel;
        return 1;
    }
    if (s.alloc_.live > s.softHeap_) {
        s.stop_ = Stop::Heap;
        return 1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s.lastClock_ < std::chrono::milliseconds(1)) {
        return 0;
    }
    s.lastClock_ = now;
    if (threadCpuSeconds() - s.cpuStart_ > s.cpuLimit_) {
        s.stop_ = Stop::Cpu;
        return 1;
    }
    return 0;
}

void Session::installSandbox()
{
    // A raw context gets only the intrinsics named here (engine.h lists what is left out and why).
    ctx_ = JS_NewContextRaw(rt_);
    JS_SetContextOpaque(ctx_, this);
    JS_AddIntrinsicBaseObjects(ctx_);
    JS_AddIntrinsicRegExp(ctx_);
    JS_AddIntrinsicJSON(ctx_);
    JS_AddIntrinsicMapSet(ctx_);
    // Promise is added although jobs never run: without it, import() leaked memory in spike S8.
    JS_AddIntrinsicPromise(ctx_);

    JSValue global = JS_GetGlobalObject(ctx_);
    deleteProperty(ctx_, global, "queueMicrotask");
    JSValue math = JS_GetPropertyStr(ctx_, global, "Math");
    deleteProperty(ctx_, math, "random");
    static constexpr std::array<const char*, 6> kUnary = {"sin", "cos", "tan", "asin", "acos", "atan"};
    for (std::size_t i = 0; i < kUnary.size(); ++i) {
        JS_DefinePropertyValueStr(
            ctx_, math, kUnary[i],
            JS_NewCFunctionMagic(ctx_, &jsMathUnary, kUnary[i], 1, JS_CFUNC_generic_magic, static_cast<int>(i)),
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    JS_DefinePropertyValueStr(ctx_, math, "atan2", JS_NewCFunction(ctx_, &jsMathAtan2, "atan2", 2),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx_, math, "hypot", JS_NewCFunction(ctx_, &jsMathHypot, "hypot", 2),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx_, math);
    JSValue error = JS_GetPropertyStr(ctx_, global, "Error");
    deleteProperty(ctx_, error, "prepareStackTrace");
    deleteProperty(ctx_, error, "captureStackTrace");
    deleteProperty(ctx_, error, "stackTraceLimit");
    JS_FreeValue(ctx_, error);
    JS_SetPropertyStr(ctx_, global, "definePost", JS_NewCFunction(ctx_, &jsDefinePost, "definePost", 1));
    JS_FreeValue(ctx_, global);
}

Status Session::load(std::string_view source, std::size_t& bytecodeBytes)
{
    bytecodeBytes = 0;
    // JS_Eval needs the Eval intrinsic, which is also what makes eval() and Function("...") work. So the source is
    // compiled in a second, throwaway context of the same runtime (same heap limit, same interrupt handler), and only
    // the bytecode is moved into the sandbox context, which never gets the compiler.
    const std::string text(source); // JS_Eval wants a terminating NUL
    JSContext* compiler = JS_NewContextRaw(rt_);
    JS_SetContextOpaque(compiler, this);
    JS_AddIntrinsicBaseObjects(compiler);
    JS_AddIntrinsicRegExpCompiler(compiler); // regular expression literals are compiled with the script
    JS_AddIntrinsicEval(compiler);
    JSValue fn = JS_Eval(compiler, text.c_str(), text.size(), scriptName_.c_str(),
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_COMPILE_ONLY);
    std::uint8_t* code = nullptr;
    std::size_t codeSize = 0;
    std::optional<Error> failure;
    if (!JS_IsException(fn)) {
        code = JS_WriteObject(compiler, &codeSize, fn, JS_WRITE_OBJ_BYTECODE);
    }
    if (!code) {
        // A syntax error: read it in the compiler context, where it is pending.
        JSContext* sandbox = ctx_;
        ctx_ = compiler;
        failure = error();
        ctx_ = sandbox;
    }
    JS_FreeValue(compiler, fn);
    JS_FreeContext(compiler);
    if (failure) {
        return tl::unexpected(*failure);
    }
    bytecodeBytes = codeSize;
    JSValue program = JS_ReadObject(ctx_, code, codeSize, JS_READ_OBJ_BYTECODE);
    js_free_rt(rt_, code);
    if (JS_IsException(program)) {
        return tl::unexpected(error());
    }
    JSValue result = JS_EvalFunction(ctx_, program); // takes the function
    if (JS_IsException(result)) {
        return tl::unexpected(error());
    }
    JS_FreeValue(ctx_, result);
    if (defineCalls_ == 0) {
        return tl::unexpected(Error{ErrorCode::ScriptError, scriptName_ + ": the script did not call definePost", {}});
    }
    return std::monostate{};
}

// ---------- Declaration ----------
namespace {

struct Reader {
    Session& s;
    JSContext* ctx;
    std::optional<Error> problem;

    void fail(std::string what)
    {
        if (!problem) {
            problem = Error{ErrorCode::ScriptError, s.scriptName() + ": definePost: " + std::move(what), {}};
        }
    }
    // A property; JS_UNDEFINED when missing. On a script exception (a throwing getter) `problem` is the error.
    JSValue get(JSValueConst obj, const char* key)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        if (JS_IsException(v)) {
            if (!problem) {
                problem = s.error();
            }
            return JS_UNDEFINED;
        }
        return v;
    }
    std::optional<std::string> string(JSValueConst obj, const char* key, const std::string& path, bool required)
    {
        JSValue v = get(obj, key);
        if (JS_IsUndefined(v)) {
            if (required) {
                fail(path + " is missing");
            }
            return std::nullopt;
        }
        if (!JS_IsString(v)) {
            JS_FreeValue(ctx, v);
            fail(path + " must be a string");
            return std::nullopt;
        }
        bool ok = false;
        std::string text = toStdString(ctx, v, ok);
        JS_FreeValue(ctx, v);
        if (!ok) {
            problem = s.error();
            return std::nullopt;
        }
        return text;
    }
    std::optional<bool> boolean(JSValueConst obj, const char* key, const std::string& path, bool required)
    {
        JSValue v = get(obj, key);
        if (JS_IsUndefined(v)) {
            if (required) {
                fail(path + " is missing");
            }
            return std::nullopt;
        }
        if (!JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            fail(path + " must be true or false");
            return std::nullopt;
        }
        const bool b = JS_ToBool(ctx, v) != 0;
        JS_FreeValue(ctx, v);
        return b;
    }
    std::optional<double> number(JSValueConst obj, const char* key, const std::string& path, bool required)
    {
        JSValue v = get(obj, key);
        if (JS_IsUndefined(v)) {
            if (required) {
                fail(path + " is missing");
            }
            return std::nullopt;
        }
        double d = 0;
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || !std::isfinite(d)) {
            JS_FreeValue(ctx, v);
            fail(path + " must be a finite number");
            return std::nullopt;
        }
        JS_FreeValue(ctx, v);
        return d;
    }
    // Number(value) as JavaScript writes it: the text of a numeric property default.
    std::string numberText(double d)
    {
        JSValue v = JS_NewFloat64(ctx, d);
        bool ok = false;
        std::string text = toStdString(ctx, v, ok);
        JS_FreeValue(ctx, v);
        return text;
    }
};

std::optional<PostProperty::Type> propertyType(std::string_view name)
{
    if (name == "bool") {
        return PostProperty::Type::Bool;
    }
    if (name == "int") {
        return PostProperty::Type::Int;
    }
    if (name == "number") {
        return PostProperty::Type::Number;
    }
    if (name == "enum") {
        return PostProperty::Type::Enum;
    }
    if (name == "string") {
        return PostProperty::Type::String;
    }
    return std::nullopt;
}

bool isWholeNumber(double d)
{
    return std::isfinite(d) && std::floor(d) == d && std::fabs(d) <= 9007199254740992.0;
}

void readProperty(Reader& r, JSValueConst spec, const std::string& key, std::vector<DeclaredProperty>& out)
{
    const std::string path = "properties." + key;
    if (!JS_IsObject(spec)) {
        r.fail(path + " must be an object");
        return;
    }
    DeclaredProperty p;
    p.info.key = key;
    const auto typeName = r.string(spec, "type", path + ".type", true);
    const auto title = r.string(spec, "title", path + ".title", true);
    const auto group = r.string(spec, "group", path + ".group", false);
    const auto description = r.string(spec, "description", path + ".description", false);
    if (r.problem) {
        return;
    }
    const auto type = propertyType(*typeName);
    if (!type) {
        r.fail(path + ".type must be \"bool\", \"int\", \"number\", \"enum\" or \"string\"");
        return;
    }
    p.info.type = *type;
    p.info.title = *title;
    p.info.group = group.value_or("");
    p.info.description = description.value_or("");
    switch (*type) {
    case PostProperty::Type::Bool: {
        const auto d = r.boolean(spec, "default", path + ".default", true);
        if (!d) {
            return;
        }
        p.defaultValue = *d;
        p.info.defaultValue = *d ? "true" : "false";
        break;
    }
    case PostProperty::Type::Int:
    case PostProperty::Type::Number: {
        const auto d = r.number(spec, "default", path + ".default", true);
        p.min = r.number(spec, "min", path + ".min", false);
        p.max = r.number(spec, "max", path + ".max", false);
        if (r.problem) {
            return;
        }
        if (*type == PostProperty::Type::Int &&
            (!isWholeNumber(*d) || (p.min && !isWholeNumber(*p.min)) || (p.max && !isWholeNumber(*p.max)))) {
            r.fail(path + ": an int property needs whole numbers for default, min and max");
            return;
        }
        if ((p.min && *d < *p.min) || (p.max && *d > *p.max) || (p.min && p.max && *p.min > *p.max)) {
            r.fail(path + ".default is outside min and max");
            return;
        }
        p.defaultValue = *d;
        p.info.defaultValue = r.numberText(*d);
        break;
    }
    case PostProperty::Type::Enum: {
        const auto d = r.string(spec, "default", path + ".default", true);
        JSValue values = r.get(spec, "values");
        if (r.problem) {
            JS_FreeValue(r.ctx, values);
            return;
        }
        std::int64_t count = 0;
        if (!JS_IsArray(values) || JS_GetLength(r.ctx, values, &count) || count <= 0 || count > 1000) {
            JS_FreeValue(r.ctx, values);
            r.fail(path + ".values must be a non-empty array of strings");
            return;
        }
        for (std::int64_t i = 0; i < count && !r.problem; ++i) {
            JSValue item = JS_GetPropertyUint32(r.ctx, values, static_cast<std::uint32_t>(i));
            bool ok = JS_IsString(item);
            if (ok) {
                p.info.enumValues.push_back(toStdString(r.ctx, item, ok));
            }
            JS_FreeValue(r.ctx, item);
            if (!ok) {
                r.fail(path + ".values must be a non-empty array of strings");
            }
        }
        JS_FreeValue(r.ctx, values);
        if (r.problem) {
            return;
        }
        if (std::find(p.info.enumValues.begin(), p.info.enumValues.end(), *d) == p.info.enumValues.end()) {
            r.fail(path + ".default \"" + *d + "\" is not one of its values");
            return;
        }
        p.defaultValue = *d;
        p.info.defaultValue = *d;
        break;
    }
    case PostProperty::Type::String: {
        const auto d = r.string(spec, "default", path + ".default", true);
        if (!d) {
            return;
        }
        p.defaultValue = *d;
        p.info.defaultValue = *d;
        break;
    }
    }
    out.push_back(std::move(p));
}

DeclaredProperty hostLineEnding()
{
    DeclaredProperty p;
    p.info = PostProperty{std::string(kPropLineEnding),
                          "Line endings",
                          "CRLF (the default; what most controls and DNC software expect) or LF.",
                          "Output",
                          PostProperty::Type::Enum,
                          "crlf",
                          {"crlf", "lf"}};
    p.defaultValue = std::string("crlf");
    return p;
}

DeclaredProperty hostIncludeDate()
{
    DeclaredProperty p;
    p.info = PostProperty{std::string(kPropIncludeDate),
                          "Include date",
                          "Gives the post the posting date (ProgramContext.date) to put in the program header.",
                          "Output",
                          PostProperty::Type::Bool,
                          "false",
                          {}};
    p.defaultValue = false;
    return p;
}

} // namespace

Result<Declaration> Session::readDeclaration()
{
    Reader r{*this, ctx_, std::nullopt};
    Declaration d;
    d.title = r.string(post_, "title", "title", true).value_or("");
    d.vendor = r.string(post_, "vendor", "vendor", false).value_or("");
    d.dialect = r.string(post_, "dialect", "dialect", true).value_or("");
    d.fileExtension = r.string(post_, "fileExtension", "fileExtension", true).value_or("");
    const auto api = r.number(post_, "apiVersion", "apiVersion", true);
    if (r.problem) {
        return tl::unexpected(*r.problem);
    }
    if (!isWholeNumber(*api) || *api < 1) {
        r.fail("apiVersion must be a whole number, 1 or more");
        return tl::unexpected(*r.problem);
    }
    if (*api > 1) {
        return tl::unexpected(Error{ErrorCode::FormatTooNew,
                                    scriptName_ + ": the post needs script API version " +
                                        std::to_string(static_cast<long long>(*api)) +
                                        "; this RayCAD supports version 1",
                                    {}});
    }
    d.apiVersion = 1;
    if (d.fileExtension.size() < 2 || d.fileExtension[0] != '.') {
        r.fail("fileExtension must start with a dot, e.g. \".nc\"");
        return tl::unexpected(*r.problem);
    }

    JSValue caps = r.get(post_, "capabilities");
    if (!r.problem && !JS_IsObject(caps)) {
        r.fail("capabilities must be an object");
    }
    if (!r.problem) {
        const auto arcs = r.string(caps, "arcs", "capabilities.arcs", true);
        if (arcs) {
            if (*arcs == "IJK") {
                d.motion.arcs = ArcOutput::IJK;
            } else if (*arcs == "R") {
                d.motion.arcs = ArcOutput::R;
            } else if (*arcs == "none") {
                d.motion.arcs = ArcOutput::None;
            } else {
                r.fail("capabilities.arcs must be \"IJK\", \"R\" or \"none\"");
            }
        }
        d.motion.helical = r.boolean(caps, "helical", "capabilities.helical", true).value_or(false);
        const auto maxDeg = r.number(caps, "maxArcDegrees", "capabilities.maxArcDegrees", true);
        if (maxDeg && (!isWholeNumber(*maxDeg) || *maxDeg < 1 || *maxDeg > 360)) {
            r.fail("capabilities.maxArcDegrees must be a whole number from 1 to 360");
        }
        d.motion.maxArcDegrees = maxDeg.value_or(360.0);
        d.rigidTap = r.boolean(caps, "rigidTap", "capabilities.rigidTap", true).value_or(false);
        d.motion.cutterComp = r.boolean(caps, "cutterComp", "capabilities.cutterComp", true).value_or(false);
        d.toolLengthProbe = r.boolean(caps, "toolLengthProbe", "capabilities.toolLengthProbe", false).value_or(false);
        JSValue cycles = r.get(caps, "cannedCycles");
        std::int64_t count = 0;
        if (!r.problem && (!JS_IsArray(cycles) || JS_GetLength(ctx_, cycles, &count) || count > 100)) {
            r.fail("capabilities.cannedCycles must be an array of cycle names");
        }
        for (std::int64_t i = 0; i < count && !r.problem; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx_, cycles, static_cast<std::uint32_t>(i));
            bool ok = JS_IsString(item);
            std::string name = ok ? toStdString(ctx_, item, ok) : std::string();
            JS_FreeValue(ctx_, item);
            const auto kind = cycleKindFromName(name);
            if (!ok || !kind) {
                r.fail("capabilities.cannedCycles: \"" + name +
                       "\" is not one of drill, peck, chipBreak, tap, bore, ream");
            } else {
                d.motion.cycles[static_cast<std::size_t>(*kind)] = true;
            }
        }
        JS_FreeValue(ctx_, cycles);
    }
    JS_FreeValue(ctx_, caps);
    if (r.problem) {
        return tl::unexpected(*r.problem);
    }

    JSValue props = r.get(post_, "properties");
    if (!r.problem && !JS_IsUndefined(props)) {
        if (!JS_IsObject(props)) {
            r.fail("properties must be an object");
        } else {
            JSPropertyEnum* tab = nullptr;
            std::uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx_, &tab, &len, props, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
                r.problem = error();
            } else {
                for (std::uint32_t i = 0; i < len && !r.problem; ++i) {
                    const char* name = JS_AtomToCString(ctx_, tab[i].atom);
                    const std::string key = name ? name : "";
                    JS_FreeCString(ctx_, name);
                    JSValue spec = JS_GetProperty(ctx_, props, tab[i].atom);
                    if (JS_IsException(spec)) {
                        r.problem = error();
                    } else if (key.empty()) {
                        r.fail("a property needs a non-empty name");
                    } else {
                        readProperty(r, spec, key, d.properties);
                    }
                    JS_FreeValue(ctx_, spec);
                }
                JS_FreePropertyEnum(ctx_, tab, len);
            }
        }
    }
    JS_FreeValue(ctx_, props);
    if (r.problem) {
        return tl::unexpected(*r.problem);
    }

    // The host's own properties, unless the post declares them (then with the type the host reads).
    auto declared = [&](std::string_view key) -> DeclaredProperty* {
        for (auto& p : d.properties) {
            if (p.info.key == key) {
                return &p;
            }
        }
        return nullptr;
    };
    if (const DeclaredProperty* p = declared(kPropLineEnding)) {
        const auto& v = p->info.enumValues;
        const bool ok = p->info.type == PostProperty::Type::Enum &&
                        std::all_of(v.begin(), v.end(), [](const std::string& s) { return s == "crlf" || s == "lf"; });
        if (!ok) {
            r.fail("properties.lineEnding is read by the host and must be an enum of \"crlf\" and \"lf\"");
        }
    } else {
        d.properties.push_back(hostLineEnding());
    }
    if (const DeclaredProperty* p = declared(kPropIncludeDate)) {
        if (p->info.type != PostProperty::Type::Bool) {
            r.fail("properties.includeDate is read by the host and must be a bool");
        }
    } else {
        d.properties.push_back(hostIncludeDate());
    }

    for (int h = 0; h < kHandlerCount && !r.problem; ++h) {
        const std::string name(handlerName(static_cast<Handler>(h)));
        JSValue fn = r.get(post_, name.c_str());
        if (r.problem) {
            break;
        }
        if (!JS_IsUndefined(fn) && !JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            r.fail(name + " must be a function");
            break;
        }
        JS_FreeValue(ctx_, handlers_[static_cast<std::size_t>(h)]);
        handlers_[static_cast<std::size_t>(h)] = fn;
    }
    if (r.problem) {
        return tl::unexpected(*r.problem);
    }
    return d;
}

// ---------- Running ----------
void Session::startOutput(WriterOptions options)
{
    writer_.emplace(std::move(options));
    makeInternedValues();
    installOut();
}

void Session::installOut()
{
    out_ = JS_NewObject(ctx_);
    installWriterFunctions(ctx_, out_);
}

void Session::makeInternedValues()
{
    static constexpr std::array<const char*, kAtomCount> kAtomNames = {
        "x",      "y",       "z",         "feed",        "role", "comp",          "cw",           "plane", "cx",
        "cy",     "cz",      "i",         "j",           "k",    "radius",        "sweepDegrees", "kind",  "top",
        "bottom", "retract", "clearance", "returnLevel", "peck", "peckClearance", "dwellSeconds", "pitch", "leftHand",
        "first"};
    for (std::size_t i = 0; i < kAtomCount; ++i) {
        atoms_[i] = JS_NewAtom(ctx_, kAtomNames[i]);
    }
    static constexpr std::array<const char*, 2> kReturnLevels = {"retract", "clearance"}; // CycleReturn order
    for (std::size_t i = 0; i < returnLevels_.size(); ++i) {
        returnLevels_[i] = JS_NewString(ctx_, kReturnLevels[i]);
    }
    static constexpr std::array<const char*, 8> kRoles = {"cut",     "plunge", "ramp",    "leadIn",
                                                          "leadOut", "link",   "retract", "approach"};
    for (std::size_t i = 0; i < roles_.size(); ++i) {
        roles_[i] = JS_NewString(ctx_, kRoles[i]);
    }
    static constexpr std::array<const char*, 3> kPlanes = {"XY", "XZ", "YZ"};
    for (std::size_t i = 0; i < planes_.size(); ++i) {
        planes_[i] = JS_NewString(ctx_, kPlanes[i]);
    }
    for (std::size_t i = 0; i < kCycleKindCount; ++i) {
        const std::string_view name = cycleKindName(static_cast<CycleKind>(i));
        cycles_[i] = JS_NewStringLen(ctx_, name.data(), name.size());
    }
}

JSValue Session::roleString(cam::MoveRole role) const
{
    const auto i = static_cast<std::size_t>(role);
    return JS_DupValue(ctx_, roles_[i < roles_.size() ? i : 0]);
}
JSValue Session::planeString(cam::ArcPlane plane) const
{
    const auto i = static_cast<std::size_t>(plane);
    return JS_DupValue(ctx_, planes_[i < planes_.size() ? i : 0]);
}
JSValue Session::cycleString(CycleKind kind) const
{
    return JS_DupValue(ctx_, cycles_[static_cast<std::size_t>(kind)]);
}
JSValue Session::returnLevelString(cam::CycleReturn level) const
{
    return JS_DupValue(ctx_, returnLevels_[level == cam::CycleReturn::Initial ? 1 : 0]);
}

bool Session::hasHandler(Handler h) const
{
    return !JS_IsUndefined(handlers_[h]);
}

bool Session::call(Handler h, int argc, JSValueConst* argv, bool required)
{
    const JSValue fn = handlers_[h];
    if (JS_IsUndefined(fn)) {
        if (required) {
            setHostError(Error{ErrorCode::ScriptError,
                               scriptName_ + ": the post has no " + std::string(handlerName(h)) +
                                   " handler, and the program needs one",
                               {}});
            return false;
        }
        return true;
    }
    JSValue rv = JS_Call(ctx_, fn, post_, argc, argv);
    if (JS_IsException(rv)) {
        return false;
    }
    if (JS_IsPromise(rv)) {
        JS_FreeValue(ctx_, rv);
        setHostError(Error{ErrorCode::ScriptError,
                           scriptName_ + ": " + std::string(handlerName(h)) +
                               " returned a promise; handlers must not be async (the host never runs promise jobs)",
                           {}});
        return false;
    }
    JS_FreeValue(ctx_, rv);
    return true;
}

std::optional<double> Session::parseNumber(std::string_view text)
{
    if (text.find_first_not_of(" \t") == std::string_view::npos) {
        return std::nullopt; // Number("") is 0; an empty override is not a number
    }
    JSValue v = JS_NewStringLen(ctx_, text.data(), text.size());
    double d = 0;
    const bool ok = !JS_IsException(v) && JS_ToFloat64(ctx_, &d, v) == 0 && std::isfinite(d);
    JS_FreeValue(ctx_, v);
    if (!ok) {
        if (JS_HasException(ctx_)) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
        }
        return std::nullopt;
    }
    return d;
}

JSValue Session::throwStop(Stop why, const std::string& message)
{
    stop_ = why;
    stopMessage_ = message;
    if (why == Stop::Fail) {
        JS_ThrowPlainError(ctx_, "%s", message.c_str());
    } else {
        JS_ThrowInternalError(ctx_, "%s", message.c_str());
    }
    JSValue e = JS_GetException(ctx_);
    JS_SetUncatchableError(ctx_, e); // try/catch and finally in the script cannot swallow it
    return JS_Throw(ctx_, e);
}

Error Session::error()
{
    if (hostError_) {
        return *hostError_;
    }
    // Lift the limits while the error is read, so that reading it cannot fail in turn.
    armed_ = false;
    JS_SetMemoryLimit(rt_, 0);
    JSValue exc = JS_GetException(ctx_);
    std::string message;
    std::string stack;
    const bool none = JS_IsNull(exc) || JS_IsUndefined(exc) || JS_IsUninitialized(exc);
    if (!none) {
        bool ok = false;
        message = toStdString(ctx_, exc, ok);
        if (!ok) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
            message = "an exception that cannot be shown as text";
        }
        if (JS_IsObject(exc)) {
            JSValue st = JS_GetPropertyStr(ctx_, exc, "stack");
            if (JS_IsException(st)) {
                JS_FreeValue(ctx_, JS_GetException(ctx_));
            } else if (JS_IsString(st)) {
                stack = toStdString(ctx_, st, ok);
            }
            JS_FreeValue(ctx_, st);
        }
    }
    JS_FreeValue(ctx_, exc);

    Stop why = stop_;
    if (why == Stop::None && (none || message.find("out of memory") != std::string::npos)) {
        why = Stop::Heap; // the hard limit: one allocation too big between two polls
    }
    std::string text;
    switch (why) {
    case Stop::Cpu:
        text = "the post exceeded its CPU time limit of " + seconds(cpuLimit_);
        break;
    case Stop::Heap:
        text = "the post exceeded its memory limit of " + megabytes(limits_.heapBytes);
        break;
    case Stop::Output:
    case Stop::Warnings:
    case Stop::Fail:
        text = stopMessage_;
        break;
    case Stop::Cancel:
        text = "posting was cancelled";
        break;
    case Stop::None:
        text = message;
        break;
    }
    const int line = lineFromStack(stack, scriptName_);
    std::string prefix = scriptName_;
    if (line > 0) {
        prefix += ":" + std::to_string(line);
    }
    Error e{why == Stop::Cancel ? ErrorCode::Cancelled : ErrorCode::ScriptError, prefix + ": " + text, {}};
    std::size_t from = 0;
    while (from < stack.size()) {
        std::size_t end = stack.find('\n', from);
        if (end == std::string::npos) {
            end = stack.size();
        }
        std::string frame = stack.substr(from, end - from);
        const auto first = frame.find_first_not_of(" \t");
        if (first != std::string::npos) {
            e.context.push_back(frame.substr(first));
        }
        from = end + 1;
    }
    return e;
}

} // namespace ray::post::detail
