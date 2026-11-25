// The post sandbox and its limits (PLAN.md T-039, from spike S8; the scripts started as spikes/s8-quickjs/tests).
// Every run must also free its runtime to zero bytes, and a normal post afterwards must be unaffected.
#include "engine.h"
#include "post_test_util.h"

#include <ray/core.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;

namespace {

// How late a CPU stop may be: the poll sees the limit between native calls, so it can lag by one call. One sort of
// 20,000 strings takes milliseconds in an optimised build and over half a second in a Debug (unoptimised) engine, and
// longer than the optimised allowance under AddressSanitizer on the CI runner (RAY_SANITIZED_BUILD, run 45528384533).
#if defined(NDEBUG) && !defined(RAY_SANITIZED_BUILD)
constexpr double kCpuLateness = 0.25;
#else
constexpr double kCpuLateness = 2.0;
#endif

enum class Want { Ok, Script, Heap, Cpu, Output, Warnings, Cancelled };

struct Outcome {
    Result<PostOutput> result;
    RunDiagnostics diag;
    std::string source;
};

Outcome runScript(const std::string& file, const EngineLimits& limits, const Program& program)
{
    std::string source = readFile(scriptPath(file));
    REQUIRE_FALSE(source.empty());
    source += kTestPostTail;
    QuickJsPostEngine engine(limits);
    JobContext ctx;
    const PostJob job = program.job();
    auto r = engine.runSource(source, file, job, ctx);
    return Outcome{std::move(r), engine.lastRun(), source};
}

Program arcProgram()
{
    Program p;
    cam::Toolpath& path =
        p.section("arcs", "contour2d", 7, makeTool("6 mm end mill", lib::ToolType::FlatEndMill, 6, 3));
    path.startX = mm(5);
    path.startY = 0;
    path.moves.push_back(arc(false, 0, mm(5), 0, 0, 0, 0, mmPerMin(800)));
    return p;
}

EngineLimits limitsMb(std::size_t heapMb, double cpuSeconds, std::size_t outMb)
{
    EngineLimits l;
    l.heapBytes = heapMb << 20;
    l.cpuSecondsBase = cpuSeconds;
    l.cpuSecondsPerMove = 0;
    l.outputBytesBase = outMb << 20;
    l.outputBytesPerMove = 0;
    return l;
}

const char* kindText(Want w)
{
    switch (w) {
    case Want::Heap:
        return "exceeded its memory limit";
    case Want::Cpu:
        return "exceeded its CPU time limit";
    case Want::Output:
        return "output exceeded its limit";
    case Want::Warnings:
        return "more than 1000 warnings";
    case Want::Cancelled:
        return "cancelled";
    default:
        return "";
    }
}

struct Case {
    const char* file;
    Want want;
    const char* message; // substring of the error message, or nullptr
    int line;            // 0: the /*HERE*/ line; -1: no line expected; -2: any line; >0: that line
    std::size_t heapMb = 64;
    double cpuSeconds = 5.0;
    std::size_t outMb = 16;
    std::size_t moves = 0;
    bool arcs = false;
};

void check(const Case& c)
{
    INFO("script " << c.file);
    const Program program = c.arcs ? arcProgram() : linearProgram(c.moves);
    const Outcome o = runScript(c.file, limitsMb(c.heapMb, c.cpuSeconds, c.outMb), program);
    if (c.want == Want::Ok) {
        REQUIRE(o.result.has_value());
    } else {
        REQUIRE_FALSE(o.result.has_value());
        const Error& e = o.result.error();
        INFO("error: " << e.message);
        CHECK(e.code == (c.want == Want::Cancelled ? ErrorCode::Cancelled : ErrorCode::ScriptError));
        if (c.want != Want::Script) {
            CHECK(e.message.find(kindText(c.want)) != std::string::npos);
        }
        if (c.message) {
            CHECK(e.message.find(c.message) != std::string::npos);
        }
        const int line = errorLine(e.message, c.file);
        if (c.line == 0) {
            CHECK(line == markerLine(o.source));
        } else if (c.line == -1) {
            CHECK(line == -1);
        } else if (c.line == -2) {
            CHECK(line > 0);
        } else {
            CHECK(line == c.line);
        }
        if (c.want == Want::Cpu) {
            // On time: the interrupt is polled every 100 back-edges (vendor patch), and each poll is cheap. Stock
            // QuickJS-NG stopped cpu_native_sort.js 7.4 s late here (spike S8).
            CHECK(o.diag.cpuSeconds < c.cpuSeconds + kCpuLateness);
        }
    }
    // The runtime is freed to zero bytes whatever happened, and the heap never ran far past its limit.
    CHECK(o.diag.heapAfterFreeBytes == 0);
    CHECK(o.diag.heapPeakBytes <= (c.heapMb << 20) + (std::size_t(1) << 20));
}

} // namespace

TEST_CASE("the sandbox: no Date, Math.random, timers, eval, Function, import or I/O", "[post][sandbox]")
{
    const Outcome o = runScript("sandbox_globals.js", limitsMb(64, 5, 16), linearProgram(0));
    REQUIRE(o.result.has_value());
    const std::string& text = o.result->text;
    INFO(text);
    CHECK(text.find("\"verdict\":\"clean\"") != std::string::npos);
    CHECK(text.find("\"importSettled\":false") != std::string::npos);
    CHECK(text.find("\"present\":[]") != std::string::npos);
    CHECK(text.find("\"Promise\":\"function\"") != std::string::npos);
    CHECK(text.find("\"typeof Date\":\"undefined\"") != std::string::npos);
    CHECK(text.find("eval is not supported") != std::string::npos);
    CHECK(text.find("\"definePost\"") != std::string::npos);
    CHECK(o.diag.heapAfterFreeBytes == 0);
}

TEST_CASE("static import is a syntax error; eval and Function throw inside handlers", "[post][sandbox]")
{
    check({"sandbox_import.js", Want::Script, "SyntaxError", 1});
    check({"date_in_handler.js", Want::Script, "Date is not defined", 0});
    const Outcome o = runScript("eval_in_handler.js", limitsMb(64, 5, 16), linearProgram(0));
    REQUIRE(o.result.has_value());
    CHECK(o.result->text == "eval: TypeError\r\nFunction: TypeError\r\nn=0\r\n");
}

TEST_CASE("script errors name the file and line", "[post][sandbox]")
{
    check({"err_syntax.js", Want::Script, "SyntaxError", 0});
    check({"err_toplevel.js", Want::Script, "ReferenceError: notDefinedAnywhere is not defined", 0});
    check({"err_runtime.js", Want::Script, "TypeError", 0, 64, 5, 16, 1000});
    check({"err_nested.js", Want::Script, "RangeError: bad arc radius", 0, 64, 5, 16, 0, true});
    check({"err_fail.js", Want::Script, "tool 7 needs length offset 99", 0, 64, 5, 16, 10});
    check({"err_throw_string.js", Want::Script, "just a string", -1, 64, 5, 16, 10});
    check({"stack_overflow.js", Want::Script, "Maximum call stack size exceeded", 0});
    check({"define_twice.js", Want::Script, "called more than once", 0});
    check({"no_define.js", Want::Script, "did not call definePost", -1});
    check({"missing_handler.js", Want::Script, "has no onLinear handler", -1, 64, 5, 16, 3});
    check({"async_handler.js", Want::Script, "returned a promise", -1});
}

TEST_CASE("the heap limit stops a runaway script with a line", "[post][sandbox][limits]")
{
    check({"heap_runaway.js", Want::Heap, nullptr, -2, 32, 20});
    check({"heap_string.js", Want::Heap, nullptr, -2, 32, 20});
    check({"heap_in_callback.js", Want::Heap, nullptr, -2, 32, 20, 16, 1000});
    check({"heap_catch.js", Want::Heap, nullptr, -2, 32, 20});
    // One allocation far beyond the limit, between two polls: only the hard limit can stop it.
    check({"heap_big_single.js", Want::Heap, nullptr, -2, 32, 20});
    // Lazy concatenation reaches the engine's string length limit first; it is stopped all the same.
    check({"string_too_long.js", Want::Script, "string too long", -2, 32, 20});
}

TEST_CASE("the CPU limit stops loops, also in catch, finally, regex and native calls", "[post][sandbox][limits]")
{
    check({"cpu_loop.js", Want::Cpu, nullptr, -2, 64, 0.5});
    check({"cpu_loop_catch.js", Want::Cpu, nullptr, -2, 64, 0.5});
    check({"cpu_in_callback.js", Want::Cpu, nullptr, 3, 64, 0.5, 16, 1000});
    check({"cpu_regex.js", Want::Cpu, nullptr, 2, 64, 0.5});
    check({"cpu_native_sort.js", Want::Cpu, nullptr, -2, 256, 0.5});
}

TEST_CASE("output size and warning count have limits of their own", "[post][sandbox][limits]")
{
    check({"output_flood.js", Want::Output, nullptr, 5, 64, 20, 16});
    check({"warn_flood.js", Want::Warnings, nullptr, 0, 64, 20, 16});
}

TEST_CASE("deep native recursion does not crash the host", "[post][sandbox]")
{
    const Outcome o = runScript("native_recursion.js", limitsMb(64, 5, 16), linearProgram(0));
    REQUIRE(o.result.has_value());
    CHECK(o.result->text.rfind("survived ", 0) == 0);
    CHECK(o.diag.heapAfterFreeBytes == 0);
}

TEST_CASE("Math's trigonometry comes from ray::dmath", "[post][sandbox]")
{
    const Outcome o = runScript("math_dmath.js", limitsMb(64, 5, 16), linearProgram(0));
    REQUIRE(o.result.has_value());
    const auto got = lines(o.result->text);
    const double xs[] = {0.5, -1.25, 3, 1e-3};
    REQUIRE(got.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = xs[i];
        const double want[] = {dmath::sin(x),      dmath::cos(x),      dmath::tan(x),      dmath::atan2(x, 1.0),
                               dmath::atan2(x, 2), dmath::hypot(x, 3), dmath::asin(x / 4), dmath::acos(x / 4)};
        // JavaScript writes the shortest text that reads back as the same double: compare the doubles.
        std::size_t from = 0;
        for (const double w : want) {
            const std::size_t end = got[i].find(' ', from);
            const std::string field = got[i].substr(from, end == std::string::npos ? std::string::npos : end - from);
            CHECK(std::strtod(field.c_str(), nullptr) == w);
            from = end + 1;
        }
    }
}

TEST_CASE("cancel stops a running post", "[post][sandbox]")
{
    std::string source = readFile(scriptPath("cpu_loop.js"));
    source += kTestPostTail;
    QuickJsPostEngine engine(limitsMb(64, 60, 16));
    CancelSource cancel;
    JobContext ctx{cancel.token(), Progress()};
    const Program program = linearProgram(0);
    const PostJob job = program.job();
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel.cancel();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = engine.runSource(source, "cpu_loop.js", job, ctx);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    canceller.join();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::seconds(10)); // a 60 s CPU limit was not what stopped it
    CHECK(engine.lastRun().heapAfterFreeBytes == 0);

    // Cancelled before it starts: nothing runs.
    const auto again = engine.runSource(source, "cpu_loop.js", job, ctx);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::Cancelled);
}

TEST_CASE("after every failure, a normal post run gives the same bytes", "[post][sandbox]")
{
    const Program program = linearProgram(500);
    const PostJob job = program.job();
    QuickJsPostEngine engine;
    JobContext ctx;
    const auto before = engine.run(postPath("iso-test.post.js"), job, ctx);
    REQUIRE(before.has_value());
    for (const char* file : {"heap_runaway.js", "cpu_loop.js", "output_flood.js", "err_runtime.js", "warn_flood.js"}) {
        const Case c{file, Want::Script, nullptr, -2, 32, 0.3, 1, 600};
        (void)runScript(c.file, limitsMb(c.heapMb, c.cpuSeconds, c.outMb), linearProgram(c.moves));
    }
    const auto after = engine.run(postPath("iso-test.post.js"), job, ctx);
    REQUIRE(after.has_value());
    CHECK(after->text == before->text);
    CHECK(after->hash() == before->hash());
    CHECK(engine.lastRun().heapAfterFreeBytes == 0);
}

TEST_CASE("the default limits are those of post.h and PLAN.md section 7", "[post][sandbox][limits]")
{
    const EngineLimits l;
    CHECK(l.heapBytes == std::size_t(64) << 20);
    CHECK(l.cpuSecondsBase == 10.0);
    CHECK(l.cpuSecondsPerMove == 20.0e-6);
    CHECK(l.outputBytesBase == std::size_t(1) << 20);
    CHECK(l.outputBytesPerMove == 256);
    CHECK(l.maxWarnings == 1000);
    CHECK(l.jsStackBytes == 256 * 1024);
    CHECK(l.threadStackBytes >= std::size_t(1) << 20);
}
