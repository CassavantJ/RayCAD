// Property test (task T-090): 10,000 random well-constrained sketches (fixed seed) solve, and every constraint holds to
// 1e-9 by the independent geometric check. Each sketch is one to three motifs, each built from a random true geometry
// with constraints measured from it (so they are consistent and independent), then started from coordinates moved by
// up to 0.2 mm. Runs in its own CTest entry.
#include "sketch_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using CT = ConstraintType;

namespace {

constexpr double kPi = std::numbers::pi;

struct Gen {
    Builder b;
    Lcg rng;
    double noise = 0.2;
    Vec2 origin{};

    SketchEntId pt(Vec2 p) { return b.point(p.x + rng.sym(noise), p.y + rng.sym(noise)); }
    SketchEntId circ(SketchEntId c, double r) { return b.circle(c, r * (1 + rng.sym(0.02))); }
    Vec2 at(double x, double y) const { return {origin.x + x, origin.y + y}; }
    static Vec2 add(Vec2 a, Vec2 d, double s = 1) { return {a.x + d.x * s, a.y + d.y * s}; }
    static Vec2 dir(double a) { return {std::cos(a), std::sin(a)}; }
    static double dist(Vec2 a, Vec2 c) { return std::hypot(c.x - a.x, c.y - a.y); }
    static double angleOf(Vec2 a, Vec2 c) { return std::atan2(c.y - a.y, c.x - a.x); }

    // A rectangle at a random angle: fix, angle, two lengths, parallel, perpendicular or equal.
    void rectangle()
    {
        const bool axis = rng.next() < 0.3;
        const double th = axis ? 0 : rng.range(-1.0, 1.0), w = rng.range(5, 40), h = rng.range(5, 40);
        const Vec2 A = at(rng.range(0, 20), rng.range(0, 20)), u = dir(th), v = dir(th + kPi / 2);
        const Vec2 B = add(A, u, w), C = add(B, v, h), D = add(A, v, h);
        const auto a = pt(A), bb = pt(B), c = pt(C), d = pt(D);
        const auto ab = b.line(a, bb), bc = b.line(bb, c), cd = b.line(c, d), da = b.line(d, a);
        b.constrain(CT::Fix, {a});
        b.constrain(CT::Distance, {ab}, w);
        b.constrain(CT::Distance, {bc}, h);
        if (axis) {
            b.constrain(CT::Horizontal, {ab});
            b.constrain(CT::Vertical, {bc});
            b.constrain(CT::Horizontal, {cd});
            b.constrain(CT::Vertical, {da});
        } else {
            b.constrain(CT::Angle, {ab}, th);
            b.constrain(CT::Perpendicular, {bc, ab});
            b.constrain(CT::Parallel, {cd, ab});
            if (rng.next() < 0.5) {
                b.constrain(CT::Perpendicular, {da, ab});
            } else {
                b.constrain(CT::Equal, {cd, ab});
            }
        }
    }

    // A triangle with all angles over 25 degrees: fix, one absolute angle, three lengths or two lengths and an angle.
    void triangle()
    {
        const double a1 = rng.range(25, 80) * kPi / 180, a2 = rng.range(25, 155 - a1 * 180 / kPi) * kPi / 180;
        const double base = rng.range(8, 40), th = rng.range(-kPi, kPi);
        const Vec2 A = at(rng.range(0, 20), rng.range(0, 20)), B = add(A, dir(th), base);
        // apex from the two base angles (law of sines)
        const double a3 = kPi - a1 - a2, sideAC = base * std::sin(a2) / std::sin(a3);
        const Vec2 C = add(A, dir(th + a1), sideAC);
        const auto a = pt(A), bb = pt(B), c = pt(C);
        const auto ab = b.line(a, bb), bc = b.line(bb, c);
        b.constrain(CT::Fix, {a});
        b.constrain(CT::Angle, {ab}, th);
        b.constrain(CT::Distance, {ab}, base);
        if (rng.next() < 0.5) {
            const auto ca = b.line(c, a);
            b.constrain(CT::Distance, {bc}, dist(B, C));
            b.constrain(CT::Distance, {ca}, sideAC);
        } else {
            const auto ac = b.line(a, c);
            b.constrain(CT::Angle, {ab, ac}, a1);
            b.constrain(CT::Distance, {a, c}, sideAC);
        }
    }

    // Circles: one tangent to a fixed line, a second externally tangent to the first.
    void tangentCircles()
    {
        const Vec2 L0 = at(0, rng.range(0, 10));
        const double slope = rng.range(-0.5, 0.5), len = 60;
        const Vec2 ldir = dir(slope), nrm = dir(slope + kPi / 2);
        const Vec2 L1 = add(L0, ldir, len);
        const auto l0 = b.point(L0.x, L0.y), l1 = b.point(L1.x, L1.y);
        const auto line = b.line(l0, l1);
        b.constrain(CT::Fix, {line});
        const double r1 = rng.range(3, 12), s = rng.range(15, 45);
        const Vec2 C1 = add(add(L0, ldir, s), nrm, r1);
        const auto c1 = circ(pt(C1), r1);
        const auto c1c = b.s.entities[b.s.entities.size() - 2].id;
        b.constrain(CT::Radius, {c1}, r1);
        b.constrain(CT::Tangent, {line, c1});
        b.constrain(CT::DistanceX, {c1c}, C1.x);
        const double r2 = rng.range(2, 10), phi = rng.range(-0.8, 0.8);
        const Vec2 C2 = add(C1, dir(phi), r1 + r2);
        const auto c2 = circ(pt(C2), r2);
        const auto c2c = b.s.entities[b.s.entities.size() - 2].id;
        b.constrain(CT::Diameter, {c2}, 2 * r2);
        b.constrain(CT::Tangent, {c1, c2});
        b.constrain(CT::DistanceY, {c2c}, C2.y);
    }

    // A line, a tangent arc (either turning direction) and a tangent line, all lengths and angles given.
    void jShape()
    {
        const Vec2 P0 = at(rng.range(0, 20), rng.range(0, 20));
        const double th = rng.range(-kPi, kPi), len1 = rng.range(5, 30), r = rng.range(3, 15);
        const double sweep = rng.range(0.5, 2.5), len2 = rng.range(5, 30);
        const bool left = rng.next() < 0.5;
        const Vec2 P1 = add(P0, dir(th), len1);
        const Vec2 C = add(P1, dir(th + (left ? kPi / 2 : -kPi / 2)), r);
        const double a0 = th + (left ? -kPi / 2 : kPi / 2), a1 = left ? a0 + sweep : a0 - sweep;
        const Vec2 P2 = add(C, dir(a1), r);
        const double th2 = left ? th + sweep : th - sweep;
        const Vec2 P3 = add(P2, dir(th2), len2);
        const auto p0 = pt(P0), p1 = pt(P1), c = pt(C), p2 = pt(P2), p3 = pt(P3);
        const auto l1 = b.line(p0, p1), l2 = b.line(p2, p3);
        const auto arc = b.arc(c, p1, p2, left);
        b.constrain(CT::Fix, {p0});
        b.constrain(CT::Angle, {l1}, th);
        b.constrain(CT::Distance, {l1}, len1);
        b.constrain(CT::Tangent, {l1, arc, p1});
        b.constrain(CT::Radius, {arc}, r);
        b.constrain(CT::Tangent, {arc, l2, p2});
        b.constrain(CT::Angle, {l2}, std::remainder(th2, 2 * kPi));
        b.constrain(CT::Distance, {l2}, len2);
    }

    // Mirror images about a fixed axis and about a point, and a midpoint.
    void symmetry()
    {
        const Vec2 A0 = at(rng.range(0, 10), -20), A1 = add(A0, dir(kPi / 2 + rng.range(-0.4, 0.4)), 40);
        const auto a0 = b.point(A0.x, A0.y), a1 = b.point(A1.x, A1.y);
        const auto axis = b.line(a0, a1);
        b.constrain(CT::Fix, {axis});
        const Vec2 P = at(rng.range(-25, -8), rng.range(-15, 15));
        // mirror of P about the axis
        const Vec2 d{A1.x - A0.x, A1.y - A0.y};
        const double t = ((P.x - A0.x) * d.x + (P.y - A0.y) * d.y) / (d.x * d.x + d.y * d.y);
        const Vec2 F{A0.x + t * d.x, A0.y + t * d.y};
        const Vec2 Q{2 * F.x - P.x, 2 * F.y - P.y};
        const auto p = pt(P), q = pt(Q);
        b.constrain(CT::DistanceX, {p}, P.x);
        b.constrain(CT::DistanceY, {p}, P.y);
        b.constrain(CT::Symmetric, {p, q, axis});
        const Vec2 R = at(rng.range(20, 40), rng.range(-20, 20));
        const Vec2 M{(Q.x + R.x) / 2, (Q.y + R.y) / 2};
        const auto rr = pt(R), m = pt(M);
        const auto qr = b.line(q, rr);
        b.constrain(CT::Fix, {rr});
        b.constrain(CT::Midpoint, {m, qr});
        const Vec2 S = add(M, dir(rng.range(-kPi, kPi)), rng.range(3, 15));
        const Vec2 T{2 * M.x - S.x, 2 * M.y - S.y};
        const auto s = pt(S), tt = pt(T);
        b.constrain(CT::Fix, {s});
        b.constrain(CT::Symmetric, {s, tt, m});
    }

    // Circles and points tied by concentric, equal, point-on-curve and every distance variant.
    void circleFamily()
    {
        const Vec2 C1 = at(rng.range(0, 10), rng.range(0, 10));
        const double r1 = rng.range(5, 15), r2 = r1 + rng.range(2, 8);
        const auto c1c = pt(C1);
        const auto k1 = circ(c1c, r1);
        b.constrain(CT::Fix, {c1c});
        b.constrain(CT::Radius, {k1}, r1);
        const auto k2 = circ(pt(C1), r2);
        b.constrain(CT::Concentric, {k1, k2});
        b.constrain(CT::Diameter, {k2}, 2 * r2);
        const double phi = rng.range(-1.0, 1.0);
        const Vec2 W = add(C1, dir(phi), r1);
        const auto w = pt(W);
        b.constrain(CT::PointOnCurve, {w, k1});
        b.constrain(CT::DistanceY, {c1c, w}, W.y - C1.y);
        const Vec2 LL0 = at(-30, -40), LL1 = at(60, -40 + rng.range(-10, 10));
        const auto ll0 = b.point(LL0.x, LL0.y), ll1 = b.point(LL1.x, LL1.y);
        const auto ll = b.line(ll0, ll1);
        b.constrain(CT::Fix, {ll});
        const double vx = rng.range(-20, 50);
        const Vec2 V{vx, LL0.y + (vx - LL0.x) * (LL1.y - LL0.y) / (LL1.x - LL0.x)};
        const auto v = pt(V);
        b.constrain(CT::PointOnCurve, {ll, v});
        b.constrain(CT::DistanceX, {w, v}, V.x - W.x);
        const auto k3c = b.point(C1.x + 40, C1.y);
        const auto k3 = circ(k3c, r1);
        b.constrain(CT::Fix, {k3c});
        b.constrain(CT::Equal, {k3, k1});
        const double psi = (rng.next() < 0.5 ? 1 : -1) * rng.range(0.6, 2.5), gap = rng.range(1, 8);
        const Vec2 Z = add(C1, dir(psi), r2 + gap);
        const auto z = pt(Z);
        b.constrain(CT::Distance, {z, k2}, gap);
        b.constrain(CT::DistanceX, {z}, Z.x);
        const double r4 = rng.range(2, 6), x4 = C1.x + rng.range(-20, 20), g4 = rng.range(1, 10);
        const double yLine = LL0.y + (x4 - LL0.x) * (LL1.y - LL0.y) / (LL1.x - LL0.x);
        // The centre straight above the line at x4, at normal distance r4 + g4: a vertical offset of (r4 + g4) / cos
        // of the line's angle.
        const double cosLine = std::cos(std::atan2(LL1.y - LL0.y, LL1.x - LL0.x));
        const Vec2 C4{x4, yLine + (r4 + g4) / cosLine};
        const auto k4c = pt(C4);
        const auto k4 = circ(k4c, r4);
        b.constrain(CT::Radius, {k4}, r4);
        b.constrain(CT::DistanceX, {k4c}, x4);
        b.constrain(CT::Distance, {k4, ll}, g4);
        const double r5 = rng.range(2, 6), g5 = rng.range(1, 8);
        const double om = (rng.next() < 0.5 ? 0 : kPi) + rng.range(-1.0, 1.0);
        const Vec2 C5 = add(C1, dir(om), r1 + r5 + g5);
        const auto k5c = pt(C5);
        const auto k5 = circ(k5c, r5);
        b.constrain(CT::Radius, {k5}, r5);
        b.constrain(CT::DistanceY, {k5c}, C5.y);
        b.constrain(CT::Distance, {k5, k1}, g5);
    }

    // A rhombus by an absolute angle, a line-to-line angle, equal sides and two parallels; and a coincident chain.
    void rhombusAndChain()
    {
        const Vec2 A = at(rng.range(0, 20), rng.range(0, 20));
        const double th = rng.range(-kPi, kPi), beta = rng.range(0.5, 2.5), side = rng.range(8, 30);
        const Vec2 B = add(A, dir(th), side), D = add(A, dir(th + beta), side), C = add(B, dir(th + beta), side);
        const auto a = pt(A), bb = pt(B), c = pt(C), d = pt(D);
        const auto ab = b.line(a, bb), ad = b.line(a, d), dc = b.line(d, c), bc = b.line(bb, c);
        b.constrain(CT::Fix, {a});
        b.constrain(CT::Angle, {ab}, th);
        b.constrain(CT::Distance, {ab}, side);
        b.constrain(CT::Angle, {ab, ad}, beta);
        b.constrain(CT::Equal, {ad, ab});
        b.constrain(CT::Parallel, {dc, ab});
        b.constrain(CT::Parallel, {bc, ad});
        const Vec2 E = at(rng.range(30, 50), rng.range(0, 20));
        const double dx = rng.range(5, 20) * (rng.next() < 0.5 ? 1 : -1), dy = rng.range(5, 20);
        const Vec2 F{E.x + dx, E.y}, G{F.x, F.y + dy};
        const auto e = pt(E), f = pt(F), g = pt(G), h = pt(G);
        b.constrain(CT::Fix, {e});
        b.constrain(CT::Horizontal, {e, f});
        b.constrain(CT::DistanceX, {e, f}, dx);
        b.constrain(CT::Vertical, {f, g});
        b.constrain(CT::DistanceY, {f, g}, dy);
        b.constrain(CT::Coincident, {h, g});
    }
};

} // namespace

TEST_CASE("10,000 random well-constrained sketches satisfy every constraint to 1e-9", "[sketch][property]")
{
    constexpr int kSketches = 10000;
    auto solver = makePlaneGcsSolver();
    int fully = 0, under = 0, failures = 0;
    double worst = 0;
    int motifCount[8] = {};
    const auto t0 = Clock::now();
    for (int i = 0; i < kSketches; ++i) {
        Gen g;
        g.rng = Lcg{0x5EEDull * 1000003ull + static_cast<std::uint64_t>(i)};
        const int motifs = 1 + g.rng.below(3);
        for (int k = 0; k < motifs; ++k) {
            g.origin = {k * 250.0, 0};
            const int m = g.rng.below(7);
            ++motifCount[m];
            switch (m) {
            case 0:
                g.rectangle();
                break;
            case 1:
                g.triangle();
                break;
            case 2:
                g.tangentCircles();
                break;
            case 3:
                g.jShape();
                break;
            case 4:
                g.symmetry();
                break;
            case 5:
                g.circleFamily();
                break;
            default:
                g.rhombusAndChain();
                break;
            }
        }
        JobContext ctx;
        const SolveResult r = take(solver->solve(g.b.s, ctx));
        const bool ok = r.status == SolveStatus::FullyConstrained || r.status == SolveStatus::UnderConstrained;
        const double err = ok ? maxError(r.solved, g.b.s) : 1e300;
        if (!ok || err > 1e-9 || !r.redundant.empty() || !r.partiallyRedundant.empty()) {
            ++failures;
            if (failures <= 10) {
                std::printf("property: sketch %d (%d motifs) status %d, dof %d, redundant %zu, partial %zu, max error "
                            "%.3g\n",
                            i, motifs, static_cast<int>(r.status), r.freeDof, r.redundant.size(),
                            r.partiallyRedundant.size(), err);
            }
        } else {
            worst = std::max(worst, err);
            (r.status == SolveStatus::FullyConstrained ? fully : under)++;
        }
    }
    std::printf("property: %d sketches in %.1f s; %d fully constrained, %d under-constrained, %d failures; worst error "
                "%.3g; motifs:",
                kSketches, msSince(t0) / 1000, fully, under, failures, worst);
    for (int c : motifCount) {
        std::printf(" %d", c);
    }
    std::printf("\n");
    CHECK(failures == 0);
    CHECK(worst <= 1e-9);
}
