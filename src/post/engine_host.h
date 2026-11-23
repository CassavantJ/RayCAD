// One post run's QuickJS-NG runtime: sandbox, limits, compilation, declaration and error reading (engine.h has the
// rules). Everything here runs on the post thread. Internal to src/post.
#pragma once
#include "engine.h"
#include "engine_motion.h"
#include "writer.h"

#include <quickjs.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ray::post::detail {

// Live and peak bytes of the runtime's allocator. Each block carries its size in a 16-byte header, so no
// platform-specific usable-size call is needed and the count is the same on every OS.
struct AllocatorStats {
    std::size_t live = 0;
    std::size_t peak = 0;
};
const JSMallocFunctions& countingAllocator();

// Why a run stopped early. Set by the interrupt handler and by host functions that refuse to go on.
enum class Stop : std::uint8_t { None, Cpu, Heap, Output, Warnings, Cancel, Fail };

enum Handler : std::uint8_t {
    kOnProgramStart,
    kOnSectionStart,
    kOnRapid,
    kOnLinear,
    kOnArc,
    kOnCycle,
    kOnCycleEnd,
    kOnDwell,
    kOnSectionEnd,
    kOnProgramEnd,
    kHandlerCount
};
std::string_view handlerName(Handler h);

// A declared post property with its default, typed.
using PropertyValue = std::variant<bool, double, std::string>;
struct DeclaredProperty {
    PostProperty info; // as PostInfo reports it (defaultValue as text)
    PropertyValue defaultValue;
    std::optional<double> min, max; // int and number
};

// definePost({...}) read into C++ (post_api.d.ts, PostDefinition).
struct Declaration {
    std::string title, vendor, dialect, fileExtension;
    int apiVersion = 1;
    MotionCaps motion;
    bool rigidTap = false;
    bool toolLengthProbe = false;
    std::vector<DeclaredProperty> properties; // the post's, then the host's own (lineEnding, includeDate)
    std::vector<std::string> verifiedOn;      // the controls a program from the post has run on (PostInfo::verifiedOn)
};

// The host properties every post has, declared by the host unless the post declares them itself.
constexpr std::string_view kPropLineEnding = "lineEnding";   // enum "crlf" | "lf", default "crlf"
constexpr std::string_view kPropIncludeDate = "includeDate"; // bool, default false

class Session {
public:
    Session(std::string scriptName, const EngineLimits& limits, double cpuLimitSeconds, const CancelToken* cancel);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Compiles the source in a throwaway context, runs the bytecode in the sandbox context, and checks that it
    // called definePost exactly once.
    Status load(std::string_view source, std::size_t& bytecodeBytes);
    // Reads and checks the declaration (handlers included).
    Result<Declaration> readDeclaration();

    // The run: the writer exists from here on, and `out` is the script's Writer object.
    void startOutput(WriterOptions options);
    Writer& writer() { return *writer_; }
    JSValueConst out() const { return out_; }
    JSContext* context() const { return ctx_; }

    bool hasHandler(Handler h) const;
    // Calls handler h with this = the post definition. False when the script failed (error() says why); a missing
    // handler is skipped (true) unless `required`, which is a host error.
    bool call(Handler h, int argc, JSValueConst* argv, bool required);

    // A failure found by the host, not by the script (missing handler, a promise returned, a bad property).
    void setHostError(Error e) { hostError_ = std::move(e); }
    void setStop(Stop s) { stop_ = s; }
    Stop stop() const { return stop_; }
    // The error that ended the run: the host error if any, else the pending exception, read with every limit off.
    Error error();

    const EngineLimits& limits() const { return limits_; }
    const std::string& scriptName() const { return scriptName_; }
    // The destructor writes the heap peak, the live bytes left after JS_FreeRuntime and the poll count here.
    void setReport(RunDiagnostics* report) { report_ = report; }

    // Number(text) as JavaScript reads it (locale-free, the same on every platform); nullopt unless finite.
    std::optional<double> parseNumber(std::string_view text);

    // Throws an uncatchable InternalError (or, for Stop::Fail, a plain Error) and records why.
    JSValue throwStop(Stop why, const std::string& message);

    // Interned atoms for move objects (engine_program.cpp builds them).
    enum Atom : std::uint8_t {
        aX,
        aY,
        aZ,
        aFeed,
        aRole,
        aComp,
        aCw,
        aPlane,
        aCx,
        aCy,
        aCz,
        aI,
        aJ,
        aK,
        aRadius,
        aSweep,
        aKind,
        aTop,
        aBottom,
        aRetract,
        aClearance,
        aReturnLevel,
        aPeck,
        aPeckClearance,
        aDwell,
        aPitch,
        aLeftHand,
        aFirst,
        kAtomCount
    };
    JSAtom atom(Atom a) const { return atoms_[a]; }
    // Interned strings: move roles, arc planes, cycle kinds and cycle return levels.
    JSValue roleString(cam::MoveRole role) const;
    JSValue planeString(cam::ArcPlane plane) const;
    JSValue cycleString(CycleKind kind) const;
    JSValue returnLevelString(cam::CycleReturn level) const;

private:
    static int interruptHandler(JSContext* ctx, void* opaque);
    void installSandbox();
    void installOut();
    void makeInternedValues();
    void freeValues();

    std::string scriptName_;
    EngineLimits limits_;
    double cpuLimit_;
    double cpuStart_;
    std::size_t softHeap_;
    const CancelToken* cancel_;
    AllocatorStats alloc_;
    std::chrono::steady_clock::time_point lastClock_{};
    std::uint64_t polls_ = 0;
    bool armed_ = true;
    Stop stop_ = Stop::None;
    std::string stopMessage_;
    std::optional<Error> hostError_;
    RunDiagnostics* report_ = nullptr;

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue post_ = JS_UNDEFINED; // the object given to definePost
    int defineCalls_ = 0;
    std::array<JSValue, kHandlerCount> handlers_{};
    JSValue out_ = JS_UNDEFINED;
    std::array<JSAtom, kAtomCount> atoms_{};
    std::array<JSValue, 8> roles_{};
    std::array<JSValue, 3> planes_{};
    std::array<JSValue, kCycleKindCount> cycles_{};
    std::array<JSValue, 2> returnLevels_{}; // "retract", "clearance"
    std::optional<Writer> writer_;

    friend JSValue jsDefinePost(JSContext*, JSValueConst, int, JSValueConst*);
};

Session& sessionOf(JSContext* ctx);

// The first "<scriptName>:<line>" in a stack trace, or -1.
int lineFromStack(std::string_view stack, std::string_view scriptName);

// engine_out.cpp: the functions of the script's `out` object, installed on `out`.
void installWriterFunctions(JSContext* ctx, JSValueConst out);

} // namespace ray::post::detail
