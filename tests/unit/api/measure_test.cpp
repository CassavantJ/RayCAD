// Measure and analysis tools (task T-022): ray_api/measure.h on reference parts with known values, the display
// formatting in inch and mm, and the measure.* actions on a real corpus part through a headless app.
//
// Reference parts: kernel::makeBox / makeCylinder and the builders of src/kernel/shape_testing.h (a hole plate, a
// wedge, a chamfered box, a cone, a sphere, a torus, a tube). Every expected value is worked out here from the
// dimensions the part was built with, never read back from the kernel. Lengths must match to 1e-6 mm.
#include "api_test_util.h"
#include "json.h"
#include "shape_testing.h"

#include <ray/kernel.h>
#include <ray_api/measure.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace ray;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace measure = ray::api::measure;
using kernel::SubId;
using kernel::SubKind;

namespace {

constexpr double kTol = 1e-6;    // mm
constexpr double kAngTol = 1e-9; // rad
constexpr double kPi = 3.14159265358979323846;

JobContext& ctx()
{
    static JobContext c;
    return c;
}

measure::Element el(const kernel::Shape& s, SubKind kind, std::uint32_t index)
{
    return measure::Element{s, SubId{kind, index}, "part"};
}

measure::Measurement ok(Result<measure::Measurement> r)
{
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    return std::move(*r);
}

double num(const measure::Measurement& m, const char* key)
{
    const measure::Value* v = m.find(key);
    INFO("value " << key << " of " << m.title);
    REQUIRE(v != nullptr);
    return v->number;
}

Vec3 vec(const measure::Measurement& m, const char* key)
{
    const measure::Value* v = m.find(key);
    INFO("value " << key << " of " << m.title);
    REQUIRE(v != nullptr);
    return v->vector;
}

std::string primaryKey(const measure::Measurement& m)
{
    const measure::Value* v = m.primary();
    return v ? v->key : std::string();
}

void requireVec(Vec3 a, Vec3 b, double tol = kTol)
{
    CHECK_THAT(a.x, WithinAbs(b.x, tol));
    CHECK_THAT(a.y, WithinAbs(b.y, tol));
    CHECK_THAT(a.z, WithinAbs(b.z, tol));
}

// The planar face on the side `n` of the part: its plane is perpendicular to `n` and its centroid is nearest `at`
// (by default the middle of the bounding box side that `n` points to). Found by position, not by the kernel's normals.
std::uint32_t planeFace(const kernel::Shape& s, Vec3 n, std::optional<Vec3> at = std::nullopt)
{
    if (!at) {
        const Box3 b = kernel::bounds(s);
        at = Vec3{(b.min.x + b.max.x) / 2 + n.x * (b.max.x - b.min.x) / 2,
                  (b.min.y + b.max.y) / 2 + n.y * (b.max.y - b.min.y) / 2,
                  (b.min.z + b.max.z) / 2 + n.z * (b.max.z - b.min.z) / 2};
    }
    std::optional<std::uint32_t> best;
    double bestD = 1e300;
    for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i) {
        auto info = kernel::surfaceInfo(s, SubId{SubKind::Face, i});
        REQUIRE(info);
        if (info->type != kernel::SurfaceType::Plane)
            continue;
        const Vec3 a = info->axis;
        const double sine = std::hypot(a.y * n.z - a.z * n.y, a.z * n.x - a.x * n.z, a.x * n.y - a.y * n.x);
        if (sine > 1e-9)
            continue;
        const double d = std::hypot(info->centroid.x - at->x, info->centroid.y - at->y, info->centroid.z - at->z);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    INFO("planar face with normal " << n.x << "," << n.y << "," << n.z);
    REQUIRE(best.has_value());
    return *best;
}

// Faces of a surface type, with their info.
std::vector<std::pair<std::uint32_t, kernel::SurfaceInfo>> facesOf(const kernel::Shape& s, kernel::SurfaceType type)
{
    std::vector<std::pair<std::uint32_t, kernel::SurfaceInfo>> out;
    for (std::uint32_t i = 0; i < s.count(SubKind::Face); ++i) {
        auto info = kernel::surfaceInfo(s, SubId{SubKind::Face, i});
        REQUIRE(info);
        if (info->type == type)
            out.emplace_back(i, *info);
    }
    return out;
}

// The cylinder face whose axis passes nearest (x, y) (axes along Z).
std::uint32_t holeAt(const kernel::Shape& s, double x, double y)
{
    std::optional<std::uint32_t> best;
    double bestD = 1e300;
    for (const auto& [i, info] : facesOf(s, kernel::SurfaceType::Cylinder)) {
        const double d = std::hypot(info.centroid.x - x, info.centroid.y - y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    INFO("hole at " << x << ", " << y);
    REQUIRE(best.has_value());
    REQUIRE(bestD < 1e-6);
    return *best;
}

// An edge of the shape between two given points (either direction).
std::uint32_t edgeBetween(const kernel::Shape& s, Vec3 a, Vec3 b)
{
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < s.count(SubKind::Edge); ++i) {
        auto c = kernel::curveInfo(s, SubId{SubKind::Edge, i});
        REQUIRE(c);
        const auto near = [](Vec3 p, Vec3 q) {
            return std::hypot(p.x - q.x, p.y - q.y, p.z - q.z) < 1e-7;
        };
        if ((near(c->start, a) && near(c->end, b)) || (near(c->start, b) && near(c->end, a)))
            found = i;
    }
    INFO("an edge between the points");
    REQUIRE(found.has_value());
    return *found;
}

std::uint32_t vertexAt(const kernel::Shape& s, Vec3 p)
{
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < s.count(SubKind::Vertex); ++i) {
        auto v = kernel::vertexPoint(s, SubId{SubKind::Vertex, i});
        REQUIRE(v);
        if (std::hypot(v->x - p.x, v->y - p.y, v->z - p.z) < 1e-7)
            found = i;
    }
    INFO("a vertex there");
    REQUIRE(found.has_value());
    return *found;
}

kernel::Shape box(Vec3 lo, Vec3 hi)
{
    auto b = kernel::makeBox(lo, hi);
    REQUIRE(b);
    return *b;
}

kernel::Shape testPart(int index)
{
    auto p = kernel::testing::makeTestPart(index);
    REQUIRE(p);
    return *p;
}

std::string fmt(const measure::Measurement& m, const char* key, UnitFormat f)
{
    const measure::Value* v = m.find(key);
    REQUIRE(v != nullptr);
    return measure::formatValue(*v, f);
}

constexpr UnitFormat kInch{UnitSystem::Inch, 4};
constexpr UnitFormat kMm{UnitSystem::Metric, 3};

} // namespace

TEST_CASE("box: face to face distance is the exact size; mass properties and bounding box", "[api][measure][box]")
{
    // Sizes that are not round numbers.
    const Vec3 lo{-3.25, 1.5, 0.75};
    const Vec3 hi{34.25, 23.75, 13.875};
    const Vec3 size{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    const kernel::Shape s = box(lo, hi);

    SECTION("opposite faces: plane to plane = size, minimum distance = size, components along the axis")
    {
        const struct {
            Vec3 n;
            double expected;
            const char* component;
        } cases[] = {{{1, 0, 0}, size.x, "dx"}, {{0, 1, 0}, size.y, "dy"}, {{0, 0, 1}, size.z, "dz"}};
        for (const auto& c : cases) {
            const std::uint32_t f1 = planeFace(s, Vec3{-c.n.x, -c.n.y, -c.n.z});
            const std::uint32_t f2 = planeFace(s, c.n);
            const measure::Measurement m =
                ok(measure::distance(el(s, SubKind::Face, f1), el(s, SubKind::Face, f2), ctx()));
            CHECK(m.kind == measure::Kind::Distance);
            CHECK_THAT(num(m, "planeDistance"), WithinAbs(c.expected, kTol));
            CHECK_THAT(num(m, "distance"), WithinAbs(c.expected, kTol));
            CHECK_THAT(std::abs(num(m, c.component)), WithinAbs(c.expected, kTol));
            CHECK(primaryKey(m) == "planeDistance");
            // Witness: a line of that length, and a label on it.
            REQUIRE_FALSE(m.witness.lines.empty());
            REQUIRE_FALSE(m.witness.labels.empty());
            CHECK(m.witness.labels.front().key == "planeDistance");
        }
    }
    SECTION("adjacent faces: not parallel, they touch; the angle is 90 degrees")
    {
        const std::uint32_t top = planeFace(s, Vec3{0, 0, 1});
        const std::uint32_t front = planeFace(s, Vec3{0, -1, 0});
        const measure::Measurement d =
            ok(measure::distance(el(s, SubKind::Face, top), el(s, SubKind::Face, front), ctx()));
        CHECK_THAT(num(d, "distance"), WithinAbs(0.0, kTol));
        CHECK(d.find("planeDistance") == nullptr);
        const measure::Measurement a =
            ok(measure::angle(el(s, SubKind::Face, top), el(s, SubKind::Face, front), ctx()));
        CHECK_THAT(num(a, "angle"), WithinAbs(kPi / 2, kAngTol));
        CHECK_THAT(num(a, "supplement"), WithinAbs(kPi / 2, kAngTol));
        CHECK_THAT(num(a, "edgeAngle"), WithinAbs(kPi / 2, kAngTol));
        CHECK(primaryKey(a) == "edgeAngle");
        CHECK(a.witness.lines.size() > 10); // the arc
        CHECK(fmt(a, "angle", kInch) == "90.000\u00b0");
    }
    SECTION("vertex to vertex: the space diagonal with its components; point to plane; edge to edge")
    {
        const std::uint32_t v1 = vertexAt(s, lo);
        const std::uint32_t v2 = vertexAt(s, hi);
        const measure::Measurement m =
            ok(measure::distance(el(s, SubKind::Vertex, v1), el(s, SubKind::Vertex, v2), ctx()));
        CHECK_THAT(num(m, "distance"), WithinAbs(std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z), kTol));
        requireVec(Vec3{num(m, "dx"), num(m, "dy"), num(m, "dz")}, size);
        requireVec(vec(m, "p1"), lo);
        requireVec(vec(m, "p2"), hi);

        const std::uint32_t top = planeFace(s, Vec3{0, 0, 1});
        const measure::Measurement pf =
            ok(measure::distance(el(s, SubKind::Vertex, v1), el(s, SubKind::Face, top), ctx()));
        CHECK_THAT(num(pf, "distance"), WithinAbs(size.z, kTol));
        CHECK_THAT(num(pf, "pointToPlane"), WithinAbs(size.z, kTol));

        // Two opposite parallel edges of the top face: line to line = size y.
        const std::uint32_t e1 = edgeBetween(s, Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z});
        const std::uint32_t e2 = edgeBetween(s, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z});
        const measure::Measurement ee =
            ok(measure::distance(el(s, SubKind::Edge, e1), el(s, SubKind::Edge, e2), ctx()));
        CHECK_THAT(num(ee, "lineDistance"), WithinAbs(size.y, kTol));
        CHECK_THAT(num(ee, "distance"), WithinAbs(size.y, kTol));
        // Point to edge: the far corner to a bottom edge.
        const std::uint32_t bottomEdge = edgeBetween(s, Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z});
        const measure::Measurement pe =
            ok(measure::distance(el(s, SubKind::Vertex, v2), el(s, SubKind::Edge, bottomEdge), ctx()));
        CHECK_THAT(num(pe, "distance"), WithinAbs(std::hypot(size.y, size.z), kTol));
        CHECK_THAT(num(pe, "pointToLine"), WithinAbs(std::hypot(size.y, size.z), kTol));
        // Edge to face: a top edge parallel to the bottom face.
        const std::uint32_t bottom = planeFace(s, Vec3{0, 0, -1});
        const measure::Measurement ef =
            ok(measure::distance(el(s, SubKind::Edge, e1), el(s, SubKind::Face, bottom), ctx()));
        CHECK_THAT(num(ef, "lineToPlane"), WithinAbs(size.z, kTol));
        CHECK_THAT(num(ef, "distance"), WithinAbs(size.z, kTol));
        // Two edges meeting at a corner: the included angle is 90 degrees.
        const std::uint32_t e3 = edgeBetween(s, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z});
        const measure::Measurement ll = ok(measure::angle(el(s, SubKind::Edge, e1), el(s, SubKind::Edge, e3), ctx()));
        CHECK_THAT(num(ll, "includedAngle"), WithinAbs(kPi / 2, kAngTol));
        CHECK_THAT(num(ll, "angle"), WithinAbs(kPi / 2, kAngTol));
        // A vertical edge and the top face: 90 degrees to the plane.
        const std::uint32_t vertical = edgeBetween(s, Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z});
        const measure::Measurement lp =
            ok(measure::angle(el(s, SubKind::Edge, vertical), el(s, SubKind::Face, top), ctx()));
        CHECK_THAT(num(lp, "angle"), WithinAbs(kPi / 2, kAngTol));
        CHECK_THAT(num(lp, "toNormal"), WithinAbs(0.0, kAngTol));
    }
    SECTION("mass properties, the bounding box, and the body as one element")
    {
        const kernel::Shape shapes[] = {s};
        const measure::Measurement m = ok(measure::massProperties(shapes, 2.70, ctx()));
        const double volume = size.x * size.y * size.z;
        const double area = 2 * (size.x * size.y + size.y * size.z + size.z * size.x);
        CHECK_THAT(num(m, "volume"), WithinRel(volume, 1e-12));
        CHECK_THAT(num(m, "area"), WithinRel(area, 1e-12));
        CHECK_THAT(num(m, "mass"), WithinRel(volume * 1e-3 * 2.70, 1e-12));
        requireVec(vec(m, "centroid"), Vec3{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2});
        CHECK(primaryKey(m) == "volume");

        const measure::Measurement b = ok(measure::boundingBox(shapes, ctx()));
        requireVec(vec(b, "size"), size);
        requireVec(vec(b, "min"), lo);
        requireVec(vec(b, "max"), hi);
        CHECK_THAT(num(b, "sizeX"), WithinAbs(size.x, kTol));
        CHECK(b.witness.lines.size() == 12);

        const measure::Measurement body =
            ok(measure::element(el(s, SubKind::Solid, 0), measure::kDefaultDensity, ctx()));
        CHECK(body.title == "Body");
        CHECK_THAT(num(body, "volume"), WithinRel(volume, 1e-12));
        requireVec(vec(body, "size"), size);
        CHECK_THAT(num(body, "mass"), WithinRel(volume * 1e-3 * measure::kDefaultDensity, 1e-12));
        CHECK_FALSE(measure::massProperties(shapes, 0.0, ctx()));
    }
    SECTION("one face or edge or vertex: its own measures")
    {
        const std::uint32_t top = planeFace(s, Vec3{0, 0, 1});
        const measure::Measurement f = ok(measure::element(el(s, SubKind::Face, top), 1.0, ctx()));
        CHECK_THAT(num(f, "area"), WithinRel(size.x * size.y, 1e-12));
        requireVec(vec(f, "normal"), Vec3{0, 0, 1}, 1e-12);
        CHECK(primaryKey(f) == "area");
        const std::uint32_t e1 = edgeBetween(s, Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z});
        const measure::Measurement e = ok(measure::element(el(s, SubKind::Edge, e1), 1.0, ctx()));
        CHECK_THAT(num(e, "length"), WithinAbs(size.x, kTol));
        const measure::Measurement v = ok(measure::element(el(s, SubKind::Vertex, vertexAt(s, hi)), 1.0, ctx()));
        requireVec(vec(v, "position"), hi);
        CHECK_THAT(num(v, "z"), WithinAbs(hi.z, kTol));
    }
}

TEST_CASE("cylinder: radius, diameter, edge length, face area and volume", "[api][measure][cylinder]")
{
    const double r = 7.125, h = 31.5;
    auto made = kernel::makeCylinder(Vec3{4, -2, 1}, Vec3{0, 0, 1}, r, h);
    REQUIRE(made);
    const kernel::Shape& s = *made;
    const auto lateral = facesOf(s, kernel::SurfaceType::Cylinder);
    REQUIRE(lateral.size() == 1);
    const measure::Measurement m = ok(measure::radius(el(s, SubKind::Face, lateral.front().first), ctx()));
    CHECK_THAT(num(m, "radius"), WithinAbs(r, kTol));
    CHECK_THAT(num(m, "diameter"), WithinAbs(2 * r, kTol));
    requireVec(vec(m, "axis"), Vec3{0, 0, 1}, 1e-12);
    // The face's own measures: its area, the diameter as the headline.
    const measure::Measurement f = ok(measure::element(el(s, SubKind::Face, lateral.front().first), 1.0, ctx()));
    CHECK_THAT(num(f, "area"), WithinRel(2 * kPi * r * h, 1e-9));
    CHECK(primaryKey(f) == "diameter");
    // The circular edges.
    int circles = 0;
    for (std::uint32_t i = 0; i < s.count(SubKind::Edge); ++i) {
        auto c = kernel::curveInfo(s, SubId{SubKind::Edge, i});
        REQUIRE(c);
        if (c->type != kernel::CurveType::Circle)
            continue;
        ++circles;
        const measure::Measurement e = ok(measure::radius(el(s, SubKind::Edge, i), ctx()));
        CHECK_THAT(num(e, "radius"), WithinAbs(r, kTol));
        CHECK_THAT(num(e, "diameter"), WithinAbs(2 * r, kTol));
        const Vec3 centre = vec(e, "center");
        CHECK_THAT(centre.x, WithinAbs(4, kTol));
        CHECK_THAT(centre.y, WithinAbs(-2, kTol));
        const measure::Measurement own = ok(measure::element(el(s, SubKind::Edge, i), 1.0, ctx()));
        CHECK_THAT(num(own, "length"), WithinAbs(2 * kPi * r, kTol));
        CHECK(primaryKey(own) == "diameter");
    }
    CHECK(circles == 2);
    // The two cap circles are coaxial: centre (axis) distance 0, centres h apart.
    std::vector<std::uint32_t> caps;
    for (std::uint32_t i = 0; i < s.count(SubKind::Edge); ++i) {
        auto c = kernel::curveInfo(s, SubId{SubKind::Edge, i});
        if (c && c->type == kernel::CurveType::Circle)
            caps.push_back(i);
    }
    REQUIRE(caps.size() == 2);
    const measure::Measurement cc =
        ok(measure::distance(el(s, SubKind::Edge, caps[0]), el(s, SubKind::Edge, caps[1]), ctx()));
    CHECK_THAT(num(cc, "centerDistance"), WithinAbs(0.0, kTol));
    CHECK_THAT(num(cc, "centerToCenter"), WithinAbs(h, kTol));
    const kernel::Shape shapes[] = {s};
    const measure::Measurement mp = ok(measure::massProperties(shapes, 1.0, ctx()));
    CHECK_THAT(num(mp, "volume"), WithinRel(kPi * r * r * h, 1e-9));
    CHECK_FALSE(measure::radius(el(s, SubKind::Face, planeFace(s, Vec3{0, 0, 1})), ctx()));
}

TEST_CASE("hole plate: hole to hole centre distance and the wall between them", "[api][measure][holes]")
{
    // makeHolePlate(2, 3): a 30 x 20 x 5 plate, a radius 3 through hole in the middle of every 10 x 10 cell.
    auto made = kernel::testing::makeHolePlate(2, 3);
    REQUIRE(made);
    const kernel::Shape& s = *made;
    const std::uint32_t h00 = holeAt(s, 5, 5);
    const std::uint32_t h10 = holeAt(s, 15, 5);
    const std::uint32_t h21 = holeAt(s, 25, 15);
    const measure::Measurement m = ok(measure::distance(el(s, SubKind::Face, h00), el(s, SubKind::Face, h10), ctx()));
    CHECK(primaryKey(m) == "centerDistance");
    CHECK_THAT(num(m, "centerDistance"), WithinAbs(10.0, kTol));
    CHECK_THAT(std::abs(num(m, "centerDx")), WithinAbs(10.0, kTol));
    CHECK_THAT(num(m, "centerDy"), WithinAbs(0.0, kTol));
    CHECK_THAT(num(m, "distance"), WithinAbs(10.0 - 6.0, kTol)); // the wall
    // The witness: the dimension line between the axes at the plate's top face, the axes, the minimum distance.
    bool dimension = false;
    for (const measure::Segment& seg : m.witness.lines) {
        if (!seg.construction && std::abs(std::hypot(seg.b.x - seg.a.x, seg.b.y - seg.a.y) - 10.0) < kTol) {
            dimension = true;
            CHECK_THAT(seg.a.z, WithinAbs(5.0, kTol));
            CHECK_THAT(seg.b.z, WithinAbs(5.0, kTol));
        }
    }
    CHECK(dimension);
    const measure::Measurement diag =
        ok(measure::distance(el(s, SubKind::Face, h00), el(s, SubKind::Face, h21), ctx()));
    CHECK_THAT(num(diag, "centerDistance"), WithinAbs(std::hypot(20.0, 10.0), kTol));
    CHECK_THAT(num(diag, "distance"), WithinAbs(std::hypot(20.0, 10.0) - 6.0, kTol));
    // A hole and a side face of the plate: axis to plane.
    const std::uint32_t left = planeFace(s, Vec3{-1, 0, 0});
    const measure::Measurement ap = ok(measure::distance(el(s, SubKind::Face, h10), el(s, SubKind::Face, left), ctx()));
    CHECK(primaryKey(ap) == "axisToPlane");
    CHECK_THAT(num(ap, "axisToPlane"), WithinAbs(15.0, kTol));
    // The plate's volume and the hole's radius.
    const kernel::Shape shapes[] = {s};
    const measure::Measurement mp = ok(measure::massProperties(shapes, 1.0, ctx()));
    CHECK_THAT(num(mp, "volume"), WithinRel(30.0 * 20.0 * 5.0 - 6 * kPi * 9.0 * 5.0, 1e-9));
    const measure::Measurement rr = ok(measure::radius(el(s, SubKind::Face, h21), ctx()));
    CHECK_THAT(num(rr, "radius"), WithinAbs(3.0, kTol));

    // Test part 14: an 80 x 80 x 6 plate with holes of radius 3, 4 and 5 on a 20 mm grid from (20, 20).
    const kernel::Shape p = testPart(14);
    const measure::Measurement big =
        ok(measure::distance(el(p, SubKind::Face, holeAt(p, 20, 20)), el(p, SubKind::Face, holeAt(p, 60, 60)), ctx()));
    CHECK_THAT(num(big, "centerDistance"), WithinAbs(40.0 * std::sqrt(2.0), kTol));
    CHECK_THAT(num(big, "distance"), WithinAbs(40.0 * std::sqrt(2.0) - 3.0 - 5.0, kTol));
}

TEST_CASE("wedge and chamfered box: angles between faces", "[api][measure][angle]")
{
    SECTION("wedge (20 x 10 x 15, top length 5): the sloped face meets the base at atan(10 / 15)")
    {
        const kernel::Shape w = testPart(8);
        const std::uint32_t base = planeFace(w, Vec3{0, -1, 0});
        const std::uint32_t top = planeFace(w, Vec3{0, 1, 0});
        // The sloped face: x from 20 (y = 0) to 5 (y = 10); its outward normal is (10, 15, 0) / |.|.
        const double n = std::hypot(10.0, 15.0);
        const std::uint32_t slope = planeFace(w, Vec3{10 / n, 15 / n, 0}, Vec3{12.5, 5, 7.5});
        const double expected = std::atan2(10.0, 15.0);
        const measure::Measurement a =
            ok(measure::angle(el(w, SubKind::Face, base), el(w, SubKind::Face, slope), ctx()));
        CHECK_THAT(num(a, "angle"), WithinAbs(expected, kAngTol));
        CHECK_THAT(num(a, "supplement"), WithinAbs(kPi - expected, kAngTol));
        CHECK_THAT(num(a, "edgeAngle"), WithinAbs(expected, kAngTol)); // the material angle at that edge
        const measure::Measurement b =
            ok(measure::angle(el(w, SubKind::Face, top), el(w, SubKind::Face, slope), ctx()));
        CHECK_THAT(num(b, "angle"), WithinAbs(expected, kAngTol));
        CHECK_THAT(num(b, "edgeAngle"), WithinAbs(kPi - expected, kAngTol));
        CHECK(fmt(a, "angle", kMm) == "33.690\u00b0");
        CHECK(fmt(a, "supplement", kMm) == "146.310\u00b0");
        // The two parallel faces: angle 0, the plane distance is the height.
        const measure::Measurement p = ok(measure::angle(el(w, SubKind::Face, base), el(w, SubKind::Face, top), ctx()));
        CHECK_THAT(num(p, "angle"), WithinAbs(0.0, kAngTol));
        const measure::Measurement d =
            ok(measure::distance(el(w, SubKind::Face, base), el(w, SubKind::Face, top), ctx()));
        CHECK_THAT(num(d, "planeDistance"), WithinAbs(10.0, kTol));
        // A straight edge of the slope (along z) and the base: they are perpendicular.
        const std::uint32_t edge = edgeBetween(w, Vec3{20, 0, 0}, Vec3{5, 10, 0});
        const measure::Measurement lp =
            ok(measure::angle(el(w, SubKind::Edge, edge), el(w, SubKind::Face, base), ctx()));
        CHECK_THAT(num(lp, "angle"), WithinAbs(expected, kAngTol));
    }
    SECTION("chamfered box (every edge 1.5): a chamfer face is 45 degrees to its neighbours")
    {
        const kernel::Shape c = testPart(7);
        const double r = std::sqrt(0.5);
        // The chamfer of the front top edge (y = 0, z = 10) runs from (y 0, z 8.5) to (y 1.5, z 10).
        const std::uint32_t chamfer = planeFace(c, Vec3{0, -r, r}, Vec3{15, 0.75, 9.25});
        const std::uint32_t top = planeFace(c, Vec3{0, 0, 1});
        const measure::Measurement a =
            ok(measure::angle(el(c, SubKind::Face, chamfer), el(c, SubKind::Face, top), ctx()));
        CHECK_THAT(num(a, "angle"), WithinAbs(kPi / 4, kAngTol));
        CHECK_THAT(num(a, "supplement"), WithinAbs(3 * kPi / 4, kAngTol));
        CHECK_THAT(num(a, "edgeAngle"), WithinAbs(3 * kPi / 4, kAngTol));
        // Its outward normal points away from the material, whichever way the plane's own axis points.
        const measure::Measurement f = ok(measure::element(el(c, SubKind::Face, chamfer), 1.0, ctx()));
        requireVec(vec(f, "normal"), Vec3{0, -r, r}, 1e-9);
        const std::uint32_t front = planeFace(c, Vec3{0, -1, 0});
        const measure::Measurement b =
            ok(measure::angle(el(c, SubKind::Face, chamfer), el(c, SubKind::Face, front), ctx()));
        CHECK_THAT(num(b, "edgeAngle"), WithinAbs(3 * kPi / 4, kAngTol));
    }
}

TEST_CASE("cone, sphere, torus and a tube: the round measures and what is not available", "[api][measure][round]")
{
    // Test part 3: a cone of radii 8 and 3 over height 12.
    const kernel::Shape cone = testPart(3);
    const auto cones = facesOf(cone, kernel::SurfaceType::Cone);
    REQUIRE(cones.size() == 1);
    const measure::Measurement c = ok(measure::radius(el(cone, SubKind::Face, cones.front().first), ctx()));
    CHECK_THAT(std::abs(num(c, "halfAngle")), WithinAbs(std::atan2(5.0, 12.0), kAngTol));
    CHECK_THAT(std::abs(num(c, "includedAngle")), WithinAbs(2 * std::atan2(5.0, 12.0), kAngTol));

    // Test part 2: a sphere of radius 7 at (1, 2, 3).
    const kernel::Shape sphere = testPart(2);
    const auto spheres = facesOf(sphere, kernel::SurfaceType::Sphere);
    REQUIRE(spheres.size() == 1);
    const measure::Measurement sp = ok(measure::radius(el(sphere, SubKind::Face, spheres.front().first), ctx()));
    CHECK_THAT(num(sp, "radius"), WithinAbs(7.0, kTol));
    CHECK_THAT(num(sp, "diameter"), WithinAbs(14.0, kTol));
    requireVec(vec(sp, "center"), Vec3{1, 2, 3});

    // Test part 4: a torus 15 / 4. The tube radius is not in SurfaceInfo: said, not guessed.
    const kernel::Shape torus = testPart(4);
    const auto tori = facesOf(torus, kernel::SurfaceType::Torus);
    REQUIRE(tori.size() == 1);
    const measure::Measurement t = ok(measure::radius(el(torus, SubKind::Face, tori.front().first), ctx()));
    CHECK_THAT(num(t, "majorRadius"), WithinAbs(15.0, kTol));
    REQUIRE_FALSE(t.notes.empty());
    CHECK_THAT(t.notes.front(), Catch::Matchers::ContainsSubstring("not available"));

    // Test part 12: a tube, radius 10 outside and 7 inside: coaxial, wall 3.
    const kernel::Shape tube = testPart(12);
    const auto cylinders = facesOf(tube, kernel::SurfaceType::Cylinder);
    REQUIRE(cylinders.size() == 2);
    const measure::Measurement w = ok(measure::distance(el(tube, SubKind::Face, cylinders[0].first),
                                                        el(tube, SubKind::Face, cylinders[1].first), ctx()));
    CHECK_THAT(num(w, "centerDistance"), WithinAbs(0.0, kTol));
    CHECK_THAT(num(w, "distance"), WithinAbs(3.0, kTol));
    bool coaxial = false;
    for (const std::string& n : w.notes)
        coaxial = coaxial || n.find("coaxial") != std::string::npos;
    CHECK(coaxial);
    // The angle between two coaxial axes is 0; a plane cannot give a radius.
    const measure::Measurement aa = ok(measure::angle(el(tube, SubKind::Face, cylinders[0].first),
                                                      el(tube, SubKind::Face, cylinders[1].first), ctx()));
    CHECK_THAT(num(aa, "angle"), WithinAbs(0.0, kAngTol));
    // A vertex has no direction: no angle.
    auto bad = measure::angle(el(cone, SubKind::Vertex, 0), el(cone, SubKind::Face, cones.front().first), ctx());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("selection picks the measurement; two bodies; display in inch and mm; JSON", "[api][measure][selection]")
{
    const kernel::Shape a = box(Vec3{0, 0, 0}, Vec3{10, 10, 10});
    const kernel::Shape b = box(Vec3{25.4, 0, 0}, Vec3{35.4, 10, 10});
    // Two bodies: the gap between them is 15.4 mm.
    const measure::Element bodies[] = {el(a, SubKind::Solid, 0), el(b, SubKind::Solid, 0)};
    const measure::Measurement gap = ok(measure::selection(bodies, 1.0, ctx()));
    CHECK(gap.kind == measure::Kind::Distance);
    CHECK_THAT(num(gap, "distance"), WithinAbs(15.4, kTol));

    // Two faces of different bodies: distance and angle together.
    const measure::Element faces[] = {el(a, SubKind::Face, planeFace(a, Vec3{1, 0, 0})),
                                      el(b, SubKind::Face, planeFace(b, Vec3{-1, 0, 0}))};
    const measure::Measurement pair = ok(measure::selection(faces, 1.0, ctx()));
    // Parallel: the plane distance, no angle of 0 (a note says they are parallel).
    CHECK(pair.kind == measure::Kind::Distance);
    CHECK_THAT(num(pair, "planeDistance"), WithinAbs(15.4, kTol));
    CHECK(pair.find("angle") == nullptr);
    CHECK(std::find(pair.notes.begin(), pair.notes.end(), "The faces are parallel.") != pair.notes.end());
    CHECK(primaryKey(pair) == "planeDistance");
    // Not parallel: distance and angle together (a box top and a side of the other box).
    const measure::Element skew[] = {el(a, SubKind::Face, planeFace(a, Vec3{0, 0, 1})), faces[1]};
    const measure::Measurement both = ok(measure::selection(skew, 1.0, ctx()));
    CHECK(both.kind == measure::Kind::Pair);
    CHECK_THAT(num(both, "angle"), WithinAbs(kPi / 2, kAngTol));
    CHECK(primaryKey(both) == "distance");
    const measure::Element one[] = {faces[0]};
    CHECK(ok(measure::selection(one, 1.0, ctx())).kind == measure::Kind::Element);
    CHECK_FALSE(measure::selection(std::span<const measure::Element>(), 1.0, ctx()));

    // Display: inch with 4 decimals, mm with 3.
    CHECK(fmt(pair, "planeDistance", kInch) == "0.6063 in");
    CHECK(fmt(pair, "planeDistance", kMm) == "15.400 mm");
    const measure::Measurement mp = ok(measure::element(el(a, SubKind::Solid, 0), 7.85, ctx()));
    CHECK(fmt(mp, "volume", kMm) == "1000.000 mm\u00b3");
    CHECK(fmt(mp, "volume", kInch) == "0.0610 in\u00b3");
    CHECK(fmt(mp, "area", kMm) == "600.000 mm\u00b2");
    CHECK(fmt(mp, "mass", kMm) == "7.850 g");
    CHECK(fmt(mp, "mass", kInch) == "0.0173 lb");
    CHECK(fmt(mp, "size", kMm) == "10.000 \u00d7 10.000 \u00d7 10.000 mm");
    CHECK(fmt(mp, "centroid", kInch) == "0.1969, 0.1969, 0.1969 in");
    CHECK(fmt(mp, "density", kMm) == "7.850 g/cm\u00b3");

    // JSON: kind, ordered values, the measures map, witness and notes; numbers in mm.
    auto parsed = api::json::parse(measure::toJson(pair));
    REQUIRE(parsed);
    CHECK(parsed->stringOr("kind", "") == "distance");
    const api::json::Value* measures = parsed->find("measures");
    REQUIRE(measures != nullptr);
    CHECK_THAT(measures->numberOr("planeDistance", -1), WithinAbs(15.4, kTol));
    const api::json::Value* values = parsed->find("values");
    REQUIRE(values != nullptr);
    REQUIRE(values->isArray());
    CHECK(values->asArray().front().stringOr("quantity", "") == "length");
    const api::json::Value* witness = parsed->find("witness");
    REQUIRE(witness != nullptr);
    CHECK(witness->find("lines")->isArray());
    CHECK(witness->find("labels")->asArray().front().stringOr("key", "") == "planeDistance");
}

TEST_CASE("measure actions on a corpus part: values computed from its drawing dimensions", "[api][measure][actions]")
{
    // raycad-synthetic/plate_thru_holes_1.stp (tools/corpus_gen, plateThruHoles variant 0): a 120 x 80 x 10 mm plate
    // with a 3 x 2 grid of 6.6 mm through holes at x = 30, 60, 90 and y = 80/3, 160/3.
    const double L = 120, W = 80, T = 10, r = 3.3;
    const double my = W / 3;
    ray::test::TempDir dir;
    auto app = ray::test::makeTestApp(dir.file("user"));
    auto od = app->wait(app->open(ray::test::corpusFile("raycad-synthetic/plate_thru_holes_1.stp")), ray::test::kWait);
    REQUIRE(od);
    REQUIRE(app->waitForBuild(**od, ray::test::kWait));
    const doc::BuildResultPtr build = (*od)->document->build();
    REQUIRE(build);
    REQUIRE(build->bodies.size() == 1);
    const kernel::Shape& s = build->bodies.begin()->second.shape;
    const std::string body = build->bodies.begin()->first.value.toString();

    const auto run = [&](const char* id, const std::string& args) {
        auto r = app->wait(app->actions().invoke(id, args, api::Caller::Script), ray::test::kWait);
        INFO(id << " " << args << ": " << (r ? std::string() : r.error().message));
        REQUIRE(r);
        auto parsed = api::json::parse(r->resultJson);
        REQUIRE(parsed);
        return std::move(*parsed);
    };
    const auto face = [&](std::uint32_t i) {
        return "{\"body\":\"" + body + "\",\"kind\":\"face\",\"index\":" + std::to_string(i) + "}";
    };
    const auto measureNum = [](const api::json::Value& v, const char* key) {
        const api::json::Value* m = v.find("measures");
        REQUIRE(m != nullptr);
        const api::json::Value* n = m->find(key);
        INFO("measure " << key);
        REQUIRE(n != nullptr);
        return n->asNumber();
    };

    // Hole to hole: 30 mm along X; diagonally sqrt(30² + (80/3)²).
    const std::uint32_t h1 = holeAt(s, 30, my);
    const std::uint32_t h2 = holeAt(s, 60, my);
    const std::uint32_t h3 = holeAt(s, 60, 2 * my);
    auto d = run("measure.distance", "{\"a\":" + face(h1) + ",\"b\":" + face(h2) + "}");
    CHECK(d.stringOr("kind", "") == "distance");
    CHECK_THAT(measureNum(d, "centerDistance"), WithinAbs(30.0, kTol));
    CHECK_THAT(measureNum(d, "distance"), WithinAbs(30.0 - 2 * r, kTol));
    auto diag = run("measure.distance", "{\"a\":" + face(h1) + ",\"b\":" + face(h3) + "}");
    CHECK_THAT(measureNum(diag, "centerDistance"), WithinAbs(std::hypot(30.0, my), kTol));
    // The plate's thickness: top to bottom face, plane to plane.
    const std::uint32_t top = planeFace(s, Vec3{0, 0, 1});
    const std::uint32_t bottom = planeFace(s, Vec3{0, 0, -1});
    auto thick = run("measure.selection", "{\"elements\":[" + face(top) + "," + face(bottom) + "]}");
    CHECK_THAT(measureNum(thick, "planeDistance"), WithinAbs(T, kTol));
    auto rad = run("measure.radius", "{\"element\":" + face(h1) + "}");
    CHECK_THAT(measureNum(rad, "diameter"), WithinAbs(6.6, kTol));
    // Mass properties and the bounding box of the whole document.
    const double volume = L * W * T - 6 * kPi * r * r * T;
    const double area = 2 * (L * W - 6 * kPi * r * r) + 2 * (L + W) * T + 6 * 2 * kPi * r * T;
    auto mass = run("measure.massProperties", "{\"density\":2.7}");
    CHECK_THAT(measureNum(mass, "volume"), WithinRel(volume, 1e-9));
    CHECK_THAT(measureNum(mass, "area"), WithinRel(area, 1e-9));
    CHECK_THAT(measureNum(mass, "mass"), WithinRel(volume * 2.7e-3, 1e-9));
    auto bbox = run("measure.boundingBox", "{}");
    const api::json::Value* size = bbox.find("measures")->find("size");
    REQUIRE(size != nullptr);
    CHECK_THAT(size->asArray()[0].asNumber(), WithinAbs(L, kTol));
    CHECK_THAT(size->asArray()[1].asNumber(), WithinAbs(W, kTol));
    CHECK_THAT(size->asArray()[2].asNumber(), WithinAbs(T, kTol));
    // The angle between the top face and a side face.
    const std::uint32_t side = planeFace(s, Vec3{1, 0, 0});
    auto ang = run("measure.angle", "{\"a\":" + face(top) + ",\"b\":" + face(side) + "}");
    CHECK_THAT(measureNum(ang, "angle"), WithinAbs(kPi / 2, kAngTol));

    // Bad arguments: an index out of range, a plane for a radius, a missing element.
    auto outOfRange = app->wait(
        app->actions().invoke("measure.radius", "{\"element\":{\"kind\":\"face\",\"index\":9999}}", api::Caller::User),
        ray::test::kWait);
    REQUIRE_FALSE(outOfRange);
    CHECK(outOfRange.error().code == ErrorCode::InvalidArgument);
    auto notRound =
        app->wait(app->actions().invoke("measure.radius", "{\"element\":" + face(top) + "}", api::Caller::User),
                  ray::test::kWait);
    REQUIRE_FALSE(notRound);
    CHECK(notRound.error().code == ErrorCode::InvalidArgument);
    auto missing = app->wait(app->actions().invoke("measure.distance", "{}", api::Caller::User), ray::test::kWait);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::InvalidArgument);

    // Every measure action is registered, in the Measure category, as a long-running (worker) action.
    for (const char* id : {"measure.distance", "measure.angle", "measure.radius", "measure.massProperties",
                           "measure.boundingBox", "measure.selection"}) {
        auto a = app->actions().find(id);
        INFO(id);
        REQUIRE(a.has_value());
        CHECK(a->category == "Measure");
        CHECK(a->longRunning);
        CHECK_FALSE(a->changesDocument);
        CHECK(a->defaultShortcut.empty());
    }
}
