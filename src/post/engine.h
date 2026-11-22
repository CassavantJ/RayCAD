// The QuickJS-NG post host (PLAN.md T-039, spike S8). Internal to src/post: the product entry point is
// makeQuickJsPostEngine() in post.h. Tests use this header for limits small enough to hit and for the numbers a run
// leaves behind (heap after teardown, CPU time).
//
// Sandbox (one runtime per post run, created and destroyed on a post thread of its own):
//  - The script context is a raw context with base objects, RegExp, JSON, Map/Set and Promise only: no Date, no
//    Eval intrinsic (so eval() and Function("...") throw), no Proxy, typed arrays, Atomics, WeakRef, no module
//    loader, no quickjs-libc (no std, os, console, print, timers). Math.random and queueMicrotask are deleted, and
//    Error.prepareStackTrace, Error.captureStackTrace and Error.stackTraceLimit too (they would run script code
//    while the host reads an error). Math.sin, cos, tan, asin, acos, atan, atan2 and hypot are replaced by
//    ray::dmath, so posts that use them give the same output on every OS; Math.exp, log, pow and the other
//    functions still use the platform's C library.
//  - The source is compiled in a second, throwaway context of the same runtime (it alone has the Eval intrinsic) and
//    moved across as bytecode (JS_WriteObject / JS_ReadObject). Both contexts share the heap limit and interrupt.
//  - Promise exists, but jobs are never run; a handler that returns a promise is a script error.
// Limits (post.h): hard heap limit, soft limit at 7/8 of it checked in the interrupt handler (so an out-of-memory
// script reports a line); thread CPU time and cancel through the interrupt handler, polled every 100 back-edges or
// calls (vendor patch; a script inside one long native call, such as a big sort, overruns the CPU limit by that
// call's length); output bytes and warning count in the host. Each aborts with an uncatchable error and the run
// fails with ErrorCode::ScriptError (Cancelled for a cancel); the message carries "<script>:<line>:" when the
// engine has a line (not for a thrown value that is not an Error; an interrupt in top-level code may say line 1).
#pragma once
#include <ray/post.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ray::post {

struct EngineLimits {
    std::size_t heapBytes = std::size_t(64) << 20; // hard; the soft limit is 7/8 of it
    double cpuSecondsBase = 10.0;                  // CPU limit: base + per move
    double cpuSecondsPerMove = 20.0e-6;
    std::size_t outputBytesBase = std::size_t(1) << 20; // output limit: base + per move
    std::size_t outputBytesPerMove = 256;
    std::size_t maxWarnings = 1000;
    std::size_t jsStackBytes = 256 * 1024;
    std::size_t threadStackBytes = std::size_t(4) << 20; // at least 1 MB (PLAN.md section 7)
    double inspectCpuSeconds = 10.0;                     // inspect() runs only the declaration block
};

// What the last inspect() or run() left behind, for tests and the bench.
struct RunDiagnostics {
    std::size_t heapPeakBytes = 0;      // live bytes of the JS allocator, highest
    std::size_t heapAfterFreeBytes = 0; // after JS_FreeRuntime: 0 unless the engine leaked
    std::size_t bytecodeBytes = 0;
    double cpuSeconds = 0; // thread CPU time of the whole run
    std::uint64_t interruptPolls = 0;
    std::size_t movesPosted = 0; // IR moves handed to the script
};

class QuickJsPostEngine final : public IPostEngine {
public:
    explicit QuickJsPostEngine(EngineLimits limits = {}) : limits_(limits) {}
    Result<PostInfo> inspect(const std::string& scriptPath) override;
    Result<PostOutput> run(const std::string& scriptPath, const PostJob& job, JobContext& ctx) override;

    // The same with the script text given directly; `scriptName` is what error messages and stack traces name.
    Result<PostInfo> inspectSource(std::string_view source, std::string_view scriptName);
    Result<PostOutput> runSource(std::string_view source, std::string_view scriptName, const PostJob& job,
                                 JobContext& ctx);

    const RunDiagnostics& lastRun() const { return diagnostics_; }
    const EngineLimits& limits() const { return limits_; }

private:
    EngineLimits limits_;
    RunDiagnostics diagnostics_;
};

// ---------- Platform pieces (engine_thread.cpp) ----------
// Runs `work` on a new thread with a stack of `stackBytes` and waits for it. False if the thread could not start.
bool runOnPostThread(std::size_t stackBytes, const std::function<void()>& work);
// CPU time (user + system) used so far by the calling thread, in seconds.
double threadCpuSeconds();

// The file stem a post is known by: "generic-iso" for ".../generic-iso.post.js", else the stem.
std::string postIdFromPath(std::string_view path);

} // namespace ray::post
