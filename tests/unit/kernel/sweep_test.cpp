// extrude and revolve: closed-form volumes, SweepFaceOrigin, validation and errors (task T-094).
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <shape_testing.h>

#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

constexpr double kVolumeRel = 1e-9;

double volumeOf(const Shape& shape)
{
    return take(massProps(shape)).volume;
}

// The checks every successful sweep must pass: one valid solid, one origin per face in SubId order, side tags that
// are curve tags of the right loop, caps of the outer loop.
void checkSweep(const SweepResult& sweep, const Profile& p, bool caps)
{
    const Shape& shape = sweep.op.shape;
    REQUIRE_FALSE(shape.isNull());
    CHECK(shape.count(SubKind::Solid) == 1);
    CHECK(take(testing::isValidShape(shape)));
    CHECK(sweep.op.warnings.empty());
    CHECK(sweep.op.history.entries.empty());
    REQUIRE(sweep.origins.size() == shape.count(SubKind::Face));
    int startCaps = 0;
    int endCaps = 0;
    const std::vector<TagInfo> tags = tagsOf(p);
    for (std::size_t i = 0; i < sweep.origins.size(); ++i) {
        const SweepFaceOrigin& o = sweep.origins[i];
        CHECK(o.face == SubId{SubKind::Face, static_cast<std::uint32_t>(i)});
        switch (o.role) {
        case SweepFaceOrigin::Role::Side: {
            bool known = false;
            for (const TagInfo& t : tags)
                known = known || (t.curve == o.curveTag && t.loop == o.loopTag);
            CHECK(known);
            break;
        }
        case SweepFaceOrigin::Role::StartCap:
            ++startCaps;
            CHECK(o.loopTag == p.loops.front().tag);
            CHECK(o.curveTag == 0);
            break;
        case SweepFaceOrigin::Role::EndCap:
            ++endCaps;
            CHECK(o.loopTag == p.loops.front().tag);
            CHECK(o.curveTag == 0);
            break;
        }
    }
    CHECK(startCaps == (caps ? 1 : 0));
    CHECK(endCaps == (caps ? 1 : 0));
}

SweepResult extrudeOk(const Profile& p, double distance, double draft = 0.0)
{
    JobContext ctx;
    return take(extrude(p, distance, draft, ctx));
}

SweepResult revolveOk(const Profile& p, Vec3 origin, Vec3 dir, double angle)
{
    JobContext ctx;
    return take(revolve(p, origin, dir, angle, ctx));
}

struct VolumeCase {
    std::string name;
    std::function<SweepResult()> build;
    double expected;
    Profile profile;
    bool caps;
};

std::vector<VolumeCase> volumeCases()
{
    std::vector<VolumeCase> cases;
    const auto addExtrude = [&](std::string name, Profile p, double distance, double draft, double expected) {
        cases.push_back(
            {std::move(name), [p, distance, draft] { return extrudeOk(p, distance, draft); }, expected, p, true});
    };
    const auto addRevolve = [&](std::string name, Profile p, Vec3 o, Vec3 d, double angle, double expected, bool caps) {
        cases.push_back({std::move(name), [p, o, d, angle] { return revolveOk(p, o, d, angle); }, expected, p, caps});
    };

    // 1-3: rectangle up, rectangle down (negative distance), rectangle on a plane whose normal points down.
    addExtrude("rectangle", profile(xyPlane(), {rectangle(0, 0, 30, 20, 1, 100)}), 10.0, 0.0, 6000.0);
    addExtrude("rectangle, negative distance", profile(xyPlane(5), {rectangle(0, 0, 30, 20, 1, 100)}), -7.0, 0.0,
               4200.0);
    addExtrude("rectangle on a downward plane", profile(xyPlaneDown(), {rectangle(-4, -3, 4, 3, 1, 100)}), 2.5, 0.0,
               120.0);
    // 4: circle.
    addExtrude("circle", profile(xyPlane(), {circleLoop({3, 4}, 5.0, 1, 100)}), 12.0, 0.0, kPi * 25.0 * 12.0);
    // 5-6: profiles with holes (nested loops).
    addExtrude("rectangle with two round holes",
               profile(xyPlane(), {rectangle(0, 0, 100, 60, 1, 100), circleLoop({25, 30}, 8.0, 11, 101),
                                   circleLoop({75, 30}, 6.0, 12, 102)}),
               15.0, 0.0, (6000.0 - kPi * 64.0 - kPi * 36.0) * 15.0);
    addExtrude("square frame",
               profile(xyPlane(), {rectangle(0, 0, 40, 40, 1, 100), rectangle(10, 10, 30, 30, 11, 101)}), 5.0, 0.0,
               (1600.0 - 400.0) * 5.0);
    // 7: obround (lines and arcs, tangent joints).
    addExtrude("slot", profile(xyPlane(), {slot(0, 30, 0, 5, 1, 100)}), 4.0, 0.0, (2 * 5.0 * 30.0 + kPi * 25.0) * 4.0);
    // 8: on a tilted plane.
    addExtrude("rectangle on a tilted plane",
               profile(Plane{Vec3{10, -5, 3}, Vec3{1, 1, 1}, Vec3{1, -1, 0}}, {rectangle(-6, -2, 6, 2, 1, 100)}), 9.0,
               0.0, 12.0 * 4.0 * 9.0);
    // 9: triangle.
    addExtrude("triangle", profile(xyPlane(), {polygon({{0, 0}, {12, 0}, {3, 8}}, 1, 100)}), 3.0, 0.0,
               0.5 * 12.0 * 8.0 * 3.0);
    // 10: D shape, the arc given clockwise.
    {
        ProfileLoop d;
        d.tag = 100;
        d.curves.push_back(line({-10, 0}, {10, 0}, 1));
        d.curves.push_back(arc({-10, 0}, {10, 0}, {0, 0}, false, 2)); // given start-to-end; it runs back over the top
        addExtrude("D shape with a clockwise arc", profile(xyPlane(), {d}), 6.0, 0.0, 0.5 * kPi * 100.0 * 6.0);
    }
    // 11: quadratic B-spline (a Bezier) closed by its chord: area = 2/3 of the control triangle.
    {
        ProfileLoop b;
        b.tag = 100;
        b.curves.push_back(bspline({{0, 0}, {10, 12}, {20, 0}}, 1));
        b.curves.push_back(line({20, 0}, {0, 0}, 2));
        addExtrude("B-spline", profile(xyPlane(), {b}), 5.0, 0.0, (2.0 / 3.0) * (0.5 * 20.0 * 12.0) * 5.0);
    }
    // 12-14: drafted square (inward and outward) and drafted circle (cone frustum).
    {
        const double s = 20.0;
        const double h = 10.0;
        const double a = 5.0 * kPi / 180.0;
        const double t = std::tan(a);
        const double inward = s * s * h - 2.0 * s * h * h * t + (4.0 / 3.0) * h * h * h * t * t;
        const double outward = s * s * h + 2.0 * s * h * h * t + (4.0 / 3.0) * h * h * h * t * t;
        addExtrude("square, 5 degree draft", profile(xyPlane(), {rectangle(0, 0, s, s, 1, 100)}), h, a, inward);
        addExtrude("square, -5 degree draft", profile(xyPlane(), {rectangle(0, 0, s, s, 1, 100)}), h, -a, outward);
        const double r0 = 8.0;
        const double r1 = r0 - h * t;
        addExtrude("circle, 5 degree draft", profile(xyPlane(), {circleLoop({0, 0}, r0, 1, 100)}), h, a,
                   kPi * h * (r0 * r0 + r0 * r1 + r1 * r1) / 3.0);
    }

    // Revolve about Z, profiles in the XZ plane (local x = radius, local y = height).
    const Vec3 o{0, 0, 0};
    const Vec3 z{0, 0, 1};
    // 15-16: washer, full and a quarter turn.
    addRevolve("washer, full turn", profile(xzPlane(), {rectangle(5, 0, 9, 4, 1, 100)}), o, z, 2 * kPi,
               kPi * (81.0 - 25.0) * 4.0, false);
    addRevolve("washer, quarter turn", profile(xzPlane(), {rectangle(5, 0, 9, 4, 1, 100)}), o, z, kPi / 2,
               kPi * (81.0 - 25.0) * 4.0 / 4.0, true);
    // 17-18: torus, full and half.
    addRevolve("torus", profile(xzPlane(), {circleLoop({20, 0}, 3.0, 1, 100)}), o, z, 2 * kPi,
               2.0 * kPi * kPi * 20.0 * 9.0, false);
    addRevolve("half torus", profile(xzPlane(), {circleLoop({20, 0}, 3.0, 1, 100)}), o, z, kPi, kPi * kPi * 20.0 * 9.0,
               true);
    // 19: rectangle touching the axis: a cylinder.
    addRevolve("cylinder from a rectangle on the axis", profile(xzPlane(), {rectangle(0, 0, 6, 10, 1, 100)}), o, z,
               2 * kPi, kPi * 36.0 * 10.0, false);
    // 20: triangle touching the axis, revolved backwards by 120 degrees: a third of a cone.
    addRevolve("cone sector, negative angle", profile(xzPlane(), {polygon({{0, 0}, {6, 0}, {0, 9}}, 1, 100)}), o, z,
               -2.0 * kPi / 3.0, kPi * 36.0 * 9.0 / 3.0 / 3.0, true);
    return cases;
}

} // namespace

TEST_CASE("sweep volumes match closed form on 20 cases", "[kernel][sweep]")
{
    const std::vector<VolumeCase> cases = volumeCases();
    REQUIRE(cases.size() == 20);
    for (const VolumeCase& c : cases) {
        INFO(c.name);
        const SweepResult sweep = c.build();
        const double volume = volumeOf(sweep.op.shape);
        INFO("volume " << volume << " expected " << c.expected);
        CHECK(std::abs(volume - c.expected) <= kVolumeRel * c.expected);
        checkSweep(sweep, c.profile, c.caps);
    }
}

TEST_CASE("extrude side faces map to their profile curves and caps to the outer loop", "[kernel][sweep]")
{
    const Profile p = profile(xyPlane(2.0), {rectangle(0, 0, 100, 60, 1, 100), circleLoop({25, 30}, 8.0, 11, 101),
                                             slot(60, 80, 30, 5, 21, 102)});
    const SweepResult sweep = extrudeOk(p, 15.0);
    checkSweep(sweep, p, true);
    // 4 rectangle sides + 1 hole wall + 4 slot walls + 2 caps.
    REQUIRE(sweep.origins.size() == 11);
    std::set<std::uint64_t> sideTags;
    for (const SweepFaceOrigin& o : sweep.origins) {
        const SurfaceInfo info = take(surfaceInfo(sweep.op.shape, o.face));
        if (o.role == SweepFaceOrigin::Role::StartCap) {
            CHECK(info.type == SurfaceType::Plane);
            CHECK(close(info.centroid.z, 2.0, 1e-12));
            continue;
        }
        if (o.role == SweepFaceOrigin::Role::EndCap) {
            CHECK(info.type == SurfaceType::Plane);
            CHECK(close(info.centroid.z, 17.0, 1e-12));
            continue;
        }
        sideTags.insert(o.curveTag);
        INFO("curve tag " << o.curveTag);
        CHECK(close(info.centroid.z, 9.5, 1e-9));
        const ProfileCurve* source = nullptr;
        for (const TagInfo& t : tagsOf(p))
            if (t.curve == o.curveTag)
                source = t.source;
        REQUIRE(source != nullptr);
        if (source->kind == ProfileCurve::Kind::Line) {
            CHECK(info.type == SurfaceType::Plane);
            const Vec2 mid{(source->pts[0].x + source->pts[1].x) / 2, (source->pts[0].y + source->pts[1].y) / 2};
            CHECK(close(Vec3{info.centroid.x, info.centroid.y, 0}, Vec3{mid.x, mid.y, 0}, 1e-9));
        } else {
            CHECK(info.type == SurfaceType::Cylinder);
            const Vec2 centre = source->kind == ProfileCurve::Kind::Circle ? source->pts[0] : source->pts[2];
            const double radius = source->kind == ProfileCurve::Kind::Circle ? source->radius : 5.0;
            CHECK(close(info.radius, radius, 1e-9));
            CHECK(close(Vec3{info.origin.x, info.origin.y, 0}, Vec3{centre.x, centre.y, 0}, 1e-9));
        }
    }
    CHECK(sideTags == std::set<std::uint64_t>{1, 2, 3, 4, 11, 21, 22, 23, 24});
    for (const SweepFaceOrigin& o : sweep.origins)
        if (o.role == SweepFaceOrigin::Role::Side)
            CHECK(o.loopTag == (o.curveTag < 10 ? 100u : o.curveTag < 20 ? 101u : 102u));
}

TEST_CASE("extrude goes both ways along the normal", "[kernel][sweep]")
{
    const Profile p = profile(xyPlane(1.0), {rectangle(0, 0, 10, 10, 1, 100)});
    const Box3 up = bounds(extrudeOk(p, 4.0).op.shape);
    CHECK(close(up.min.z, 1.0, 1e-12));
    CHECK(close(up.max.z, 5.0, 1e-12));
    const SweepResult downSweep = extrudeOk(p, -4.0);
    const Box3 down = bounds(downSweep.op.shape);
    CHECK(close(down.min.z, -3.0, 1e-12));
    CHECK(close(down.max.z, 1.0, 1e-12));
    // The start cap is always the one on the profile plane.
    for (const SweepFaceOrigin& o : downSweep.origins) {
        const SurfaceInfo info = take(surfaceInfo(downSweep.op.shape, o.face));
        if (o.role == SweepFaceOrigin::Role::StartCap)
            CHECK(close(info.centroid.z, 1.0, 1e-12));
        if (o.role == SweepFaceOrigin::Role::EndCap)
            CHECK(close(info.centroid.z, -3.0, 1e-12));
    }
    // A symmetric extrusion is a shifted plane and the full distance.
    const Profile shifted = profile(xyPlane(1.0 - 2.0), {rectangle(0, 0, 10, 10, 1, 100)});
    const Box3 both = bounds(extrudeOk(shifted, 4.0).op.shape);
    CHECK(close(both.min.z, -1.0, 1e-12));
    CHECK(close(both.max.z, 3.0, 1e-12));
}

TEST_CASE("a drafted extrusion keeps one origin per face and tapers the right way", "[kernel][sweep]")
{
    const double a = 3.0 * kPi / 180.0;
    const Profile p = profile(xyPlane(), {rectangle(0, 0, 40, 30, 1, 100), circleLoop({20, 15}, 6.0, 11, 101)});
    const SweepResult sweep = extrudeOk(p, 10.0, a);
    checkSweep(sweep, p, true);
    double startArea = 0.0;
    double endArea = 0.0;
    for (const SweepFaceOrigin& o : sweep.origins) {
        const SurfaceInfo info = take(surfaceInfo(sweep.op.shape, o.face));
        if (o.role == SweepFaceOrigin::Role::StartCap)
            startArea = info.area;
        else if (o.role == SweepFaceOrigin::Role::EndCap)
            endArea = info.area;
        else if (o.curveTag == 11)
            CHECK(info.type == SurfaceType::Cone);
        else
            CHECK(info.type == SurfaceType::Plane);
    }
    // Outer walls lean in, the hole widens: the end cap is smaller.
    const double t = std::tan(a);
    const double expectedEnd = (40.0 - 20.0 * t) * (30.0 - 20.0 * t) - kPi * std::pow(6.0 + 10.0 * t, 2);
    CHECK(close(startArea, 1200.0 - kPi * 36.0, 1e-9));
    CHECK(close(endArea, expectedEnd, 1e-9));
}

TEST_CASE("profile curves may come in either direction and loops in either sense", "[kernel][sweep]")
{
    ProfileLoop scrambled;
    scrambled.tag = 7;
    scrambled.curves.push_back(line({10, 0}, {0, 0}, 1));  // reversed
    scrambled.curves.push_back(line({10, 8}, {10, 0}, 2)); // reversed
    scrambled.curves.push_back(line({10, 8}, {0, 8}, 3));
    scrambled.curves.push_back(line({0, 0}, {0, 8}, 4));                       // reversed
    const ProfileLoop hole = polygon({{2, 2}, {2, 6}, {6, 6}, {6, 2}}, 11, 8); // counter-clockwise as a hole
    const Profile p = profile(xyPlane(), {scrambled, hole});
    const SweepResult sweep = extrudeOk(p, 2.0);
    checkSweep(sweep, p, true);
    CHECK(close(volumeOf(sweep.op.shape), (80.0 - 16.0) * 2.0, 1e-12));
}

TEST_CASE("revolve side faces map to their curves; a curve on the axis makes no face", "[kernel][sweep]")
{
    const Profile p = profile(xzPlane(), {rectangle(0, 0, 6, 10, 1, 100)}); // curve 4 lies on the axis
    const SweepResult full = revolveOk(p, Vec3{0, 0, 0}, Vec3{0, 0, 1}, 2 * kPi);
    checkSweep(full, p, false);
    std::set<std::uint64_t> tags;
    for (const SweepFaceOrigin& o : full.origins)
        tags.insert(o.curveTag);
    CHECK(tags == std::set<std::uint64_t>{1, 2, 3});
    for (const SweepFaceOrigin& o : full.origins) {
        const SurfaceInfo info = take(surfaceInfo(full.op.shape, o.face));
        if (o.curveTag == 2) {
            CHECK(info.type == SurfaceType::Cylinder);
            CHECK(close(info.radius, 6.0, 1e-12));
        } else {
            CHECK(info.type == SurfaceType::Plane);
            CHECK(close(info.centroid.z, o.curveTag == 1 ? 0.0 : 10.0, 1e-12));
        }
    }
    const SweepResult part = revolveOk(p, Vec3{0, 0, 0}, Vec3{0, 0, 1}, kPi / 3);
    checkSweep(part, p, true);
    CHECK(part.origins.size() == 5);
}

TEST_CASE("bad profiles are rejected with a message, never thrown", "[kernel][sweep][errors]")
{
    JobContext ctx;
    const auto expectError = [&](const Profile& p, ErrorCode code, double distance = 5.0, double draft = 0.0) {
        auto result = extrude(p, distance, draft, ctx);
        REQUIRE_FALSE(result.has_value());
        INFO(result.error().message);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().message.empty());
    };
    const Profile good = profile(xyPlane(), {rectangle(0, 0, 10, 10, 1, 100)});
    SECTION("no loops")
    {
        expectError(profile(xyPlane(), {}), ErrorCode::InvalidArgument);
    }
    SECTION("an open loop")
    {
        ProfileLoop open;
        open.curves = {line({0, 0}, {10, 0}, 1), line({10, 0}, {10, 10}, 2), line({10, 10}, {0, 10}, 3)};
        expectError(profile(xyPlane(), {open}), ErrorCode::InvalidArgument);
    }
    SECTION("a gap between curves")
    {
        ProfileLoop gap = rectangle(0, 0, 10, 10, 1, 100);
        gap.curves[2].pts[0] = Vec2{10, 10.01};
        expectError(profile(xyPlane(), {gap}), ErrorCode::InvalidArgument);
    }
    SECTION("a loop that crosses itself")
    {
        expectError(profile(xyPlane(), {polygon({{0, 0}, {10, 10}, {10, 0}, {0, 10}}, 1, 100)}),
                    ErrorCode::InvalidArgument);
    }
    SECTION("a hole outside the boundary")
    {
        expectError(profile(xyPlane(), {rectangle(0, 0, 10, 10, 1, 100), circleLoop({30, 5}, 2, 11, 101)}),
                    ErrorCode::InvalidArgument);
    }
    SECTION("a hole that crosses the boundary")
    {
        expectError(profile(xyPlane(), {rectangle(0, 0, 10, 10, 1, 100), circleLoop({10, 5}, 2, 11, 101)}),
                    ErrorCode::InvalidArgument);
    }
    SECTION("an island inside a hole")
    {
        expectError(profile(xyPlane(), {rectangle(0, 0, 50, 50, 1, 100), circleLoop({25, 25}, 10, 11, 101),
                                        circleLoop({25, 25}, 3, 12, 102)}),
                    ErrorCode::InvalidArgument);
    }
    SECTION("an arc whose end is off its circle")
    {
        ProfileLoop d;
        d.curves = {line({-10, 0}, {10, 0}, 1), arc({10, 0}, {-10, 0.5}, {0, 0}, true, 2)};
        expectError(profile(xyPlane(), {d}), ErrorCode::InvalidArgument);
    }
    SECTION("a circle joined to other curves")
    {
        ProfileLoop mixed = rectangle(0, 0, 10, 10, 1, 100);
        mixed.curves.push_back(circle({5, 5}, 2, 9));
        expectError(profile(xyPlane(), {mixed}), ErrorCode::InvalidArgument);
    }
    SECTION("a degenerate plane")
    {
        expectError(profile(Plane{Vec3{0, 0, 0}, Vec3{0, 0, 0}, Vec3{1, 0, 0}}, {rectangle(0, 0, 10, 10, 1, 100)}),
                    ErrorCode::InvalidArgument);
        expectError(profile(Plane{Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 0, 2}}, {rectangle(0, 0, 10, 10, 1, 100)}),
                    ErrorCode::InvalidArgument);
    }
    SECTION("zero, infinite or NaN distance and draft")
    {
        expectError(good, ErrorCode::InvalidArgument, 0.0);
        expectError(good, ErrorCode::InvalidArgument, std::nan(""));
        expectError(good, ErrorCode::InvalidArgument, 5.0, kPi / 2);
        expectError(good, ErrorCode::InvalidArgument, 5.0, std::nan(""));
    }
    SECTION("a draft that closes the walls")
    {
        // A 10 mm square drafted 45 degrees over 20 mm would shrink to nothing at 5 mm.
        expectError(good, ErrorCode::KernelFailure, 20.0, kPi / 4);
    }
    SECTION("a draft on a B-spline profile")
    {
        ProfileLoop b;
        b.curves = {bspline({{0, 0}, {10, 12}, {20, 0}}, 1), line({20, 0}, {0, 0}, 2)};
        expectError(profile(xyPlane(), {b}), ErrorCode::InvalidArgument, 5.0, 0.05);
    }
}

TEST_CASE("bad revolve arguments are rejected with a message", "[kernel][sweep][errors]")
{
    JobContext ctx;
    const Profile p = profile(xzPlane(), {rectangle(5, 0, 9, 4, 1, 100)});
    const auto expectError = [&](Vec3 origin, Vec3 dir, double angle) {
        auto result = revolve(p, origin, dir, angle, ctx);
        REQUIRE_FALSE(result.has_value());
        INFO(result.error().message);
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(result.error().message.empty());
    };
    SECTION("axis out of the plane")
    {
        expectError(Vec3{0, 0, 0}, Vec3{0, 1, 0}, kPi);
    }
    SECTION("axis parallel to the plane but off it")
    {
        expectError(Vec3{0, 3, 0}, Vec3{0, 0, 1}, kPi);
    }
    SECTION("profile crossing the axis")
    {
        expectError(Vec3{7, 0, 0}, Vec3{0, 0, 1}, kPi);
    }
    SECTION("zero axis")
    {
        expectError(Vec3{0, 0, 0}, Vec3{0, 0, 0}, kPi);
    }
    SECTION("zero or too large angle")
    {
        expectError(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 0.0);
        expectError(Vec3{0, 0, 0}, Vec3{0, 0, 1}, 7.0);
    }
}

TEST_CASE("a cancelled sweep returns Cancelled", "[kernel][sweep]")
{
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), Progress()};
    const Profile p = profile(xyPlane(), {rectangle(0, 0, 10, 10, 1, 100)});
    auto e = extrude(p, 5.0, 0.0, ctx);
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == ErrorCode::Cancelled);
    auto r = revolve(profile(xzPlane(), {rectangle(5, 0, 9, 4, 1, 100)}), Vec3{0, 0, 0}, Vec3{0, 0, 1}, kPi, ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("sweep progress ends at 1", "[kernel][sweep]")
{
    double last = -1.0;
    Progress progress([&](double fraction, std::string_view) { last = fraction; });
    JobContext ctx{CancelToken(), progress};
    take(extrude(profile(xyPlane(), {rectangle(0, 0, 10, 10, 1, 100)}), 5.0, 0.0, ctx));
    CHECK(last == 1.0);
}
