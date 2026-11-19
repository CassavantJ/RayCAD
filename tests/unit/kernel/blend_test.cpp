// fillet and chamfer: closed-form volumes, history, size failures, argument checks, cancel, determinism and an
// 8-thread stress over the corpus (task T-095).
#include <catch2/catch_test_macros.hpp>

#include "history_test_util.h"
#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

using Relation = HistoryEntry::Relation;

// Cross-section of the material a radius r fillet removes from (or adds to) a square edge, and the distance of its
// centroid from the edge line, along each face.
double filletArea(double r)
{
    return r * r * (1.0 - kPi / 4.0);
}
double filletCentroid(double r)
{
    return r * (10.0 - 3.0 * kPi) / (12.0 - 3.0 * kPi);
}

double volumeOf(const Shape& shape)
{
    return take(massProps(shape)).volume;
}

// The straight edge from a to b (either direction).
SubId lineEdge(const Shape& shape, Vec3 a, Vec3 b)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const SubId id{SubKind::Edge, i};
        const CurveInfo info = take(curveInfo(shape, id));
        if (info.type != CurveType::Line)
            continue;
        if ((close(info.start, a, 1e-9) && close(info.end, b, 1e-9)) ||
            (close(info.start, b, 1e-9) && close(info.end, a, 1e-9)))
            return id;
    }
    FAIL("no straight edge between the given points");
    return {};
}

// The circular edge with this centre and radius.
SubId circleEdge(const Shape& shape, Vec3 centre, double radius)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const SubId id{SubKind::Edge, i};
        const CurveInfo info = take(curveInfo(shape, id));
        if (info.type == CurveType::Circle && close(info.center, centre, 1e-9) && close(info.radius, radius, 1e-9))
            return id;
    }
    FAIL("no circular edge with the given centre and radius");
    return {};
}

SubId vertexAt(const Shape& shape, Vec3 p)
{
    for (std::uint32_t i = 0; i < shape.count(SubKind::Vertex); ++i)
        if (close(take(vertexPoint(shape, SubId{SubKind::Vertex, i})), p, 1e-9))
            return SubId{SubKind::Vertex, i};
    FAIL("no vertex at the given point");
    return {};
}

void checkValid(const OpResult& op)
{
    REQUIRE_FALSE(op.shape.isNull());
    CHECK(take(testing::isValidShape(op.shape)));
    CHECK(op.shape.count(SubKind::Solid) >= 1);
}

OpResult filletOk(const Shape& shape, std::vector<SubId> edges, double radius)
{
    JobContext ctx;
    return take(fillet(shape, edges, radius, ctx));
}
OpResult chamferOk(const Shape& shape, std::vector<SubId> edges, double distance)
{
    JobContext ctx;
    return take(chamfer(shape, edges, distance, ctx));
}

// Every blend face of the result: the faces of Generated entries.
std::vector<SubId> blendFaces(const OpResult& op)
{
    std::vector<SubId> out;
    for (const HistoryEntry& e : op.history.entries)
        if (e.relation == Relation::Generated)
            for (const SubId& r : e.results)
                if (r.kind == SubKind::Face)
                    out.push_back(r);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Result faces that some input face was Modified to.
std::vector<SubId> modifiedFaces(const OpResult& op)
{
    std::vector<SubId> out;
    for (const HistoryEntry& e : op.history.entries)
        if (e.relation == Relation::Modified && e.source.kind == SubKind::Face)
            out.insert(out.end(), e.results.begin(), e.results.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The acceptance rule for history: each picked (or propagated) edge is Deleted and has its blend faces Generated
// from it; blend faces come from no input face; every face around a blend is the Modified result of an input face
// (or another blend face); no result face lacks history.
void checkBlendHistory(const OpResult& op, const Shape& input, const std::vector<SubId>& blendedEdges)
{
    checkHistory(op, {&input});
    CHECK(orphans(op).empty());
    const std::vector<SubId> blends = blendFaces(op);
    const std::vector<SubId> modified = modifiedFaces(op);
    for (const SubId& edge : blendedEdges) {
        INFO("edge " << edge.index);
        bool deleted = false;
        for (const HistoryEntry& e : op.history.entries)
            deleted = deleted || (e.source == edge && e.relation == Relation::Deleted);
        CHECK(deleted);
        const std::vector<SubId> made = resultsOf(op, Relation::Generated, 0, edge);
        REQUIRE_FALSE(made.empty());
        for (const SubId& f : made)
            CHECK(f.kind == SubKind::Face);
    }
    for (const SubId& blend : blends) {
        INFO("blend face " << blend.index);
        CHECK_FALSE(std::binary_search(modified.begin(), modified.end(), blend));
        for (const SubId& next : adjacent(op.shape, blend, SubKind::Face)) {
            const bool known = std::binary_search(modified.begin(), modified.end(), next) ||
                               std::binary_search(blends.begin(), blends.end(), next);
            CHECK(known);
        }
    }
    // No Generated face is ever reported for an input face (OCCT reports changed faces as Modified here).
    for (const HistoryEntry& e : op.history.entries)
        if (e.source.kind == SubKind::Face)
            CHECK(e.relation != Relation::Generated);
}

std::string warningsText(const OpResult& op)
{
    std::string all;
    for (const std::string& w : op.warnings)
        all += w + "\n";
    return all;
}

Shape lBlock()
{
    // An L in plan, 30 high: convex vertical edge at (60, 0), concave vertical edge at (20, 20).
    JobContext ctx;
    const Profile p = profile(xyPlane(), {polygon({{0, 0}, {60, 0}, {60, 20}, {20, 20}, {20, 50}, {0, 50}}, 1, 100)});
    return take(extrude(p, 30.0, 0.0, ctx)).op.shape;
}

// A 50 long, radius 8 obround boss, 15 high.
Shape obroundBoss()
{
    JobContext ctx;
    return take(extrude(profile(xyPlane(), {slot(20, 70, 30, 8, 1, 100)}), 15.0, 0.0, ctx)).op.shape;
}

} // namespace

TEST_CASE("fillet and chamfer one straight edge: closed-form volume and history", "[kernel][blend]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const double before = 100.0 * 60.0 * 20.0;
    const SubId edge = lineEdge(block, {0, 0, 20}, {100, 0, 20}); // top front, convex
    SECTION("fillet")
    {
        const OpResult op = filletOk(block, {edge}, 5.0);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(close(volumeOf(op.shape), before - 100.0 * filletArea(5.0), 1e-9));
        CHECK(op.shape.count(SubKind::Face) == 7);
        checkBlendHistory(op, block, {edge});
        const std::vector<SubId> blends = blendFaces(op);
        REQUIRE(blends.size() == 1);
        const SurfaceInfo blend = take(surfaceInfo(op.shape, blends[0]));
        CHECK(blend.type == SurfaceType::Cylinder);
        CHECK(close(blend.radius, 5.0, 1e-9));
        CHECK(close(blend.area, 100.0 * kPi * 5.0 / 2.0, 1e-9));
        // The blend touches four faces: top and front (trimmed by 5) and both ends (a corner cut off). All four
        // are Modified from the input face on the same plane; the other two input faces are Modified unchanged.
        const std::vector<SubId> around = adjacent(op.shape, blends[0], SubKind::Face);
        CHECK(around.size() == 4);
        for (std::uint32_t f = 0; f < block.count(SubKind::Face); ++f) {
            const std::vector<SubId> to = modifiedTo(op, 0, SubId{SubKind::Face, f});
            REQUIRE(to.size() == 1);
            const SurfaceInfo was = take(surfaceInfo(block, SubId{SubKind::Face, f}));
            const SurfaceInfo now = take(surfaceInfo(op.shape, to[0]));
            CHECK(now.type == SurfaceType::Plane);
            const bool trimmed = now.area < was.area - 1e-9;
            const bool touches = std::find(around.begin(), around.end(), to[0]) != around.end();
            CHECK(trimmed == touches);
        }
        // The two end vertices of the edge are gone; the four new boundary edges of the blend have no ancestor
        // in OCCT's history (spike S4), which naming handles.
        CHECK(resultsOf(op, Relation::Modified, 0, vertexAt(block, {0, 0, 20})).empty());
        CHECK(resultsOf(op, Relation::Modified, 0, vertexAt(block, {100, 0, 20})).empty());
    }
    SECTION("chamfer")
    {
        const OpResult op = chamferOk(block, {edge}, 3.0);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(close(volumeOf(op.shape), before - 100.0 * 9.0 / 2.0, 1e-9));
        checkBlendHistory(op, block, {edge});
        const std::vector<SubId> blends = blendFaces(op);
        REQUIRE(blends.size() == 1);
        const SurfaceInfo face = take(surfaceInfo(op.shape, blends[0]));
        CHECK(face.type == SurfaceType::Plane);
        CHECK(close(face.area, 100.0 * 3.0 * std::sqrt(2.0), 1e-9));
    }
    SECTION("a duplicated pick is the same as one pick")
    {
        const OpResult once = filletOk(block, {edge}, 5.0);
        const OpResult twice = filletOk(block, {edge, edge}, 5.0);
        CHECK(once.shape.contentHash() == twice.shape.contentHash());
    }
}

TEST_CASE("concave edges add material, convex and concave together", "[kernel][blend]")
{
    const Shape block = lBlock();
    const double before = (60.0 * 20.0 + 20.0 * 30.0) * 30.0;
    REQUIRE(close(volumeOf(block), before, 1e-12));
    const SubId inner = lineEdge(block, {20, 20, 0}, {20, 20, 30});
    const SubId outer = lineEdge(block, {60, 0, 0}, {60, 0, 30});
    SECTION("concave fillet")
    {
        const OpResult op = filletOk(block, {inner}, 4.0);
        checkValid(op);
        CHECK(close(volumeOf(op.shape), before + 30.0 * filletArea(4.0), 1e-9));
        checkBlendHistory(op, block, {inner});
    }
    SECTION("concave chamfer")
    {
        const OpResult op = chamferOk(block, {inner}, 4.0);
        checkValid(op);
        CHECK(close(volumeOf(op.shape), before + 30.0 * 8.0, 1e-9));
        checkBlendHistory(op, block, {inner});
    }
    SECTION("one convex and one concave edge of equal length cancel out in volume, but are not a no-op")
    {
        const OpResult op = filletOk(block, {inner, outer}, 4.0);
        checkValid(op);
        CHECK(close(volumeOf(op.shape), before, 1e-9));
        checkBlendHistory(op, block, {inner, outer});
        CHECK(blendFaces(op).size() == 2);
        const OpResult cham = chamferOk(block, {outer, inner}, 2.0);
        checkValid(cham);
        CHECK(close(volumeOf(cham.shape), before, 1e-9));
        checkBlendHistory(cham, block, {inner, outer});
    }
}

TEST_CASE("several edges at once and edges meeting at a corner", "[kernel][blend]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const double before = 100.0 * 60.0 * 20.0;
    SECTION("four parallel vertical edges")
    {
        const std::vector<SubId> edges{
            lineEdge(block, {0, 0, 0}, {0, 0, 20}), lineEdge(block, {100, 0, 0}, {100, 0, 20}),
            lineEdge(block, {100, 60, 0}, {100, 60, 20}), lineEdge(block, {0, 60, 0}, {0, 60, 20})};
        const OpResult op = filletOk(block, edges, 5.0);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(close(volumeOf(op.shape), before - 4.0 * 20.0 * filletArea(5.0), 1e-9));
        checkBlendHistory(op, block, edges);
        CHECK(blendFaces(op).size() == 4);
        const OpResult cham = chamferOk(block, edges, 5.0);
        checkValid(cham);
        CHECK(close(volumeOf(cham.shape), before - 4.0 * 20.0 * 12.5, 1e-9));
        checkBlendHistory(cham, block, edges);
    }
    SECTION("three edges at one corner: a corner patch generated from the vertex")
    {
        const std::vector<SubId> edges{lineEdge(block, {0, 0, 20}, {100, 0, 20}),
                                       lineEdge(block, {0, 0, 20}, {0, 60, 20}),
                                       lineEdge(block, {0, 0, 0}, {0, 0, 20})};
        const double r = 3.0;
        const OpResult op = filletOk(block, edges, r);
        checkValid(op);
        const double removed = (97.0 + 57.0 + 17.0) * filletArea(r) + r * r * r * (1.0 - kPi / 6.0);
        CHECK(close(volumeOf(op.shape), before - removed, 1e-9));
        checkBlendHistory(op, block, edges);
        const std::vector<SubId> corner = resultsOf(op, Relation::Generated, 0, vertexAt(block, {0, 0, 20}));
        REQUIRE(corner.size() == 1);
        const SurfaceInfo patch = take(surfaceInfo(op.shape, corner[0]));
        CHECK(patch.type == SurfaceType::Sphere);
        CHECK(close(patch.radius, r, 1e-9));
        CHECK(close(patch.area, kPi * r * r / 2.0, 1e-9)); // one eighth of the sphere

        const double d = 2.0;
        const OpResult cham = chamferOk(block, edges, d);
        checkValid(cham);
        // OCCT closes the corner with a triangle on the plane x + y + w = 2d (w measured down from the top), through
        // the ends of the three chamfer strips, so inside the d-cube at the corner only the tetrahedron beyond that
        // plane stays: 5/6 of the cube goes.
        const double cut = (98.0 + 58.0 + 18.0) * d * d / 2.0 + 5.0 / 6.0 * d * d * d;
        CHECK(close(volumeOf(cham.shape), before - cut, 1e-9));
        CHECK(cham.shape.count(SubKind::Face) == 10);
        checkBlendHistory(cham, block, edges);
        const std::vector<SubId> triangle = resultsOf(cham, Relation::Generated, 0, vertexAt(block, {0, 0, 20}));
        REQUIRE(triangle.size() == 1);
        const SurfaceInfo facet = take(surfaceInfo(cham.shape, triangle[0]));
        CHECK(facet.type == SurfaceType::Plane);
        CHECK(close(facet.area, 2.0 * std::sqrt(3.0), 1e-9)); // equilateral, side 2 * sqrt(2)
    }
    SECTION("all twelve edges of a box")
    {
        const Shape small = box({0, 0, 0}, {40, 30, 20});
        std::vector<SubId> edges;
        for (std::uint32_t i = 0; i < small.count(SubKind::Edge); ++i)
            edges.push_back(SubId{SubKind::Edge, i});
        const double r = 4.0;
        const OpResult op = filletOk(small, edges, r);
        checkValid(op);
        const double a = 40.0 - 2 * r;
        const double b = 30.0 - 2 * r;
        const double c = 20.0 - 2 * r;
        const double rounded =
            a * b * c + 2 * r * (a * b + b * c + a * c) + kPi * r * r * (a + b + c) + 4.0 / 3.0 * kPi * r * r * r;
        CHECK(close(volumeOf(op.shape), rounded, 1e-9));
        CHECK(op.shape.count(SubKind::Face) == 26);
        checkBlendHistory(op, small, edges);
        int corners = 0;
        for (std::uint32_t v = 0; v < small.count(SubKind::Vertex); ++v)
            corners += resultsOf(op, Relation::Generated, 0, SubId{SubKind::Vertex, v}).size() == 1 ? 1 : 0;
        CHECK(corners == 8);
    }
}

TEST_CASE("a tangent chain is blended whole and the extra edges are reported", "[kernel][blend]")
{
    const Shape boss = obroundBoss();
    const double l = 50.0;
    const double R = 8.0;
    const double before = 15.0 * (2 * R * l + kPi * R * R);
    REQUIRE(close(volumeOf(boss), before, 1e-9));
    const SubId topLine = lineEdge(boss, {20, 22, 15}, {70, 22, 15});
    const std::vector<SubId> chain{topLine, lineEdge(boss, {20, 38, 15}, {70, 38, 15}),
                                   circleEdge(boss, {20, 30, 15}, R), circleEdge(boss, {70, 30, 15}, R)};
    SECTION("fillet")
    {
        const double r = 2.0;
        const OpResult op = filletOk(boss, {topLine}, r);
        checkValid(op);
        // Pappus for the arcs: the removed section turns about the arc axis at R minus its centroid offset.
        const double removed = filletArea(r) * (2 * l + 2 * kPi * (R - filletCentroid(r)));
        CHECK(close(volumeOf(op.shape), before - removed, 1e-9));
        checkBlendHistory(op, boss, chain);
        REQUIRE(op.warnings.size() == 1);
        CHECK(op.warnings[0].find("also blends edges") != std::string::npos);
        for (std::size_t i = 1; i < chain.size(); ++i)
            CHECK(op.warnings[0].find(std::to_string(chain[i].index)) != std::string::npos);
        // Picking the whole chain gives the same body and no warning.
        const OpResult all = filletOk(boss, chain, r);
        CHECK(all.warnings.empty());
        CHECK(all.shape.contentHash() == op.shape.contentHash());
    }
    SECTION("chamfer")
    {
        const double d = 1.5;
        const OpResult op = chamferOk(boss, {chain[2]}, d);
        checkValid(op);
        const double removed = d * d / 2.0 * (2 * l + 2 * kPi * (R - d / 3.0));
        CHECK(close(volumeOf(op.shape), before - removed, 1e-9));
        checkBlendHistory(op, boss, chain);
        REQUIRE(op.warnings.size() == 1);
    }
}

TEST_CASE("blends on circular edges: a cylinder rim and a hole rim", "[kernel][blend]")
{
    const double R = 10.0;
    const Shape cyl = take(makeCylinder({0, 0, 0}, {0, 0, 1}, R, 20.0));
    const double before = kPi * R * R * 20.0;
    const SubId rim = circleEdge(cyl, {0, 0, 20}, R);
    SECTION("convex rim")
    {
        const OpResult f = filletOk(cyl, {rim}, 2.0);
        checkValid(f);
        CHECK(close(volumeOf(f.shape), before - filletArea(2.0) * 2 * kPi * (R - filletCentroid(2.0)), 1e-9));
        checkBlendHistory(f, cyl, {rim});
        CHECK(take(surfaceInfo(f.shape, blendFaces(f).at(0))).type == SurfaceType::Torus);
        const OpResult c = chamferOk(cyl, {rim}, 2.0);
        checkValid(c);
        CHECK(close(volumeOf(c.shape), before - 2.0 * 2 * kPi * (R - 2.0 / 3.0), 1e-9));
        checkBlendHistory(c, cyl, {rim});
    }
    SECTION("concave edge where a boss meets a plate, and a hole rim")
    {
        JobContext ctx;
        const Shape plate = box({-30, -30, -10}, {30, 30, 0});
        const Shape withBoss = take(boolean(BoolOp::Union, plate, cyl, ctx)).shape;
        const SubId root = circleEdge(withBoss, {0, 0, 0}, R);
        const double v0 = volumeOf(withBoss);
        const OpResult f = filletOk(withBoss, {root}, 2.0);
        checkValid(f);
        // A concave fillet adds the section outside the edge: centroid at R plus the offset.
        CHECK(close(volumeOf(f.shape), v0 + filletArea(2.0) * 2 * kPi * (R + filletCentroid(2.0)), 1e-9));
        checkBlendHistory(f, withBoss, {root});

        const Shape hole = take(makeCylinder({0, 0, -20}, {0, 0, 1}, 6.0, 40.0));
        const Shape drilled = take(boolean(BoolOp::Cut, plate, hole, ctx)).shape;
        const SubId mouth = circleEdge(drilled, {0, 0, 0}, 6.0);
        const double v1 = volumeOf(drilled);
        const OpResult c = chamferOk(drilled, {mouth}, 1.0);
        checkValid(c);
        // A countersink-like chamfer on a convex hole rim: the section sits outside the hole radius.
        CHECK(close(volumeOf(c.shape), v1 - 0.5 * 2 * kPi * (6.0 + 1.0 / 3.0), 1e-9));
        checkBlendHistory(c, drilled, {mouth});
    }
}

namespace {

// The number quoted in a "too large" message ("about 4.69 mm").
double quotedSize(const std::string& message)
{
    static const std::regex about("about ([0-9.]+(?:e[-+]?[0-9]+)?) mm");
    std::smatch m;
    if (!std::regex_search(message, m, about))
        return 0.0;
    return std::strtod(m[1].str().c_str(), nullptr);
}

} // namespace

TEST_CASE("a radius or distance too large fails clearly and names a size that works", "[kernel][blend][errors]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const SubId top = lineEdge(block, {0, 0, 20}, {100, 0, 20});
    const std::vector<SubId> verticals{
        lineEdge(block, {0, 0, 0}, {0, 0, 20}), lineEdge(block, {100, 0, 0}, {100, 0, 20}),
        lineEdge(block, {100, 60, 0}, {100, 60, 20}), lineEdge(block, {0, 60, 0}, {0, 60, 20})};
    struct Case {
        const char* name;
        bool isFillet;
        std::vector<SubId> edges;
        double size;
    };
    const Case cases[] = {
        {"fillet taller than the block", true, {top}, 25.0},
        {"chamfer taller than the block", false, {top}, 30.0},
        {"fillets on the verticals that would meet", true, verticals, 40.0},
        {"chamfers on the verticals that would meet", false, verticals, 45.0},
        {"absurd fillet", true, {top}, 1000.0},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        JobContext ctx;
        const auto started = std::chrono::steady_clock::now();
        auto result = c.isFillet ? fillet(block, c.edges, c.size, ctx) : chamfer(block, c.edges, c.size, ctx);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        CHECK(seconds < 30.0); // no hang
        REQUIRE_FALSE(result.has_value());
        const Error& error = result.error();
        INFO(error.message);
        CHECK(error.code == ErrorCode::KernelFailure);
        CHECK(error.message.find("too large") != std::string::npos);
        const double works = quotedSize(error.message);
        REQUIRE(works > 0.0);
        CHECK(works < c.size);
        // The size quoted works.
        JobContext again;
        auto retry = c.isFillet ? fillet(block, c.edges, works, again) : chamfer(block, c.edges, works, again);
        INFO((retry ? std::string() : retry.error().message));
        REQUIRE(retry.has_value());
        checkValid(*retry);
        CHECK(volumeOf(retry->shape) < 100.0 * 60.0 * 20.0);
    }
}

TEST_CASE("an invalid body from OCCT is refused, never returned", "[kernel][blend][errors]")
{
    // Corpus parts on which OCCT 8.0.1 reports success for a 2.5 mm blend of edge 0 but BRepCheck rejects the
    // result (found by the corpus stress test). The kernel must refuse it and still name a size that works.
    struct Case {
        const char* file;
        bool isFillet;
    };
    for (const Case& c :
         {Case{"fixture_plate_1.stp", true}, Case{"fixture_plate_1.stp", false}, Case{"thin_wall_box_3.stp", false}}) {
        INFO(c.file << (c.isFillet ? " fillet" : " chamfer"));
        JobContext ctx;
        const std::string path = std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/" + c.file;
        const ImportResult imported = take(importStep(path, ImportOptions{}, ctx));
        REQUIRE(imported.bodies.size() == 1);
        const Shape& part = imported.bodies[0].shape;
        const SubId edge{SubKind::Edge, 0};
        auto result = c.isFillet ? fillet(part, {&edge, 1}, 2.5, ctx) : chamfer(part, {&edge, 1}, 2.5, ctx);
        REQUIRE_FALSE(result.has_value());
        INFO(result.error().message);
        CHECK(result.error().code == ErrorCode::KernelFailure);
        bool rejectedByCheck = false;
        for (const std::string& line : result.error().context)
            rejectedByCheck = rejectedByCheck || line.find("invalid solid (BRepCheck)") != std::string::npos;
        CHECK(rejectedByCheck);
        const double works = quotedSize(result.error().message);
        REQUIRE(works > 0.0);
        auto retry = c.isFillet ? fillet(part, {&edge, 1}, works, ctx) : chamfer(part, {&edge, 1}, works, ctx);
        REQUIRE(retry.has_value());
        checkValid(*retry);
        checkBlendHistory(*retry, part, {edge});
    }
}

namespace {

// A body written by kernel::serialize() into tests/unit/kernel/data.
Shape loadBody(const char* name)
{
    const std::filesystem::path file = std::filesystem::path(RAY_KERNEL_TEST_DATA_DIR) / name;
    std::ifstream in(file, std::ios::binary);
    INFO(file.string());
    REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return take(deserialize(bytes));
}

// Fillets `edge` of `body` twice: OCCT must not fault (the Windows guard never fires; off Windows a fault would end
// the process), and both calls give the same answer.
void filletWithoutFault(const Shape& body, SubId edge, double radius)
{
    const std::uint64_t guardBefore = testing::structuredExceptions();
    JobContext ctx;
    auto first = fillet(body, {&edge, 1}, radius, ctx);
    auto second = fillet(body, {&edge, 1}, radius, ctx);
    CHECK(testing::structuredExceptions() == guardBefore);
    REQUIRE(first.has_value() == second.has_value());
    if (first) {
        checkValid(*first);
        CHECK(first->shape.contentHash() == second->shape.contentHash());
    } else {
        INFO(first.error().message);
        CHECK(first.error().code == ErrorCode::KernelFailure);
        CHECK(first.error().message == second.error().message);
    }
}

} // namespace

TEST_CASE("a trial blend that would crash OCCT's checker is refused, not checked", "[kernel][blend][errors]")
{
    // The body and pick of the naming random test (part 0, sequence 6: block_blind_holes_1 after a cut and a boss)
    // whose fillet killed the process on Linux. One of the size search's trial results has a face whose pcurve OCCT
    // cannot sample, and BRepCheck_Analyzer then read through a null pointer in BRepTopAdaptor_FClass2d. The
    // structural pre-check (src/kernel/shape_check.cpp) must refuse that trial before BRepCheck sees it. (On Windows
    // OCCT computes that trial a little differently and does not fault; the test must pass there too.)
    const Shape body = loadBody("fillet_classifier_fault.rbrep");
    REQUIRE(body.count(SubKind::Solid) == 1);
    filletWithoutFault(body, SubId{SubKind::Edge, 14}, 0.56046961970619469);
}

TEST_CASE("a fillet whose stripe OCCT reprocesses next to a collapsed end fails cleanly", "[kernel][blend][errors]")
{
    // The body and pick of naming random sequence 18:37 (nist 827-9999-905 after three features): edge 21, a 10 um
    // sliver edge at the end of a tangent chain. The 0.6254 mm fillet fails and its size search tries 0.156 mm, where
    // OCCT 8.0.1's ChFi3d_Builder::PerformSetOfKGen called D1 through a null 3D curve (access violation on Windows,
    // SIGSEGV on Linux). Fixed in OCCT by ports/opencascade/0006-chfi3d-null-neighbour-curve.patch: the stripe fails.
    const Shape body = loadBody("fillet_builder_fault.rbrep");
    REQUIRE(body.count(SubKind::Solid) == 1);
    const SubId edge{SubKind::Edge, 21};
    // The size-search trial on its own: an ordinary failure, the guard does not fire.
    {
        const std::uint64_t guardBefore = testing::structuredExceptions();
        JobContext ctx;
        auto trial = fillet(body, {&edge, 1}, 0.15633965180909595, ctx);
        CHECK(testing::structuredExceptions() == guardBefore);
        REQUIRE_FALSE(trial.has_value());
        CHECK(trial.error().code == ErrorCode::KernelFailure);
    }
    // The request of the naming sequence, size search included.
    filletWithoutFault(body, edge, 0.62535860723638381);
}

TEST_CASE("a size equal to a face width gives a valid body or a clear failure", "[kernel][blend][errors]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const SubId top = lineEdge(block, {0, 0, 20}, {100, 0, 20});
    for (const bool isFillet : {true, false}) {
        for (const double size : {19.999, 20.0, 20.001}) {
            INFO((isFillet ? "fillet " : "chamfer ") << size);
            JobContext ctx;
            auto result = isFillet ? fillet(block, {&top, 1}, size, ctx) : chamfer(block, {&top, 1}, size, ctx);
            if (result) {
                checkValid(*result);
                checkHistory(*result, {&block});
                CHECK(volumeOf(result->shape) < 100.0 * 60.0 * 20.0);
            } else {
                CHECK(result.error().code == ErrorCode::KernelFailure);
                CHECK_FALSE(result.error().message.empty());
            }
        }
    }
}

TEST_CASE("blend arguments are checked", "[kernel][blend][errors]")
{
    const Shape block = box({0, 0, 0}, {10, 10, 10});
    const SubId edge{SubKind::Edge, 0};
    const auto rejects = [](Result<OpResult> result, ErrorCode code) {
        REQUIRE_FALSE(result.has_value());
        INFO(result.error().message);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().message.empty());
    };
    JobContext ctx;
    const std::vector<SubId> none;
    rejects(fillet(block, none, 1.0, ctx), ErrorCode::InvalidArgument);
    rejects(chamfer(block, none, 1.0, ctx), ErrorCode::InvalidArgument);
    const std::vector<SubId> face{SubId{SubKind::Face, 0}};
    rejects(fillet(block, face, 1.0, ctx), ErrorCode::InvalidArgument);
    const std::vector<SubId> outOfRange{SubId{SubKind::Edge, 12}};
    rejects(fillet(block, outOfRange, 1.0, ctx), ErrorCode::InvalidArgument);
    for (const double bad : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity(), 1e-9}) {
        rejects(fillet(block, {&edge, 1}, bad, ctx), ErrorCode::InvalidArgument);
        rejects(chamfer(block, {&edge, 1}, bad, ctx), ErrorCode::InvalidArgument);
    }
    rejects(fillet(Shape(), {&edge, 1}, 1.0, ctx), ErrorCode::InvalidArgument);
    const Shape lone = take(testing::makePlanarFace());
    rejects(fillet(lone, {&edge, 1}, 1.0, ctx), ErrorCode::InvalidArgument);

    SECTION("a seam edge has one face")
    {
        const Shape cyl = take(makeCylinder({0, 0, 0}, {0, 0, 1}, 5.0, 10.0));
        std::vector<SubId> seam;
        for (std::uint32_t i = 0; i < cyl.count(SubKind::Edge); ++i)
            if (adjacent(cyl, SubId{SubKind::Edge, i}, SubKind::Face).size() == 1)
                seam.push_back(SubId{SubKind::Edge, i});
        REQUIRE(seam.size() == 1);
        rejects(fillet(cyl, seam, 1.0, ctx), ErrorCode::InvalidArgument);
        rejects(chamfer(cyl, seam, 1.0, ctx), ErrorCode::InvalidArgument);
    }
    SECTION("a smooth edge between a fillet and a face")
    {
        const SubId top = lineEdge(block, {0, 0, 10}, {10, 0, 10});
        const OpResult once = filletOk(block, {top}, 2.0);
        const SubId blend = blendFaces(once).at(0);
        std::vector<SubId> smooth;
        for (const SubId& e : adjacent(once.shape, blend, SubKind::Edge)) {
            if (take(curveInfo(once.shape, e)).type != CurveType::Line)
                continue;
            smooth.push_back(e); // the two straight boundary edges of the blend are tangent edges
        }
        REQUIRE(smooth.size() == 2);
        auto result = fillet(once.shape, {&smooth[0], 1}, 1.0, ctx);
        rejects(std::move(result), ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a cancelled blend returns Cancelled", "[kernel][blend][cancel]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const SubId top = lineEdge(block, {0, 0, 20}, {100, 0, 20});
    SECTION("before it starts")
    {
        CancelSource source;
        source.cancel();
        JobContext ctx{source.token(), Progress()};
        auto f = fillet(block, {&top, 1}, 5.0, ctx);
        REQUIRE_FALSE(f.has_value());
        CHECK(f.error().code == ErrorCode::Cancelled);
        auto c = chamfer(block, {&top, 1}, 5.0, ctx);
        REQUIRE_FALSE(c.has_value());
        CHECK(c.error().code == ErrorCode::Cancelled);
    }
    SECTION("while it runs")
    {
        CancelSource source;
        // The first progress report comes after the arguments are checked and before OCCT builds.
        JobContext ctx{source.token(), Progress([&source](double, std::string_view) { source.cancel(); })};
        auto f = fillet(block, {&top, 1}, 5.0, ctx);
        REQUIRE_FALSE(f.has_value());
        CHECK(f.error().code == ErrorCode::Cancelled);
    }
    SECTION("while it looks for a size that fits")
    {
        CancelSource source;
        std::atomic<int> searching{0};
        // Each report sleeps past the 30 Hz progress rate limit so the stage change is always delivered.
        JobContext ctx{source.token(), Progress([&](double, std::string_view stage) {
                           std::this_thread::sleep_for(std::chrono::milliseconds(40));
                           if (stage == "Finding a size that fits") {
                               searching.fetch_add(1);
                               source.cancel();
                           }
                       })};
        auto f = fillet(block, {&top, 1}, 25.0, ctx);
        REQUIRE_FALSE(f.has_value());
        CHECK(searching.load() >= 1);
        CHECK(f.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("blend inputs that are several solids keep them apart", "[kernel][blend]")
{
    JobContext ctx;
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const Shape wall = box({40, -5, -5}, {60, 65, 25});
    const OpResult two = take(boolean(BoolOp::Cut, block, wall, ctx));
    REQUIRE(two.shape.count(SubKind::Solid) == 2);
    const SubId edge = lineEdge(two.shape, {0, 0, 20}, {40, 0, 20});
    const OpResult op = filletOk(two.shape, {edge}, 3.0);
    checkValid(op);
    CHECK(op.shape.count(SubKind::Solid) == 2);
    CHECK(close(volumeOf(op.shape), 80.0 * 60.0 * 20.0 - 40.0 * filletArea(3.0), 1e-9));
    checkBlendHistory(op, two.shape, {edge});
}

TEST_CASE("a blend on a blended body: history per operation", "[kernel][blend][history]")
{
    const Shape body = take(testing::makeFilletedBody());
    // The bottom rims of the two holes and the bottom outline (a tangent chain of lines and the corner fillets).
    const SubId rimA = circleEdge(body, {20, 25, 0}, 6.0);
    const SubId rimB = circleEdge(body, {60, 25, 0}, 6.0);
    const OpResult first = chamferOk(body, {rimA, rimB}, 1.0);
    checkValid(first);
    checkBlendHistory(first, body, {rimA, rimB});
    const SubId outline = lineEdge(first.shape, {5, 0, 0}, {75, 0, 0});
    const OpResult second = filletOk(first.shape, {outline}, 1.0);
    checkValid(second);
    checkHistory(second, {&first.shape});
    CHECK(orphans(second).empty());
    CHECK(volumeOf(second.shape) < volumeOf(first.shape));
    REQUIRE(second.warnings.size() == 1); // the other 3 lines and 4 corner arcs of the outline
    CHECK(blendFaces(second).size() == 8);
}

namespace {

// A fixed chain of fillets and chamfers. Content hash of every result and its history, as bytes; empty on any error.
// No Catch2 assertions (safe on worker threads).
std::vector<unsigned char> blendPipeline()
{
    std::vector<unsigned char> out;
    const auto fail = [&] {
        out.clear();
        return out;
    };
    JobContext ctx;
    const Profile plate = profile(
        xyPlane(), {rectangle(0, 0, 120, 80, 1, 100), circleLoop({30, 40}, 8, 11, 101), slot(60, 95, 40, 7, 21, 102)});
    auto base = extrude(plate, 25.0, 0.0, ctx);
    if (!base)
        return fail();
    const Shape& b = base->op.shape;
    // Vertical outer edges: lines parallel to Z of length 25 at the rectangle corners.
    std::vector<SubId> verticals;
    std::vector<SubId> topRims;
    for (std::uint32_t i = 0; i < b.count(SubKind::Edge); ++i) {
        auto info = curveInfo(b, SubId{SubKind::Edge, i});
        if (!info)
            return fail();
        const bool vertical = info->type == CurveType::Line && std::abs(info->start.z - info->end.z) > 24.0;
        const bool corner =
            (info->start.x == 0.0 || info->start.x == 120.0) && (info->start.y == 0.0 || info->start.y == 80.0);
        if (vertical && corner)
            verticals.push_back(SubId{SubKind::Edge, i});
        if (info->type == CurveType::Circle && info->start.z == 25.0)
            topRims.push_back(SubId{SubKind::Edge, i});
    }
    auto rounded = fillet(b, verticals, 6.0, ctx);
    if (!rounded)
        return fail();
    appendHash(out, rounded->shape);
    appendHistory(out, rounded->history);
    // Re-find the top rims on the new body by geometry: circles at z = 25 (the hole and the slot ends).
    std::vector<SubId> rims;
    for (std::uint32_t i = 0; i < rounded->shape.count(SubKind::Edge); ++i) {
        auto info = curveInfo(rounded->shape, SubId{SubKind::Edge, i});
        if (!info)
            return fail();
        if (info->type == CurveType::Circle && info->start.z == 25.0 && info->radius == 8.0)
            rims.push_back(SubId{SubKind::Edge, i});
    }
    auto chamfered = chamfer(rounded->shape, rims, 1.0, ctx);
    if (!chamfered)
        return fail();
    appendHash(out, chamfered->shape);
    appendHistory(out, chamfered->history);
    for (const std::string& w : chamfered->warnings)
        out.insert(out.end(), w.begin(), w.end());
    // A failure is part of the result too: its message must not change either.
    auto tooBig = fillet(chamfered->shape, rims, 50.0, ctx);
    if (tooBig)
        return fail();
    out.insert(out.end(), tooBig.error().message.begin(), tooBig.error().message.end());
    if (topRims.empty())
        return fail();
    auto slotRim = fillet(b, topRims, 1.5, ctx);
    if (!slotRim)
        return fail();
    appendHash(out, slotRim->shape);
    appendHistory(out, slotRim->history);
    return out;
}

} // namespace

TEST_CASE("fillet and chamfer are deterministic over repeated runs and 8 threads", "[kernel][blend][threads]")
{
    const std::vector<unsigned char> reference = blendPipeline();
    REQUIRE_FALSE(reference.empty());
    for (int run = 0; run < 3; ++run)
        CHECK(blendPipeline() == reference);
    constexpr int kThreads = 8;
    std::vector<std::vector<unsigned char>> results(kThreads);
    runOnThreads(kThreads, [&results](int t) {
        for (int run = 0; run < 2; ++run)
            results[static_cast<std::size_t>(t)] = blendPipeline();
    });
    for (const auto& r : results)
        CHECK(r == reference);
}

TEST_CASE("8 threads blending the same shape at once leave it untouched", "[kernel][blend][threads]")
{
    const Shape body = take(testing::makeFilletedBody());
    const std::vector<std::byte> bytes = serialize(body);
    const SubId rim = circleEdge(body, {20, 25, 0}, 6.0);
    const Hash256 expectedFillet = filletOk(body, {rim}, 1.0).shape.contentHash();
    const Hash256 expectedChamfer = chamferOk(body, {rim}, 1.0).shape.contentHash();
    constexpr int kThreads = 8;
    std::vector<int> ok(kThreads, 0);
    runOnThreads(kThreads, [&](int t) {
        JobContext ctx;
        for (int run = 0; run < 3; ++run) {
            auto result = (t % 2 == 0) ? fillet(body, {&rim, 1}, 1.0, ctx) : chamfer(body, {&rim, 1}, 1.0, ctx);
            if (!result || result->shape.contentHash() != (t % 2 == 0 ? expectedFillet : expectedChamfer))
                return;
        }
        ok[static_cast<std::size_t>(t)] = 1;
    });
    for (int t = 0; t < kThreads; ++t)
        CHECK(ok[static_cast<std::size_t>(t)] == 1);
    CHECK(serialize(body) == bytes);
}

namespace {

// One corpus body with the edges the stress test blends.
struct StressPart {
    std::string name;
    Shape shape;
    std::vector<std::vector<SubId>> picks; // each one blend call
};

// Up to four picks per body: the first and the middle straight edge between two planes, the first circle between a
// plane and a cylinder, and the first six straight edges together. Deterministic: SubId order only.
std::vector<std::vector<SubId>> stressPicks(const Shape& shape)
{
    std::vector<SubId> lines;
    std::vector<SubId> circles;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const SubId id{SubKind::Edge, i};
        const std::vector<SubId> faces = adjacent(shape, id, SubKind::Face);
        if (faces.size() != 2)
            continue;
        auto info = curveInfo(shape, id);
        auto a = surfaceInfo(shape, faces[0]);
        auto b = surfaceInfo(shape, faces[1]);
        if (!info || !a || !b)
            continue;
        if (info->type == CurveType::Line && a->type == SurfaceType::Plane && b->type == SurfaceType::Plane &&
            std::abs(std::abs(dot(a->axis, b->axis)) - 1.0) > 1e-6 && info->length > 2.0)
            lines.push_back(id);
        const bool planeCyl = (a->type == SurfaceType::Plane && b->type == SurfaceType::Cylinder) ||
                              (a->type == SurfaceType::Cylinder && b->type == SurfaceType::Plane);
        if (info->type == CurveType::Circle && planeCyl && info->radius > 1.0)
            circles.push_back(id);
    }
    std::vector<std::vector<SubId>> picks;
    if (!lines.empty())
        picks.push_back({lines.front()});
    if (lines.size() > 2)
        picks.push_back({lines[lines.size() / 2]});
    if (!circles.empty())
        picks.push_back({circles.front()});
    if (lines.size() > 6)
        picks.emplace_back(lines.begin(), lines.begin() + 6);
    return picks;
}

// The outcome of one blend call as bytes: content hash and history, or the error code and message.
std::vector<unsigned char> stressOutcome(const Shape& shape, const std::vector<SubId>& edges, bool isFillet,
                                         double size)
{
    std::vector<unsigned char> out;
    JobContext ctx;
    auto result = isFillet ? fillet(shape, edges, size, ctx) : chamfer(shape, edges, size, ctx);
    if (result) {
        out.push_back(1);
        appendHash(out, result->shape);
        appendHistory(out, result->history);
        for (const std::string& w : result->warnings)
            out.insert(out.end(), w.begin(), w.end());
    } else {
        out.push_back(0);
        out.push_back(static_cast<unsigned char>(result.error().code));
        out.insert(out.end(), result.error().message.begin(), result.error().message.end());
    }
    return out;
}

} // namespace

// Runs in its own CTest entry (ray_kernel_blend_stress), so it has its own timeout under ThreadSanitizer.
TEST_CASE("8-thread fillet and chamfer stress over the synthetic corpus", "[kernel][blend][threads][stress]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(RAY_CORPUS_DIR) / "raycad-synthetic";
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".stp" && entry.path().filename().string().rfind("broken_", 0) != 0)
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() >= 60);

    std::vector<StressPart> parts;
    for (const fs::path& file : files) {
        JobContext ctx;
        auto imported = importStep(file.string(), ImportOptions{}, ctx);
        INFO(file.string());
        REQUIRE(imported.has_value());
        for (ImportedBody& body : imported->bodies)
            if (body.shape.count(SubKind::Solid) > 0)
                parts.push_back({file.filename().string(), body.shape, stressPicks(body.shape)});
    }
    struct Job {
        const StressPart* part;
        std::size_t pick;
        bool isFillet;
        double size;
        std::vector<unsigned char> run() const { return stressOutcome(part->shape, part->picks[pick], isFillet, size); }
    };
    std::vector<Job> jobs;
    for (const StressPart& part : parts)
        for (std::size_t p = 0; p < part.picks.size(); ++p)
            for (const bool isFillet : {true, false})
                for (const double size : {0.4, 2.5}) // 2.5 mm is too large on some thin walls and small holes
                    jobs.push_back({&part, p, isFillet, size});
    REQUIRE(jobs.size() >= 500);

    // Reference, single-threaded.
    const std::uint64_t guardBefore = testing::structuredExceptions();
    std::vector<std::vector<unsigned char>> reference(jobs.size());
    int succeeded = 0;
    double longest = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto one = std::chrono::steady_clock::now();
        reference[i] = jobs[i].run();
        longest = std::max(longest, std::chrono::duration<double>(std::chrono::steady_clock::now() - one).count());
        succeeded += reference[i].front() == 1 ? 1 : 0;
    }
    const double singleSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    WARN("corpus blend stress: " << parts.size() << " bodies, " << jobs.size() << " blends, " << succeeded
                                 << " succeeded, single thread " << singleSeconds << " s, longest call " << longest
                                 << " s");
    CHECK(succeeded * 10 >= static_cast<int>(jobs.size()) * 7); // most blends on machined parts work

    // 8 threads; threads t and t + 4 run the same jobs in the same order, so every shape is blended by two threads
    // at once while six other blends run.
    constexpr int kThreads = 8;
    std::vector<std::vector<std::vector<unsigned char>>> results(kThreads);
    runOnThreads(kThreads, [&](int t) {
        auto& mine = results[static_cast<std::size_t>(t)];
        mine.resize(jobs.size());
        for (std::size_t i = static_cast<std::size_t>(t % 4); i < jobs.size(); i += 4)
            mine[i] = jobs[i].run();
    });
    int mismatches = 0;
    for (int t = 0; t < kThreads; ++t)
        for (std::size_t i = static_cast<std::size_t>(t % 4); i < jobs.size(); i += 4)
            if (results[static_cast<std::size_t>(t)][i] != reference[i]) {
                ++mismatches;
                UNSCOPED_INFO(jobs[i].part->name << " pick " << jobs[i].pick
                                                 << (jobs[i].isFillet ? " fillet " : " chamfer ") << jobs[i].size);
            }
    CHECK(mismatches == 0);
    // No OCCT call faulted: the structural pre-check (src/kernel/shape_check.cpp) refuses the trial results that
    // made BRepCheck read through a null pointer, so the Windows structured-exception guard never has to catch one
    // (a caught access violation may leave OCCT state corrupt; off Windows it would have killed the process).
    const std::uint64_t guardFired = testing::structuredExceptions() - guardBefore;
    WARN("structured exceptions caught during the blend stress: " << guardFired);
    CHECK(guardFired == 0);
}
