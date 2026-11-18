// Geometry queries (task T-004): surfaceInfo, curveInfo, vertexPoint, adjacent, bounds, massProps,
// minDistance, and their errors.
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using ray::kernel::test::box;
using ray::kernel::test::close;
using ray::kernel::test::take;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kRel = 1e-9; // acceptance: closed-form values to 1e-9 relative

std::vector<SubId> all(const Shape& shape, SubKind kind)
{
    std::vector<SubId> ids;
    for (std::uint32_t i = 0; i < shape.count(kind); ++i)
        ids.push_back(SubId{kind, i});
    return ids;
}

Vec3 scaled(const Vec3& v, double s)
{
    return Vec3{v.x * s, v.y * s, v.z * s};
}
Vec3 added(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

// The face of an axis-aligned box whose outward normal is `normal`.
SubId boxFace(const Shape& shape, const Vec3& normal)
{
    for (const SubId id : all(shape, SubKind::Face)) {
        const SurfaceInfo info = take(surfaceInfo(shape, id));
        const Vec3 outward = info.reversed ? scaled(info.axis, -1) : info.axis;
        if (close(outward, normal, 1e-12))
            return id;
    }
    FAIL("no face with that normal");
    return SubId{SubKind::Face, 0};
}

SubId vertexAt(const Shape& shape, const Vec3& point)
{
    for (const SubId id : all(shape, SubKind::Vertex))
        if (close(take(vertexPoint(shape, id)), point, 1e-12))
            return id;
    FAIL("no vertex there");
    return SubId{SubKind::Vertex, 0};
}

} // namespace

TEST_CASE("mass properties of a box match closed form", "[kernel][query][mass]")
{
    const Shape b = box(Vec3{1, 2, 3}, Vec3{11, 22, 33});
    const MassProps props = take(massProps(b));
    CHECK(close(props.volume, 10.0 * 20.0 * 30.0, kRel));
    CHECK(close(props.area, 2.0 * (10.0 * 20.0 + 10.0 * 30.0 + 20.0 * 30.0), kRel));
    CHECK(close(props.centroid, Vec3{6, 12, 18}, kRel));

    // Density does not change geometric properties.
    const MassProps dense = take(massProps(b, 7.85e-6));
    CHECK(dense.volume == props.volume);
}

TEST_CASE("mass properties of a cylinder match closed form", "[kernel][query][mass]")
{
    SECTION("axis along z")
    {
        const Shape c = take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0));
        const MassProps props = take(massProps(c));
        CHECK(close(props.volume, kPi * 25.0 * 12.0, kRel));
        CHECK(close(props.area, 2.0 * kPi * 5.0 * 12.0 + 2.0 * kPi * 25.0, kRel));
        CHECK(close(props.centroid, Vec3{1, 2, 9}, kRel));
    }
    SECTION("oblique axis")
    {
        const Vec3 base{-2, 0, 4};
        const double r = 2.5;
        const double h = 7.0;
        const Shape c = take(makeCylinder(base, Vec3{1, 1, 1}, r, h));
        const MassProps props = take(massProps(c));
        CHECK(close(props.volume, kPi * r * r * h, kRel));
        CHECK(close(props.area, 2.0 * kPi * r * h + 2.0 * kPi * r * r, kRel));
        const double s = h / 2.0 / std::sqrt(3.0);
        CHECK(close(props.centroid, Vec3{base.x + s, base.y + s, base.z + s}, kRel));
    }
}

TEST_CASE("mass properties of shapes without solids", "[kernel][query][mass]")
{
    const Shape face = take(testing::makePlanarFace());
    const MassProps props = take(massProps(face));
    CHECK(props.volume == 0.0);
    CHECK(close(props.area, 200.0, kRel));
    CHECK(close(props.centroid, Vec3{5, 10, 5}, kRel));
}

TEST_CASE("surfaceInfo of planes", "[kernel][query][surface]")
{
    const Vec3 lo{0, 0, 0};
    const Vec3 hi{10, 20, 30};
    const Vec3 center{5, 10, 15};
    const Shape b = box(lo, hi);
    std::multiset<long> areas;
    for (const SubId id : all(b, SubKind::Face)) {
        const SurfaceInfo info = take(surfaceInfo(b, id));
        REQUIRE(info.type == SurfaceType::Plane);
        CHECK(close(test::length(info.axis), 1.0, 1e-14));
        CHECK(std::abs(test::dot(test::sub(info.origin, info.centroid), info.axis)) < 1e-9);
        CHECK(info.radius == 0.0);
        areas.insert(std::lround(info.area));

        // The material-side (outward) normal points away from the box centre by half the box size.
        const Vec3 outward = info.reversed ? scaled(info.axis, -1) : info.axis;
        const double offset = test::dot(outward, test::sub(info.centroid, center));
        CHECK(offset > 0);
        CHECK(std::abs(info.area * offset - 3000.0) < 1e-6); // area * half-depth = half the volume

        // Face bounds: tight, and flat along the normal.
        const Vec3 extent = test::sub(info.bounds.max, info.bounds.min);
        CHECK(std::abs(test::dot(extent, outward)) < 1e-9);
        CHECK(info.bounds.min.x >= -1e-9);
        CHECK(info.bounds.max.z <= 30 + 1e-9);
    }
    CHECK(areas == std::multiset<long>{200, 200, 300, 300, 600, 600});
}

TEST_CASE("surfaceInfo of a cylinder", "[kernel][query][surface]")
{
    const Shape c = take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0));
    int cylinders = 0;
    int planes = 0;
    for (const SubId id : all(c, SubKind::Face)) {
        const SurfaceInfo info = take(surfaceInfo(c, id));
        if (info.type == SurfaceType::Cylinder) {
            ++cylinders;
            CHECK(close(info.radius, 5.0, kRel));
            CHECK(close(std::abs(info.axis.z), 1.0, 1e-14));
            CHECK(close(info.origin.x, 1.0, 1e-12)); // origin on the axis
            CHECK(close(info.origin.y, 2.0, 1e-12));
            CHECK(close(info.area, 2.0 * kPi * 5.0 * 12.0, kRel));
            CHECK(close(info.centroid, Vec3{1, 2, 9}, kRel));
            CHECK_FALSE(info.reversed); // outward normal of a solid cylinder = surface normal
            CHECK(close(info.bounds.min, Vec3{-4, -3, 3}, 1e-7));
            CHECK(close(info.bounds.max, Vec3{6, 7, 15}, 1e-7));
        } else {
            REQUIRE(info.type == SurfaceType::Plane);
            ++planes;
            CHECK(close(info.area, kPi * 25.0, kRel));
            const Vec3 outward = info.reversed ? scaled(info.axis, -1) : info.axis;
            if (info.centroid.z > 9.0) {
                CHECK(close(outward, Vec3{0, 0, 1}, 1e-14));
                CHECK(close(info.centroid, Vec3{1, 2, 15}, kRel));
            } else {
                CHECK(close(outward, Vec3{0, 0, -1}, 1e-14));
                CHECK(close(info.centroid, Vec3{1, 2, 3}, kRel));
            }
        }
    }
    CHECK(cylinders == 1);
    CHECK(planes == 2);
}

TEST_CASE("curveInfo of lines and circles", "[kernel][query][curve]")
{
    SECTION("box edges are lines")
    {
        const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
        std::multiset<long> lengths;
        for (const SubId id : all(b, SubKind::Edge)) {
            const CurveInfo info = take(curveInfo(b, id));
            REQUIRE(info.type == CurveType::Line);
            CHECK_FALSE(info.closed);
            const Vec3 chord = test::sub(info.end, info.start);
            CHECK(close(test::length(chord), info.length, kRel));
            CHECK(close(info.axis, scaled(chord, 1.0 / test::length(chord)), 1e-14));
            CHECK(close(info.center, scaled(added(info.start, info.end), 0.5), 1e-14));
            lengths.insert(std::lround(info.length));
        }
        CHECK(lengths == std::multiset<long>{10, 10, 10, 10, 20, 20, 20, 20, 30, 30, 30, 30});
    }
    SECTION("cylinder edges: two circles and a seam line")
    {
        const Shape c = take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0));
        int circles = 0;
        int lines = 0;
        for (const SubId id : all(c, SubKind::Edge)) {
            const CurveInfo info = take(curveInfo(c, id));
            if (info.type == CurveType::Circle) {
                ++circles;
                CHECK(info.closed);
                CHECK(close(info.radius, 5.0, kRel));
                CHECK(close(info.length, 2.0 * kPi * 5.0, kRel));
                CHECK(close(std::abs(info.axis.z), 1.0, 1e-14));
                CHECK(close(info.center.x, 1.0, 1e-12));
                CHECK(close(info.center.y, 2.0, 1e-12));
                CHECK((close(info.center.z, 3.0, 1e-12) || close(info.center.z, 15.0, 1e-12)));
                CHECK(close(info.start, info.end, 1e-12));
            } else {
                REQUIRE(info.type == CurveType::Line);
                ++lines;
                CHECK_FALSE(info.closed);
                CHECK(close(info.length, 12.0, kRel));
                CHECK(close(std::abs(info.axis.z), 1.0, 1e-14));
            }
        }
        CHECK(circles == 2);
        CHECK(lines == 1);
    }
}

TEST_CASE("vertexPoint gives the box corners", "[kernel][query]")
{
    const Shape b = box(Vec3{-1, -2, -3}, Vec3{1, 2, 3});
    std::set<std::array<long, 3>> corners;
    for (const SubId id : all(b, SubKind::Vertex)) {
        const Vec3 p = take(vertexPoint(b, id));
        corners.insert({std::lround(p.x), std::lround(p.y), std::lround(p.z)});
        CHECK(std::abs(std::abs(p.x) - 1) + std::abs(std::abs(p.y) - 2) + std::abs(std::abs(p.z) - 3) < 1e-15);
    }
    CHECK(corners.size() == 8);
}

TEST_CASE("adjacent on a box", "[kernel][query][adjacent]")
{
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const std::vector<SubKind> kinds{SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid};
    // Expected count of `wanted` around one element of `of` on a box.
    const std::map<std::pair<SubKind, SubKind>, std::size_t> expected{
        {{SubKind::Vertex, SubKind::Vertex}, 3}, {{SubKind::Vertex, SubKind::Edge}, 3},
        {{SubKind::Vertex, SubKind::Face}, 3},   {{SubKind::Vertex, SubKind::Solid}, 1},
        {{SubKind::Edge, SubKind::Vertex}, 2},   {{SubKind::Edge, SubKind::Edge}, 4},
        {{SubKind::Edge, SubKind::Face}, 2},     {{SubKind::Edge, SubKind::Solid}, 1},
        {{SubKind::Face, SubKind::Vertex}, 4},   {{SubKind::Face, SubKind::Edge}, 4},
        {{SubKind::Face, SubKind::Face}, 4},     {{SubKind::Face, SubKind::Solid}, 1},
        {{SubKind::Solid, SubKind::Vertex}, 8},  {{SubKind::Solid, SubKind::Edge}, 12},
        {{SubKind::Solid, SubKind::Face}, 6},    {{SubKind::Solid, SubKind::Solid}, 0},
    };
    for (const SubKind of : kinds)
        for (const SubId id : all(b, of))
            for (const SubKind wanted : kinds) {
                const std::vector<SubId> around = adjacent(b, id, wanted);
                INFO("of kind " << static_cast<int>(of) << " #" << id.index << ", wanted " << static_cast<int>(wanted));
                CHECK(around.size() == expected.at({of, wanted}));
                CHECK(std::is_sorted(around.begin(), around.end()));
                CHECK(std::adjacent_find(around.begin(), around.end()) == around.end());
                for (const SubId other : around) {
                    CHECK(other.kind == wanted);
                    CHECK(other != id);
                    // Symmetric: if B is around A then A is around B.
                    const std::vector<SubId> back = adjacent(b, other, of);
                    CHECK(std::find(back.begin(), back.end(), id) != back.end());
                }
            }

    // Geometric check: the edges of the face x = 0 all lie in x = 0, and its vertices too.
    const SubId left = boxFace(b, Vec3{-1, 0, 0});
    for (const SubId edge : adjacent(b, left, SubKind::Edge)) {
        const CurveInfo info = take(curveInfo(b, edge));
        CHECK(info.start.x == 0.0);
        CHECK(info.end.x == 0.0);
    }
    for (const SubId vertex : adjacent(b, left, SubKind::Vertex))
        CHECK(take(vertexPoint(b, vertex)).x == 0.0);
    // The faces around the face x = 0 are the four whose normal is perpendicular to x.
    for (const SubId face : adjacent(b, left, SubKind::Face)) {
        const SurfaceInfo info = take(surfaceInfo(b, face));
        CHECK(std::abs(info.axis.x) < 1e-15);
    }
    // The corner (10, 20, 30) touches the faces x = 10, y = 20, z = 30.
    const std::vector<SubId> cornerFaces = adjacent(b, vertexAt(b, Vec3{10, 20, 30}), SubKind::Face);
    std::vector<SubId> expectedFaces{boxFace(b, Vec3{1, 0, 0}), boxFace(b, Vec3{0, 1, 0}), boxFace(b, Vec3{0, 0, 1})};
    std::sort(expectedFaces.begin(), expectedFaces.end());
    CHECK(cornerFaces == expectedFaces);
}

TEST_CASE("minDistance between faces and between a vertex and a face", "[kernel][query][distance]")
{
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const SubId left = boxFace(b, Vec3{-1, 0, 0});
    const SubId right = boxFace(b, Vec3{1, 0, 0});

    const Distance faces = take(minDistance(b, left, b, right));
    CHECK(close(faces.value, 10.0, kRel));
    CHECK(close(faces.p1.x, 0.0, 1e-12));
    CHECK(close(faces.p2.x, 10.0, 1e-12));
    CHECK(close(test::length(test::sub(faces.p2, faces.p1)), 10.0, kRel));

    const Distance corner = take(minDistance(b, vertexAt(b, Vec3{10, 20, 30}), b, left));
    CHECK(close(corner.value, 10.0, kRel));
    CHECK(close(corner.p1, Vec3{10, 20, 30}, 1e-12));
    CHECK(close(corner.p2, Vec3{0, 20, 30}, 1e-12));

    const Distance same = take(minDistance(b, left, b, left));
    CHECK(same.value < 1e-12);

    // Two shapes: the face x = 10 of the first and the face x = 15 of the second.
    const Shape other = box(Vec3{15, 2, 3}, Vec3{20, 5, 5});
    const Distance between = take(minDistance(b, right, other, boxFace(other, Vec3{-1, 0, 0})));
    CHECK(close(between.value, 5.0, kRel));
    // A vertex of the second shape to a face of the first that it does not face directly.
    const Distance diagonal = take(minDistance(other, vertexAt(other, Vec3{20, 5, 5}), b, boxFace(b, Vec3{0, 0, 1})));
    CHECK(close(diagonal.value, std::sqrt(10.0 * 10.0 + 25.0 * 25.0), kRel));
}

TEST_CASE("bounds are tight and come from the geometry", "[kernel][query][bounds]")
{
    const Box3 boxBounds = bounds(box(Vec3{-1, 2, -3}, Vec3{10, 20, 30}));
    CHECK(close(boxBounds.min, Vec3{-1, 2, -3}, 1e-12));
    CHECK(close(boxBounds.max, Vec3{10, 20, 30}, 1e-12));

    const Box3 cylinderBounds = bounds(take(makeCylinder(Vec3{1, 2, 3}, Vec3{0, 0, 1}, 5.0, 12.0)));
    CHECK(close(cylinderBounds.min, Vec3{-4, -3, 3}, 1e-7));
    CHECK(close(cylinderBounds.max, Vec3{6, 7, 15}, 1e-7));

    // Oblique cylinder: along each axis the caps reach r * sqrt(1 - a_i^2) beyond their centres.
    const Vec3 base{0, 0, 0};
    const double r = 2.0;
    const double h = 9.0;
    const Box3 oblique = bounds(take(makeCylinder(base, Vec3{1, 2, 2}, r, h)));
    const Vec3 a{1.0 / 3, 2.0 / 3, 2.0 / 3};
    const Vec3 top = scaled(a, h);
    const Vec3 reach{r * std::sqrt(1 - a.x * a.x), r * std::sqrt(1 - a.y * a.y), r * std::sqrt(1 - a.z * a.z)};
    CHECK(close(oblique.min,
                test::sub(Vec3{std::min(base.x, top.x), std::min(base.y, top.y), std::min(base.z, top.z)}, reach),
                1e-7));
    CHECK(close(oblique.max,
                added(Vec3{std::max(base.x, top.x), std::max(base.y, top.y), std::max(base.z, top.z)}, reach), 1e-7));

    // Bounds do not change when the shape is meshed (they never come from a mesh).
    const Shape cut = take(testing::makeCutBody());
    const Box3 before = bounds(cut);
    CHECK(close(before.min, Vec3{0, 0, 0}, 1e-12));
    CHECK(close(before.max, Vec3{100, 60, 20}, 1e-12));
    REQUIRE(testing::meshAndShare(cut).has_value());
    const Box3 after = bounds(cut);
    CHECK(after.min.x == before.min.x);
    CHECK(after.max.z == before.max.z);
}

TEST_CASE("every query returns an error and never throws on bad input", "[kernel][query][errors]")
{
    const Shape null;
    const Shape b = box(Vec3{0, 0, 0}, Vec3{10, 20, 30});
    const SubId face0{SubKind::Face, 0};
    const SubId edge0{SubKind::Edge, 0};
    const SubId vertex0{SubKind::Vertex, 0};
    const SubId badKind{static_cast<SubKind>(9), 0};

    auto code = [](const auto& result) {
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };

    SECTION("null shape")
    {
        CHECK(code(surfaceInfo(null, face0)) == ErrorCode::KernelFailure);
        CHECK(code(curveInfo(null, edge0)) == ErrorCode::KernelFailure);
        CHECK(code(vertexPoint(null, vertex0)) == ErrorCode::KernelFailure);
        CHECK(code(massProps(null)) == ErrorCode::KernelFailure);
        CHECK(code(minDistance(null, face0, b, face0)) == ErrorCode::KernelFailure);
        CHECK(code(minDistance(b, face0, null, face0)) == ErrorCode::KernelFailure);
        CHECK(adjacent(null, face0, SubKind::Edge).empty());
        const Box3 nothing = bounds(null);
        CHECK(nothing.min.x == 0.0);
        CHECK(nothing.max.z == 0.0);
        CHECK(serialize(null).empty());
    }
    SECTION("SubId out of range")
    {
        const SubId face6{SubKind::Face, 6};
        CHECK(code(surfaceInfo(b, face6)) == ErrorCode::InvalidArgument);
        CHECK(code(curveInfo(b, SubId{SubKind::Edge, 12})) == ErrorCode::InvalidArgument);
        CHECK(code(vertexPoint(b, SubId{SubKind::Vertex, 8})) == ErrorCode::InvalidArgument);
        CHECK(code(vertexPoint(b, SubId{SubKind::Vertex, 0xFFFFFFFFu})) == ErrorCode::InvalidArgument);
        CHECK(code(minDistance(b, face6, b, face0)) == ErrorCode::InvalidArgument);
        CHECK(code(minDistance(b, face0, b, SubId{SubKind::Solid, 1})) == ErrorCode::InvalidArgument);
        CHECK(adjacent(b, face6, SubKind::Edge).empty());
    }
    SECTION("SubId of the wrong kind")
    {
        CHECK(code(surfaceInfo(b, edge0)) == ErrorCode::InvalidArgument);
        CHECK(code(surfaceInfo(b, SubId{SubKind::Solid, 0})) == ErrorCode::InvalidArgument);
        CHECK(code(curveInfo(b, face0)) == ErrorCode::InvalidArgument);
        CHECK(code(curveInfo(b, vertex0)) == ErrorCode::InvalidArgument);
        CHECK(code(vertexPoint(b, edge0)) == ErrorCode::InvalidArgument);
    }
    SECTION("kind that is not a SubKind")
    {
        CHECK(code(surfaceInfo(b, badKind)) == ErrorCode::InvalidArgument);
        CHECK(code(curveInfo(b, badKind)) == ErrorCode::InvalidArgument);
        CHECK(code(vertexPoint(b, badKind)) == ErrorCode::InvalidArgument);
        CHECK(code(minDistance(b, badKind, b, face0)) == ErrorCode::InvalidArgument);
        CHECK(adjacent(b, badKind, SubKind::Edge).empty());
        CHECK(adjacent(b, face0, static_cast<SubKind>(9)).empty());
    }
    SECTION("bad density")
    {
        CHECK(code(massProps(b, 0.0)) == ErrorCode::InvalidArgument);
        CHECK(code(massProps(b, -1.0)) == ErrorCode::InvalidArgument);
        CHECK(code(massProps(b, std::numeric_limits<double>::quiet_NaN())) == ErrorCode::InvalidArgument);
        CHECK(code(massProps(b, std::numeric_limits<double>::infinity())) == ErrorCode::InvalidArgument);
    }
    SECTION("a shape that came out of corrupt bytes does not exist")
    {
        std::vector<std::byte> bytes = serialize(b);
        bytes[bytes.size() / 2] ^= std::byte{1};
        const Result<Shape> corrupt = deserialize(bytes);
        REQUIRE_FALSE(corrupt.has_value());
        CHECK(corrupt.error().code == ErrorCode::FormatError);
    }
    SECTION("nothing throws")
    {
        for (const Shape* shape : {&null, &b})
            for (const SubId id : {face0, edge0, vertex0, badKind, SubId{SubKind::Face, 99}}) {
                CHECK_NOTHROW(surfaceInfo(*shape, id));
                CHECK_NOTHROW(curveInfo(*shape, id));
                CHECK_NOTHROW(vertexPoint(*shape, id));
                CHECK_NOTHROW(adjacent(*shape, id, SubKind::Face));
                CHECK_NOTHROW(minDistance(*shape, id, b, face0));
                CHECK_NOTHROW(bounds(*shape));
                CHECK_NOTHROW(massProps(*shape));
                CHECK_NOTHROW(serialize(*shape));
                CHECK_NOTHROW(shape->contentHash());
            }
    }
}
