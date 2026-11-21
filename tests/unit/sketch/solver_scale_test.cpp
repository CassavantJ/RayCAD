// The size switch of the full solve (pre-M2 performance work; kDenseLimit in src/sketch/solver_internal.h). A decoupled
// component below the limit is diagnosed and solved with PlaneGCS's dense linear algebra (same bits as GCS::System,
// solver_determinism_test.cpp); one at or above it with a sparse QR diagnosis and a sparse DogLeg. Here:
//   - the sparse path gives PlaneGCS's diagnosis (status, degrees of freedom, conflicting, redundant and partially
//     redundant constraints) on the S7 grids and the diagnosis cases, and, where the solution is unique, its
//     coordinates to 1e-9 mm; every constraint holds wherever it solves
//   - a sketch of many small components is diagnosed per component, all of them dense
//   - a 2000-entity chain is solved by a bounded amount of work (the work is counted, not timed)
#include "sketch_test_util.h"

#include "solver_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdio>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using CT = ConstraintType;

namespace {

SolveResult solveWith(const Sketch& s, int denseLimit)
{
    JobContext ctx;
    sketch::detail::SolveOptions o;
    o.denseLimit = denseLimit;
    return take(sketch::detail::solveSketch(s, ctx, o));
}

SolveResult solvePlaneGcs(const Sketch& s)
{
    JobContext ctx;
    sketch::detail::SolveOptions o;
    o.planeGcsSolve = true;
    return take(sketch::detail::solveSketch(s, ctx, o));
}

bool solves(SolveStatus s)
{
    return s != SolveStatus::Failed && s != SolveStatus::Conflicting;
}

// Every component sparse.
constexpr int kAllSparse = 1;

void requireSameDiagnosis(const SolveResult& ours, const SolveResult& theirs)
{
    CHECK(ours.status == theirs.status);
    CHECK(ours.freeDof == theirs.freeDof);
    CHECK(ours.conflicting == theirs.conflicting);
    CHECK(ours.redundant == theirs.redundant);
    CHECK(ours.partiallyRedundant == theirs.partiallyRedundant);
}

} // namespace

TEST_CASE("The sparse diagnosis and DogLeg agree with PlaneGCS on the S7 grids", "[sketch][scale]")
{
    const std::vector<std::pair<int, GridVariant>> cases = {{1, GridVariant::Nearly},    {1, GridVariant::Loose},
                                                            {1, GridVariant::Decoupled}, {2, GridVariant::Nearly},
                                                            {2, GridVariant::Fully},     {2, GridVariant::Redundant}};
    for (const auto& [rows, variant] : cases) {
        INFO("rows " << rows << ", variant " << static_cast<int>(variant));
        const Grid g = makeGrid(rows, variant, 0.3);
        const SolveResult theirs = solvePlaneGcs(g.sketch);
        REQUIRE(solves(theirs.status));
        sketch::detail::solverCounters() = {};
        const SolveResult ours = solveWith(g.sketch, kAllSparse);
        CHECK(sketch::detail::solverCounters().denseDiagnoses == 0);
        CHECK(sketch::detail::solverCounters().sparseDiagnoses > 0);
        CHECK(sketch::detail::solverCounters().sparseDogLegs > 0);
        requireSameDiagnosis(ours, theirs);
        CHECK(maxError(ours.solved, g.sketch) <= 1e-9);
        if (variant == GridVariant::Fully) { // a unique solution: the same one
            CHECK(maxDiff(coordinates(ours.solved), coordinates(theirs.solved)) <= 1e-9);
        }
        // The default size switch: the same diagnosis, every constraint met.
        JobContext ctx;
        const SolveResult byDefault = take(makePlaneGcsSolver()->solve(g.sketch, ctx));
        requireSameDiagnosis(byDefault, theirs);
        CHECK(maxError(byDefault.solved, g.sketch) <= 1e-9);
    }
}

TEST_CASE("The sparse diagnosis names the same conflicting and redundant constraints as PlaneGCS", "[sketch][scale]")
{
    std::vector<Sketch> cases;
    {
        Builder b; // two radii on one circle conflict
        const auto c = b.circle(b.point(0, 0), 5);
        b.constrain(CT::Fix, {b.s.entities.front().id});
        b.constrain(CT::Radius, {c}, 5);
        b.constrain(CT::Radius, {c}, 6);
        cases.push_back(b.s);
    }
    {
        Builder b; // a redundant horizontal
        const auto a = b.point(0, 0), c = b.point(10, 0), d = b.point(0.3, 5), e = b.point(10, 5);
        const auto bottom = b.line(a, c), top = b.line(d, e);
        b.constrain(CT::Fix, {a});
        b.constrain(CT::Horizontal, {bottom});
        b.constrain(CT::Parallel, {top, bottom});
        b.constrain(CT::Horizontal, {top});
        cases.push_back(b.s);
    }
    {
        Builder b; // a partially redundant fix
        const auto pa = b.point(0, 0), pb = b.point(10, 0), pc = b.point(10, 10), pd = b.point(0, 12);
        const auto l1 = b.line(pa, pb), l2 = b.line(pb, pc), l3 = b.line(pc, pd);
        b.constrain(CT::Fix, {l1});
        b.constrain(CT::Fix, {l2});
        b.constrain(CT::Distance, {l3}, 11);
        cases.push_back(b.s);
    }
    {
        Builder b; // a fixed arc next to a free one, in two components
        const auto c = b.point(0, 0), s = b.point(3, 0), e = b.point(0, 3);
        b.constrain(CT::Fix, {b.arc(c, s, e)});
        const auto c2 = b.point(10, 0), s2 = b.point(13, 0), e2 = b.point(10, 3);
        b.arc(c2, s2, e2);
        cases.push_back(b.s);
    }
    for (std::size_t i = 0; i < cases.size(); ++i) {
        INFO("case " << i);
        const SolveResult theirs = solvePlaneGcs(cases[i]);
        requireSameDiagnosis(solveWith(cases[i], kAllSparse), theirs);
        requireSameDiagnosis(solveWith(cases[i], INT_MAX), theirs);
    }
}

TEST_CASE("Many small components are diagnosed one by one, all of them dense", "[sketch][scale]")
{
    // 300 circles, each with its centre fixed and its radius dimensioned, plus a redundant diameter on every tenth: at
    // least 300 components of at most 3 unknowns. Every one is dense; the redundant diameters are found per component.
    Builder b;
    std::vector<Uuid> extra;
    for (int i = 0; i < 300; ++i) {
        const auto centre = b.point(10.0 * i, 0);
        const auto c = b.circle(centre, 2 + 0.01 * i);
        b.constrain(CT::Fix, {centre});
        b.constrain(CT::Radius, {c}, 3);
        if (i % 10 == 0) {
            extra.push_back(b.constrain(CT::Diameter, {c}, 6));
        }
    }
    sketch::detail::solverCounters() = {};
    JobContext ctx;
    const SolveResult r = take(makePlaneGcsSolver()->solve(b.s, ctx));
    CHECK(sketch::detail::solverCounters().denseDiagnoses >=
          300); // a fixed point may split into one component per coordinate
    CHECK(sketch::detail::solverCounters().sparseDiagnoses == 0);
    CHECK(sketch::detail::solverCounters().redundancySolves == 30);
    CHECK(r.status == SolveStatus::OverConstrained);
    CHECK(r.freeDof == 0);
    CHECK(r.redundant == extra);
    CHECK(maxError(r.solved, b.s) <= 1e-9);
    const SolveResult theirs = solvePlaneGcs(b.s);
    requireSameDiagnosis(r, theirs);
}

TEST_CASE("A 2000-entity chain solves with a bounded amount of work", "[sketch][scale]")
{
    // The zig-zag chain of the doc timing tests: 1000 lines of fixed length from a fixed first point, one length
    // changed, so the whole chain moves (under-constrained by 1000 degrees of freedom: one component of 2002 unknowns).
    Builder b;
    std::vector<SketchEntId> points, lines;
    for (int i = 0; i <= 1000; ++i) {
        points.push_back(b.point(i * 0.5, (i % 2) * 0.4));
    }
    for (int i = 0; i < 1000; ++i) {
        lines.push_back(b.line(points[static_cast<std::size_t>(i)], points[static_cast<std::size_t>(i) + 1]));
        b.constrain(CT::Distance, {lines.back()}, i == 500 ? 0.8 : std::hypot(0.5, 0.4));
    }
    b.constrain(CT::Fix, {points.front()});
    sketch::detail::solverCounters() = {};
    JobContext ctx;
    const auto t0 = Clock::now();
    const SolveResult r = take(makePlaneGcsSolver()->solve(b.s, ctx));
    const double ms = msSince(t0);
    const sketch::detail::SolverCounters n = sketch::detail::solverCounters();
    std::printf("2000-entity chain: %.0f ms, %d DogLeg iterations, %d sparse diagnoses\n", ms, n.dogLegIterations,
                n.sparseDiagnoses);
    CHECK(r.status == SolveStatus::UnderConstrained);
    CHECK(r.freeDof == 1000);
    CHECK(maxError(r.solved, b.s) <= 1e-9);
    CHECK(n.denseDiagnoses == 0);
    CHECK(n.sparseDiagnoses == 1);
    CHECK(n.denseDogLegs == 0);
    CHECK(n.sparseDogLegs == 1);
    CHECK(n.dogLegIterations <= 30);
    CHECK(n.redundancySolves == 0);
}
