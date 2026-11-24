// Tool shapes (task T-070): makeToolShape against the toolpath invariant checker's tool model, the convex pieces of
// any profile, and the closed forms the stock model uses against brute force.
#include "tool_profile.h"
#include "tridexel_impl.h"
#include "tridexel_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <random>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// The radius of a profile polyline at height h (the largest where h is on a level step or a breakpoint); -1 outside.
double radiusOf(const std::vector<Vec2>& p, double h)
{
    double r = -1;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const Vec2 a = p[i], b = p[i + 1];
        if (h < std::min(a.y, b.y) || h > std::max(a.y, b.y)) {
            continue;
        }
        if (a.y == b.y) {
            r = std::max({r, a.x, b.x});
        } else {
            r = std::max(r, a.x + (b.x - a.x) * (h - a.y) / (b.y - a.y));
        }
    }
    return r;
}

lib::Holder holder()
{
    lib::Holder h;
    h.gaugeLength = 60;
    h.segments = {{20, 63, 63}, {15, 40, 50}, {10, 25, 30}}; // from the gauge line down
    return h;
}

std::vector<lib::Tool> library()
{
    std::vector<lib::Tool> tools;
    tools.push_back(tool(lib::ToolType::FlatEndMill, 12.0, 30.0));
    tools.push_back(tool(lib::ToolType::BallEndMill, 6.0, 18.0));
    lib::Tool bullT = tool(lib::ToolType::BullEndMill, 10.0, 25.0);
    bullT.cornerRadius = 1.5;
    tools.push_back(bullT);
    lib::Tool ch = tool(lib::ToolType::ChamferMill, 12.0, 10.0);
    ch.tipAngleDeg = 90;
    ch.tipDiameter = 1.0;
    ch.shankDiameter = 10.0;
    tools.push_back(ch);
    lib::Tool dr = tool(lib::ToolType::Drill, 8.0, 40.0);
    dr.tipAngleDeg = 118;
    tools.push_back(dr);
    lib::Tool spot = tool(lib::ToolType::SpotDrill, 10.0, 15.0);
    spot.tipAngleDeg = 90;
    tools.push_back(spot);
    lib::Tool neck = tool(lib::ToolType::FlatEndMill, 6.0, 8.0);
    neck.neckDiameter = 5.6;
    neck.neckLength = 30;
    neck.shoulderLength = 35;
    neck.shankDiameter = 6.0;
    tools.push_back(neck);
    lib::Tool longBall = tool(lib::ToolType::BallEndMill, 4.0, 6.0);
    longBall.neckDiameter = 3.8;
    longBall.shankDiameter = 6.0;
    longBall.shoulderLength = 25;
    tools.push_back(longBall);
    lib::Tool tm = tool(lib::ToolType::ThreadMill, 8.0, 12.0);
    tm.threadPitch = 1.25;
    tm.shankDiameter = 10.0;
    tm.shoulderLength = 20;
    tools.push_back(tm);
    lib::Tool small = tool(lib::ToolType::FlatEndMill, 3.0, 9.0);
    small.shankDiameter = 6.0;
    small.shoulderLength = 15;
    tools.push_back(small);
    lib::Tool eng = tool(lib::ToolType::EngraveCutter, 3.175, 5.0);
    eng.tipAngleDeg = 60;
    eng.tipDiameter = 0.2;
    tools.push_back(eng);
    return tools;
}

} // namespace

TEST_CASE("makeToolShape agrees with the invariant checker's tool model", "[sim][toolshape]")
{
    const lib::Holder h = holder();
    for (const lib::Tool& t : library()) {
        for (const lib::Holder* hp : {static_cast<const lib::Holder*>(nullptr), &h}) {
            for (double stick : {0.0, 45.0}) {
                const ToolShape s = makeToolShape(t, hp, stick);
                const cam::invariants::ToolProfile ref = cam::invariants::makeProfile(t, hp, stick);
                INFO("tool type " << static_cast<int>(t.type) << " diameter " << t.diameter << ", holder "
                                  << (hp != nullptr) << ", stick-out " << stick);
                REQUIRE(s.cuttingProfile.size() >= 2);
                CHECK(s.cuttingProfile.front().y == 0.0);
                // Heights never go down; the three profiles follow each other.
                for (const std::vector<Vec2>* p : {&s.cuttingProfile, &s.shankProfile, &s.holderProfile}) {
                    for (std::size_t i = 0; i + 1 < p->size(); ++i) {
                        CHECK((*p)[i + 1].y >= (*p)[i].y);
                    }
                }
                CHECK(s.shankProfile.front().y == s.cuttingProfile.back().y);
                CHECK(s.holderProfile.empty() == (hp == nullptr));
                if (hp != nullptr) {
                    CHECK(s.holderProfile.front().y == s.shankProfile.back().y);
                }
                // Zone by zone, the radius at heights between breakpoints matches the checker to the chord tolerance.
                const auto compare = [&](const std::vector<Vec2>& p, cam::invariants::Zone zone) {
                    if (p.size() < 2) {
                        return;
                    }
                    const double lo = p.front().y, hi = p.back().y;
                    CHECK(std::abs(ref.zoneBottom(zone) - lo) < 1e-9);
                    for (int i = 1; i < 400; ++i) {
                        const double hh = lo + (hi - lo) * (i + 0.37) / 400.0;
                        const double mine = radiusOf(p, hh);
                        const double theirs = ref.radiusAt(hh);
                        INFO("zone " << static_cast<int>(zone) << " height " << hh);
                        CHECK(mine <= theirs + 1e-9);
                        // Chords lie inside the arc within the tolerance measured across it, which along a level
                        // line near the bottom of a ball is much more: within the tolerance of some arc point.
                        const double tol = sim::detail::kArcChordTolMm;
                        CHECK(mine >= std::min(ref.radiusAt(hh - tol), ref.radiusAt(hh + tol)) - tol - 1e-9);
                    }
                };
                compare(s.cuttingProfile, cam::invariants::Zone::Cutting);
                compare(s.shankProfile, cam::invariants::Zone::Body);
                compare(s.holderProfile, cam::invariants::Zone::Holder);
            }
        }
    }
    // A tool without a diameter has no shape.
    lib::Tool none;
    CHECK(makeToolShape(none, nullptr, 0).cuttingProfile.empty());
}

TEST_CASE("Ball and corner chords lie on the arc within the chord tolerance", "[sim][toolshape]")
{
    for (double rc : {0.2, 1.5, 3.175, 12.5}) {
        lib::Tool t = tool(lib::ToolType::BullEndMill, 2 * std::max(rc, 1.0) + 2, 40);
        t.cornerRadius = rc;
        const ToolShape s = makeToolShape(t, nullptr, 0);
        const double R = t.diameter / 2, cr = R - rc;
        int chords = 0;
        for (std::size_t i = 0; i + 1 < s.cuttingProfile.size(); ++i) {
            const Vec2 a = s.cuttingProfile[i], b = s.cuttingProfile[i + 1];
            if (a.y >= rc || b.y > rc + 1e-12 || a.y == b.y) {
                continue;
            }
            ++chords;
            const auto dist = [&](Vec2 q) {
                return std::hypot(q.x - cr, q.y - rc);
            };
            CHECK(std::abs(dist(a) - rc) < 1e-9);
            CHECK(std::abs(dist(b) - rc) < 1e-9);
            const Vec2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
            CHECK(rc - dist(mid) <= sim::detail::kArcChordTolMm + 1e-12);
        }
        CHECK(chords >= sim::detail::kArcMinChords);
    }
}

TEST_CASE("Profiles become convex pieces whose union is the shape", "[sim][toolshape]")
{
    using sim::detail::compileProfile;
    CHECK(compileProfile(flat(10).cuttingProfile).pieces.size() == 1);
    CHECK(compileProfile(ball(6).cuttingProfile).pieces.size() == 1);
    CHECK(compileProfile(bull(10, 2).cuttingProfile).pieces.size() == 1);
    CHECK(compileProfile(pointed(lib::ToolType::Drill, 8, 118, 0).cuttingProfile).pieces.size() == 1);
    // A T-slot cutter: the head, then the neck.
    const auto tslot = compileProfile({{0, 0}, {6, 0}, {6, 2}, {2, 2}, {2, 12}});
    REQUIRE(tslot.pieces.size() == 2);
    CHECK(tslot.pieces[0].rmax == 6);
    CHECK(tslot.pieces[1].rmax == 2);
    CHECK(tslot.rmax == 6);
    // A dovetail (radius falling with height) is convex: one piece.
    CHECK(compileProfile({{0, 0}, {6, 0}, {3, 4}, {3, 10}}).pieces.size() == 2); // the cone, then the neck above it
    CHECK(compileProfile({{0, 0}, {6, 0}, {3, 4}}).pieces.size() == 1);
    // Teeth: one piece per tooth.
    std::vector<Vec2> teeth = {{0, 0}, {3, 0}};
    for (int i = 0; i < 5; ++i) {
        teeth.push_back({4, i * 1.0 + 0.5});
        teeth.push_back({3, i * 1.0 + 1.0});
    }
    CHECK(compileProfile(teeth).pieces.size() == 5);
    // The radius of the union matches the profile.
    const auto shape = compileProfile(teeth);
    for (double h = 0.01; h < 5; h += 0.07) {
        double r = 0;
        for (const auto& p : shape.pieces) {
            if (h >= p.h0 && h <= p.h1) {
                r = std::max(r, p.radiusAt(h));
            }
        }
        CHECK(std::abs(r - radiusOf(teeth, h)) < 1e-12);
    }
    // Endless: the top continues upward.
    const auto endless = compileProfile(flat(10).shankProfile, true);
    CHECK(endless.hmax >= sim::detail::kInfHeight);
    CHECK(compileProfile({}).empty());
    CHECK(compileProfile({{0, 0}, {0, 5}}).empty());
}

TEST_CASE("The swept-envelope minimum and the disc hulls match brute force", "[sim][toolshape]")
{
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    // Random concave profiles (radius rising with falling slope, then flat): their lower envelopes.
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<Vec2> prof = {{0, 0}};
        double r = u(rng) < 0.3 ? 0.0 : u(rng) * 2.0, h = 0, slope = 0.2 + u(rng) * 5.0;
        prof.push_back({r, 0});
        const int n = 1 + static_cast<int>(u(rng) * 8);
        for (int i = 0; i < n; ++i) {
            const double dh = 0.1 + u(rng);
            h += dh;
            r += dh * slope;
            slope *= 0.3 + 0.6 * u(rng);
            prof.push_back({r, h});
        }
        prof.push_back({r, h + 5});
        const auto shape = sim::detail::compileProfile(prof);
        REQUIRE(shape.pieces.size() == 1);
        const sim::detail::ConvexPiece& pc = shape.pieces[0];
        for (int q = 0; q < 20; ++q) {
            const double m = (u(rng) - 0.5) * 4.0;
            const double e = u(rng) * pc.rmax;
            const double len = u(rng) * 10.0;
            const double p0 = (u(rng) - 0.3) * len * 1.5;
            const double half = std::sqrt(pc.rmax * pc.rmax - e * e);
            const double pa = std::max(0.0, p0 - half), pb = std::min(len, p0 + half);
            if (pa > pb) {
                continue;
            }
            std::vector<double> c;
            sim::detail::prepareSlopes(pc.low, m, c);
            const double got = sim::detail::minimiseAlong(pc.low, c.data(), m, p0, e, pa, pb);
            double best = std::numeric_limits<double>::infinity();
            for (int i = 0; i <= 20000; ++i) {
                const double p = pa + (pb - pa) * i / 20000.0;
                const double d = std::min(std::sqrt(e * e + (p - p0) * (p - p0)), pc.rmax);
                best = std::min(best, m * p + pc.low.eval(d));
            }
            INFO("trial " << trial << " m " << m << " e " << e << " p0 " << p0 << " [" << pa << ", " << pb << "]");
            // The sampled minimum is above the true one by at most the function's slope times half a step.
            const double lip = std::abs(m) + *std::max_element(pc.low.k.begin(), pc.low.k.end());
            CHECK(got <= best + 1e-9);
            CHECK(got >= best - lip * (pb - pa) / 20000.0 - 1e-9);
        }
    }
    // Disc hulls: the interval a line crosses against sampling the swept discs.
    for (int trial = 0; trial < 2000; ++trial) {
        sim::detail::DiscHull hull{u(rng) * 10, u(rng) * 10, u(rng) * 3, u(rng) * 10, u(rng) * 10, u(rng) * 3,
                                   0,           0,           false,      {},          {}};
        hull.build();
        const double y = hull.bmin + (hull.bmax - hull.bmin) * u(rng);
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        const bool hit = hull.cross(y, lo, hi);
        double blo = lo, bhi = hi;
        bool bhit = false;
        blo = std::numeric_limits<double>::infinity();
        bhi = -blo;
        for (int i = 0; i <= 4000; ++i) {
            const double t = i / 4000.0;
            const double a = hull.a1 + (hull.a2 - hull.a1) * t, b = hull.b1 + (hull.b2 - hull.b1) * t;
            const double r = hull.r1 + (hull.r2 - hull.r1) * t;
            const double q = r * r - (y - b) * (y - b);
            if (q >= 0) {
                bhit = true;
                blo = std::min(blo, a - std::sqrt(q));
                bhi = std::max(bhi, a + std::sqrt(q));
            }
        }
        if (bhit) {
            REQUIRE(hit);
            CHECK(lo <= blo + 1e-9);
            CHECK(hi >= bhi - 1e-9);
            CHECK(lo >= blo - 2e-3);
            CHECK(hi <= bhi + 2e-3);
        }
    }
}

TEST_CASE("Z-ray intervals of swept pieces match brute force", "[sim][toolshape]")
{
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const ToolShape shapes[] = {flat(6), ball(6), bull(8, 1.5), pointed(lib::ToolType::ChamferMill, 8, 90, 1),
                                pointed(lib::ToolType::Drill, 6, 118, 0)};
    for (const ToolShape& ts : shapes) {
        const auto shape = sim::detail::compileProfile(ts.cuttingProfile);
        REQUIRE(shape.pieces.size() == 1);
        const sim::detail::ConvexPiece& pc = shape.pieces[0];
        for (int trial = 0; trial < 300; ++trial) {
            const double ax = u(rng) * 4, ay = u(rng) * 4, az = u(rng) * 4;
            const double bx = u(rng) < 0.2 ? ax : u(rng) * 4, by = u(rng) < 0.2 ? ay : u(rng) * 4;
            const double bz = u(rng) < 0.4 ? az : u(rng) * 4;
            const sim::detail::Seg g = sim::detail::makeSeg(ax, ay, az, bx, by, bz, 0);
            sim::detail::ShapeTables layout;
            layout.layout(shape);
            std::vector<double> tables(layout.perSeg);
            layout.fill(shape, g.m, tables.data());
            const double x = u(rng) * 8 - 2, y = u(rng) * 8 - 2;
            double lo = 0, hi = 0;
            const bool hit = sim::detail::zInterval(pc, g, tables.data() + layout.lowOff[0],
                                                    tables.data() + layout.highOff[0], x, y, lo, hi);
            // Brute force: sample the move; at each position the ray meets [z + E(d), z + G(d)].
            double blo = std::numeric_limits<double>::infinity(), bhi = -blo;
            for (int i = 0; i <= 20000; ++i) {
                const double t = i / 20000.0;
                const double px = ax + (bx - ax) * t, py = ay + (by - ay) * t, pz = az + (bz - az) * t;
                const double d = std::hypot(x - px, y - py);
                if (d > pc.rmax) {
                    continue;
                }
                blo = std::min(blo, pz + pc.low.eval(d));
                bhi = std::max(bhi, pz - pc.highNeg.eval(d));
            }
            if (blo > bhi) {
                CHECK((!hit || hi - lo < 1e-3));
                continue;
            }
            REQUIRE(hit);
            INFO("piece rmax " << pc.rmax << " move (" << ax << ", " << ay << ", " << az << ") -> (" << bx << ", " << by
                               << ", " << bz << ") ray (" << x << ", " << y << ")");
            CHECK(lo <= blo + 1e-9);
            CHECK(hi >= bhi - 1e-9);
            CHECK(lo >= blo - 2e-3);
            CHECK(hi <= bhi + 2e-3);
        }
    }
}
