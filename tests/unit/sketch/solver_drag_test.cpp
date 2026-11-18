// Drag sessions (task T-090): the session contract, the partially redundant fallback, and the constraint types spike
// S11 did not drag (arc tangency, angle, point on curve, symmetric) plus drags longer than 50 mm, each compared step
// by step with PlaneGCS's own drag.
#include "drag_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using CT = ConstraintType;

namespace {

bool sameBits(const Sketch& a, const Sketch& b)
{
    const auto ca = coordinates(a), cb = coordinates(b);
    return ca.size() == cb.size() && std::memcmp(ca.data(), cb.data(), ca.size() * sizeof(double)) == 0;
}

// A circular path around `c`.
std::vector<Vec2> arcTargets(Vec2 c, double radius, double fromAngle, double toAngle, int steps)
{
    std::vector<Vec2> v;
    for (int k = 1; k <= steps; ++k) {
        const double a = fromAngle + (toAngle - fromAngle) * k / steps;
        v.push_back({c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    return v;
}

} // namespace

TEST_CASE("A drag is a session; every move solves from the drag-start state", "[sketch][drag]")
{
    const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
    const Sketch start = solvedStart(g.sketch);
    const SketchEntId pt = g.units.back().bottomP2;
    auto solver = makePlaneGcsSolver();
    JobContext ctx;
    auto s1 = take(solver->beginDrag(start, pt, ctx));
    const Vec2 p0 = pointAt(start, pt);
    const SolveResult a = take(s1->move({p0.x + 1.0, p0.y + 0.5}));
    const SolveResult b = take(s1->move({p0.x + 2.0, p0.y + 1.0}));
    REQUIRE(a.status == SolveStatus::UnderConstrained);
    REQUIRE(b.status == SolveStatus::UnderConstrained);
    CHECK(b.freeDof == 5);
    // The result of a move depends only on its target: a fresh session moved straight there gives the same bits.
    auto s2 = take(solver->beginDrag(start, pt, ctx));
    const SolveResult b2 = take(s2->move({p0.x + 2.0, p0.y + 1.0}));
    CHECK(sameBits(b.solved, b2.solved));
    // Moving back to an earlier target repeats that result exactly.
    const SolveResult a2 = take(s1->move({p0.x + 1.0, p0.y + 0.5}));
    CHECK(sameBits(a.solved, a2.solved));
    // end() returns the last result.
    const Sketch fin = s1->end();
    CHECK(sameBits(fin, a2.solved));
    // The dragged point slides along its horizontal line as far as asked; it cannot follow the target upwards.
    const Vec2 p = pointAt(b.solved, pt);
    CHECK(std::fabs(p.x - (p0.x + 2.0)) <= 1e-9);
    CHECK(maxError(b.solved, start) <= 1e-9);
    // Only points can be dragged.
    auto bad = solver->beginDrag(start, g.units.back().bottom, ctx);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    // A session made from a pre-cancelled context does not start.
    CancelSource cancel;
    cancel.cancel();
    JobContext cancelled{cancel.token(), {}};
    auto none = solver->beginDrag(start, pt, cancelled);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::Cancelled);
}

TEST_CASE("A blocked point does not move and nothing else does", "[sketch][drag]")
{
    const Grid g = makeGrid(1, GridVariant::Fully, 0.3);
    const Sketch start = solvedStart(g.sketch);
    const SketchEntId pt = g.units.back().bottomP2;
    const DragComparison c = compareDrag(start, pt, straightTargets(pointAt(start, pt), 0.1, 0.05, 20));
    requireAgreement(c);
    CHECK_FALSE(c.stats.pointFree);
}

TEST_CASE("Partially redundant constraints drag with PlaneGCS's own solve", "[sketch][drag]")
{
    // Two fixed lines share a point (the second fix is partially redundant); a third line hangs off them.
    Builder b;
    const auto pa = b.point(0, 0), pb = b.point(10, 0), pc = b.point(10, 10), pd = b.point(0, 12);
    const auto l1 = b.line(pa, pb), l2 = b.line(pb, pc), l3 = b.line(pc, pd);
    b.constrain(CT::Fix, {l1});
    b.constrain(CT::Fix, {l2});
    b.constrain(CT::Distance, {l3}, 11);
    const Sketch start = solvedStart(b.s);
    JobContext ctx;
    {
        const SolveResult r = take(makePlaneGcsSolver()->solve(start, ctx));
        REQUIRE(r.partiallyRedundant.size() == 1);
    }
    const DragComparison c = compareDrag(start, pd, arcTargets({10, 10}, 14, 1.8, 3.6, 40));
    requireAgreement(c);
    CHECK(c.stats.mode == sketch::detail::DragMode::PlaneGcs);
    CHECK(c.maxDiff == 0); // the same algorithm as the reference
    CHECK(c.stats.fullSteps == 40);
}

// Tolerance of the four tests below. Spike S11's cases (drag_s11_test.cpp, all S7 sketches, 2,400 steps) agree with
// PlaneGCS's own drag to 1e-9 mm on every step. On these sketches most steps do too, but not all: both iterations stop
// when their line search fails, which happens at the floating-point noise of PlaneGCS's merit function
// (mu * |residual| dominates it near the solution), and each stops at its own place. Measured, on Windows (MSVC) and
// Linux (GCC): up to 6.3e-9 mm, in a few steps per hundred, both where the point reaches its target and where it is
// held on a line or at the edge of its reach; every constraint holds to 1e-14 and both points end equally far from
// the target (to 1e-13). These drags are checked to kNewCases; drag_runner.h prints the count above 1e-9.
constexpr double kNewCases = 2e-8;

TEST_CASE("Drag with arc tangency", "[sketch][drag]")
{
    // A J shape: a horizontal line, a tangent arc, a tangent line, and a circle tangent to that line.
    Builder b;
    const auto p0 = b.point(0, 0), p1 = b.point(20, 0), ctr = b.point(20, 8), p2 = b.point(28, 8);
    const auto p3 = b.point(28, 30);
    const auto l1 = b.line(p0, p1);
    const auto arc = b.arc(ctr, p1, p2);
    const auto l2 = b.line(p2, p3);
    b.constrain(CT::Fix, {p0});
    b.constrain(CT::Horizontal, {l1});
    b.constrain(CT::Tangent, {l1, arc, p1});
    b.constrain(CT::Tangent, {arc, l2, p2});
    b.constrain(CT::Radius, {arc}, 8);
    const auto cc = b.point(45, 20);
    const auto circ = b.circle(cc, 17);
    b.constrain(CT::Fix, {cc});
    b.constrain(CT::Tangent, {l2, circ});
    const Sketch start = solvedStart(b.s);
    // Held on a line: the arc's centre stays 8 mm above the horizontal line, so it slides along x only.
    const DragComparison c = compareDrag(start, ctr, straightTargets(pointAt(start, ctr), 0.1, 0.05, 100));
    requireAgreement(c, kNewCases);
    CHECK(c.stats.reducedUsable);
    // Free: the end goes where it is dragged.
    const DragComparison c2 = compareDrag(start, p3, straightTargets(pointAt(start, p3), -0.2, 0.15, 100));
    requireAgreement(c2, kNewCases);
}

TEST_CASE("Drag with angle constraints", "[sketch][drag]")
{
    Builder b;
    const auto o = b.point(0, 0), a = b.point(20, 0), q = b.point(10, 15);
    const auto l1 = b.line(o, a), l2 = b.line(o, q);
    b.constrain(CT::Fix, {o});
    b.constrain(CT::Distance, {l1}, 20);
    b.constrain(CT::Angle, {l1, l2}, 0.9);
    b.constrain(CT::Distance, {l2}, 18);
    // a line at a fixed absolute angle hanging off q
    const auto r = b.point(30, 20);
    const auto l3 = b.line(q, r);
    b.constrain(CT::Angle, {l3}, -0.25);
    const Sketch start = solvedStart(b.s);
    // Out of reach: the target circles at 25 mm while l1 is 20 long; l1 turns after it and l2 turns with l1.
    const DragComparison c = compareDrag(start, a, arcTargets({0, 0}, 25, 0.0, 1.5, 100));
    requireAgreement(c, kNewCases);
    // Free to turn: rotating l1 by its end.
    const DragComparison c2 = compareDrag(start, a, arcTargets({0, 0}, 20, 0.0, 1.5, 100));
    requireAgreement(c2, kNewCases);
    // Free: pull r along its fixed direction u and slowly across it (n), which turns l1 and l2. r reaches the
    // target while its offset across u stays within q's circle (18 mm); it starts at about 16 and the path moves in.
    const double au = -0.25;
    const Vec2 u{std::cos(au), std::sin(au)}, n{-std::sin(au), std::cos(au)};
    const DragComparison c3 =
        compareDrag(start, r, straightTargets(pointAt(start, r), 0.3 * u.x - 0.05 * n.x, 0.3 * u.y - 0.05 * n.y, 100));
    requireAgreement(c3, kNewCases);
}

TEST_CASE("Drag with point-on-curve constraints", "[sketch][drag]")
{
    Builder b;
    const auto cc = b.point(0, 0);
    const auto circ = b.circle(cc, 10);
    const auto la = b.point(-30, -20), lb = b.point(30, -20);
    const auto rail = b.line(la, lb);
    const auto q = b.point(10, 0), r = b.point(-15, -20);
    const auto rod = b.line(q, r);
    b.constrain(CT::Fix, {cc});
    b.constrain(CT::Radius, {circ}, 10);
    b.constrain(CT::Fix, {rail});
    b.constrain(CT::PointOnCurve, {q, circ});
    b.constrain(CT::PointOnCurve, {r, rail});
    // longer than the largest crank-to-rail distance (30), so the mechanism never locks
    b.constrain(CT::Distance, {rod}, 34);
    // and a point riding an arc
    const auto ac = b.point(50, 0), as = b.point(60, 0), ae = b.point(50, 10);
    const auto arc = b.arc(ac, as, ae);
    b.constrain(CT::Fix, {ac});
    b.constrain(CT::Fix, {as});
    const auto rider = b.point(57, 7);
    b.constrain(CT::PointOnCurve, {rider, arc});
    const Sketch start = solvedStart(b.s);
    // Out of reach: the rider stays on its arc (radius 10) while the target circles at 12.
    const DragComparison c = compareDrag(start, rider, arcTargets({50, 0}, 12, 0.8, 2.5, 60));
    requireAgreement(c, kNewCases);
    // Out of reach: the slider r stays on its rail; the target leaves it upwards. The crank follows.
    const DragComparison c2 = compareDrag(start, r, straightTargets(pointAt(start, r), 0.15, 0.1, 100));
    requireAgreement(c2, kNewCases);
    // On its circle: drag the crank pin around its circle; r slides on the rail.
    const DragComparison c3 = compareDrag(start, q, arcTargets({0, 0}, 10, 0.0, 2.0, 100));
    requireAgreement(c3, kNewCases);
}

TEST_CASE("Drag with symmetric constraints", "[sketch][drag]")
{
    Builder b;
    const auto aa = b.point(0, -30), ab = b.point(0, 30);
    const auto axis = b.line(aa, ab);
    const auto p = b.point(-10, 5), q = b.point(10, 5);
    const auto m = b.point(30, 0), u = b.point(25, 3), v = b.point(35, -3);
    const auto ga = b.point(-40, 5), gb = b.point(40, 5);
    const auto guide = b.line(ga, gb);
    b.constrain(CT::Fix, {axis});
    b.constrain(CT::Symmetric, {p, q, axis});
    b.constrain(CT::Fix, {guide});
    b.constrain(CT::PointOnCurve, {p, guide}); // so p and its mirror q move along y = 5 only
    b.constrain(CT::Fix, {m});
    b.constrain(CT::Symmetric, {u, v, m});
    const auto l = b.line(p, u);
    b.constrain(CT::Distance, {l}, 18);
    const Sketch start = solvedStart(b.s);
    // Out of reach: q (the mirror of p) is dragged off its line; it slides along it, and p mirrors it.
    const DragComparison c = compareDrag(start, q, straightTargets(pointAt(start, q), 0.15, 0.2, 100));
    requireAgreement(c, kNewCases);
    // Out of reach: v (the mirror of u about m) is dragged around a 30 mm circle about m while u, and so v, stay
    // within 18 mm of p's line (|y| <= 13 for v; the targets are at y > 16).
    const DragComparison c2 = compareDrag(start, v, arcTargets({30, 0}, 30, 0.6, 1.4, 80));
    requireAgreement(c2, kNewCases);
    // Free: u moves where it is dragged, p follows along its line, v mirrors u.
    const DragComparison c3 = compareDrag(start, u, arcTargets({30, 0}, 6, 2.5, 4.5, 80));
    requireAgreement(c3, kNewCases);
}

TEST_CASE("Drags longer than 50 mm", "[sketch][drag]")
{
    {
        // S7's 50-curve sketch, the D1 slide carried on to 67 mm in 1.1 mm steps.
        const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
        const Sketch start = solvedStart(g.sketch);
        const SketchEntId pt = g.units.back().bottomP2;
        const DragComparison c = compareDrag(start, pt, straightTargets(pointAt(start, pt), 1.0, 0.5, 60));
        requireAgreement(c);
        CHECK(c.maxTravel > 50);
    }
    {
        // The J shape's free end swept 60 mm in one move each way, then along a 77 mm path. The end can reach every
        // target in a one-parameter family of ways (first line length, sweep, second line length), so where each
        // solver lands depends on its path: the sparse step and PlaneGCS's dense step agree to about 1e-6 mm here,
        // not 1e-9 (both hold every constraint to 1e-14 and both put the point on the target; at some steps PlaneGCS
        // stops a few nanometres short of it). Measured: 12 of 120 steps differ by more than 1e-9 mm, the largest by
        // 1.1e-6 mm (Windows, MSVC); 15, the largest by 6.8e-7 mm (Linux, GCC, debug). See the T-090 report.
        Builder b;
        const auto p0 = b.point(0, 0), p1 = b.point(20, 0), c = b.point(20, 8), p2 = b.point(28, 8);
        const auto p3 = b.point(28, 30);
        const auto l1 = b.line(p0, p1);
        const auto arc = b.arc(c, p1, p2);
        const auto l2 = b.line(p2, p3);
        b.constrain(CT::Fix, {p0});
        b.constrain(CT::Horizontal, {l1});
        b.constrain(CT::Tangent, {l1, arc, p1});
        b.constrain(CT::Tangent, {arc, l2, p2});
        b.constrain(CT::Radius, {arc}, 8);
        const Sketch start = solvedStart(b.s);
        const Vec2 s3 = pointAt(start, p3);
        const DragComparison c1 =
            compareDrag(start, p3, {{s3.x + 60, s3.y}, {s3.x, s3.y + 60}, {s3.x - 40, s3.y + 45}});
        requirePathAgreement(c1, 1e-5);
        CHECK(c1.maxTravel >= 60);
        const DragComparison c2 = compareDrag(start, p3, straightTargets(s3, 0.5, 0.4, 120));
        requirePathAgreement(c2, 1e-5);
        CHECK(c2.maxTravel > 50);
    }
}
