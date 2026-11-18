// Every constraint type of sketch.h solves on unit cases (task T-090), judged by the independent geometric check in
// sketch_test_util.h; plus model validation, canonical order, driven dimensions and unsupported combinations.
#include "sketch_test_util.h"

#include "model.h"
#include "solver_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using CT = ConstraintType;

namespace {

SolveResult solveIt(const Sketch& s)
{
    auto solver = makePlaneGcsSolver();
    JobContext ctx;
    return take(solver->solve(s, ctx));
}

// Solves and checks that every constraint holds to 1e-9 (mm or rad) by the independent check.
SolveResult solvesTo(const Sketch& s, std::optional<SolveStatus> expected = std::nullopt)
{
    const SolveResult r = solveIt(s);
    INFO("status " << static_cast<int>(r.status) << ", dof " << r.freeDof);
    REQUIRE(r.status != SolveStatus::Failed);
    REQUIRE(r.status != SolveStatus::Conflicting);
    if (expected) {
        CHECK(r.status == *expected);
    }
    CHECK(maxError(r.solved, canonicalSketch(s)) <= 1e-9);
    CHECK(r.solved.entities.size() == s.entities.size());
    CHECK(r.solved.constraints.size() == s.constraints.size());
    return r;
}

} // namespace

TEST_CASE("Coincident, horizontal and vertical", "[sketch][constraints]")
{
    Builder b;
    const auto p0 = b.point(0, 0), p1 = b.point(10.2, 0.3), p2 = b.point(10.1, 7.9), p3 = b.point(0.4, 8.2);
    const auto q = b.point(3, 3);
    const auto l0 = b.line(p0, p1), l1 = b.line(p3, p0);
    b.constrain(CT::Fix, {p0});
    b.constrain(CT::Horizontal, {l0});
    b.constrain(CT::Horizontal, {p2, p3});
    b.constrain(CT::Vertical, {p1, p2});
    b.constrain(CT::Vertical, {l1});
    b.constrain(CT::Coincident, {q, p2});
    const SolveResult r = solvesTo(b.s);
    CHECK(r.freeDof == 2);
    CHECK(pointAt(r.solved, q).x == pointAt(r.solved, p2).x);
}

TEST_CASE("Parallel, perpendicular, equal, angle and distance on lines", "[sketch][constraints]")
{
    Builder b;
    const auto a = b.point(0, 0), c = b.point(20, 1), d = b.point(1, 12), e = b.point(21, 13.5);
    const auto f = b.point(30, 0), g = b.point(31, 9);
    const auto base = b.line(a, c), top = b.line(d, e), side = b.line(a, d), other = b.line(f, g);
    b.constrain(CT::Fix, {a});
    b.constrain(CT::Angle, {base}, 0.1);
    b.constrain(CT::Distance, {base}, 20);
    b.constrain(CT::Parallel, {top, base});
    b.constrain(CT::Perpendicular, {side, base});
    b.constrain(CT::Equal, {top, base});
    b.constrain(CT::Distance, {side}, 12);
    b.constrain(CT::Angle, {base, other}, 1.2);
    b.constrain(CT::Equal, {other, side});
    b.constrain(CT::DistanceX, {f}, 30);
    b.constrain(CT::DistanceY, {f}, -1);
    solvesTo(b.s, SolveStatus::FullyConstrained);
}

TEST_CASE("Distance variants and horizontal or vertical distances", "[sketch][constraints]")
{
    Builder b;
    const auto o = b.point(0, 0), p = b.point(5, 1), q = b.point(-3, 4);
    const auto la = b.point(0, -5), lb = b.point(10, -5.5);
    const auto line = b.line(la, lb);
    const auto c1 = b.circle(b.point(20, 0), 3), c2 = b.circle(b.point(30, 1), 2.2);
    const auto ac = b.point(0, 20), as = b.point(4, 20), ae = b.point(0, 24);
    const auto arc = b.arc(ac, as, ae);
    b.constrain(CT::Fix, {o});
    b.constrain(CT::Distance, {o, p}, 6);
    b.constrain(CT::Distance, {q, line}, 7.5);
    b.constrain(CT::Distance, {p, c1}, 9);
    b.constrain(CT::Distance, {c1, c2}, 4);
    b.constrain(CT::Distance, {c2, line}, 2.5);
    b.constrain(CT::Distance, {arc, line}, 20);
    b.constrain(CT::DistanceX, {o, q}, -3.5);
    b.constrain(CT::DistanceY, {line}, -0.25);
    b.constrain(CT::DistanceX, {line}, 11);
    solvesTo(b.s);
}

TEST_CASE("Radius, diameter, concentric and equal radius on circles and arcs", "[sketch][constraints]")
{
    Builder b;
    const auto cc = b.point(0, 0);
    const auto c = b.circle(cc, 5.5);
    const auto ac = b.point(0.2, -0.1), as = b.point(8, 0), ae = b.point(0, 8.3);
    const auto arc = b.arc(ac, as, ae);
    const auto c3 = b.circle(b.point(20, 0), 2);
    b.constrain(CT::Fix, {cc});
    b.constrain(CT::Radius, {c}, 5);
    b.constrain(CT::Concentric, {c, arc});
    b.constrain(CT::Diameter, {arc}, 16);
    b.constrain(CT::Equal, {c3, c});
    const SolveResult r = solvesTo(b.s);
    for (const Entity& e : r.solved.entities) {
        if (e.id == c3) {
            CHECK(std::fabs(std::get<Circle>(e.geom).radius - 5) <= 1e-12);
        }
    }
}

TEST_CASE("Tangency: line to circle and arc, circle to circle, and at a shared endpoint", "[sketch][constraints]")
{
    Builder b;
    const auto la = b.point(-10, 0), lb = b.point(10, 0.2);
    const auto line = b.line(la, lb);
    const auto c1 = b.circle(b.point(0, 3.1), 3);
    const auto c2 = b.circle(b.point(7.2, 3.3), 4);   // external tangency with c1
    const auto c3 = b.circle(b.point(0.5, 3.0), 1.4); // internal tangency with c1
    const auto ac = b.point(-20, 5), as = b.point(-15.2, 5), ae = b.point(-20, 9.8);
    const auto arc = b.arc(ac, as, ae);
    // a line ending at the arc's end point, tangent there
    const auto tq = b.point(-30, 10.2);
    const auto tl = b.line(ae, tq);
    b.constrain(CT::Fix, {line});
    b.constrain(CT::Tangent, {line, c1});
    b.constrain(CT::Tangent, {c1, c2});
    b.constrain(CT::Tangent, {c3, c1});
    b.constrain(CT::Tangent, {arc, line});
    b.constrain(CT::Tangent, {tl, arc, ae});
    b.constrain(CT::Radius, {arc}, 5);
    solvesTo(b.s);
}

TEST_CASE("Symmetric, midpoint and point on curve", "[sketch][constraints]")
{
    Builder b;
    const auto ma = b.point(0, -10), mb = b.point(0.3, 10);
    const auto mirror = b.line(ma, mb);
    const auto p = b.point(-4, 2), q = b.point(4.5, 2.4);
    const auto c = b.point(10, 10), r = b.point(14, 10.5), s = b.point(6.2, 9.1);
    const auto m = b.point(20, 1), la = b.point(18, -2), lb = b.point(23, 5);
    const auto seg = b.line(la, lb);
    const auto onLine = b.point(19, 3);
    const auto circ = b.circle(b.point(40, 0), 5);
    const auto onCircle = b.point(44, 2.5);
    const auto ac = b.point(60, 0), as = b.point(65, 0), ae = b.point(60, 5);
    const auto arc = b.arc(ac, as, ae);
    const auto onArc = b.point(63.8, 3.9);
    b.constrain(CT::Fix, {mirror});
    b.constrain(CT::Symmetric, {p, q, mirror});
    b.constrain(CT::Symmetric, {r, s, c});
    b.constrain(CT::Midpoint, {m, seg});
    const auto m2 = b.point(10, -10), lc = b.point(6, -14), ld = b.point(15, -5);
    b.constrain(CT::Midpoint, {b.line(lc, ld), m2}); // either order
    b.constrain(CT::Fix, {m2});
    b.constrain(CT::PointOnCurve, {onLine, seg});
    b.constrain(CT::PointOnCurve, {circ, onCircle});
    b.constrain(CT::PointOnCurve, {onArc, arc});
    solvesTo(b.s);
}

TEST_CASE("Fix holds points, lines, circles, arcs, slots and spline control points", "[sketch][constraints]")
{
    Builder b;
    const auto p = b.point(1, 2);
    const auto l = b.line(b.point(3, 4), b.point(5, 6));
    const auto c = b.circle(b.point(10, 0), 2.5);
    const auto ac = b.point(20, 0), as = b.point(23, 0), ae = b.point(20, 3);
    const auto arc = b.arc(ac, as, ae);
    const auto sa = b.point(30, 0), sb = b.point(40, 0);
    const auto slot = b.add(Slot{sa, sb, 4});
    const auto k0 = b.point(0, 20), k1 = b.point(5, 25), k2 = b.point(10, 20), k3 = b.point(15, 26);
    const auto spline = b.add(Spline{{k0, k1, k2, k3}, 3, false});
    const auto free = b.point(50, 50);
    b.constrain(CT::Fix, {p});
    b.constrain(CT::Fix, {l});
    b.constrain(CT::Fix, {c});
    b.constrain(CT::Fix, {arc});
    b.constrain(CT::Fix, {slot});
    b.constrain(CT::Fix, {spline});
    b.constrain(CT::Distance, {free, k3}, 30);
    const SolveResult r = solvesTo(b.s);
    CHECK(r.freeDof == 1);
    CHECK(pointAt(r.solved, k1).x == 5);
    CHECK(pointAt(r.solved, sb).x == 40);
}

TEST_CASE("Clockwise arcs solve like counter-clockwise ones", "[sketch][constraints]")
{
    for (bool ccw : {true, false}) {
        Builder b;
        const auto c = b.point(0, 0), st = b.point(5, 0.2), en = b.point(0.1, 5);
        const auto arc = b.arc(c, st, en, ccw);
        const auto q = b.point(10, 10.5);
        const auto l = b.line(en, q);
        b.constrain(CT::Fix, {c});
        b.constrain(CT::Radius, {arc}, 5);
        b.constrain(CT::Horizontal, {c, st});
        b.constrain(CT::Tangent, {l, arc, en});
        b.constrain(CT::Distance, {l}, 8);
        b.constrain(CT::Angle, {l}, 0.3);
        INFO("ccw " << ccw);
        const SolveResult r = solvesTo(b.s);
        CHECK(std::fabs(pointAt(r.solved, st).x - 5) <= 1e-9);
    }
}

TEST_CASE("Driven dimensions are reported, not enforced", "[sketch][constraints]")
{
    Builder b;
    const auto a = b.point(0, 0), c = b.point(3, 4);
    const auto l = b.line(a, c);
    const auto circ = b.circle(b.point(10, 0), 2);
    b.constrain(CT::Fix, {a});
    b.constrain(CT::Distance, {l}, 10);
    const auto drivenLen = b.constrain(CT::Distance, {l}, 1.0, true); // reference: 10 after the solve
    const auto drivenAng = b.constrain(CT::Angle, {l}, 0.0, true);    // reference: atan2(4, 3)
    const auto drivenDia = b.constrain(CT::Diameter, {circ}, 0.0, true);
    const SolveResult r = solvesTo(b.s);
    for (const Constraint& c2 : r.solved.constraints) {
        if (c2.id == drivenLen) {
            CHECK(std::fabs(c2.value - 10) <= 1e-9);
        }
        if (c2.id == drivenAng) {
            const Vec2 pa = pointAt(r.solved, a), pc = pointAt(r.solved, c);
            CHECK(std::fabs(c2.value - std::atan2(pc.y - pa.y, pc.x - pa.x)) <= 1e-12);
        }
        if (c2.id == drivenDia) {
            CHECK(c2.value == 4);
        }
    }
    // A driven non-dimension is rejected.
    Builder bad;
    const auto l2 = bad.line(bad.point(0, 0), bad.point(1, 1));
    bad.constrain(CT::Horizontal, {l2}, 0, true);
    JobContext ctx;
    auto res = makePlaneGcsSolver()->solve(bad.s, ctx);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Unsupported combinations and malformed sketches are invalid arguments", "[sketch][constraints]")
{
    auto expectInvalid = [](const Sketch& s, const char* what) {
        INFO(what);
        JobContext ctx;
        auto res = makePlaneGcsSolver()->solve(s, ctx);
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(res.error().message.empty());
        auto drag = makePlaneGcsSolver()->beginDrag(s, s.entities.front().id, ctx);
        CHECK_FALSE(drag.has_value());
    };
    {
        Builder b;
        const auto l = b.line(b.point(0, 0), b.point(1, 0));
        const auto c = b.circle(b.point(5, 5), 1);
        b.constrain(CT::Parallel, {l, c});
        expectInvalid(b.s, "parallel line-circle");
    }
    {
        Builder b;
        const auto k0 = b.point(0, 0), k1 = b.point(1, 1);
        const auto sp = b.add(Spline{{k0, k1}, 1, false});
        const auto l = b.line(b.point(0, 5), b.point(5, 5));
        b.constrain(CT::Tangent, {sp, l});
        expectInvalid(b.s, "tangent to a spline");
    }
    {
        Builder b;
        const auto l = b.line(b.point(0, 0), b.point(1, 0));
        b.constrain(CT::Radius, {l}, 3);
        expectInvalid(b.s, "radius of a line");
    }
    {
        Builder b;
        const auto p = b.point(0, 0);
        b.line(p, p);
        expectInvalid(b.s, "degenerate line");
    }
    {
        Builder b;
        b.point(0, 0);
        b.constrain(CT::Fix, {SketchEntId{orderedUuid(999, 0x10)}});
        expectInvalid(b.s, "missing reference");
    }
    {
        Builder b;
        b.point(0, 0);
        b.s.entities.push_back(b.s.entities.front());
        expectInvalid(b.s, "duplicate id");
    }
    {
        Builder b;
        const auto p = b.point(0, 0), q = b.point(1, 0);
        b.constrain(CT::Distance, {p, q}, 0);
        expectInvalid(b.s, "zero point distance");
    }
}

TEST_CASE("The order of the entity and constraint lists does not change the result", "[sketch][constraints]")
{
    const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
    const SolveResult a = solveIt(g.sketch);
    REQUIRE(a.status == SolveStatus::UnderConstrained);
    Sketch shuffled = g.sketch;
    std::mt19937 rng(7);
    std::shuffle(shuffled.entities.begin(), shuffled.entities.end(), rng);
    std::shuffle(shuffled.constraints.begin(), shuffled.constraints.end(), rng);
    const SolveResult b = solveIt(shuffled);
    REQUIRE(b.status == a.status);
    const auto ca = coordinates(a.solved), cb = coordinates(b.solved);
    CHECK(std::memcmp(ca.data(), cb.data(), ca.size() * sizeof(double)) == 0);
    // The solved sketch is canonical: sorted by id.
    CHECK(std::is_sorted(b.solved.entities.begin(), b.solved.entities.end(),
                         [](const Entity& x, const Entity& y) { return x.id < y.id; }));
}

TEST_CASE("Model helpers: canonical order, lookup, validation, measurement", "[sketch][model]")
{
    Builder b;
    const auto a = b.point(0, 0), c = b.point(3, 4);
    const auto l = b.line(a, c);
    const auto circ = b.circle(b.point(10, 0), 2);
    Sketch s = b.s;
    std::reverse(s.entities.begin(), s.entities.end());
    const Sketch canon = canonicalSketch(s);
    const EntityIndex index(canon);
    CHECK(index.indexOf(a) == 0);
    CHECK(index.find(l) != nullptr);
    CHECK(kindOf(*index.find(circ)) == EntityKind::Circle);
    CHECK(index.find(SketchEntId{orderedUuid(12345, 0x10)}) == nullptr);
    CHECK(validateSketch(s).has_value());
    Constraint len;
    len.type = CT::Distance;
    len.refs = {l};
    CHECK(measureConstraint(index, len).value() == 5);
    Constraint ang;
    ang.type = CT::Angle;
    ang.refs = {l};
    CHECK(std::fabs(measureConstraint(index, ang).value() - std::atan2(4.0, 3.0)) <= 1e-15);
    Constraint dia;
    dia.type = CT::Diameter;
    dia.refs = {circ};
    CHECK(measureConstraint(index, dia).value() == 4);
    Constraint par;
    par.type = CT::Parallel;
    par.refs = {l, l};
    CHECK_FALSE(measureConstraint(index, par).has_value());
}
