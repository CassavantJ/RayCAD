// inferConstraints (task T-091): coincident, horizontal, vertical, parallel, perpendicular and tangent within the
// tolerance, nothing outside it, nothing the sketch already has, nothing spurious on test drawings, and every
// proposal holds on the geometry it was proposed for.
#include <catch2/catch_test_macros.hpp>

#include "sketch_test_util.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;

namespace {

constexpr double kTol = 0.5; // mm

struct Want {
    ConstraintType type;
    std::vector<SketchEntId> refs;
};

std::vector<Want> sorted(std::vector<Want> w)
{
    for (Want& x : w) {
        std::sort(x.refs.begin(), x.refs.end());
    }
    std::sort(w.begin(), w.end(), [](const Want& a, const Want& b) {
        if (a.type != b.type) {
            return a.type < b.type;
        }
        return a.refs < b.refs;
    });
    return w;
}

// The proposals must be exactly `want` (any order, references in any order).
void expectExactly(const std::vector<Constraint>& got, std::vector<Want> want)
{
    std::vector<Want> g;
    for (const Constraint& c : got) {
        g.push_back({c.type, c.refs});
    }
    const auto a = sorted(g);
    const auto b = sorted(std::move(want));
    INFO("proposed " << a.size() << ", expected " << b.size());
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].type == b[i].type);
        CHECK(a[i].refs == b[i].refs);
    }
}

// Every proposal holds within the tolerance on the current geometry (the independent oracle of sketch_test_util.h),
// and is one the solver accepts: solving the sketch with all proposals added does not fail or conflict.
void checkHolds(const Sketch& s, const std::vector<Constraint>& got)
{
    for (const Constraint& c : got) {
        const auto err = constraintError(s, s, c);
        REQUIRE(err.has_value());
        CHECK(*err <= kTol);
    }
    Sketch with = s;
    with.constraints.insert(with.constraints.end(), got.begin(), got.end());
    auto solver = makePlaneGcsSolver();
    JobContext ctx;
    const SolveResult r = take(solver->solve(with, ctx));
    CHECK(r.status != SolveStatus::Conflicting);
    CHECK(r.status != SolveStatus::Failed);
    CHECK(r.redundant.empty());
}

} // namespace

TEST_CASE("infer: horizontal and vertical lines", "[infer]")
{
    Builder b;
    const SketchEntId h = b.line(b.point(0, 0), b.point(40, 0.3));
    const SketchEntId v = b.line(b.point(100, 0), b.point(99.6, 30));
    const SketchEntId slanted = b.line(b.point(200, 0), b.point(240, 1.2)); // 1.2 mm off: not horizontal
    expectExactly(inferConstraints(b.s, h, kTol), {{ConstraintType::Horizontal, {h}}});
    expectExactly(inferConstraints(b.s, v, kTol), {{ConstraintType::Vertical, {v}}});
    expectExactly(inferConstraints(b.s, slanted, kTol), {});
    checkHolds(b.s, inferConstraints(b.s, h, kTol));
    // Already constrained: nothing new.
    b.constrain(ConstraintType::Horizontal, {h});
    expectExactly(inferConstraints(b.s, h, kTol), {});
}

TEST_CASE("infer: coincident end points, one proposal per end, nearest first", "[infer]")
{
    Builder b;
    const SketchEntId a = b.point(0, 0);
    const SketchEntId c = b.point(10, 10);
    const SketchEntId near1 = b.point(10.3, 10.1);
    const SketchEntId near2 = b.point(10.24, 10.04); // the nearest to the line's end
    const SketchEntId far = b.point(50, 50);
    (void)far;
    const SketchEntId p0 = b.point(0.2, -0.1);
    const SketchEntId p1 = b.point(10.25, 10.05);
    const SketchEntId l = b.line(p0, p1);
    expectExactly(inferConstraints(b.s, l, kTol),
                  {{ConstraintType::Coincident, {p0, a}}, {ConstraintType::Coincident, {p1, near2}}});
    (void)c;
    (void)near1;

    SECTION("a short line with both ends near one point snaps only one end")
    {
        Builder s;
        const SketchEntId q = s.point(0, 0);
        const SketchEntId e0 = s.point(0.1, 0);
        const SketchEntId e1 = s.point(0.3, 0);
        const SketchEntId tiny = s.line(e0, e1);
        const auto got = inferConstraints(s.s, tiny, kTol);
        int coincident = 0;
        for (const Constraint& k : got) {
            coincident += k.type == ConstraintType::Coincident;
        }
        CHECK(coincident == 1);
        (void)q;
    }
}

TEST_CASE("infer: parallel and perpendicular to the best single line", "[infer]")
{
    Builder b;
    const double ang = 0.5; // rad: neither horizontal nor vertical
    const Vec2 u{std::cos(ang), std::sin(ang)};
    const Vec2 n{-u.y, u.x};
    const SketchEntId ref = b.line(b.point(0, 0), b.point(40 * u.x, 40 * u.y));
    const SketchEntId ref2 = b.line(b.point(5, 50), b.point(5 + 40 * u.x, 50 + 40 * u.y)); // parallel to ref
    // A new line 30 mm long, 0.3 mm off parallel at its far end.
    const Vec2 s0{100, 0};
    const Vec2 s1{100 + 30 * u.x + 0.3 * n.x, 30 * u.y + 0.3 * n.y};
    const SketchEntId par = b.line(b.point(s0.x, s0.y), b.point(s1.x, s1.y));
    const auto got = inferConstraints(b.s, par, kTol);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == ConstraintType::Parallel);
    CHECK((got[0].refs[1] == ref || got[0].refs[1] == ref2));

    const Vec2 t1{200 + 30 * n.x + 0.2 * u.x, 30 * n.y + 0.2 * u.y};
    const SketchEntId perp = b.line(b.point(200, 0), b.point(t1.x, t1.y));
    const auto gotPerp = inferConstraints(b.s, perp, kTol);
    REQUIRE(gotPerp.size() == 1);
    CHECK(gotPerp[0].type == ConstraintType::Perpendicular);

    const Vec2 w1{300 + 30 * u.x + 2 * n.x, 30 * u.y + 2 * n.y}; // 2 mm off: neither
    const SketchEntId neither = b.line(b.point(300, 0), b.point(w1.x, w1.y));
    expectExactly(inferConstraints(b.s, neither, kTol), {});
    checkHolds(b.s, got);
    checkHolds(b.s, gotPerp);
}

TEST_CASE("infer: tangent at a joint and where curves touch", "[infer]")
{
    SECTION("a line leaving an arc's end along its tangent")
    {
        Builder b;
        const double r = std::sqrt(0.5);
        const SketchEntId c = b.point(0, 0);
        const SketchEntId s = b.point(10, 0);
        const SketchEntId e = b.point(10 * r, 10 * r); // the arc ends at 45 degrees
        const SketchEntId arc = b.arc(c, s, e, true);
        // Shares the arc's end point and leaves along its tangent (-r, r), 0.2 mm off at the far end.
        const SketchEntId l = b.line(e, b.point(10 * r - 20 * r + 0.2 * r, 10 * r + 20 * r + 0.2 * r));
        expectExactly(inferConstraints(b.s, l, kTol), {{ConstraintType::Tangent, {l, arc, e}}});
        checkHolds(b.s, inferConstraints(b.s, l, kTol));
        // A corner instead: nothing.
        Builder k;
        const SketchEntId kc = k.point(0, 0), ks = k.point(10, 0), ke = k.point(0, 10);
        k.arc(kc, ks, ke, true);
        const SketchEntId corner = k.line(ke, k.point(-14, 24));
        expectExactly(inferConstraints(k.s, corner, kTol), {});
    }
    SECTION("an arc drawn on from a line end, joined within tolerance")
    {
        Builder b;
        const SketchEntId a0 = b.point(-20, 10);
        const SketchEntId a1 = b.point(0, 10);
        const SketchEntId l = b.line(a0, a1);
        const SketchEntId c = b.point(0, 0);
        const SketchEntId s = b.point(10, 0);
        const SketchEntId e = b.point(0.1, 10.0); // 0.1 mm from the line's end
        const SketchEntId arc = b.arc(c, s, e, true);
        const auto got = inferConstraints(b.s, arc, kTol);
        expectExactly(got, {{ConstraintType::Coincident, {e, a1}}, {ConstraintType::Tangent, {arc, l, e}}});
    }
    SECTION("a line touching a circle")
    {
        Builder b;
        const SketchEntId circle = b.circle(b.point(0, 0), 10);
        const SketchEntId l = b.line(b.point(-30, 10.3), b.point(30, 10.3));
        const auto got = inferConstraints(b.s, l, kTol);
        expectExactly(got, {{ConstraintType::Horizontal, {l}}, {ConstraintType::Tangent, {l, circle}}});
        checkHolds(b.s, got);
        Builder far;
        far.circle(far.point(0, 0), 10);
        const SketchEntId miss = far.line(far.point(-30, 11), far.point(30, 11.2));
        expectExactly(inferConstraints(far.s, miss, kTol), {{ConstraintType::Horizontal, {miss}}});
    }
    SECTION("two circles touching, outside and inside")
    {
        Builder b;
        const SketchEntId c1 = b.circle(b.point(0, 0), 10);
        const SketchEntId c2 = b.circle(b.point(15.2, 0), 5);
        expectExactly(inferConstraints(b.s, c2, kTol), {{ConstraintType::Tangent, {c2, c1}}});
        const SketchEntId c3 = b.circle(b.point(0, 6.8), 3); // inside c1, touching
        expectExactly(inferConstraints(b.s, c3, kTol), {{ConstraintType::Tangent, {c3, c1}}});
        checkHolds(b.s, inferConstraints(b.s, c2, kTol));
    }
}

TEST_CASE("infer: nothing spurious on test drawings", "[infer]")
{
    SECTION("a bracket drawn line by line with small hand errors")
    {
        // Each new line proposes exactly what the drawing intends.
        Builder b;
        const SketchEntId p0 = b.point(0, 0);
        const SketchEntId p1 = b.point(60.2, 0.3);
        const SketchEntId l0 = b.line(p0, p1);
        expectExactly(inferConstraints(b.s, l0, kTol), {{ConstraintType::Horizontal, {l0}}});

        const SketchEntId q1 = b.point(60.3, 0.2);
        const SketchEntId q2 = b.point(60.1, 20);
        const SketchEntId l1 = b.line(q1, q2);
        expectExactly(inferConstraints(b.s, l1, kTol),
                      {{ConstraintType::Coincident, {q1, p1}}, {ConstraintType::Vertical, {l1}}});

        const SketchEntId r2 = b.point(60, 20.1);
        const SketchEntId r3 = b.point(20, 20.3);
        const SketchEntId l2 = b.line(r2, r3);
        expectExactly(inferConstraints(b.s, l2, kTol),
                      {{ConstraintType::Coincident, {r2, q2}}, {ConstraintType::Horizontal, {l2}}});

        // A 45 degree chamfer-like line: no axis, not parallel or perpendicular to anything.
        const SketchEntId s3 = b.point(20.1, 20.2);
        const SketchEntId s4 = b.point(0.2, 40);
        const SketchEntId l3 = b.line(s3, s4);
        expectExactly(inferConstraints(b.s, l3, kTol), {{ConstraintType::Coincident, {s3, r3}}});

        // Closing line: both ends snap, vertical.
        const SketchEntId t4 = b.point(0.1, 40.1);
        const SketchEntId t0 = b.point(-0.1, 0.1);
        const SketchEntId l4 = b.line(t4, t0);
        expectExactly(inferConstraints(b.s, l4, kTol), {{ConstraintType::Coincident, {t4, s4}},
                                                        {ConstraintType::Coincident, {t0, p0}},
                                                        {ConstraintType::Vertical, {l4}}});
    }
    SECTION("random lines and circles far apart propose nothing")
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> angle(0.2, std::numbers::pi / 2 - 0.2);
        std::uniform_real_distribution<double> len(10, 50);
        std::uniform_real_distribution<double> rad(2, 8);
        int proposals = 0;
        for (int trial = 0; trial < 200; ++trial) {
            Builder b;
            std::vector<double> used;
            for (int i = 0; i < 6; ++i) {
                // Directions at least 0.05 rad apart from each other and from the axes' neighbourhood.
                double a = 0;
                do {
                    a = angle(rng);
                } while (std::any_of(used.begin(), used.end(), [&](double u) {
                    return std::fabs(u - a) < 0.05 || std::fabs(std::fabs(u - a) - std::numbers::pi / 2) < 0.05;
                }));
                used.push_back(a);
                const double x = 200.0 * i;
                const double l = len(rng);
                b.line(b.point(x, 0), b.point(x + l * std::cos(a), l * std::sin(a)));
                b.circle(b.point(x + 100, 300), rad(rng));
            }
            for (const Entity& e : b.s.entities) {
                if (!std::holds_alternative<Point>(e.geom)) {
                    proposals += static_cast<int>(inferConstraints(b.s, e.id, kTol).size());
                }
            }
        }
        CHECK(proposals == 0);
    }
    SECTION("a zero or invalid tolerance and a missing entity give nothing")
    {
        Builder b;
        const SketchEntId l = b.line(b.point(0, 0), b.point(10, 0));
        CHECK(inferConstraints(b.s, l, 0).empty());
        CHECK(inferConstraints(b.s, l, -1).empty());
        CHECK(inferConstraints(b.s, l, std::nan("")).empty());
        CHECK(inferConstraints(b.s, SketchEntId{orderedUuid(999, 1)}, kTol).empty());
    }
}

TEST_CASE("infer: proposals are deterministic and have distinct ids", "[infer]")
{
    Builder b;
    const SketchEntId a = b.point(0, 0);
    (void)a;
    const SketchEntId l = b.line(b.point(0.1, 0), b.point(30, 0.2));
    const auto x = inferConstraints(b.s, l, kTol);
    const auto y = inferConstraints(b.s, l, kTol);
    REQUIRE(x.size() == 2);
    REQUIRE(y.size() == 2);
    CHECK(x[0].id == y[0].id);
    CHECK(x[1].id == y[1].id);
    CHECK(x[0].id != x[1].id);
}
