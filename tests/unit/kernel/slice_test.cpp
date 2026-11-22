// sliceAt, silhouette, faceLoops, edgeCurves and minInternalCornerRadius (task T-050) on reference parts built with the
// kernel: a block with through holes, a pocket with an island and filleted corners, a cone, a sphere, a torus, a
// drafted block, and slices exactly at a floor height, at the top, at a pole and in a turned frame. Each slice is
// compared with its analytic loops to 1 um (acceptance), and lines and arcs must come back as lines and arcs.
#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ray::kernel::test {
namespace {

constexpr double kAcceptance = 1e-3; // mm: 1 um
using Kind = ProfileCurve::Kind;

Xform identity()
{
    return Xform::identity();
}

Shape sweepShape(const Profile& p, double distance, double draft = 0.0)
{
    JobContext ctx;
    return take(extrude(p, distance, draft, ctx)).op.shape;
}
Shape cut(const Shape& a, const Shape& b)
{
    JobContext ctx;
    return take(boolean(BoolOp::Cut, a, b, ctx)).shape;
}
Shape revolved(const Profile& p)
{
    JobContext ctx;
    return take(revolve(p, Vec3{0, 0, 0}, Vec3{0, 0, 1}, 2 * kPi, ctx)).op.shape;
}

std::vector<ProfileLoop> slice(const Shape& s, double z, const Xform& f = Xform::identity())
{
    JobContext ctx;
    return take(sliceAt(s, f, z, ctx));
}

// Block 0..100 x 0..60 x 0..20 with through holes r 5 at (25, 30) and r 8 at (75, 30).
Shape blockWithHoles()
{
    const Profile p = profile(
        xyPlane(), {rectangle(0, 0, 100, 60, 1, 1), circleLoop({25, 30}, 5, 10, 2), circleLoop({75, 30}, 8, 11, 3)});
    return sweepShape(p, 20);
}

// A rounded rectangle loop (corner radius r), counter-clockwise from the bottom edge.
ProfileLoop roundedRect(double x0, double y0, double x1, double y1, double r, std::uint64_t tag)
{
    ProfileLoop l;
    l.tag = tag;
    l.curves.push_back(line({x0 + r, y0}, {x1 - r, y0}, tag));
    l.curves.push_back(arc({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, true, tag + 1));
    l.curves.push_back(line({x1, y0 + r}, {x1, y1 - r}, tag + 2));
    l.curves.push_back(arc({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, true, tag + 3));
    l.curves.push_back(line({x1 - r, y1}, {x0 + r, y1}, tag + 4));
    l.curves.push_back(arc({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, true, tag + 5));
    l.curves.push_back(line({x0, y1 - r}, {x0, y0 + r}, tag + 6));
    l.curves.push_back(arc({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, true, tag + 7));
    return l;
}

// Block 0..100 x 0..80 x 0..30 with a pocket 10..90 x 10..70 (corner radius 6) from z = 20 up, around an island
// 40..60 x 30..50 (corner radius 3) that stands to the top.
Shape pocketPart()
{
    const Shape block = box({0, 0, 0}, {100, 80, 30});
    const Profile tool = profile(xyPlane(20), {roundedRect(10, 10, 90, 70, 6, 1), roundedRect(40, 30, 60, 50, 3, 20)});
    return cut(block, sweepShape(tool, 15));
}

// A loop's curves as text, for messages.
std::string describe(const std::vector<ProfileLoop>& loops)
{
    std::string t;
    for (const ProfileLoop& l : loops) {
        t += "loop:";
        for (const ProfileCurve& c : l.curves) {
            t += c.kind == Kind::Line ? " L" : c.kind == Kind::Arc ? (c.ccw ? " A+" : " A-") : (c.ccw ? " C+" : " C-");
            for (const Vec2& p : c.pts)
                t += " (" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
            if (c.kind != Kind::Line)
                t += " r" + std::to_string(c.radius);
        }
        t += "\n";
    }
    return t;
}

double d(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

// Expected curve: kind, points (as ProfileCurve::pts), radius, ccw. Returns the largest coordinate error, or +inf if
// the kinds, directions or counts differ.
double curveError(const ProfileCurve& got, const ProfileCurve& want)
{
    if (got.kind != want.kind || got.pts.size() != want.pts.size())
        return std::numeric_limits<double>::infinity();
    if (got.kind != Kind::Line && got.ccw != want.ccw)
        return std::numeric_limits<double>::infinity();
    double e = 0.0;
    for (std::size_t i = 0; i < got.pts.size(); ++i)
        e = std::max(e, d(got.pts[i], want.pts[i]));
    if (got.kind != Kind::Line)
        e = std::max(e, std::abs(got.radius - want.radius));
    return e;
}
double loopError(const ProfileLoop& got, const ProfileLoop& want)
{
    if (got.curves.size() != want.curves.size())
        return std::numeric_limits<double>::infinity();
    double e = 0.0;
    for (std::size_t i = 0; i < got.curves.size(); ++i)
        e = std::max(e, curveError(got.curves[i], want.curves[i]));
    return e;
}
ProfileCurve L(Vec2 a, Vec2 b)
{
    return ProfileCurve{Kind::Line, {a, b}, 0.0, false, 0};
}
ProfileCurve A(Vec2 a, Vec2 b, Vec2 c, double r, bool ccw)
{
    return ProfileCurve{Kind::Arc, {a, b, c}, r, ccw, 0};
}
ProfileCurve C(Vec2 c, double r, bool ccw)
{
    return ProfileCurve{Kind::Circle, {c}, r, ccw, 0};
}
ProfileLoop loopOf(std::vector<ProfileCurve> curves)
{
    return ProfileLoop{std::move(curves), 0};
}

void requireLoops(const std::vector<ProfileLoop>& got, const std::vector<ProfileLoop>& want, double tolerance)
{
    INFO("got:\n" << describe(got) << "want:\n" << describe(want));
    REQUIRE(got.size() == want.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < got.size(); ++i)
        worst = std::max(worst, loopError(got[i], want[i]));
    INFO("largest error " << worst << " mm");
    CHECK(worst <= tolerance);
}

// The pocket part's slice between the floor and the top: outer, the pocket wall as a hole (clockwise, starting at its
// lowest vertex (10, 16)), and the island as a region of its own.
std::vector<ProfileLoop> pocketSlice()
{
    return {
        loopOf({L({0, 0}, {100, 0}), L({100, 0}, {100, 80}), L({100, 80}, {0, 80}), L({0, 80}, {0, 0})}),
        loopOf({L({10, 16}, {10, 64}), A({10, 64}, {16, 70}, {16, 64}, 6, false), L({16, 70}, {84, 70}),
                A({84, 70}, {90, 64}, {84, 64}, 6, false), L({90, 64}, {90, 16}),
                A({90, 16}, {84, 10}, {84, 16}, 6, false), L({84, 10}, {16, 10}),
                A({16, 10}, {10, 16}, {16, 16}, 6, false)}),
        loopOf({L({40, 33}, {40, 47}), A({40, 47}, {43, 50}, {43, 47}, 3, false), L({43, 50}, {57, 50}),
                A({57, 50}, {60, 47}, {57, 47}, 3, false), L({60, 47}, {60, 33}),
                A({60, 33}, {57, 30}, {57, 33}, 3, false), L({57, 30}, {43, 30}),
                A({43, 30}, {40, 33}, {43, 33}, 3, false)}),
    };
}

} // namespace

TEST_CASE("sliceAt: a block with through holes gives its outline and circles, exactly", "[slice]")
{
    const Shape part = blockWithHoles();
    const auto loops = slice(part, 10);
    requireLoops(loops,
                 {loopOf({L({0, 0}, {100, 0}), L({100, 0}, {100, 60}), L({100, 60}, {0, 60}), L({0, 60}, {0, 0})}),
                  loopOf({C({25, 30}, 5, false)}), loopOf({C({75, 30}, 8, false)})},
                 1e-9);
}

TEST_CASE("sliceAt: a pocket with an island and filleted corners keeps its arcs as arcs", "[slice]")
{
    const Shape part = pocketPart();
    // The block's outline, the pocket wall as its hole, then the island as a region of its own (it sorts after the
    // block: its lowest vertex is right of the block's), counter-clockwise from its lowest vertex (40, 33).
    auto want = pocketSlice();
    want[2] = loopOf({A({40, 33}, {43, 30}, {43, 33}, 3, true), L({43, 30}, {57, 30}),
                      A({57, 30}, {60, 33}, {57, 33}, 3, true), L({60, 33}, {60, 47}),
                      A({60, 47}, {57, 50}, {57, 47}, 3, true), L({57, 50}, {43, 50}),
                      A({43, 50}, {40, 47}, {43, 47}, 3, true), L({40, 47}, {40, 33})});
    SECTION("between the floor and the top")
    {
        requireLoops(slice(part, 25), want, kAcceptance);
        requireLoops(slice(part, 25), want, 1e-9);
    }
    SECTION("exactly at the floor height the pocket is open (the material just above)")
    {
        requireLoops(slice(part, 20), want, 1e-9);
    }
    SECTION("within 1e-6 mm of the floor counts as the floor")
    {
        requireLoops(slice(part, 20 - 5e-7), want, 1e-9);
        requireLoops(slice(part, 20 + 5e-7), want, 1e-9);
    }
    SECTION("below the floor the section is solid")
    {
        requireLoops(slice(part, 19.9),
                     {loopOf({L({0, 0}, {100, 0}), L({100, 0}, {100, 80}), L({100, 80}, {0, 80}), L({0, 80}, {0, 0})})},
                     1e-9);
    }
    SECTION("at the bottom it is the footprint, at the top and above it nothing")
    {
        CHECK(slice(part, 0).size() == 1);
        CHECK(slice(part, 30).empty());
        CHECK(slice(part, 31).empty());
        CHECK(slice(part, -1).empty());
    }
}

TEST_CASE("sliceAt: a cone, a sphere and a torus cut across their axis give circles", "[slice]")
{
    SECTION("cone (frustum r 20 at z 0 to r 10 at z 30)")
    {
        const Shape cone = revolved(profile(xzPlane(), {polygon({{0, 0}, {20, 0}, {10, 30}, {0, 30}}, 1, 1)}));
        requireLoops(slice(cone, 15), {loopOf({C({0, 0}, 15, true)})}, 1e-9);
        requireLoops(slice(cone, 0), {loopOf({C({0, 0}, 20, true)})}, 1e-9);
    }
    SECTION("sphere r 10 about the origin; the poles touch the plane in a point and give nothing")
    {
        ProfileLoop half;
        half.curves.push_back(arc({0, -10}, {0, 10}, {0, 0}, true, 1));
        half.curves.push_back(line({0, 10}, {0, -10}, 2));
        const Shape ball = revolved(profile(xzPlane(), {half}));
        requireLoops(slice(ball, 6), {loopOf({C({0, 0}, 8, true)})}, 1e-9);
        requireLoops(slice(ball, 0), {loopOf({C({0, 0}, 10, true)})}, 1e-9);
        requireLoops(slice(ball, -6), {loopOf({C({0, 0}, 8, true)})}, 1e-9);
        CHECK(slice(ball, 10).empty());
        CHECK(slice(ball, -10).empty());
    }
    SECTION("torus: tube r 5 about a circle r 30")
    {
        const Shape ring = revolved(profile(xzPlane(), {circleLoop({30, 0}, 5, 1, 1)}));
        requireLoops(slice(ring, 3), {loopOf({C({0, 0}, 34, true)}), loopOf({C({0, 0}, 26, false)})}, 1e-6);
    }
}

TEST_CASE("sliceAt: a drafted wall gives the rectangle moved in by h tan(draft)", "[slice]")
{
    const double draft = 5.0 * kPi / 180.0;
    const Shape part = sweepShape(profile(xyPlane(), {rectangle(0, 0, 40, 20, 1, 1)}), 10, draft);
    for (const double h : {0.0, 2.5, 5.0, 9.0}) {
        const double in = h * std::tan(draft);
        INFO("height " << h);
        requireLoops(slice(part, h),
                     {loopOf({L({in, in}, {40 - in, in}), L({40 - in, in}, {40 - in, 20 - in}),
                              L({40 - in, 20 - in}, {in, 20 - in}), L({in, 20 - in}, {in, in})})},
                     kAcceptance);
        requireLoops(slice(part, h),
                     {loopOf({L({in, in}, {40 - in, in}), L({40 - in, in}, {40 - in, 20 - in}),
                              L({40 - in, 20 - in}, {in, 20 - in}), L({in, 20 - in}, {in, in})})},
                     1e-7);
    }
}

TEST_CASE("sliceAt: the frame decides the plane and the XY of the loops", "[slice]")
{
    // WCS with Z = model +X, X = model +Y, Y = model +Z (right-handed), origin at model (100, 0, 0): heights are model
    // x - 100, and a model point (x, y, z) is at frame (y, z, x - 100).
    Xform f{};
    const double m[12] = {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, -100};
    std::copy(std::begin(m), std::end(m), f.m);
    const Shape part = blockWithHoles();
    // At model x = 25 (frame z = -75) the slice crosses the hole r 5 at (25, 30): a rectangle 0..60 x 0..20 in the
    // frame with a gap from y 25 to 35, i.e. two rectangles.
    const auto loops = slice(part, -75, f);
    requireLoops(loops,
                 {loopOf({L({0, 0}, {25, 0}), L({25, 0}, {25, 20}), L({25, 20}, {0, 20}), L({0, 20}, {0, 0})}),
                  loopOf({L({35, 0}, {60, 0}), L({60, 0}, {60, 20}), L({60, 20}, {35, 20}), L({35, 20}, {35, 0})})},
                 1e-9);
    JobContext ctx;
    Xform scaled = Xform::identity();
    scaled.m[0] = 2.0;
    CHECK(sliceAt(part, scaled, 0, ctx).error().code == ErrorCode::InvalidArgument);
    CHECK(sliceAt(Shape{}, identity(), 0, ctx).error().code == ErrorCode::KernelFailure);
}

TEST_CASE("sliceAt and silhouette honour cancellation", "[slice]")
{
    const Shape part = pocketPart();
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), {}};
    CHECK(sliceAt(part, identity(), 25, ctx).error().code == ErrorCode::Cancelled);
    CHECK(silhouette(part, identity(), ctx).error().code == ErrorCode::Cancelled);
    CHECK(minInternalCornerRadius(part, {0, 0, 1}, ctx).error().code == ErrorCode::Cancelled);
}

TEST_CASE("silhouette: the shadow along frame Z", "[slice]")
{
    JobContext ctx;
    SECTION("through holes show, a blind pocket does not")
    {
        requireLoops(take(silhouette(blockWithHoles(), identity(), ctx)),
                     {loopOf({L({0, 0}, {100, 0}), L({100, 0}, {100, 60}), L({100, 60}, {0, 60}), L({0, 60}, {0, 0})}),
                      loopOf({C({25, 30}, 5, false)}), loopOf({C({75, 30}, 8, false)})},
                     1e-9);
        requireLoops(take(silhouette(pocketPart(), identity(), ctx)),
                     {loopOf({L({0, 0}, {100, 0}), L({100, 0}, {100, 80}), L({100, 80}, {0, 80}), L({0, 80}, {0, 0})})},
                     1e-9);
    }
    SECTION("a sphere's shadow is its equator, a torus's an annulus, a frustum's its base")
    {
        ProfileLoop half;
        half.curves.push_back(arc({0, -10}, {0, 10}, {0, 0}, true, 1));
        half.curves.push_back(line({0, 10}, {0, -10}, 2));
        requireLoops(take(silhouette(revolved(profile(xzPlane(), {half})), identity(), ctx)),
                     {loopOf({C({0, 0}, 10, true)})}, 1e-9);
        requireLoops(take(silhouette(revolved(profile(xzPlane(), {circleLoop({30, 0}, 5, 1, 1)})), identity(), ctx)),
                     {loopOf({C({0, 0}, 35, true)}), loopOf({C({0, 0}, 25, false)})}, 1e-6);
        requireLoops(
            take(silhouette(revolved(profile(xzPlane(), {polygon({{0, 0}, {20, 0}, {10, 30}, {0, 30}}, 1, 1)})),
                            identity(), ctx)),
            {loopOf({C({0, 0}, 20, true)})}, 1e-9);
    }
    SECTION("a cylinder lying across Z casts a rectangle")
    {
        const Shape roller = take(makeCylinder({0, 0, 0}, {1, 0, 0}, 5, 40));
        requireLoops(take(silhouette(roller, identity(), ctx)),
                     {loopOf({L({0, -5}, {40, -5}), L({40, -5}, {40, 5}), L({40, 5}, {0, 5}), L({0, 5}, {0, -5})})},
                     1e-6);
    }
    SECTION("a drafted block's shadow is its base")
    {
        const Shape part = sweepShape(profile(xyPlane(), {rectangle(0, 0, 40, 20, 1, 1)}), 10, 5.0 * kPi / 180.0);
        requireLoops(take(silhouette(part, identity(), ctx)),
                     {loopOf({L({0, 0}, {40, 0}), L({40, 0}, {40, 20}), L({40, 20}, {0, 20}), L({0, 20}, {0, 0})})},
                     1e-7);
    }
}

TEST_CASE("faceLoops and edgeCurves read picked geometry in the frame", "[slice]")
{
    const Shape part = pocketPart();
    // The pocket floor: the face looking up at z = 20.
    std::optional<SubId> floor;
    for (std::uint32_t i = 0; i < part.count(SubKind::Face); ++i) {
        const SurfaceInfo s = take(surfaceInfo(part, SubId{SubKind::Face, i}));
        if (s.type == SurfaceType::Plane && std::abs(s.centroid.z - 20) < 1e-9)
            floor = SubId{SubKind::Face, i};
    }
    REQUIRE(floor);
    const FaceLoops fl = take(faceLoops(part, *floor, identity()));
    CHECK(fl.z == 20.0);
    CHECK(fl.facesUp);
    // The floor's outer boundary is the pocket outline (counter-clockwise), its hole the island's footprint.
    requireLoops(fl.loops,
                 {loopOf({A({10, 16}, {16, 10}, {16, 16}, 6, true), L({16, 10}, {84, 10}),
                          A({84, 10}, {90, 16}, {84, 16}, 6, true), L({90, 16}, {90, 64}),
                          A({90, 64}, {84, 70}, {84, 64}, 6, true), L({84, 70}, {16, 70}),
                          A({16, 70}, {10, 64}, {16, 64}, 6, true), L({10, 64}, {10, 16})}),
                  pocketSlice()[2]},
                 1e-9);
    // A wall face is refused.
    std::optional<SubId> wall;
    for (std::uint32_t i = 0; i < part.count(SubKind::Face) && !wall; ++i) {
        const SurfaceInfo s = take(surfaceInfo(part, SubId{SubKind::Face, i}));
        if (s.type == SurfaceType::Cylinder)
            wall = SubId{SubKind::Face, i};
    }
    REQUIRE(wall);
    CHECK(faceLoops(part, *wall, identity()).error().code == ErrorCode::InvalidArgument);
    // Edges: a corner arc of the pocket at the top stays an arc; a vertical edge gives nothing.
    bool sawArc = false, sawVertical = false;
    for (std::uint32_t i = 0; i < part.count(SubKind::Edge); ++i) {
        const SubId e{SubKind::Edge, i};
        const CurveInfo ci = take(curveInfo(part, e));
        const EdgeCurves ec = take(edgeCurves(part, e, identity()));
        if (ci.type == CurveType::Circle && std::abs(ci.radius - 6) < 1e-9 && std::abs(ci.start.z - 30) < 1e-9) {
            REQUIRE(ec.curves.size() == 1);
            CHECK(ec.curves[0].kind == Kind::Arc);
            CHECK(std::abs(ec.curves[0].radius - 6) < 1e-12);
            CHECK(ec.zMin == 30.0);
            sawArc = true;
        }
        if (ci.type == CurveType::Line && std::abs(ci.start.x - ci.end.x) < 1e-12 &&
            std::abs(ci.start.y - ci.end.y) < 1e-12) {
            CHECK(ec.curves.empty());
            sawVertical = true;
        }
    }
    CHECK(sawArc);
    CHECK(sawVertical);
}

TEST_CASE("minInternalCornerRadius: the smallest concave radius along the tool axis", "[slice]")
{
    JobContext ctx;
    // Pocket corners r 6; the island's r 3 corners are convex and do not count.
    CHECK(take(minInternalCornerRadius(pocketPart(), {0, 0, 1}, ctx)) == 6.0);
    // Holes are concave: the smaller one limits.
    CHECK(take(minInternalCornerRadius(blockWithHoles(), {0, 0, 1}, ctx)) == 5.0);
    CHECK(take(minInternalCornerRadius(blockWithHoles(), {0, 0, -1}, ctx)) == 5.0);
    // Nothing concave: no limit.
    CHECK(std::isinf(take(minInternalCornerRadius(box({0, 0, 0}, {10, 10, 10}), {0, 0, 1}, ctx))));
    // A pocket with sharp corners: 0.
    const Shape sharp = cut(box({0, 0, 0}, {50, 50, 20}), box({10, 10, 10}, {40, 40, 25}));
    CHECK(take(minInternalCornerRadius(sharp, {0, 0, 1}, ctx)) == 0.0);
    // A step across a block: its concave edge runs along Y. Seen down Z it is a floor edge, which a flat end mill
    // cuts (nothing limits); seen along Y it is a sharp internal corner.
    const Shape step = cut(box({0, 0, 0}, {50, 50, 20}), box({25, -1, 10}, {60, 60, 30}));
    CHECK(std::isinf(take(minInternalCornerRadius(step, {0, 0, 1}, ctx))));
    CHECK(take(minInternalCornerRadius(step, {0, 1, 0}, ctx)) == 0.0);
    CHECK(minInternalCornerRadius(sharp, {0, 0, 0}, ctx).error().code == ErrorCode::InvalidArgument);
}

} // namespace ray::kernel::test
