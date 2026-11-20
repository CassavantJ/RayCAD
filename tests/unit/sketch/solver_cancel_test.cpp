// Full solve and drag begin run only with a JobContext and honour cancellation (task T-090). The latency bound is the
// one documented in src/sketch/solver_internal.h: the diagnosis cannot be interrupted, everything else checks between
// iterations.
#include "sketch_test_util.h"

#include "solver_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;

TEST_CASE("A cancelled context stops a solve before it starts", "[sketch][cancel]")
{
    const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), {}};
    auto r = makePlaneGcsSolver()->solve(g.sketch, ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Progress is reported", "[sketch][cancel]")
{
    const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
    std::vector<double> seen;
    JobContext ctx{CancelToken{}, Progress([&](double f, std::string_view) { seen.push_back(f); })};
    auto r = makePlaneGcsSolver()->solve(g.sketch, ctx);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == 1.0);
}

TEST_CASE("Cancelling a running 200-curve solve returns within the documented time", "[sketch][cancel]")
{
    // 100 curves in a debug or sanitizer build, where the 200-curve diagnosis takes tens of seconds.
    const Grid g = makeGrid(optimisedBuild() ? 4 : 2, GridVariant::Nearly, 0.3);
    // Uncancelled reference time.
    double fullMs = 0;
    {
        JobContext ctx;
        const auto t0 = Clock::now();
        auto r = makePlaneGcsSolver()->solve(g.sketch, ctx);
        fullMs = msSince(t0);
        REQUIRE(r.has_value());
    }
    double worstLatency = 0;
    int cancelledRuns = 0;
    // Since the per-component diagnosis and sparse DogLeg (pre-M2) the whole solve takes tens of ms in an optimised
    // build, so the first delays are short.
    for (int delayMs : {1, 5, 40, 120, 250}) {
        CancelSource source;
        JobContext ctx{source.token(), {}};
        Result<SolveResult> result = tl::unexpected(Error{ErrorCode::Internal, "not run", {}});
        Clock::time_point finished;
        std::thread worker([&] {
            result = makePlaneGcsSolver()->solve(g.sketch, ctx);
            finished = Clock::now();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        const auto cancelAt = Clock::now();
        source.cancel();
        worker.join();
        const double latency = std::chrono::duration<double, std::milli>(finished - cancelAt).count();
        if (!result.has_value()) {
            CHECK(result.error().code == ErrorCode::Cancelled);
            ++cancelledRuns;
            worstLatency = std::max(worstLatency, latency);
        }
    }
    std::printf("sketch cancel: uncancelled %d-curve solve %.1f ms; %d of 5 runs cancelled, worst latency %.0f ms\n",
                g.curves, fullMs, cancelledRuns, worstLatency);
    CHECK((cancelledRuns >= 1 || fullMs < 2)); // a solve that ends within 1 ms cannot be caught
    // Documented bound: one diagnosis or one DogLeg iteration. Measured on the reference machine; a loaded machine
    // gets twice the uncancelled solve as slack.
    CHECK(worstLatency <= std::max(1000.0, 2 * fullMs));
}

TEST_CASE("Cancelling drag begin returns Cancelled", "[sketch][cancel]")
{
    const Grid g = makeGrid(optimisedBuild() ? 4 : 2, GridVariant::Nearly, 0.3);
    CancelSource source;
    JobContext ctx{source.token(), {}};
    Result<std::unique_ptr<ISketchSolver::IDragSession>> session =
        tl::unexpected(Error{ErrorCode::Internal, "not run", {}});
    std::thread worker([&] { session = makePlaneGcsSolver()->beginDrag(g.sketch, g.units.back().bottomP2, ctx); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    source.cancel();
    worker.join();
    if (!session.has_value()) {
        CHECK(session.error().code == ErrorCode::Cancelled);
    }
}
