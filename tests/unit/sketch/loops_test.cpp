// closedLoops and analyzeLoops (task T-091): regions, holes, islands, shared edges, slots, splines, open curves,
// construction geometry, tags, determinism, and an end-to-end extrude through the kernel.
#include <catch2/catch_test_macros.hpp>

#include "sketch_test_util.h"

#include <loops.h>

#include <ray/kernel.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <set>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;
using kernel::ProfileCurve;
using kernel::ProfileLoop;

namespace {

constexpr double kPi = std::numbers::pi;

double dist(Vec2 a, Vec2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

Vec2 startOf(const ProfileCurve& c)
{
    return c.pts.front();
}
Vec2 endOf(const ProfileCurve& c)
{
    return c.kind == ProfileCurve::Kind::Bspline ? c.pts.back() : c.pts[1];
}

// The documented encoding (kernel.h above ProfileCurve, and loops.h): every curve is well formed, curves come in
// chain order and direction, joints meet within 1e-6 mm, the loop closes, an arc's end lies on its circle within
// 1e-6 mm. Outer loops run counter-clockwise, holes clockwise.
void checkLoop(const ProfileLoop& loop, bool outer)
{
    REQUIRE_FALSE(loop.curves.empty());
    for (const ProfileCurve& c : loop.curves) {
        switch (c.kind) {
        case ProfileCurve::Kind::Line:
            REQUIRE(c.pts.size() == 2);
            CHECK(dist(c.pts[0], c.pts[1]) > 1e-6);
            break;
        case ProfileCurve::Kind::Arc:
            REQUIRE(c.pts.size() == 3);
            CHECK(std::fabs(dist(c.pts[1], c.pts[2]) - dist(c.pts[0], c.pts[2])) <= 1e-6);
            CHECK(dist(c.pts[0], c.pts[1]) > 1e-6);
            break;
        case ProfileCurve::Kind::Circle:
            REQUIRE(c.pts.size() == 1);
            CHECK(c.radius > 0);
            CHECK(loop.curves.size() == 1);
            break;
        case ProfileCurve::Kind::Bspline:
            REQUIRE(c.pts.size() >= 2);
            break;
        }
    }
    if (loop.curves.size() > 1 || loop.curves[0].kind != ProfileCurve::Kind::Circle) {
        for (std::size_t i = 0; i < loop.curves.size(); ++i) {
            const ProfileCurve& a = loop.curves[i];
            const ProfileCurve& b = loop.curves[(i + 1) % loop.curves.size()];
            CHECK(dist(endOf(a), startOf(b)) <= 1e-6);
        }
    }
    const double a2 = loopSignedArea2(loop);
    CHECK((outer ? a2 > 0 : a2 < 0));
}

LoopAnalysis analyze(const Sketch& s)
{
    LoopAnalysis a = take(analyzeLoops(s));
    for (const LoopRegion& r : a.regions) {
        checkLoop(r.outer, true);
        for (const ProfileLoop& h : r.holes) {
            checkLoop(h, false);
        }
    }
    return a;
}

std::multiset<std::uint64_t> tagsOf(const ProfileLoop& loop)
{
    std::multiset<std::uint64_t> out;
    for (const ProfileCurve& c : loop.curves) {
        out.insert(c.tag);
    }
    return out;
}

const LoopRegion* regionWithArea(const LoopAnalysis& a, double area, double tol = 1e-9)
{
    for (const LoopRegion& r : a.regions) {
        if (std::fabs(r.area - area) <= tol * std::max(1.0, area)) {
            return &r;
        }
    }
    return nullptr;
}

struct Rect {
    SketchEntId p[4];
    SketchEntId l[4];
};
// Counter-clockwise from (x0, y0); lines share their corner points.
Rect rect(Builder& b, double x0, double y0, double x1, double y1)
{
    Rect r;
    r.p[0] = b.point(x0, y0);
    r.p[1] = b.point(x1, y0);
    r.p[2] = b.point(x1, y1);
    r.p[3] = b.point(x0, y1);
    for (int i = 0; i < 4; ++i) {
        r.l[i] = b.line(r.p[i], r.p[(i + 1) % 4]);
    }
    return r;
}

bool hasOpen(const LoopAnalysis& a, SketchEntId id, OpenCurve::Why why)
{
    for (const OpenCurve& o : a.open) {
        if (o.entity == id && o.why == why) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("loops: a rectangle is one region tagged by its lines", "[loops]")
{
    Builder b;
    const Rect r = rect(b, 0, 0, 40, 25);
    const LoopAnalysis a = analyze(b.s);
    REQUIRE(a.regions.size() == 1);
    CHECK(a.open.empty());
    const LoopRegion& reg = a.regions[0];
    CHECK(reg.holes.empty());
    CHECK(std::fabs(reg.area - 1000) < 1e-9);
    CHECK(tagsOf(reg.outer) ==
          std::multiset<std::uint64_t>{curveTag(r.l[0]), curveTag(r.l[1]), curveTag(r.l[2]), curveTag(r.l[3])});
    CHECK(reg.outer.tag != 0);

    const auto loops = take(closedLoops(b.s));
    REQUIRE(loops.size() == 1);
    CHECK(loops[0].tag == reg.outer.tag);
}

TEST_CASE("loops: the curve tag is the low 64 bits of the BLAKE3 hash of the entity id", "[loops]")
{
    SketchEntId id{orderedUuid(7, 0x10)};
    Hasher h;
    h.bytes(std::as_bytes(std::span(id.value.bytes)));
    const Hash256 hash = h.finish();
    std::uint64_t expect = 0;
    for (int i = 0; i < 8; ++i) {
        expect |= static_cast<std::uint64_t>(hash.bytes[static_cast<std::size_t>(i)]) << (8 * i);
    }
    CHECK(curveTag(id) == expect);
    CHECK(slotPartTag(id, 0) != slotPartTag(id, 1));
    CHECK(slotPartTag(id, 0) != curveTag(id));
}

TEST_CASE("loops: outer boundary with holes, and an island inside a hole", "[loops]")
{
    Builder b;
    const Rect outer = rect(b, 0, 0, 100, 60);
    const SketchEntId hole = b.circle(b.point(30, 30), 20);
    const Rect island = rect(b, 25, 25, 35, 35);            // inside the circular hole
    const SketchEntId hole2 = b.circle(b.point(80, 30), 5); // a plain second hole
    const LoopAnalysis a = analyze(b.s);
    CHECK(a.open.empty());
    // Regions: plate minus both holes, ring between hole and island, the island, the second hole's disc.
    REQUIRE(a.regions.size() == 4);
    const double plate = 6000 - kPi * 400 - kPi * 25;
    const LoopRegion* p = regionWithArea(a, plate);
    REQUIRE(p);
    CHECK(tagsOf(p->outer) == std::multiset<std::uint64_t>{curveTag(outer.l[0]), curveTag(outer.l[1]),
                                                           curveTag(outer.l[2]), curveTag(outer.l[3])});
    REQUIRE(p->holes.size() == 2);
    std::multiset<std::uint64_t> holeTags;
    for (const ProfileLoop& h : p->holes) {
        REQUIRE(h.curves.size() == 1);
        CHECK(h.curves[0].kind == ProfileCurve::Kind::Circle);
        holeTags.insert(h.curves[0].tag);
    }
    CHECK(holeTags == std::multiset<std::uint64_t>{curveTag(hole), curveTag(hole2)});

    const LoopRegion* ring = regionWithArea(a, kPi * 400 - 100);
    REQUIRE(ring);
    REQUIRE(ring->holes.size() == 1);
    CHECK(tagsOf(ring->outer) == std::multiset<std::uint64_t>{curveTag(hole)});
    CHECK(tagsOf(ring->holes[0]) == std::multiset<std::uint64_t>{curveTag(island.l[0]), curveTag(island.l[1]),
                                                                 curveTag(island.l[2]), curveTag(island.l[3])});
    const LoopRegion* isl = regionWithArea(a, 100);
    REQUIRE(isl);
    CHECK(isl->holes.empty());
    // The island is one loop seen from two sides: hole of the ring (clockwise), outer loop of its own region.
    CHECK(isl->outer.tag != ring->holes[0].tag);
    const LoopRegion* disc = regionWithArea(a, kPi * 25);
    REQUIRE(disc);

    // The flat closedLoops list splits back into the same regions.
    const auto loops = take(closedLoops(b.s));
    const auto regions = regionsOf(loops);
    REQUIRE(regions.size() == a.regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        CHECK(regions[i].front().tag == a.regions[i].outer.tag);
        CHECK(regions[i].size() == 1 + a.regions[i].holes.size());
    }
}

TEST_CASE("loops: two regions sharing an edge", "[loops]")
{
    SECTION("the divider ends on the rectangle's sides (T-junctions)")
    {
        Builder b;
        const Rect r = rect(b, 0, 0, 20, 10);
        const SketchEntId divider = b.line(b.point(8, 0), b.point(8, 10));
        const LoopAnalysis a = analyze(b.s);
        CHECK(a.open.empty());
        REQUIRE(a.regions.size() == 2);
        const LoopRegion* left = regionWithArea(a, 80);
        const LoopRegion* right = regionWithArea(a, 120);
        REQUIRE(left);
        REQUIRE(right);
        // Bottom and top lines are cut in two; each half keeps the line's tag.
        CHECK(tagsOf(left->outer) ==
              std::multiset<std::uint64_t>{curveTag(r.l[0]), curveTag(divider), curveTag(r.l[2]), curveTag(r.l[3])});
        CHECK(tagsOf(right->outer) ==
              std::multiset<std::uint64_t>{curveTag(r.l[0]), curveTag(r.l[1]), curveTag(r.l[2]), curveTag(divider)});
        CHECK(left->outer.tag != right->outer.tag);
    }
    SECTION("seven lines sharing points")
    {
        Builder b;
        const SketchEntId p0 = b.point(0, 0), p1 = b.point(10, 0), p2 = b.point(20, 0);
        const SketchEntId p3 = b.point(20, 10), p4 = b.point(10, 10), p5 = b.point(0, 10);
        for (auto [x, y] : {std::pair{p0, p1}, std::pair{p1, p2}, std::pair{p2, p3}, std::pair{p3, p4},
                            std::pair{p4, p5}, std::pair{p5, p0}, std::pair{p1, p4}}) {
            b.line(x, y);
        }
        const LoopAnalysis a = analyze(b.s);
        CHECK(a.open.empty());
        REQUIRE(a.regions.size() == 2);
        CHECK(regionWithArea(a, 100));
        CHECK(a.regions[0].outer.curves.size() == 4);
        CHECK(a.regions[1].outer.curves.size() == 4);
    }
    SECTION("crossing lines cut each other into four regions")
    {
        Builder b;
        rect(b, 0, 0, 10, 10);
        b.line(b.point(5, -2), b.point(5, 12)); // sticks out at both ends
        b.line(b.point(-2, 5), b.point(12, 5));
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 4);
        for (const LoopRegion& r : a.regions) {
            CHECK(std::fabs(r.area - 25) < 1e-9);
        }
        // The four overhangs dangle.
        CHECK(a.open.size() == 2);
    }
}

TEST_CASE("loops: a slot is one region of two lines and two arcs", "[loops]")
{
    Builder b;
    const SketchEntId a0 = b.point(0, 0);
    const SketchEntId b0 = b.point(30, 10);
    const SketchEntId slot = b.add(Slot{a0, b0, 8});
    const LoopAnalysis a = analyze(b.s);
    CHECK(a.open.empty());
    REQUIRE(a.regions.size() == 1);
    const ProfileLoop& loop = a.regions[0].outer;
    REQUIRE(loop.curves.size() == 4);
    int lines = 0;
    int arcs = 0;
    for (const ProfileCurve& c : loop.curves) {
        lines += c.kind == ProfileCurve::Kind::Line;
        arcs += c.kind == ProfileCurve::Kind::Arc;
    }
    CHECK(lines == 2);
    CHECK(arcs == 2);
    CHECK(tagsOf(loop) == std::multiset<std::uint64_t>{slotPartTag(slot, 0), slotPartTag(slot, 1), slotPartTag(slot, 2),
                                                       slotPartTag(slot, 3)});
    const double len = std::sqrt(30.0 * 30 + 10 * 10);
    CHECK(std::fabs(a.regions[0].area - (len * 8 + kPi * 16)) < 1e-9);
}

TEST_CASE("loops: open chains, dangling segments, bridges and duplicates are reported", "[loops]")
{
    Builder b;
    const Rect r = rect(b, 0, 0, 10, 10);
    const SketchEntId dangling = b.line(r.p[2], b.point(15, 15)); // hangs off a corner
    const SketchEntId q0 = b.point(30, 0), q1 = b.point(40, 0), q2 = b.point(40, 10);
    const SketchEntId chain1 = b.line(q0, q1); // an open chain of two lines
    const SketchEntId chain2 = b.line(q1, q2);
    const Rect r2 = rect(b, 0, 20, 10, 30);
    const SketchEntId bridge = b.line(r.p[3], r2.p[0]);              // links two closed loops
    const SketchEntId dup = b.line(b.point(0, 20), b.point(10, 20)); // on top of r2's bottom line
    const SketchEntId lone = b.line(b.point(50, 50), b.point(60, 50));
    const LoopAnalysis a = analyze(b.s);
    REQUIRE(a.regions.size() == 2);
    CHECK(hasOpen(a, dangling, OpenCurve::Why::OpenChain));
    CHECK(hasOpen(a, chain1, OpenCurve::Why::OpenChain));
    CHECK(hasOpen(a, chain2, OpenCurve::Why::OpenChain));
    CHECK(hasOpen(a, lone, OpenCurve::Why::OpenChain));
    CHECK(hasOpen(a, bridge, OpenCurve::Why::Bridge));
    CHECK((hasOpen(a, dup, OpenCurve::Why::Duplicate) || hasOpen(a, r2.l[0], OpenCurve::Why::Duplicate)));
    CHECK(a.open.size() == 6);
    CHECK(a.warnings.size() == a.open.size());
    for (const std::string& w : a.warnings) {
        CHECK(w.find("Sketch ") == 0);
    }
    // closedLoops still returns both regions; the open curves go to the log.
    CHECK(regionsOf(take(closedLoops(b.s))).size() == 2);

    SECTION("a tiny gap leaves the chain open")
    {
        Builder g;
        const SketchEntId a0 = g.point(0, 0), a1 = g.point(10, 0), a2 = g.point(10, 10), a3 = g.point(0, 10);
        g.line(a0, a1);
        g.line(a1, a2);
        g.line(a2, a3);
        g.line(a3, g.point(0, 1e-4));
        const LoopAnalysis ga = analyze(g.s);
        CHECK(ga.regions.empty());
        CHECK(ga.open.size() == 4);
        CHECK(take(closedLoops(g.s)).empty());
    }
    SECTION("an arc whose ends meet is reported, not used")
    {
        Builder g;
        const SketchEntId arc = g.arc(g.point(0, 0), g.point(5, 0), g.point(5, 1e-7), true);
        const LoopAnalysis ga = analyze(g.s);
        CHECK(ga.regions.empty());
        CHECK(hasOpen(ga, arc, OpenCurve::Why::Degenerate));
    }
    SECTION("a gap below the join tolerance is closed")
    {
        Builder g;
        const SketchEntId a0 = g.point(0, 0), a1 = g.point(10, 0), a2 = g.point(10, 10), a3 = g.point(0, 10);
        g.line(a0, a1);
        g.line(a1, a2);
        g.line(a2, a3);
        g.line(a3, g.point(0, 4e-7));
        CHECK(analyze(g.s).regions.size() == 1);
    }
}

TEST_CASE("loops: construction geometry is ignored", "[loops]")
{
    Builder b;
    rect(b, 0, 0, 10, 10);
    b.add(Line{b.point(-5, 5), b.point(15, 5)}, true); // would split the square
    b.add(Circle{b.point(5, 5), 3}, true);             // would be a hole
    b.add(Line{b.point(20, 0), b.point(30, 0)}, true); // would be open
    const LoopAnalysis a = analyze(b.s);
    REQUIRE(a.regions.size() == 1);
    CHECK(a.regions[0].holes.empty());
    CHECK(a.open.empty());
    CHECK(std::fabs(a.regions[0].area - 100) < 1e-9);
}

TEST_CASE("loops: arcs, circles and tangent joins", "[loops]")
{
    SECTION("a D shape: a line closed by a half circle, clockwise arc")
    {
        Builder b;
        const SketchEntId c = b.point(0, 0);
        const SketchEntId p = b.point(10, 0);
        const SketchEntId q = b.point(-10, 0);
        b.line(q, p);
        b.arc(c, q, p, false); // from (-10,0) clockwise over the top to (10,0)
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 1);
        CHECK(std::fabs(a.regions[0].area - kPi * 50) < 1e-9);
    }
    SECTION("two crossing circles give three regions with different tags")
    {
        Builder b;
        b.circle(b.point(0, 0), 10);
        b.circle(b.point(12, 0), 10);
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 3);
        std::set<std::uint64_t> tags;
        double total = 0;
        for (const LoopRegion& r : a.regions) {
            tags.insert(r.outer.tag);
            total += r.area;
            CHECK(r.outer.curves.size() == 2);
        }
        CHECK(tags.size() == 3);
        // Union of two discs: 2 pi r^2 minus the lens.
        const double d = 12, r = 10;
        const double lens = 2 * r * r * std::acos(d / (2 * r)) - d / 2 * std::sqrt(4 * r * r - d * d);
        CHECK(std::fabs(total - (2 * kPi * r * r - lens)) < 1e-9);
        CHECK(regionWithArea(a, lens));
    }
    SECTION("a circle touched by a line is cut there and stays one region")
    {
        Builder b;
        rect(b, -10, -10, 10, 20);
        b.circle(b.point(0, 0), 10); // touches the bottom and both sides of the rectangle
        const LoopAnalysis a = analyze(b.s);
        double total = 0;
        for (const LoopRegion& r : a.regions) {
            total += r.area;
        }
        CHECK(std::fabs(total - 600) < 1e-9);
        CHECK(regionWithArea(a, kPi * 100));
    }
    SECTION("an arc whose end lies a little off its circle is balanced onto it")
    {
        Builder b;
        const SketchEntId c = b.point(0, 0);
        const SketchEntId s = b.point(10, 0);
        const SketchEntId e = b.point(0, 10 + 2e-7);
        b.arc(c, s, e, true);
        b.line(e, c);
        b.line(c, s);
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 1); // checkLoop verified the arc's two radii agree within 1e-6
    }
}

TEST_CASE("loops: splines", "[loops]")
{
    SECTION("a cubic with four control points goes to the kernel whole")
    {
        Builder b;
        const SketchEntId p0 = b.point(0, 0), p1 = b.point(3, 5), p2 = b.point(7, 5), p3 = b.point(10, 0);
        const SketchEntId sp = b.add(Spline{{p0, p1, p2, p3}, 3, false});
        b.line(p3, p0);
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 1);
        const ProfileLoop& loop = a.regions[0].outer;
        REQUIRE(loop.curves.size() == 2);
        int splines = 0;
        for (const ProfileCurve& c : loop.curves) {
            if (c.kind == ProfileCurve::Kind::Bspline) {
                ++splines;
                CHECK(c.pts.size() == 4);
                CHECK(c.tag == curveTag(sp));
            }
        }
        CHECK(splines == 1);
        // Area under the curve by Simpson's rule on its Bernstein form (independent of the code under test).
        const auto bez = [](double t, double a, double b, double c, double d) {
            const double u = 1 - t;
            return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
        };
        const auto dbez = [](double t, double a, double b, double c, double d) {
            const double u = 1 - t;
            return 3 * (u * u * (b - a) + 2 * u * t * (c - b) + t * t * (d - c));
        };
        const int n = 2000;
        double area = 0;
        for (int i = 0; i <= n; ++i) {
            const double t = static_cast<double>(i) / n;
            const double w = (i == 0 || i == n) ? 1 : (i % 2 ? 4 : 2);
            area += w * bez(t, 0, 5, 5, 0) * dbez(t, 0, 3, 7, 10);
        }
        area /= 3.0 * n;
        CHECK(std::fabs(a.regions[0].area - area) < 1e-9);
    }
    SECTION("a quadratic with five control points goes as its three Bezier spans")
    {
        Builder b;
        std::vector<SketchEntId> pts;
        for (auto [x, y] : {std::pair{0.0, 0.0}, {2.0, 4.0}, {5.0, 5.0}, {8.0, 4.0}, {10.0, 0.0}}) {
            pts.push_back(b.point(x, y));
        }
        b.add(Spline{pts, 2, false});
        b.line(pts.back(), pts.front());
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 1);
        int spans = 0;
        for (const ProfileCurve& c : a.regions[0].outer.curves) {
            if (c.kind == ProfileCurve::Kind::Bspline) {
                ++spans;
                CHECK(c.pts.size() == 3); // degree min(3, 2) = 2: exactly this span
            }
        }
        CHECK(spans == 3);
    }
    SECTION("a closed (periodic) spline is a region of its own")
    {
        Builder b;
        std::vector<SketchEntId> pts;
        for (int i = 0; i < 6; ++i) {
            pts.push_back(b.point(10 * std::cos(i * kPi / 3), 10 * std::sin(i * kPi / 3)));
        }
        b.add(Spline{pts, 3, true});
        const LoopAnalysis a = analyze(b.s);
        REQUIRE(a.regions.size() == 1);
        CHECK(a.regions[0].outer.curves.size() == 6);
        CHECK(a.regions[0].area > 0);
    }
    SECTION("degree 4 has no exact kernel form: the region is rejected and closedLoops fails")
    {
        Builder b;
        std::vector<SketchEntId> pts;
        for (auto [x, y] : {std::pair{0.0, 0.0}, {2.0, 4.0}, {5.0, 6.0}, {8.0, 4.0}, {10.0, 0.0}}) {
            pts.push_back(b.point(x, y));
        }
        b.add(Spline{pts, 4, false});
        b.line(pts.back(), pts.front());
        const LoopAnalysis a = take(analyzeLoops(b.s));
        CHECK(a.regions.empty());
        REQUIRE(a.rejected.size() == 1);
        CHECK(a.rejected[0].find("degree 4") != std::string::npos);
        const auto loops = closedLoops(b.s);
        REQUIRE_FALSE(loops.has_value());
        CHECK(loops.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("a spline crossed in its interior is left out and reported")
    {
        Builder b;
        const SketchEntId p0 = b.point(0, 0), p1 = b.point(3, 5), p2 = b.point(7, 5), p3 = b.point(10, 0);
        const SketchEntId sp = b.add(Spline{{p0, p1, p2, p3}, 3, false});
        b.line(p3, p0);
        b.line(b.point(5, -1), b.point(5, 10));
        const LoopAnalysis a = analyze(b.s);
        CHECK(hasOpen(a, sp, OpenCurve::Why::SplineCrossed));
    }
}

TEST_CASE("loops: output is independent of entity order and loop tags are stable", "[loops]")
{
    Builder b;
    rect(b, 0, 0, 100, 60);
    b.circle(b.point(30, 30), 20);
    rect(b, 25, 25, 35, 35);
    b.add(Slot{b.point(60, 20), b.point(85, 40), 6});
    const auto base = take(closedLoops(b.s));
    std::mt19937 rng(12345);
    for (int i = 0; i < 20; ++i) {
        Sketch shuffled = b.s;
        std::shuffle(shuffled.entities.begin(), shuffled.entities.end(), rng);
        const auto again = take(closedLoops(shuffled));
        REQUIRE(again.size() == base.size());
        for (std::size_t l = 0; l < base.size(); ++l) {
            CHECK(again[l].tag == base[l].tag);
            REQUIRE(again[l].curves.size() == base[l].curves.size());
            for (std::size_t c = 0; c < base[l].curves.size(); ++c) {
                CHECK(again[l].curves[c].tag == base[l].curves[c].tag);
                REQUIRE(again[l].curves[c].pts.size() == base[l].curves[c].pts.size());
                for (std::size_t p = 0; p < base[l].curves[c].pts.size(); ++p) {
                    CHECK(again[l].curves[c].pts[p].x == base[l].curves[c].pts[p].x);
                    CHECK(again[l].curves[c].pts[p].y == base[l].curves[c].pts[p].y);
                }
            }
        }
    }
    // Moving geometry keeps every loop tag; adding an unrelated hole keeps the plate's region tag.
    std::set<std::uint64_t> before;
    for (const auto& l : base) {
        before.insert(l.tag);
    }
    Sketch moved = b.s;
    for (Entity& e : moved.entities) {
        if (auto* p = std::get_if<Point>(&e.geom)) {
            p->p.x += 0.5;
        }
    }
    std::set<std::uint64_t> afterMove;
    for (const auto& l : take(closedLoops(moved))) {
        afterMove.insert(l.tag);
    }
    CHECK(afterMove == before);
    const LoopAnalysis a0 = analyze(b.s);
    const LoopRegion* plate0 = nullptr;
    for (const LoopRegion& r : a0.regions) {
        if (r.holes.size() == 2) {
            plate0 = &r;
        }
    }
    REQUIRE(plate0);
    b.circle(b.point(90, 10), 3);
    const LoopAnalysis a1 = analyze(b.s);
    bool found = false;
    for (const LoopRegion& r : a1.regions) {
        if (r.holes.size() == 3) {
            found = true;
            CHECK(r.outer.tag == plate0->outer.tag);
        }
    }
    CHECK(found);
}

TEST_CASE("loops: a grid of crossing lines and scattered holes", "[loops]")
{
    std::mt19937 rng(91);
    std::uniform_real_distribution<double> jitter(-0.3, 0.3);
    for (int trial = 0; trial < 5; ++trial) {
        Builder b;
        const int n = 6 + trial; // n horizontal and n vertical lines, each overhanging the grid by 5 mm
        std::vector<double> xs, ys;
        for (int i = 0; i < n; ++i) {
            xs.push_back(i * 10 + jitter(rng));
            ys.push_back(i * 10 + jitter(rng));
        }
        for (int i = 0; i < n; ++i) {
            b.line(b.point(xs[i], -5), b.point(xs[i], ys.back() + 5));
            b.line(b.point(-5, ys[i]), b.point(xs.back() + 5, ys[i]));
        }
        // One small circle in the middle of every other cell: a hole of that cell, and a region of its own.
        int circles = 0;
        double holeArea = 0;
        for (int i = 0; i + 1 < n; i += 2) {
            for (int j = 0; j + 1 < n; j += 2) {
                b.circle(b.point((xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2), 2);
                ++circles;
                holeArea += kPi * 4;
            }
        }
        const LoopAnalysis a = analyze(b.s);
        CHECK(a.regions.size() == static_cast<std::size_t>((n - 1) * (n - 1) + circles));
        double total = 0;
        int withHole = 0;
        for (const LoopRegion& r : a.regions) {
            total += r.area;
            withHole += r.holes.size() == 1;
        }
        CHECK(withHole == circles);
        const double box = (xs.back() - xs.front()) * (ys.back() - ys.front());
        CHECK(std::fabs(total - box) < 1e-9 * box); // the cells tile the box; holes and discs cancel
        (void)holeArea;
        // The overhangs: 4 per line pair, each an open piece of its line.
        CHECK(a.open.size() == static_cast<std::size_t>(2 * n));
    }
}

TEST_CASE("loops: invalid sketches are refused", "[loops]")
{
    Builder b;
    const SketchEntId p = b.point(0, 0);
    b.add(Line{p, SketchEntId{orderedUuid(999, 0x10)}});
    const auto r = closedLoops(b.s);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(take(closedLoops(Sketch{})).empty());
}

TEST_CASE("loops: extrude of a plate with holes through the kernel has the closed-form volume", "[loops][kernel]")
{
    Builder b;
    rect(b, 0, 0, 100, 60);
    b.circle(b.point(25, 30), 10);
    b.add(Slot{b.point(60, 30), b.point(80, 30), 12});
    const Rect island = rect(b, 20, 25, 30, 35); // inside the circular hole: a separate region
    (void)island;
    const LoopAnalysis a = analyze(b.s);
    const LoopRegion* plate = nullptr;
    for (const LoopRegion& r : a.regions) {
        if (r.holes.size() == 2) {
            plate = &r;
        }
    }
    REQUIRE(plate);
    const double area = 6000 - kPi * 100 - (20 * 12 + kPi * 36);
    CHECK(std::fabs(plate->area - area) < 1e-9);

    kernel::Profile profile;
    profile.plane = {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    profile.loops.push_back(plate->outer);
    for (const ProfileLoop& h : plate->holes) {
        profile.loops.push_back(h);
    }
    JobContext ctx;
    const kernel::SweepResult sweep = take(kernel::extrude(profile, 10, 0, ctx));
    const double volume = take(kernel::massProps(sweep.op.shape)).volume;
    CHECK(std::fabs(volume - area * 10) <= 1e-9 * area * 10);
    // Every side face maps back to a curve of the sketch.
    std::set<std::uint64_t> sketchTags;
    for (const ProfileLoop* l : {&plate->outer, &plate->holes[0], &plate->holes[1]}) {
        for (const ProfileCurve& c : l->curves) {
            sketchTags.insert(c.tag);
        }
    }
    for (const kernel::SweepFaceOrigin& o : sweep.origins) {
        if (o.role == kernel::SweepFaceOrigin::Role::Side) {
            CHECK(sketchTags.count(o.curveTag) == 1);
        } else {
            CHECK(o.loopTag == plate->outer.tag);
        }
    }

    // Every region of the drawing extrudes, including the ring around the island and the island itself.
    for (const LoopRegion& r : a.regions) {
        kernel::Profile p;
        p.plane = profile.plane;
        p.loops.push_back(r.outer);
        p.loops.insert(p.loops.end(), r.holes.begin(), r.holes.end());
        const kernel::SweepResult s = take(kernel::extrude(p, 5, 0, ctx));
        const double v = take(kernel::massProps(s.op.shape)).volume;
        CHECK(std::fabs(v - r.area * 5) <= 1e-9 * r.area * 5);
    }
}

TEST_CASE("loops: shared-edge regions and spline regions extrude through the kernel", "[loops][kernel]")
{
    Builder b;
    rect(b, 0, 0, 20, 10);
    b.line(b.point(8, 0), b.point(8, 10));
    const SketchEntId p0 = b.point(30, 0), p1 = b.point(33, 5), p2 = b.point(37, 5), p3 = b.point(40, 0);
    b.add(Spline{{p0, p1, p2, p3}, 3, false});
    b.line(p3, p0);
    std::vector<SketchEntId> q;
    for (auto [x, y] : {std::pair{50.0, 0.0}, {52.0, 4.0}, {55.0, 5.0}, {58.0, 4.0}, {60.0, 0.0}}) {
        q.push_back(b.point(x, y));
    }
    b.add(Spline{q, 2, false});
    b.line(q.back(), q.front());
    const LoopAnalysis a = analyze(b.s);
    REQUIRE(a.regions.size() == 4);
    JobContext ctx;
    for (const LoopRegion& r : a.regions) {
        kernel::Profile p;
        p.plane = {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
        p.loops.push_back(r.outer);
        const kernel::SweepResult s = take(kernel::extrude(p, 3, 0, ctx));
        const double v = take(kernel::massProps(s.op.shape)).volume;
        CHECK(std::fabs(v - r.area * 3) <= 1e-7 * r.area * 3);
    }
}
