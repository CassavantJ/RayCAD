// Backplot layout, move kinds, arcs in each plane, degenerate arcs and canned cycles (task T-037).
#include "backplot_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;
namespace bd = ray::sim::detail;

namespace {

bool samePos(const BackplotVertex& v, double x, double y, double z)
{
    return v.x == f(x) && v.y == f(y) && v.z == f(z);
}

std::uint8_t k(MoveKind kind)
{
    return static_cast<std::uint8_t>(kind);
}
std::uint8_t r(MoveRole role)
{
    return static_cast<std::uint8_t>(role);
}

// Expected sub-motion of an expanded move.
struct Sub {
    double x, y, z;
    MoveKind kind;
    MoveRole role;
    float feed;
};

void checkSubs(const Backplot& bp, std::size_t m, const std::vector<Sub>& want)
{
    const std::uint32_t a = bp.moveFirstVertex[m], b = bp.moveFirstVertex[m + 1];
    REQUIRE(b - a == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const BackplotVertex& v = bp.lineVertices[a + i];
        INFO("move " << m << " sub-motion " << i << " at " << v.x << ", " << v.y << ", " << v.z);
        CHECK(samePos(v, want[i].x, want[i].y, want[i].z));
        CHECK(v.kind == k(want[i].kind));
        CHECK(v.role == r(want[i].role));
        CHECK(v.feed == want[i].feed);
    }
}

} // namespace

TEST_CASE("the start is vertex 0 and every move maps to its own non-empty vertex range", "[sim][backplot]")
{
    const cam::Toolpath tp = toolpath(1, 2, 50,
                                      {rapid(10, 0, 5, MoveRole::Approach), line(10, 0, -1, 300, MoveRole::Plunge),
                                       line(20, 0, -1, 1200), arc(true, ArcPlane::XY, 30, 0, -1, 25, 0, -1),
                                       blankMove(MoveKind::Dwell, MoveRole::Cut), rapid(30, 0, 50)});
    const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
    checkLayout(tp, bp);
    CHECK(samePos(bp.lineVertices[0], 1, 2, 50));
    // Rapid: one vertex, feed 0, its own role.
    checkSubs(bp, 0, {{10, 0, 5, MoveKind::Rapid, MoveRole::Approach, 0.0f}});
    checkSubs(bp, 1, {{10, 0, -1, MoveKind::Linear, MoveRole::Plunge, 300.0f}});
    checkSubs(bp, 2, {{20, 0, -1, MoveKind::Linear, MoveRole::Cut, 1200.0f}});
    // Arc: chords, the last vertex exactly at the end.
    const std::uint32_t lastOfArc = bp.moveFirstVertex[4] - 1;
    CHECK(bp.moveFirstVertex[4] - bp.moveFirstVertex[3] > 1);
    CHECK(samePos(bp.lineVertices[lastOfArc], 30, 0, -1));
    for (std::uint32_t v = bp.moveFirstVertex[3]; v < bp.moveFirstVertex[4]; ++v) {
        CHECK(bp.lineVertices[v].kind == k(MoveKind::ArcCCW));
        CHECK(bp.lineVertices[v].feed == 1000.0f);
    }
    // Dwell: one zero-length vertex at the current position, its own kind.
    checkSubs(bp, 4, {{30, 0, -1, MoveKind::Dwell, MoveRole::Cut, 0.0f}});
    checkSubs(bp, 5, {{30, 0, 50, MoveKind::Rapid, MoveRole::Link, 0.0f}});
}

TEST_CASE("a move range is a contiguous segment range", "[sim][backplot]")
{
    std::vector<Move> moves;
    for (int i = 0; i < 40; ++i) {
        const double x = 2.0 * i;
        moves.push_back(i % 3 == 0 ? arc(i % 2 == 0, ArcPlane::XY, x + 2, 0, 0, x + 1, 0, 0) : line(x + 2, 0, 0, 500));
    }
    const cam::Toolpath tp = toolpath(0, 0, 0, moves);
    const Backplot bp = buildBackplot(tp, Xform::identity(), 0.001);
    checkLayout(tp, bp);
    // Segments [moveFirstVertex[a] - 1, moveFirstVertex[b + 1] - 1): segment s ends at vertex s + 1, whose moveIndex
    // is in [a, b], and the segment before the range belongs to move a - 1.
    for (std::uint32_t a = 0; a < 40; a += 7) {
        for (std::uint32_t b = a; b < 40; b += 5) {
            const std::uint32_t s0 = bp.moveFirstVertex[a] - 1, s1 = bp.moveFirstVertex[b + 1] - 1;
            for (std::uint32_t s = s0; s < s1; ++s) {
                const std::uint32_t m = bp.lineVertices[s + 1].moveIndex;
                CHECK((m >= a && m <= b));
            }
            if (a > 0) {
                CHECK(bp.lineVertices[s0].moveIndex == a - 1); // the range starts where move a - 1 ended
            }
        }
    }
}

TEST_CASE("an empty toolpath is its start vertex", "[sim][backplot]")
{
    const cam::Toolpath tp = toolpath(3, 4, 5, {});
    const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
    checkLayout(tp, bp);
    REQUIRE(bp.lineVertices.size() == 1);
    CHECK(samePos(bp.lineVertices[0], 3, 4, 5));
}

TEST_CASE("modelFromWcs is applied to every vertex", "[sim][backplot]")
{
    // A quarter turn about Z and a shift: model = (-y + 100, x + 200, z - 5).
    const Xform xf{{0, -1, 0, 100, 1, 0, 0, 200, 0, 0, 1, -5}};
    const cam::Toolpath tp = toolpath(0, 0, 10, {line(10, 0, 0, 500), arc(true, ArcPlane::XY, 0, 10, 0, 0, 0, 0)});
    const Backplot bp = buildBackplot(tp, xf, 0.01);
    checkLayout(tp, bp);
    CHECK(samePos(bp.lineVertices[0], 100, 200, 5));
    CHECK(samePos(bp.lineVertices[1], 100, 210, -5));
    CHECK(samePos(bp.lineVertices.back(), 90, 200, -5));
    for (std::uint32_t v = bp.moveFirstVertex[1]; v < bp.moveFirstVertex[2]; ++v) {
        const double dx = bp.lineVertices[v].x - 100.0, dy = bp.lineVertices[v].y - 200.0;
        CHECK(std::abs(std::sqrt(dx * dx + dy * dy) - 10.0) < 3e-5); // float coordinates near 200
        CHECK(dx <= 1e-5);                                           // the arc turns from model +Y towards -X
    }
}

TEST_CASE("arc direction follows G17, G18 and G19", "[sim][backplot][arc]")
{
    // A counter-clockwise quarter arc of radius 10 about the origin in each plane; the middle vertex shows where it
    // went. G18 is seen from +Y, so G3 from +X turns towards -Z; G19 from +Y turns towards +Z.
    struct Case {
        ArcPlane plane;
        double sx, sy, sz, ex, ey, ez;
        double mx, my, mz; // direction of the arc's midpoint
    };
    const double h = std::sqrt(0.5);
    const Case cases[] = {
        {ArcPlane::XY, 10, 0, 0, 0, 10, 0, h, h, 0},
        {ArcPlane::XZ, 0, 0, 10, 10, 0, 0, h, 0, h},   // from +Z to +X (u = z, v = x)
        {ArcPlane::XZ, 10, 0, 0, 0, 0, -10, h, 0, -h}, // from +X to -Z
        {ArcPlane::YZ, 0, 10, 0, 0, 0, 10, 0, h, h},   // from +Y to +Z (u = y, v = z)
    };
    for (const Case& c : cases) {
        for (bool ccw : {true, false}) {
            const cam::Toolpath tp = toolpath(c.sx, c.sy, c.sz, {arc(ccw, c.plane, c.ex, c.ey, c.ez, 0, 0, 0)});
            const Backplot bp = buildBackplot(tp, Xform::identity(), 0.001);
            checkLayout(tp, bp);
            const std::uint32_t n = bp.moveFirstVertex[1] - bp.moveFirstVertex[0];
            // Counter-clockwise is the short way round here (a quarter); clockwise goes three quarters.
            const double sweep = ccw ? 1.5707963267948966 : 3 * 1.5707963267948966;
            CHECK(n == bd::arcSegmentCount(sweep, 10.0, 0.001));
            // The vertex nearest the middle of the sweep.
            const std::size_t mid = bp.moveFirstVertex[0] + n / 2 - 1;
            const BackplotVertex& v = bp.lineVertices[mid];
            const double dot = (v.x * c.mx + v.y * c.my + v.z * c.mz) / 10.0;
            INFO("plane " << static_cast<int>(c.plane) << (ccw ? " ccw" : " cw") << " mid " << v.x << ", " << v.y
                          << ", " << v.z);
            if (ccw) {
                CHECK(dot > 0.99);
            } else {
                CHECK(dot < -0.99); // the long way round passes the opposite side
            }
        }
    }
}

TEST_CASE("degenerate arcs", "[sim][backplot][arc]")
{
    const double tol = 0.01;
    SECTION("zero length, centre on the start: one zero-length vertex")
    {
        const cam::Toolpath tp = toolpath(5, 5, 0, {arc(true, ArcPlane::XY, 5, 5, 0, 5, 5, 0)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), tol);
        checkLayout(tp, bp);
        checkSubs(bp, 0, {{5, 5, 0, MoveKind::ArcCCW, MoveRole::Cut, 1000.0f}});
    }
    SECTION("start equals end: a full circle")
    {
        for (bool ccw : {true, false}) {
            const cam::Toolpath tp = toolpath(10, 0, 0, {arc(ccw, ArcPlane::XY, 10, 0, 0, 0, 0, 0)});
            const Backplot bp = buildBackplot(tp, Xform::identity(), tol);
            checkLayout(tp, bp);
            const std::uint32_t n = bp.moveFirstVertex[1] - bp.moveFirstVertex[0];
            CHECK(n == bd::arcSegmentCount(2 * 3.141592653589793, 10.0, tol));
            CHECK(n > 4);
            // Every vertex on the circle, the vertex a quarter of the way round on +Y (ccw) or -Y (cw).
            for (std::uint32_t v = bp.moveFirstVertex[0]; v < bp.moveFirstVertex[1]; ++v) {
                const BackplotVertex& bv = bp.lineVertices[v];
                CHECK(std::abs(std::hypot(bv.x, bv.y) - 10.0) < 1e-5);
            }
            const BackplotVertex& q = bp.lineVertices[bp.moveFirstVertex[0] + n / 4 - 1]; // about a quarter round
            CHECK((ccw ? q.y : -q.y) > 9.9);
            CHECK(samePos(bp.lineVertices.back(), 10, 0, 0));
        }
    }
    SECTION("end on the start ray with a different radius: a full turn of a spiral")
    {
        const cam::Toolpath tp = toolpath(10, 0, 0, {arc(true, ArcPlane::XY, 10.002, 0, 0, 0, 0, 0)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), tol);
        checkLayout(tp, bp);
        CHECK(
            bp.moveFirstVertex[1] - bp.moveFirstVertex[0] ==
            bd::arcSegmentCount(2 * 3.141592653589793, bd::arcEffectiveRadius(10, 10.002, 2 * 3.141592653589793), tol));
    }
    SECTION("end 1 nm past the start: a tiny arc, as a control reads it")
    {
        Move m = arc(true, ArcPlane::XY, 10, 0, 0, 0, 0, 0);
        m.y = 1; // 1 nm counter-clockwise of the start
        const cam::Toolpath tp = toolpath(10, 0, 0, {m});
        const Backplot bp = buildBackplot(tp, Xform::identity(), tol);
        checkLayout(tp, bp);
        CHECK(bp.moveFirstVertex[1] - bp.moveFirstVertex[0] == 1);
    }
    SECTION("end 1 nm before the start: almost a full circle")
    {
        Move m = arc(true, ArcPlane::XY, 10, 0, 0, 0, 0, 0);
        m.y = -1;
        const cam::Toolpath tp = toolpath(10, 0, 0, {m});
        const Backplot bp = buildBackplot(tp, Xform::identity(), tol);
        checkLayout(tp, bp);
        CHECK(bp.moveFirstVertex[1] - bp.moveFirstVertex[0] == bd::arcSegmentCount(2 * 3.141592653589793, 10, tol));
    }
    SECTION("start or end on the centre: one straight segment")
    {
        const cam::Toolpath a = toolpath(0, 0, 0, {arc(true, ArcPlane::XY, 10, 0, 0, 0, 0, 0)});
        const cam::Toolpath b = toolpath(10, 0, 0, {arc(true, ArcPlane::XY, 0, 0, -1, 0, 0, 0)});
        for (const cam::Toolpath* tp : {&a, &b}) {
            const Backplot bp = buildBackplot(*tp, Xform::identity(), tol);
            checkLayout(*tp, bp);
            CHECK(bp.moveFirstVertex[1] - bp.moveFirstVertex[0] == 1);
        }
    }
    SECTION("a circle much smaller than the tolerance is still a square")
    {
        const cam::Toolpath tp = toolpath(0.01, 0, 0, {arc(false, ArcPlane::XY, 0.01, 0, 0, 0, 0, 0)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 1.0);
        checkLayout(tp, bp);
        CHECK(bp.moveFirstVertex[1] - bp.moveFirstVertex[0] == 4);
    }
    SECTION("helix in every plane: the axis moves linearly with the angle")
    {
        // One full turn of radius 5 about the plane's axis, 2 mm of axial travel.
        struct Case {
            ArcPlane plane;
            double sx, sy, sz, ex, ey, ez;
            int axis;
        };
        const Case cases[] = {{ArcPlane::XY, 5, 0, 0, 5, 0, -2, 2},
                              {ArcPlane::XZ, 0, 0, 5, 0, -2, 5, 1},
                              {ArcPlane::YZ, 0, 5, 0, -2, 5, 0, 0}};
        for (const Case& c : cases) {
            const cam::Toolpath tp = toolpath(c.sx, c.sy, c.sz, {arc(true, c.plane, c.ex, c.ey, c.ez, 0, 0, 0)});
            const Backplot bp = buildBackplot(tp, Xform::identity(), 0.005);
            checkLayout(tp, bp);
            const std::uint32_t a = bp.moveFirstVertex[0], b = bp.moveFirstVertex[1];
            const double n = static_cast<double>(b - a);
            for (std::uint32_t v = a; v < b; ++v) {
                const BackplotVertex& bv = bp.lineVertices[v];
                const double p[3] = {bv.x, bv.y, bv.z};
                const double expect = -2.0 * static_cast<double>(v - a + 1) / n;
                CHECK(std::abs(p[c.axis] - expect) < 1e-5);
            }
        }
    }
    SECTION("a tolerance of zero, below the floor or not a number is the floor")
    {
        const cam::Toolpath tp = toolpath(100, 0, 0, {arc(true, ArcPlane::XY, -100, 0, 0, 0, 0, 0)});
        const std::size_t floor = buildBackplot(tp, Xform::identity(), bd::kMinChordTolMm).lineVertices.size();
        CHECK(buildBackplot(tp, Xform::identity(), 0.0).lineVertices.size() == floor);
        CHECK(buildBackplot(tp, Xform::identity(), -1.0).lineVertices.size() == floor);
        CHECK(buildBackplot(tp, Xform::identity(), 1e-9).lineVertices.size() == floor);
        CHECK(buildBackplot(tp, Xform::identity(), std::numeric_limits<double>::quiet_NaN()).lineVertices.size() ==
              floor);
        CHECK(buildBackplot(tp, Xform::identity(), std::numeric_limits<double>::infinity()).lineVertices.size() ==
              1 + 2); // a half circle at quarter-turn chords
    }
}

TEST_CASE("arcSweep and arcSegmentCount", "[sim][backplot][arc]")
{
    const double pi = 3.141592653589793;
    CHECK(bd::arcSweep(1, 0, 0, 1, true) == pi / 2);
    CHECK(bd::arcSweep(1, 0, 0, 1, false) == 3 * pi / 2);
    CHECK(bd::arcSweep(1, 0, -1, 0, true) == pi);
    CHECK(bd::arcSweep(1, 0, -1, 0, false) == pi);
    CHECK(bd::arcSweep(1, 0, 1, 0, true) == 2 * pi);
    CHECK(bd::arcSweep(1, 0, 1, 0, false) == 2 * pi);
    CHECK(bd::arcSweep(0, 5, 0, -5, true) == pi); // -0 cross product
    CHECK(bd::arcSweep(0, 5, 0, -5, false) == pi);
    CHECK(bd::arcSegmentCount(2 * pi, 10, 1e9) == 4);
    CHECK(bd::arcSegmentCount(1e-12, 10, 0.01) == 1);
    CHECK(bd::arcSegmentCount(2 * pi, 1e9, 1e-4) == bd::kMaxArcSegments);
    CHECK(bd::arcSegmentCount(std::numeric_limits<double>::quiet_NaN(), 10, 0.01) == 1);
}

TEST_CASE("canned cycles expand to the motion of the control", "[sim][backplot][cycle]")
{
    // Start above the first hole's XY at 50; R plane 2, bottom -10.
    const float F = 200.0f;
    const auto R = MoveKind::Rapid;
    const auto L = MoveKind::Linear;
    SECTION("drill (G81), then the next hole positions at the R plane")
    {
        const cam::Toolpath tp = toolpath(0, 0, 50,
                                          {cycle(MoveKind::CycleDrill, 20, 10, -10, 2),
                                           cycle(MoveKind::CycleDrill, 30, 10, -10, 2, 0, 200, 500),
                                           blankMove(MoveKind::CycleCancel, MoveRole::Retract), rapid(30, 10, 50)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        checkSubs(bp, 0,
                  {{20, 10, 50, R, MoveRole::Link, 0},
                   {20, 10, 2, R, MoveRole::Approach, 0},
                   {20, 10, -10, L, MoveRole::Plunge, F},
                   {20, 10, 2, R, MoveRole::Retract, 0}});
        // Already at R: no zero-length rapid to R. A dwell (G82) draws nothing.
        checkSubs(bp, 1,
                  {{30, 10, 2, R, MoveRole::Link, 0},
                   {30, 10, -10, L, MoveRole::Plunge, F},
                   {30, 10, 2, R, MoveRole::Retract, 0}});
        checkSubs(bp, 2, {{30, 10, 2, MoveKind::CycleCancel, MoveRole::Retract, 0}});
        checkSubs(bp, 3, {{30, 10, 50, R, MoveRole::Link, 0}});
    }
    SECTION("full-retract peck (G83)")
    {
        const cam::Toolpath tp = toolpath(20, 10, 2, {cycle(MoveKind::CyclePeck, 20, 10, -10, 2, 5)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        checkSubs(bp, 0,
                  {{20, 10, -3, L, MoveRole::Plunge, F},
                   {20, 10, 2, R, MoveRole::Retract, 0},
                   {20, 10, -2.5, R, MoveRole::Approach, 0},
                   {20, 10, -8, L, MoveRole::Plunge, F},
                   {20, 10, 2, R, MoveRole::Retract, 0},
                   {20, 10, -7.5, R, MoveRole::Approach, 0},
                   {20, 10, -10, L, MoveRole::Plunge, F},
                   {20, 10, 2, R, MoveRole::Retract, 0}});
    }
    SECTION("chip-break peck (G73)")
    {
        const cam::Toolpath tp = toolpath(20, 10, 2, {cycle(MoveKind::CycleChipBreak, 20, 10, -10, 2, 5)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        checkSubs(bp, 0,
                  {{20, 10, -3, L, MoveRole::Plunge, F},
                   {20, 10, -2.5, R, MoveRole::Retract, 0},
                   {20, 10, -8, L, MoveRole::Plunge, F},
                   {20, 10, -7.5, R, MoveRole::Retract, 0},
                   {20, 10, -10, L, MoveRole::Plunge, F},
                   {20, 10, 2, R, MoveRole::Retract, 0}});
    }
    SECTION("tap, bore and ream feed back out at the same feed")
    {
        for (MoveKind kind : {MoveKind::CycleTap, MoveKind::CycleBore, MoveKind::CycleReam}) {
            const cam::Toolpath tp = toolpath(20, 10, 2, {cycle(kind, 20, 10, -10, 2, 0, 125, 100)});
            const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
            checkLayout(tp, bp);
            checkSubs(bp, 0, {{20, 10, -10, L, MoveRole::Plunge, 125.0f}, {20, 10, 2, L, MoveRole::Retract, 125.0f}});
        }
    }
    SECTION("a peck cycle with no peck depth, or one peck deeper than the hole, drills in one feed")
    {
        for (double q : {0.0, -1.0, 12.0, 30.0}) {
            for (MoveKind kind : {MoveKind::CyclePeck, MoveKind::CycleChipBreak}) {
                const cam::Toolpath tp = toolpath(20, 10, 2, {cycle(kind, 20, 10, -10, 2, q)});
                const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
                checkLayout(tp, bp);
                checkSubs(bp, 0, {{20, 10, -10, L, MoveRole::Plunge, F}, {20, 10, 2, R, MoveRole::Retract, 0}});
            }
        }
    }
    SECTION("the number of pecks is capped; the last goes to the bottom")
    {
        Move m = cycle(MoveKind::CyclePeck, 20, 10, -10, 2);
        m.cyclePeck = 1; // 1 nm pecks: 12 million of them
        const cam::Toolpath tp = toolpath(20, 10, 2, {m});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        const std::uint32_t n = bp.moveFirstVertex[1] - bp.moveFirstVertex[0];
        // Each peck is a feed and a rapid back to R; the re-entry stops 0.5 mm above a depth less than 0.5 mm below R,
        // so it is above R and left out.
        CHECK(n == 2 * bd::kMaxPecksDrawn);
        CHECK(samePos(bp.lineVertices[bp.lineVertices.size() - 2], 20, 10, -10));
    }
    SECTION("G98: out to the clearance; from above the clearance, down to it before the R plane")
    {
        const auto g98 = cam::CycleReturn::Initial;
        const cam::Toolpath tp = toolpath(0, 0, 50,
                                          {levels(cycle(MoveKind::CycleDrill, 20, 10, -10, 2), 0, 8, g98),
                                           levels(cycle(MoveKind::CyclePeck, 30, 10, -10, 2, 8), 0, 8, g98),
                                           levels(cycle(MoveKind::CycleBore, 40, 10, -10, 2, 0, 125), 0, 8, g98),
                                           levels(cycle(MoveKind::CycleTap, 50, 10, -10, 2, 0, 125), 0, 8)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        checkSubs(bp, 0,
                  {{20, 10, 50, R, MoveRole::Link, 0},
                   {20, 10, 8, R, MoveRole::Approach, 0},
                   {20, 10, 2, R, MoveRole::Approach, 0},
                   {20, 10, -10, L, MoveRole::Plunge, F},
                   {20, 10, 8, R, MoveRole::Retract, 0}});
        // Pecks retract to R between pecks and to the clearance at the end.
        checkSubs(bp, 1,
                  {{30, 10, 8, R, MoveRole::Link, 0},
                   {30, 10, 2, R, MoveRole::Approach, 0},
                   {30, 10, -6, L, MoveRole::Plunge, F},
                   {30, 10, 2, R, MoveRole::Retract, 0},
                   {30, 10, -5.5, R, MoveRole::Approach, 0},
                   {30, 10, -10, L, MoveRole::Plunge, F},
                   {30, 10, 8, R, MoveRole::Retract, 0}});
        checkSubs(bp, 2,
                  {{40, 10, 8, R, MoveRole::Link, 0},
                   {40, 10, 2, R, MoveRole::Approach, 0},
                   {40, 10, -10, L, MoveRole::Plunge, 125.0f},
                   {40, 10, 2, L, MoveRole::Retract, 125.0f},
                   {40, 10, 8, R, MoveRole::Retract, 0}});
        // G99: the tap feeds out to R and stays there.
        checkSubs(bp, 3,
                  {{50, 10, 8, R, MoveRole::Link, 0},
                   {50, 10, 2, R, MoveRole::Approach, 0},
                   {50, 10, -10, L, MoveRole::Plunge, 125.0f},
                   {50, 10, 2, L, MoveRole::Retract, 125.0f}});
    }
    SECTION("the peck clearance: the move's, else the machine's, else cam::kDefaultPeckClearanceNm")
    {
        Move m = cycle(MoveKind::CycleChipBreak, 20, 10, -10, 2, 5);
        const auto backOff = [&](const Move& mv, const lib::MachineDef* machine) {
            const Backplot bp = buildBackplot(toolpath(20, 10, 2, {mv}), Xform::identity(), 0.01, machine);
            const std::uint32_t a = bp.moveFirstVertex[0];
            return static_cast<double>(bp.lineVertices[a + 1].z) - static_cast<double>(bp.lineVertices[a].z);
        };
        lib::MachineDef machine{};
        CHECK(std::abs(backOff(m, nullptr) - 0.5) < 1e-6);
        CHECK(std::abs(backOff(m, &machine) - 0.5) < 1e-6); // the machine does not know its value
        machine.peckClearance = 0.2;
        CHECK(std::abs(backOff(m, &machine) - 0.2) < 1e-6);
        m.cyclePeckClearance = nm(1.25);
        CHECK(std::abs(backOff(m, &machine) - 1.25) < 1e-6);
    }
    SECTION("the tool ends a cycle at the R plane: the next move starts there")
    {
        const cam::Toolpath tp = toolpath(0, 0, 50, {cycle(MoveKind::CycleDrill, 5, 5, -10, 3), line(5, 5, 20, 900)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        CHECK(samePos(bp.lineVertices[bp.moveFirstVertex[1] - 1], 5, 5, 3));
    }
    SECTION("a cycle with no motion still has a vertex")
    {
        const cam::Toolpath tp = toolpath(5, 5, 3, {cycle(MoveKind::CycleDrill, 5, 5, 3, 3)});
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.01);
        checkLayout(tp, bp);
        checkSubs(bp, 0, {{5, 5, 3, MoveKind::CycleDrill, MoveRole::Plunge, F}});
    }
}

TEST_CASE("over the vertex limit the backplot is built coarse", "[sim][backplot]")
{
    const cam::Toolpath tp =
        toolpath(10, 0, 2, {arc(true, ArcPlane::XY, 10, 0, 2, 0, 0, 2), cycle(MoveKind::CyclePeck, 10, 0, -10, 2, 1)});
    bool coarse = true;
    const Backplot fine = bd::buildBackplotLimited(tp, Xform::identity(), 0.001, nullptr, 1u << 20, &coarse);
    CHECK_FALSE(coarse);
    checkLayout(tp, fine);
    const Backplot rough = bd::buildBackplotLimited(tp, Xform::identity(), 0.001, nullptr, 100, &coarse);
    CHECK(coarse);
    checkLayout(tp, rough);
    CHECK(rough.moveFirstVertex[1] - rough.moveFirstVertex[0] == 4); // quarter-turn chords
    checkSubs(rough, 1,
              {{10, 0, -10, MoveKind::Linear, MoveRole::Plunge, 200.0f},
               {10, 0, 2, MoveKind::Rapid, MoveRole::Retract, 0.0f}});
}
