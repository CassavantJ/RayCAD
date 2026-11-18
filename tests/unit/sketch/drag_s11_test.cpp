// Spike S7/S11 drag cases (task T-090): agreement with PlaneGCS's own drag on every step, and the drag-step
// micro-benchmark against the 16 ms budget of PLAN.md section 9. Both run in their own CTest entries (long).
#include "drag_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;

namespace {

// Busy fraction of all logical CPUs since the previous call (whole machine, not this process).
class MachineLoad {
public:
    MachineLoad() { sample(); }
    double sample()
    {
#ifdef _WIN32
        FILETIME idle, kernel, user;
        GetSystemTimes(&idle, &kernel, &user);
        auto u64 = [](FILETIME f) {
            return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        const unsigned long long i = u64(idle), total = u64(kernel) + u64(user); // kernel time includes idle
        const double busy = total > total_ ? 1.0 - double(i - idle_) / double(total - total_) : 0.0;
        idle_ = i;
        total_ = total;
        return busy;
#else
        return -1;
#endif
    }

private:
    unsigned long long idle_ = 0, total_ = 0;
};

std::vector<Vec2> caseTargets(const Sketch& start, const Grid& g, const DragCase& d, int steps)
{
    return straightTargets(pointAt(start, dragPoint(g, d)), d.dx, d.dy, steps);
}

} // namespace

TEST_CASE("S7/S11 drag cases agree with PlaneGCS's own drag on every step", "[sketch][agreement]")
{
    int total = 0, mismatches = 0, fallbacks = 0, reducedSteps = 0;
    // Full size (50 to 200 curves, 100 steps per case) in an optimised build; the reference drag is dense and too slow
    // for that in a debug or sanitizer build, which checks 50 and 100 curves with 25 steps.
    const int maxRows = optimisedBuild() ? 4 : 2;
    const int steps = optimisedBuild() ? 100 : 25;
    for (int rows = 1; rows <= maxRows; ++rows) {
        for (const DragCase& d : dragCases()) {
            const Grid g = makeGrid(rows, d.variant, 0.3);
            const Sketch start = solvedStart(g.sketch);
            const DragComparison c = compareDrag(start, dragPoint(g, d), caseTargets(start, g, d, steps));
            std::printf("agreement %3d curves  %-58s %s\n", g.curves, d.name, describe(c).c_str());
            std::fflush(stdout);
            INFO(g.curves << " curves, " << d.name);
            requireAgreement(c);
            total += c.steps;
            mismatches += c.mismatches;
            fallbacks += c.stats.fallbacks;
            reducedSteps += c.stats.reducedSteps;
        }
    }
    std::printf("agreement: %d steps, %d mismatches > 1e-9 mm, %d answered by the reduced system, %d fallbacks\n",
                total, mismatches, reducedSteps, fallbacks);
    CHECK(mismatches == 0);
}

TEST_CASE("Drag step stays under 16 ms for one coupled group of up to 200 curves", "[sketch][bench]")
{
    if (!optimisedBuild()) {
        WARN("Drag timings are only measured in an optimised build without sanitizers; nothing measured here");
        return;
    }
    MachineLoad load;
    std::printf("bench: %u hardware threads\n", std::thread::hardware_concurrency());
    std::printf("| Curves | Case | Mode | Steps | Median ms | p95 ms | Worst ms | Reduced | Fallbacks | Begin ms | "
                "Full solve ms | Machine busy |\n|-|-|-|-|-|-|-|-|-|-|-|-|\n");
    double worstMedian = 0;
    for (int rows = 1; rows <= 4; ++rows) {
        for (const DragCase& d : dragCases()) {
            const Grid g = makeGrid(rows, d.variant, 0.3);
            const auto ts = Clock::now();
            const Sketch start = solvedStart(g.sketch);
            const double solveMs = msSince(ts);
            JobContext ctx;
            Stats steps;
            Stats begin;
            sketch::detail::DragStats stats;
            load.sample();
            for (int rep = 0; rep < 2; ++rep) {
                const auto tb = Clock::now();
                auto session = take(makePlaneGcsSolver()->beginDrag(start, dragPoint(g, d), ctx));
                begin.add(msSince(tb));
                for (const Vec2& t : caseTargets(start, g, d, 100)) {
                    const auto t0 = Clock::now();
                    const SolveResult r = take(session->move(t));
                    steps.add(msSince(t0));
                    REQUIRE(r.status != SolveStatus::Failed);
                }
                stats = *sketch::detail::dragStats(*session);
            }
            const double busy = load.sample();
            const double median = steps.median();
            worstMedian = std::max(worstMedian, median);
            std::printf("| %d | %.2s | %s | %zu | %.3f | %.3f | %.3f | %d | %d | %.1f | %.1f | %.0f%% |\n", g.curves,
                        d.name, sketch::detail::dragModeName(stats.mode), steps.v.size(), median, steps.pct(0.95),
                        steps.worst(), stats.reducedSteps, stats.fallbacks, begin.median(), solveMs, busy * 100);
            std::fflush(stdout);
            INFO(g.curves << " curves, " << d.name);
            // Gated on the median: a loaded machine may blow the worst case (PLAN.md section 9 row, T-090).
            CHECK(median < 16.0);
        }
    }
    std::printf("bench: worst median %.3f ms\n", worstMedian);
}
