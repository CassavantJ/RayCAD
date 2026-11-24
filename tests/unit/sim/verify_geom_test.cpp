// The exact geometry of the safety checks (task T-071, src/sim/verify_geom.h): a tool piece swept along a straight
// motion against a triangle, against dense sampling; inside tests; profile offsets; motions against the backplot.
#include "backplot.h"
#include "verify_impl.h"
#include "verify_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::detail;
using namespace ray::sim::test;

namespace {

struct Lcg {
    std::uint64_t s;
    double next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double in(double a, double b) { return a + (b - a) * next(); }
};

// How far point q lies outside piece `pc` with its tip at c (negative: inside by that much), as the larger of the
// radial and the height excess.
double outside(const ConvexPiece& pc, const Vec3& c, const Vec3& q)
{
    const double h = q.z - c.z;
    const double dx = q.x - c.x, dy = q.y - c.y;
    const double d = std::sqrt(dx * dx + dy * dy);
    const double hc = std::clamp(h, pc.h0, std::min(pc.h1, 1e6));
    return std::max({d - pc.radiusAt(hc), pc.h0 - h, h - pc.h1});
}

// The smallest `outside` over a dense sample of the triangle and the motion.
double sampled(const ConvexPiece& pc, const Vec3& a, const Vec3& b, const Vec3 t[3])
{
    double best = std::numeric_limits<double>::infinity();
    const int nt = 48, nu = 40;
    for (int k = 0; k <= nt; ++k) {
        const double s = static_cast<double>(k) / nt;
        const Vec3 c{a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s};
        for (int i = 0; i <= nu; ++i) {
            for (int j = 0; i + j <= nu; ++j) {
                const double u = static_cast<double>(i) / nu, v = static_cast<double>(j) / nu, w = 1 - u - v;
                const Vec3 q{t[0].x * w + t[1].x * u + t[2].x * v, t[0].y * w + t[1].y * u + t[2].y * v,
                             t[0].z * w + t[1].z * u + t[2].z * v};
                best = std::min(best, outside(pc, c, q));
            }
        }
    }
    return best;
}

std::vector<Vec2> ballProfile(double r, double top)
{
    std::vector<Vec2> p{{0, 0}};
    for (int i = 1; i <= 16; ++i) {
        const double a = 1.5707963267948966 * i / 16;
        p.push_back({r * dmath::sin(a), r - r * dmath::cos(a)});
    }
    p.push_back({r, top});
    return p;
}

} // namespace

TEST_CASE("a swept piece meets a triangle exactly when sampling finds them overlapping", "[sim][verify]")
{
    // Cylinder, ball, cone (drill point) and frustum (a holder's taper).
    const std::vector<std::vector<Vec2>> profiles = {
        {{0, 0}, {3, 0}, {3, 8}},
        ballProfile(3, 8),
        {{0, 0}, {3, 2}, {3, 8}},
        {{6, 0}, {10, 7}},
    };
    Lcg rng{12345};
    int hits = 0, misses = 0;
    for (const std::vector<Vec2>& prof : profiles) {
        const CutterShape shape = compileProfile(prof, false);
        REQUIRE(!shape.empty());
        for (int n = 0; n < 250; ++n) {
            Vec3 t[3];
            for (Vec3& v : t) {
                v = {rng.in(-12, 12), rng.in(-12, 12), rng.in(-4, 12)};
            }
            const Vec3 a{rng.in(-8, 8), rng.in(-8, 8), rng.in(-6, 6)};
            const Vec3 b = rng.next() < 0.2 ? a : Vec3{rng.in(-8, 8), rng.in(-8, 8), rng.in(-6, 6)};
            for (const ConvexPiece& pc : shape.pieces) {
                const bool exact = pieceHitsTriangle(pc, a, b, t);
                const double s = sampled(pc, a, b, t);
                // Sampling over-estimates the gap by at most about the sample spacing.
                if (s < -1e-9) {
                    CHECK(exact);
                }
                if (exact) {
                    CHECK(s < 0.6);
                    ++hits;
                } else {
                    CHECK(s > -1e-9);
                    ++misses;
                }
            }
        }
    }
    CHECK(hits > 100);
    CHECK(misses > 100);
}

TEST_CASE("a contact within the contact distance is a hit, one beyond the growth is not", "[sim][verify]")
{
    // A flat end mill of radius 5, grown, along X at y = 0, beside a wall at y = 5 + gap.
    const ToolShape tool = makeToolShape(endMill(10, 20, 60), nullptr, 0);
    const CutterShape grownTool = grownShape(compileProfile(tool.cuttingProfile, false), kVerifyGrowMm);
    for (const double gap : {0.0, 0.0005, 0.0009, 0.0021, 0.01, 1.0}) {
        const kernel::TriSoup wall = boxSoup(box(-50, 5 + gap, -20, 50, 30, 20));
        const bool hit = sweepHitsSoup(grownTool, {-10, 0, 0}, {10, 0, 0}, SoupIndex(wall));
        CHECK(hit == (gap <= kVerifyContactMm));
    }
    // Under the tip and above the top of the flutes too.
    CHECK(sweepHitsSoup(grownTool, {-10, 0, 0}, {10, 0, 0}, SoupIndex(boxSoup(box(-50, -50, -20, 50, 50, -0.0009)))));
    CHECK(!sweepHitsSoup(grownTool, {-10, 0, 0}, {10, 0, 0}, SoupIndex(boxSoup(box(-50, -50, -20, 50, 50, -0.0021)))));
    CHECK(sweepHitsSoup(grownTool, {-10, 0, 0}, {10, 0, 0}, SoupIndex(boxSoup(box(-50, -50, 20.0009, 50, 50, 30)))));
    // A ball's chords lie inside the real ball by up to 1 um; the growth covers that in every direction. Under it:
    const ToolShape b = makeToolShape(endMill(10, 20, 60, lib::ToolType::BallEndMill), nullptr, 0);
    const CutterShape grownBall = grownShape(compileProfile(b.cuttingProfile, false), kVerifyGrowMm);
    CHECK(sweepHitsSoup(grownBall, {-10, 0, 0}, {10, 0, 0}, SoupIndex(boxSoup(box(-50, -50, -20, 50, 50, -0.0009)))));
    // and 45 degrees down, against the edge of a block 0.9 um from the real ball (sqrt(2) off * = r + 0.0009).
    for (int k = 0; k < 8; ++k) {
        const double r = 5, off = (r + 0.0009 + 0.01 * k / 8.0) / std::sqrt(2.0);
        const kernel::TriSoup corner = boxSoup(box(off, -50, -50, 60, 50, r - off));
        const bool hit = sweepHitsSoup(grownBall, {0, -10, 0}, {0, 10, 0}, SoupIndex(corner));
        const double gap = off * std::sqrt(2.0) - r;
        if (gap <= kVerifyContactMm) {
            CHECK(hit);
        }
        if (gap > kVerifyGrowMm * 1.5) {
            CHECK(!hit);
        }
    }
}

TEST_CASE("a sweep wholly inside a solid is a hit; one wholly outside is not", "[sim][verify]")
{
    const kernel::TriSoup big = boxSoup(box(-100, -100, -100, 100, 100, 100));
    const SoupIndex idx(big);
    const CutterShape c = compileProfile({{0, 0}, {3, 0}, {3, 8}}, false);
    CHECK(sweepHitsSoup(c, {0, 0, 0}, {10, 5, -3}, idx));
    CHECK(!sweepHitsSoup(c, {0, 0, 200}, {10, 5, 203}, idx));
    // contains: inside, outside, and on a shared edge of the soup's faces (counted once).
    CHECK(idx.contains({0, 0, 0}));
    CHECK(idx.contains({-100, 0, 0}));
    CHECK(!idx.contains({0, 0, 101}));
    CHECK(!idx.contains({101, 0, 0}));
    // Inverted orientation reads the same.
    kernel::TriSoup flipped = big;
    for (std::size_t t = 0; t + 2 < flipped.indices.size(); t += 3) {
        std::swap(flipped.indices[t + 1], flipped.indices[t + 2]);
    }
    const SoupIndex fi(flipped);
    CHECK(fi.contains({0, 0, 0}));
    CHECK(fi.contains({50, 50, 0})); // on the diagonal of the top and bottom faces' triangles
    CHECK(!fi.contains({0, 0, 150}));
}

TEST_CASE("a grown shape holds every point within its growth; a shrunk profile every point that deep", "[sim][verify]")
{
    // A stepped, coned, balled profile: several convex pieces.
    const std::vector<Vec2> p = {{0, 0}, {2, 0.5}, {3, 2}, {3, 8}, {2, 8}, {2, 10}, {5, 13}, {5, 20}};
    const CutterShape s = compileProfile(p, false);
    const double e = 0.3;
    const CutterShape g = grownShape(s, e);
    REQUIRE(!g.empty());
    CHECK(g.hmin == s.hmin - e);
    CHECK(g.hmax == s.hmax + e);
    // Radius of a shape at a height: the largest of its pieces there.
    const auto radius = [](const CutterShape& c, double h) {
        double r = -1;
        for (const ConvexPiece& pc : c.pieces) {
            if (h >= pc.h0 && h <= pc.h1) {
                r = std::max(r, pc.radiusAt(h));
            }
        }
        return r;
    };
    // Every height: the grown radius is at least the original's largest within e of it, plus e.
    for (int i = 0; i <= 2000; ++i) {
        const double h = s.hmin - e + (s.hmax - s.hmin + 2 * e) * i / 2000.0;
        double want = -1;
        for (int j = 0; j <= 200; ++j) {
            const double hh = h - e + 2 * e * j / 200.0;
            if (hh >= s.hmin && hh <= s.hmax) {
                want = std::max(want, radius(s, hh));
            }
        }
        if (want >= 0) {
            CHECK(radius(g, h) >= want + e - 1e-9);
        }
    }
    const std::vector<Vec2> k = shrunkProfile(p, 0.5);
    REQUIRE(k.size() >= 2);
    CHECK(k.front().y == 0.5);
    CHECK(k.back().y == 19.5);
    CHECK(shrunkProfile({{1, 0}, {1, 0.5}}, 0.5).empty());
}

TEST_CASE("motions follow the backplot's expansion of arcs and canned cycles", "[sim][verify]")
{
    Path p(0, 0, 10);
    p.rapid(20, 0, 5)
        .feed(20, 0, -2)
        .arc(false, -20, 0, -2, 0, 0)
        .arc(true, 0, 20, -4, 0, 0)
        .feed(0, 20, 5)
        .cycle(cam::MoveKind::CyclePeck, 30, 30, -12, 0, 2, 5, 4)
        .cycle(cam::MoveKind::CycleTap, 40, 30, -10, 0, 2, 5)
        .cancel();
    const std::shared_ptr<const cam::Toolpath> tp = p.done();
    VerifyInput in;
    const ToolShape shape = makeToolShape(endMill(6, 20, 50), nullptr, 0);
    in.ops.push_back({OperationId{}, tp.get(), &shape, nullptr});
    std::vector<Motion> motions = expandMotions(in);
    // First the tool standing at the start, a zero-length rapid of move 0; the backplot draws motion only.
    REQUIRE(!motions.empty());
    CHECK(motions[0].rapid);
    CHECK(motions[0].move == 0);
    CHECK((motions[0].a.x == 0 && motions[0].a.y == 0 && motions[0].a.z == 10));
    CHECK((motions[0].b.x == 0 && motions[0].b.y == 0 && motions[0].b.z == 10));
    motions.erase(motions.begin());
    const Backplot bp = buildBackplot(*tp, Xform::identity(), kVerifyArcSagittaMm);
    // The backplot's segments (vertex s to s + 1, zero-length ones of moves without motion left out) are the motions.
    std::vector<std::pair<Vec3, Vec3>> segs;
    std::vector<std::uint32_t> segMove;
    for (std::size_t s = 0; s + 1 < bp.lineVertices.size(); ++s) {
        const BackplotVertex& v0 = bp.lineVertices[s];
        const BackplotVertex& v1 = bp.lineVertices[s + 1];
        if (v0.x == v1.x && v0.y == v1.y && v0.z == v1.z) {
            continue;
        }
        segs.push_back({{v0.x, v0.y, v0.z}, {v1.x, v1.y, v1.z}});
        segMove.push_back(v1.moveIndex);
    }
    REQUIRE(segs.size() == motions.size());
    for (std::size_t i = 0; i < motions.size(); ++i) {
        CHECK(motions[i].move == segMove[i]);
        const auto near = [](const Vec3& a, const Vec3& b) {
            return std::abs(a.x - b.x) < 1e-5 && std::abs(a.y - b.y) < 1e-5 && std::abs(a.z - b.z) < 1e-5;
        };
        CHECK(near(motions[i].a, segs[i].first));
        CHECK(near(motions[i].b, segs[i].second));
    }
}
