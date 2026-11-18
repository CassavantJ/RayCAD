// Runs a drag the way the drag worker does (one session, every move from the drag-start state) next to PlaneGCS's
// own drag (DragMode::PlaneGcs, FreeCAD's method) as the reference, and compares every step.
#pragma once

#include "sketch_test_util.h"

#include "solver_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace ray::sketch::test {

struct DragComparison {
    int steps = 0;
    int mismatches = 0;            // steps where the result differs from the reference by more than 1e-9 mm
    double maxDiff = 0;            // largest coordinate difference over all steps
    double maxConstraintError = 0; // largest error of any constraint in the session's results (independent check)
    int failed = 0;                // steps the session reported Failed
    int referenceFailed = 0;       // steps PlaneGCS's own drag failed
    int failedAlone = 0;           // steps exactly one of the two failed
    // Largest (session's distance from the target) - (PlaneGCS's distance from the target): positive when the
    // session's point ended farther from where it was dragged than PlaneGCS's did.
    double maxExtraDistance = 0;
    double maxTravel = 0; // largest distance of a target from the drag start
    detail::DragStats stats;
    Stats stepMs;
};

// The solved starting sketch of a test case: `noisy` solved by a full solve.
inline Sketch solvedStart(const Sketch& noisy)
{
    JobContext ctx;
    const SolveResult r = take(makePlaneGcsSolver()->solve(noisy, ctx));
    REQUIRE(r.status != SolveStatus::Failed);
    REQUIRE(r.status != SolveStatus::Conflicting);
    return r.solved;
}

inline DragComparison compareDrag(const Sketch& start, SketchEntId point, const std::vector<Vec2>& targets,
                                  detail::DragOptions options = {})
{
    JobContext ctx;
    auto session = take(detail::beginDragSession(start, point, ctx, options));
    detail::DragOptions refOptions;
    refOptions.force = detail::DragMode::PlaneGcs;
    auto reference = take(detail::beginDragSession(start, point, ctx, refOptions));
    const Vec2 p0 = pointAt(start, point);
    DragComparison out;
    for (const Vec2& t : targets) {
        const auto t0 = Clock::now();
        const SolveResult r = take(session->move(t));
        out.stepMs.add(msSince(t0));
        const SolveResult ref = take(reference->move(t));
        ++out.steps;
        out.maxTravel = std::max(out.maxTravel, std::hypot(t.x - p0.x, t.y - p0.y));
        if (r.status == SolveStatus::Failed) {
            ++out.failed;
        } else {
            out.maxConstraintError = std::max(out.maxConstraintError, maxError(r.solved, start));
        }
        if (ref.status == SolveStatus::Failed) {
            ++out.referenceFailed;
        }
        out.failedAlone += (r.status == SolveStatus::Failed) != (ref.status == SolveStatus::Failed) ? 1 : 0;
        {
            const Vec2 a = pointAt(r.solved, point), b = pointAt(ref.solved, point);
            out.maxExtraDistance =
                std::max(out.maxExtraDistance, std::hypot(a.x - t.x, a.y - t.y) - std::hypot(b.x - t.x, b.y - t.y));
        }
        const double d = maxDiff(coordinates(r.solved), coordinates(ref.solved));
        out.maxDiff = std::max(out.maxDiff, d);
        out.mismatches += d > 1e-9 ? 1 : 0;
        if (d > 1e-9 && std::getenv("RAY_SKETCH_DRAG_VERBOSE")) {
            const Vec2 a = pointAt(r.solved, point), b = pointAt(ref.solved, point);
            std::printf("  step %d: diff %.3g; distance to target: session %.17g, PlaneGCS %.17g; status %d / %d\n",
                        out.steps, d, std::hypot(a.x - t.x, a.y - t.y), std::hypot(b.x - t.x, b.y - t.y),
                        static_cast<int>(r.status), static_cast<int>(ref.status));
        }
    }
    out.stats = *detail::dragStats(*session);
    return out;
}

inline std::vector<Vec2> straightTargets(Vec2 from, double dx, double dy, int steps)
{
    std::vector<Vec2> v;
    for (int k = 1; k <= steps; ++k) {
        v.push_back({from.x + dx * k, from.y + dy * k});
    }
    return v;
}

inline std::string describe(const DragComparison& c)
{
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%d steps, mode %s (reduced usable %d), reduced %d, full %d, fallbacks %d, failed %d, reference "
                  "failed %d, mismatches %d, max diff %.3g, max constraint error %.3g, travel %.1f mm, farther from "
                  "target than PlaneGCS by at most %.3g",
                  c.steps, detail::dragModeName(c.stats.mode), int(c.stats.reducedUsable), c.stats.reducedSteps,
                  c.stats.fullSteps, c.stats.fallbacks, c.failed, c.referenceFailed, c.mismatches, c.maxDiff,
                  c.maxConstraintError, c.maxTravel, c.maxExtraDistance);
    return buf;
}

// The checks every compared drag must pass: same result as PlaneGCS's own drag to `tolerance` mm on every step (1e-9
// unless a test says why not; a step that fails in both returns the drag-start sketch in both), and every constraint
// holds to 1e-9.
inline void requireAgreement(const DragComparison& c, double tolerance = 1e-9)
{
    INFO(describe(c));
    if (tolerance > 1e-9) {
        std::printf("agreement to %.0e: %s\n", tolerance, describe(c).c_str());
    }
    CHECK(c.failedAlone == 0);
    CHECK(c.maxDiff <= tolerance);
    if (tolerance <= 1e-9) {
        CHECK(c.mismatches == 0);
    }
    CHECK(c.maxConstraintError <= 1e-9);
}

// The checks for a drag whose result is not unique: the target can be reached in a one-parameter family of ways, so
// the configuration each solver lands on depends on its path, and PlaneGCS's own iteration also stops a few
// nanometres short of a reachable target at times. Every constraint holds, failures coincide, the session's point is
// never meaningfully farther from the target than PlaneGCS's, and the configurations differ by at most `bound`.
inline void requirePathAgreement(const DragComparison& c, double bound)
{
    INFO(describe(c));
    std::printf("non-unique drag: %s\n", describe(c).c_str());
    CHECK(c.failedAlone == 0);
    CHECK(c.maxConstraintError <= 1e-9);
    CHECK(c.maxExtraDistance <= 1e-8);
    CHECK(c.maxDiff <= bound);
}

} // namespace ray::sketch::test
