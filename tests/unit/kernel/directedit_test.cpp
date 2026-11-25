// Direct edits (task T-080): deleteFaces, offsetFaces, resizeHole, unifySameDomain and blendRun on bodies with known
// volumes; history; refusals and their messages; immutability; determinism; cancel and abandon; fault containment.
// The success rates on the S9 set are in directedit_corpus_test.cpp.
#include <catch2/catch_test_macros.hpp>

#include "history_test_util.h"
#include "kernel_test_util.h"

#include <directedit_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

using Relation = HistoryEntry::Relation;
constexpr double kPiValue = 3.14159265358979323846;

double volumeOf(const Shape& shape)
{
    return take(massProps(shape)).volume;
}

Shape cut(const Shape& body, const Shape& tool)
{
    JobContext ctx;
    return take(boolean(BoolOp::Cut, body, tool, ctx)).shape;
}

// The faces whose surface is of `type` (and, for cylinders, of radius `radius` when given).
std::vector<SubId> facesOf(const Shape& shape, SurfaceType type, std::optional<double> radius = std::nullopt)
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const SurfaceInfo info = take(surfaceInfo(shape, SubId{SubKind::Face, i}));
        if (info.type == type && (!radius || std::abs(info.radius - *radius) < 1e-6))
            out.push_back(SubId{SubKind::Face, i});
    }
    return out;
}

// The planar face whose centroid is `centroid`.
SubId planeAt(const Shape& shape, Vec3 centroid)
{
    std::optional<SubId> found;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face) && !found; ++i) {
        const SurfaceInfo info = take(surfaceInfo(shape, SubId{SubKind::Face, i}));
        if (info.type == SurfaceType::Plane && close(info.centroid, centroid, 1e-6))
            found = SubId{SubKind::Face, i};
    }
    if (!found)
        FAIL("no planar face with that centroid");
    return found.value_or(SubId{});
}

// The planar face whose centroid lies at height z.
SubId planeAtHeight(const Shape& shape, double z)
{
    std::optional<SubId> found;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face) && !found; ++i) {
        const SurfaceInfo info = take(surfaceInfo(shape, SubId{SubKind::Face, i}));
        if (info.type == SurfaceType::Plane && std::abs(info.centroid.z - z) < 1e-9)
            found = SubId{SubKind::Face, i};
    }
    if (!found)
        FAIL("no planar face at that height");
    return found.value_or(SubId{});
}

bool hasToolEntries(const OpResult& op)
{
    return std::any_of(op.history.entries.begin(), op.history.entries.end(),
                       [](const HistoryEntry& e) { return e.inputIndex != 0; });
}

std::optional<Relation> primary(const OpResult& op, SubId source)
{
    for (const HistoryEntry& e : op.history.entries)
        if (e.inputIndex == 0 && e.source == source && e.relation != Relation::Generated)
            return e.relation;
    return std::nullopt;
}

void checkValidResult(const OpResult& op, const Shape& input)
{
    REQUIRE_FALSE(op.shape.isNull());
    CHECK(take(testing::isValidShape(op.shape)));
    CHECK(op.shape.count(SubKind::Solid) == input.count(SubKind::Solid));
    checkHistory(op, {&input});
    CHECK_FALSE(hasToolEntries(op));
}

Result<OpResult> deleting(const Shape& s, std::vector<SubId> faces)
{
    JobContext ctx;
    return deleteFaces(s, faces, ctx);
}
Result<OpResult> pushing(const Shape& s, std::vector<SubId> faces, double d)
{
    JobContext ctx;
    return offsetFaces(s, faces, d, ctx);
}
Result<OpResult> resizing(const Shape& s, SubId face, double diameter)
{
    JobContext ctx;
    return resizeHole(s, face, diameter, ctx);
}

// A 40 x 30 x 10 block with a radius 3 through hole at (20, 15) and a radius 5 counterbore 3 deep around it.
Shape counterboredBlock()
{
    const Shape block = box({0, 0, 0}, {40, 30, 10});
    const Shape bored = cut(block, take(makeCylinder({20, 15, -1}, {0, 0, 1}, 3.0, 12.0)));
    return cut(bored, take(makeCylinder({20, 15, 7}, {0, 0, 1}, 5.0, 4.0)));
}

// A 30 x 30 x 10 block with a closed 10 x 10 pocket 5 deep in the middle of its top.
Shape pocketBlock()
{
    return cut(box({0, 0, 0}, {30, 30, 10}), box({10, 10, 5}, {20, 20, 11}));
}

void requireFailure(const Result<OpResult>& result, ErrorCode code, const std::string& words)
{
    REQUIRE_FALSE(result.has_value());
    INFO(result.error().message);
    CHECK(result.error().code == code);
    CHECK(result.error().message.find(words) != std::string::npos);
}

} // namespace

TEST_CASE("deleteFaces fills a through hole from any face of it", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1)); // 10 x 10 x 5, radius 3 hole
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    REQUIRE_FALSE(wall.empty());
    const OpResult out = take(deleting(plate, {wall.front()}));
    checkValidResult(out, plate);
    CHECK(close(volumeOf(out.shape), 500.0, 1e-7));
    CHECK(facesOf(out.shape, SurfaceType::Cylinder).empty());
    for (const SubId& f : wall)
        CHECK(primary(out, f) == Relation::Deleted);
    CHECK(out.shape.count(SubKind::Face) == 6);
}

TEST_CASE("deleteFaces removes a counterbored hole whole when only its bore is picked", "[kernel][directedit]")
{
    const Shape block = counterboredBlock();
    const std::vector<SubId> bore = facesOf(block, SurfaceType::Cylinder, 3.0);
    REQUIRE_FALSE(bore.empty());
    const OpResult out = take(deleting(block, {bore.front()}));
    checkValidResult(out, block);
    CHECK(close(volumeOf(out.shape), 40.0 * 30.0 * 10.0, 1e-7));
    CHECK(facesOf(out.shape, SurfaceType::Cylinder).empty());
    CHECK(out.warnings.empty());
    // Picking the counterbore floor does the same.
    const SubId floor = planeAt(block, {20, 15, 7}); // annulus between r 3 and r 5: centroid on the axis
    const OpResult same = take(deleting(block, {floor}));
    CHECK(same.shape.contentHash() == out.shape.contentHash());
}

TEST_CASE("deleteFaces reports no face Modified into a face that looks the other way", "[kernel][directedit]")
{
    // T-081b (naming through direct edits): a counterbored hole with a small second hole started at the counterbore
    // floor, cutting into the bore's side. Removing the counterbore wall closes the counterbore; where the floor was
    // over air (the bore's and the small hole's openings) the solid gets a new face on the floor's plane looking down,
    // the cap of what is left of the holes. That face is not the floor (the floor looked up, with material under it;
    // the floor itself is now inside the material), so it must not be the floor's Modified image: a reference to the
    // floor would resolve to a face on the other side of the material.
    const Shape block = cut(counterboredBlock(), take(makeCylinder({20, 18.9, -1}, {0, 0, 1}, 1.4, 8.0)));
    const std::vector<SubId> counterbore = facesOf(block, SurfaceType::Cylinder, 5.0);
    REQUIRE(counterbore.size() == 1);
    const auto result = deleting(block, counterbore);
    REQUIRE(result.has_value());
    const OpResult& out = *result;
    checkValidResult(out, block);
    for (const HistoryEntry& e : out.history.entries) {
        if (e.relation != Relation::Modified || e.source.kind != SubKind::Face)
            continue;
        const FaceSample before = take(faceSample(block, e.source));
        if (take(surfaceInfo(block, e.source)).type != SurfaceType::Plane)
            continue;
        for (const SubId& r : e.results) {
            const FaceSample after = take(faceSample(out.shape, r));
            INFO("face " << e.source.index << " Modified to face " << r.index);
            CHECK(before.normal.x * after.normal.x + before.normal.y * after.normal.y +
                      before.normal.z * after.normal.z >
                  0.0);
        }
    }
}

namespace {

// A shape saved with kernel::serialize in tests/unit/kernel/data.
Shape savedShape(const char* name)
{
    const std::string path = std::string(RAY_KERNEL_TEST_DATA_DIR) + "/" + name;
    std::FILE* in = std::fopen(path.c_str(), "rb");
    INFO(path);
    REQUIRE(in != nullptr);
    std::vector<std::byte> bytes;
    for (int ch = std::fgetc(in); ch != EOF; ch = std::fgetc(in))
        bytes.push_back(static_cast<std::byte>(ch));
    std::fclose(in);
    return take(deserialize(bytes));
}

// Whether two faces share ground, told from their meshes (independently of the kernel's own test): a triangle centre
// of either within 0.01 mm of a triangle of the other.
struct Triangle {
    std::array<Vec3, 3> p;
};

std::vector<Triangle> trianglesOf(const Mesh& mesh, SubId face)
{
    std::vector<Triangle> out;
    for (const FaceRange& r : mesh.faces) {
        if (r.face != face)
            continue;
        for (std::uint32_t k = r.firstIndex; k + 2 < r.firstIndex + r.indexCount; k += 3) {
            Triangle t;
            for (std::uint32_t j = 0; j < 3; ++j) {
                const std::uint32_t v = mesh.indices[k + j];
                t.p[j] = Vec3{mesh.positions[3 * v], mesh.positions[3 * v + 1], mesh.positions[3 * v + 2]};
            }
            out.push_back(t);
        }
    }
    return out;
}

Vec3 minus(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
double dotOf(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 along(const Vec3& o, const Vec3& d, double t)
{
    return Vec3{o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
}

// Distance from p to triangle t (the closest point by its Voronoi regions).
double distanceToTriangle(const Vec3& p, const Triangle& t)
{
    const Vec3 ab = minus(t.p[1], t.p[0]), ac = minus(t.p[2], t.p[0]), ap = minus(p, t.p[0]);
    auto length = [](const Vec3& v) {
        return std::sqrt(dotOf(v, v));
    };
    const double d1 = dotOf(ab, ap), d2 = dotOf(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return length(ap);
    const Vec3 bp = minus(p, t.p[1]);
    const double d3 = dotOf(ab, bp), d4 = dotOf(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return length(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return length(minus(p, along(t.p[0], ab, d1 / (d1 - d3))));
    const Vec3 cp = minus(p, t.p[2]);
    const double d5 = dotOf(ab, cp), d6 = dotOf(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return length(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return length(minus(p, along(t.p[0], ac, d2 / (d2 - d6))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
        return length(minus(p, along(t.p[1], minus(t.p[2], t.p[1]), (d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const double denom = 1.0 / (va + vb + vc);
    return length(minus(p, along(along(t.p[0], ab, vb * denom), ac, vc * denom)));
}

bool anyCentreOn(const std::vector<Triangle>& a, const std::vector<Triangle>& b)
{
    for (const Triangle& t : a) {
        const Vec3 c{(t.p[0].x + t.p[1].x + t.p[2].x) / 3, (t.p[0].y + t.p[1].y + t.p[2].y) / 3,
                     (t.p[0].z + t.p[1].z + t.p[2].z) / 3};
        for (const Triangle& u : b)
            if (distanceToTriangle(c, u) <= 0.01)
                return true;
    }
    return false;
}

Mesh meshOf(const Shape& shape)
{
    JobContext ctx;
    return take(tessellate(shape, MeshOptions{0.005, 0.1}, ctx));
}

bool shareGround(const Mesh& a, SubId fa, const Mesh& b, SubId fb)
{
    const std::vector<Triangle> ta = trianglesOf(a, fa), tb = trianglesOf(b, fb);
    return anyCentreOn(ta, tb) || anyCentreOn(tb, ta);
}

// The (source, image) pairs of `op`'s Modified face entries that share no ground (meshes).
std::vector<std::pair<std::uint32_t, std::uint32_t>> imagesApart(const Shape& input, const OpResult& op)
{
    const Mesh before = meshOf(input), after = meshOf(op.shape);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const HistoryEntry& e : op.history.entries) {
        if (e.relation != Relation::Modified || e.source.kind != SubKind::Face)
            continue;
        for (const SubId& r : e.results)
            if (!shareGround(before, e.source, after, r))
                out.emplace_back(e.source.index, r.index);
    }
    return out;
}

// Is face `image` of `op` among the Modified results of input face `source`?
bool modifiedTo(const OpResult& op, std::uint32_t source, std::uint32_t image)
{
    for (const HistoryEntry& e : op.history.entries)
        if (e.inputIndex == 0 && e.source == SubId{SubKind::Face, source} && e.relation == Relation::Modified)
            return std::find(e.results.begin(), e.results.end(), SubId{SubKind::Face, image}) != e.results.end();
    return false;
}

} // namespace

TEST_CASE("deleteFaces reports no face Modified into a face that shares no ground with it", "[kernel][directedit]")
{
    // T-081b (naming through direct edits): the input of one pocket removal in the randomised naming block, saved as it
    // was (data/directedit_pocket_image_apart.rbrep): a block whose top (z 18) cuts had left in pieces. Removing the
    // pocket (faces 1 and 3), Defeaturing lists two pieces of the top, at x 85.5..100 and 65..81, as Modified into the
    // whole top and also into a piece at x 0..32, which shares no ground with either. That piece is not what they
    // became: a reference to one of them would resolve to it.
    const Shape block = savedShape("directedit_pocket_image_apart.rbrep");
    const std::vector<SubId> pocket{SubId{SubKind::Face, 1}, SubId{SubKind::Face, 3}};
    {
        // OpenCASCADE's own history has such images (else this test would not test the rule).
        testing::RawDirectEditHistory raw;
        const OpResult as = take(deleting(block, pocket));
        CHECK_FALSE(imagesApart(block, as).empty());
    }
    const OpResult out = take(deleting(block, pocket));
    checkValidResult(out, block);
    for (const auto& [source, image] : imagesApart(block, out)) {
        INFO("face " << source << " Modified to face " << image);
        CHECK(false);
    }
    // Both are still Modified, into the whole top.
    CHECK(primary(out, SubId{SubKind::Face, 9}) == Relation::Modified);
    CHECK(primary(out, SubId{SubKind::Face, 11}) == Relation::Modified);
}

TEST_CASE("deleteFaces keeps a blend face that a removal rebuilt unchanged as its own image", "[kernel][directedit]")
{
    // T-081b: the input of one hole removal in the randomised naming block (data/directedit_bspline_blend_kept.rbrep).
    // Defeaturing rebuilds the small B-spline blend face 2 unchanged (same area, centroid and box). Measuring a point
    // inside one on the other did not converge, and the "no shared ground" rule took the face for one apart from
    // itself: its reference broke. Every face that is still there, unchanged, must be Modified to itself.
    const Shape body = savedShape("directedit_bspline_blend_kept.rbrep");
    const OpResult out = take(deleting(body, {SubId{SubKind::Face, 26}}));
    checkValidResult(out, body);
    int unchanged = 0;
    for (std::uint32_t i = 0; i < body.count(SubKind::Face); ++i) {
        const SurfaceInfo a = take(surfaceInfo(body, SubId{SubKind::Face, i}));
        for (std::uint32_t j = 0; j < out.shape.count(SubKind::Face); ++j) {
            const SurfaceInfo b = take(surfaceInfo(out.shape, SubId{SubKind::Face, j}));
            if (a.type != b.type || std::abs(a.area - b.area) > 1e-9 * std::max(1.0, a.area) ||
                !close(a.centroid, b.centroid, 1e-9) || !close(a.bounds.min, b.bounds.min, 1e-9) ||
                !close(a.bounds.max, b.bounds.max, 1e-9))
                continue;
            ++unchanged;
            INFO("face " << i << " is unchanged as face " << j);
            CHECK(modifiedTo(out, i, j));
        }
    }
    CHECK(take(surfaceInfo(body, SubId{SubKind::Face, 2})).type == SurfaceType::Bspline);
    CHECK(unchanged > 10);
}

TEST_CASE("deleteFaces keeps a face a removal trimmed on one side and extended on the other as its image",
          "[kernel][directedit]")
{
    // T-081b: the input of one fillet removal in the randomised naming block (data/directedit_fillet_removal_overlap
    // .rbrep). Removing the fillet (face 1) trims the cylinder face 0 on one side and runs it on over the fillet's
    // ground on the other: its image, face 9, shares half of its ground, but the middles of the two faces each lie on
    // ground the other does not have, and the "no shared ground" rule, which tried those two points only, took 9 for a
    // face apart from 0. No image may share no ground with its face, and none that does is taken away.
    const Shape body = savedShape("directedit_fillet_removal_overlap.rbrep");
    const OpResult out = take(deleting(body, {SubId{SubKind::Face, 1}}));
    checkValidResult(out, body);
    CHECK(take(surfaceInfo(body, SubId{SubKind::Face, 0})).type == SurfaceType::Cylinder);
    CHECK(shareGround(meshOf(body), SubId{SubKind::Face, 0}, meshOf(out.shape), SubId{SubKind::Face, 9}));
    CHECK(modifiedTo(out, 0, 9));
    CHECK(imagesApart(body, out).empty());
}
TEST_CASE("deleteFaces fills a closed pocket", "[kernel][directedit]")
{
    const Shape block = pocketBlock();
    std::vector<SubId> pocket;
    for (const Vec3 c : {Vec3{15, 15, 5}, Vec3{10, 15, 7.5}, Vec3{20, 15, 7.5}, Vec3{15, 10, 7.5}, Vec3{15, 20, 7.5}})
        pocket.push_back(planeAt(block, c));
    const OpResult out = take(deleting(block, pocket));
    checkValidResult(out, block);
    CHECK(close(volumeOf(out.shape), 9000.0, 1e-7));
    CHECK(out.shape.count(SubKind::Face) == 6);
    // The top face is the image of the old top face with its opening closed.
    const SubId top = planeAt(block, {15, 15, 10});
    CHECK(primary(out, top) == Relation::Modified);
}

TEST_CASE("deleteFaces removes a fillet and restores the sharp edge", "[kernel][directedit]")
{
    // 80 x 50 x 20 with its four vertical edges rounded (radius 5) and two holes whose top rims are rounded.
    const Shape body = take(testing::makeFilletedBody());
    const std::vector<SubId> rounds = facesOf(body, SurfaceType::Cylinder, 5.0);
    REQUIRE(rounds.size() == 4);
    for (const SubId& r : rounds) {
        const std::vector<SubId> run = blendRun(body, r);
        CHECK(run == std::vector<SubId>{r}); // a corner round touches no other blend
    }
    const double before = volumeOf(body);
    const OpResult out = take(deleting(body, {rounds.front()}));
    checkValidResult(out, body);
    CHECK(close(volumeOf(out.shape) - before, 25.0 * (1.0 - kPiValue / 4.0) * 20.0, 1e-6));
    CHECK(primary(out, rounds.front()) == Relation::Deleted);
    // A face that is not a blend has no blend run; one that is out of range neither.
    CHECK(blendRun(body, planeAt(body, {40, 25, 0})).empty());
    CHECK(blendRun(body, SubId{SubKind::Face, 9999}).empty());
}

TEST_CASE("blendRun follows a chain of fillets around a top edge loop", "[kernel][directedit]")
{
    const Shape block = box({0, 0, 0}, {30, 20, 10});
    std::vector<SubId> topEdges;
    for (std::uint32_t i = 0; i < block.count(SubKind::Edge); ++i) {
        const CurveInfo info = take(curveInfo(block, SubId{SubKind::Edge, i}));
        if (std::abs(info.start.z - 10.0) < 1e-9 && std::abs(info.end.z - 10.0) < 1e-9)
            topEdges.push_back(SubId{SubKind::Edge, i});
    }
    REQUIRE(topEdges.size() == 4);
    JobContext ctx;
    const Shape rounded = take(fillet(block, topEdges, 2.0, ctx)).shape;
    const std::vector<SubId> rounds = facesOf(rounded, SurfaceType::Cylinder, 2.0);
    REQUIRE(rounds.size() >= 4);
    const std::vector<SubId> run = blendRun(rounded, rounds.front());
    CHECK(run.size() >= 4);
    for (const SubId& r : rounds)
        CHECK(std::binary_search(run.begin(), run.end(), r));
    // The whole run comes out; one piece alone either comes out or is refused with the "whole blend" advice.
    const OpResult whole = take(deleting(rounded, run));
    checkValidResult(whole, rounded);
    CHECK(close(volumeOf(whole.shape), 6000.0, 1e-6));
    const Result<OpResult> piece = deleting(rounded, {rounds.front()});
    if (!piece)
        requireFailure(piece, ErrorCode::KernelFailure, "Select the whole blend");
}

TEST_CASE("deleteFaces refuses bad selections", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1));
    requireFailure(deleting(plate, {}), ErrorCode::InvalidArgument, "Select at least one face");
    requireFailure(deleting(plate, {SubId{SubKind::Face, 999}}), ErrorCode::InvalidArgument, "does not have");
    requireFailure(deleting(plate, {SubId{SubKind::Edge, 0}}), ErrorCode::InvalidArgument, "does not have");
    requireFailure(deleting(Shape{}, {SubId{SubKind::Face, 0}}), ErrorCode::InvalidArgument, "must be a solid");
    // Every face of a box: nothing is left to extend.
    const Shape block = box({0, 0, 0}, {10, 10, 10});
    std::vector<SubId> all;
    for (std::uint32_t i = 0; i < 6; ++i)
        all.push_back(SubId{SubKind::Face, i});
    const Result<OpResult> none = deleting(block, all);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::KernelFailure);
    CHECK(none.error().message.find("Nothing was changed.") != std::string::npos);
    // One face of a box: Defeaturing reports success and changes nothing (a no-op is never a success).
    const Result<OpResult> one = deleting(block, {SubId{SubKind::Face, 0}});
    requireFailure(one, ErrorCode::KernelFailure, "can't be extended to close the gap");
}

TEST_CASE("offsetFaces pushes and pulls a face square to its neighbours", "[kernel][directedit]")
{
    const Shape block = box({0, 0, 0}, {10, 20, 30});
    const SubId top = planeAt(block, {5, 10, 30});
    for (const double d : {2.0, -2.0}) {
        INFO("distance " << d);
        const OpResult out = take(pushing(block, {top}, d));
        checkValidResult(out, block);
        CHECK(close(volumeOf(out.shape), 10.0 * 20.0 * (30.0 + d), 1e-9));
        CHECK(out.warnings.empty());
        // The moved face is the image of the old top face, not a new face from a tool.
        REQUIRE(primary(out, top) == Relation::Modified);
        const std::vector<SubId> moved = modifiedTo(out, 0, top);
        REQUIRE(moved.size() == 1);
        CHECK(close(take(surfaceInfo(out.shape, moved.front())).centroid, Vec3{5, 10, 30 + d}, 1e-9));
        CHECK(orphans(out).empty());
    }
}

TEST_CASE("offsetFaces raises a pocket floor and warns about angled neighbours", "[kernel][directedit]")
{
    const Shape block = pocketBlock();
    const SubId floor = planeAt(block, {15, 15, 5});
    const OpResult raised = take(pushing(block, {floor}, 2.0));
    checkValidResult(raised, block);
    CHECK(close(volumeOf(raised.shape), volumeOf(block) + 200.0, 1e-9));
    CHECK(raised.warnings.empty());

    // The top face of the filleted body meets the rounded rims of its two holes: a slab, with a warning.
    const Shape body = take(testing::makeFilletedBody());
    const SubId top = planeAtHeight(body, 20.0);
    const OpResult slab = take(pushing(body, {top}, 1.0));
    checkValidResult(slab, body);
    CHECK(volumeOf(slab.shape) > volumeOf(body));
    REQUIRE_FALSE(slab.warnings.empty());
    CHECK(slab.warnings.front().find("step") != std::string::npos);
}

TEST_CASE("offsetFaces refuses curved faces, zero distances and moves that break the part", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1));
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    requireFailure(pushing(plate, {wall.front()}, 1.0), ErrorCode::InvalidArgument, "flat faces only");
    const SubId top = planeAt(plate, {5, 5, 5});
    requireFailure(pushing(plate, {top}, 0.0), ErrorCode::InvalidArgument, "other than zero");
    requireFailure(pushing(plate, {top}, std::nan("")), ErrorCode::InvalidArgument, "must be a number");
    // Pulled in past the bottom: nothing would be left.
    const Result<OpResult> through = pushing(plate, {top}, -8.0);
    REQUIRE_FALSE(through.has_value());
    CHECK(through.error().code == ErrorCode::KernelFailure);
    CHECK(through.error().message.find("Nothing was changed.") != std::string::npos);
    // A U-shaped part whose floor is 1 mm thick: pulling the floor of the slot in by 2 mm cuts it in two.
    const Shape u = cut(box({0, 0, 0}, {30, 10, 10}), box({10, -1, 1}, {20, 11, 11}));
    requireFailure(pushing(u, {planeAt(u, {15, 5, 1})}, -2.0), ErrorCode::KernelFailure, "cut the part in two");
}

TEST_CASE("resizeHole opens up and closes down a through hole", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1)); // radius 3, 5 deep
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    REQUIRE_FALSE(wall.empty());
    for (const double diameter : {8.0, 4.0}) {
        INFO("diameter " << diameter);
        const OpResult out = take(resizing(plate, wall.back(), diameter));
        checkValidResult(out, plate);
        const double r = diameter / 2.0;
        CHECK(close(volumeOf(out.shape), 500.0 - kPiValue * r * r * 5.0, 1e-7));
        // Every old wall face is Modified to faces of the new wall.
        for (const SubId& f : wall) {
            REQUIRE(primary(out, f) == Relation::Modified);
            const std::vector<SubId> now = modifiedTo(out, 0, f);
            REQUIRE_FALSE(now.empty());
            for (const SubId& n : now) {
                const SurfaceInfo info = take(surfaceInfo(out.shape, n));
                CHECK(info.type == SurfaceType::Cylinder);
                CHECK(close(info.radius, r, 1e-9));
            }
        }
        CHECK(orphans(out).empty());
    }
}

TEST_CASE("resizeHole closes down a hole whose end is not a plain circle", "[kernel][directedit]")
{
    // A radius 3 through hole whose top end runs out into a slot across it: the sleeve does not apply, so the bore is
    // filled and cut again at the new size.
    const Shape bored = cut(box({0, 0, 0}, {20, 20, 10}), take(makeCylinder({10, 10, -1}, {0, 0, 1}, 3.0, 12.0)));
    const Shape slotted = cut(bored, box({0, 9, 6}, {20, 11, 11}));
    const std::vector<SubId> wall = facesOf(slotted, SurfaceType::Cylinder, 3.0);
    REQUIRE_FALSE(wall.empty());
    const OpResult out = take(resizing(slotted, wall.front(), 4.0));
    checkValidResult(out, slotted);
    CHECK(volumeOf(out.shape) > volumeOf(slotted));
    CHECK(facesOf(out.shape, SurfaceType::Cylinder, 3.0).empty());
    CHECK_FALSE(facesOf(out.shape, SurfaceType::Cylinder, 2.0).empty());
    for (const SubId& f : wall)
        CHECK(primary(out, f) == Relation::Modified);
}

TEST_CASE("resizeHole refuses faces that are not holes and sizes that change nothing", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1));
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    requireFailure(resizing(plate, wall.front(), 6.0), ErrorCode::InvalidArgument, "already 6 mm across");
    requireFailure(resizing(plate, wall.front(), 0.0), ErrorCode::InvalidArgument, "larger than zero");
    requireFailure(resizing(plate, planeAt(plate, {5, 0, 2.5}), 4.0), ErrorCode::InvalidArgument,
                   "not part of a round hole");
    // A corner round is a cylinder but not a hole.
    const Shape body = take(testing::makeFilletedBody());
    requireFailure(resizing(body, facesOf(body, SurfaceType::Cylinder, 5.0).front(), 4.0), ErrorCode::InvalidArgument,
                   "not part of a round hole");
    // So big that it breaks out of the plate (10 wide): the part would be in pieces or the wall is gone.
    const Result<OpResult> huge = resizing(plate, wall.front(), 30.0);
    REQUIRE_FALSE(huge.has_value());
    CHECK(huge.error().code == ErrorCode::KernelFailure);
}

TEST_CASE("unifySameDomain merges split faces and refuses when there is nothing to merge", "[kernel][directedit]")
{
    // A plate whose hole walls arrive as two half faces each, as many exporters write them (S9 synthetic part).
    JobContext ctx;
    const auto imported =
        take(importStep(std::string(RAY_CORPUS_ROOT_DIR) + "/directedit/synthetic/syn_02_plate_holes_1.stp", {}, ctx));
    REQUIRE(imported.bodies.size() == 1);
    const Shape split = imported.bodies.front().shape;
    const OpResult out = take(unifySameDomain(split, ctx));
    checkValidResult(out, split);
    CHECK(out.shape.count(SubKind::Face) < split.count(SubKind::Face));
    CHECK(close(volumeOf(out.shape), volumeOf(split), 1e-9));
    const Result<OpResult> again = unifySameDomain(out.shape, ctx);
    requireFailure(again, ErrorCode::KernelFailure, "nothing to merge");
    requireFailure(unifySameDomain(box({0, 0, 0}, {1, 2, 3}), ctx), ErrorCode::KernelFailure, "nothing to merge");
}

TEST_CASE("direct edits leave their input untouched and repeat exactly", "[kernel][directedit]")
{
    const Shape block = counterboredBlock();
    const std::string before = testing::rawState(block);
    const std::vector<SubId> bore = facesOf(block, SurfaceType::Cylinder, 3.0);
    const SubId top = planeAtHeight(block, 10.0);
    std::vector<unsigned char> first, second;
    for (int run = 0; run < 2; ++run) {
        std::vector<unsigned char>& bytes = run == 0 ? first : second;
        for (const OpResult& op : {take(deleting(block, {bore.front()})), take(resizing(block, bore.front(), 5.0)),
                                   take(resizing(block, bore.front(), 7.0)), take(pushing(block, {top}, 1.5)),
                                   take(pushing(block, {top}, -1.5))}) {
            appendHash(bytes, op.shape);
            appendHistory(bytes, op.history);
        }
        CHECK(testing::rawState(block) == before);
    }
    CHECK(first == second);
}

TEST_CASE("a cancelled direct edit returns at once", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1));
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), {}};
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    const std::vector<SubId> top{planeAt(plate, {5, 5, 5})};
    const std::vector<Result<OpResult>> results{deleteFaces(plate, wall, ctx),
                                                resizeHole(plate, wall.front(), 4.0, ctx),
                                                offsetFaces(plate, top, 1.0, ctx), unifySameDomain(plate, ctx)};
    for (const Result<OpResult>& r : results) {
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("cancelling a long fillet removal releases the caller within 100 ms and stops it within 10 s",
          "[kernel][directedit][wallclock]")
{
    // Blend runs of NIST CTC 05 whose removal runs for minutes (spike S9: past 150 s; T-080 measurement).
    using Clock = std::chrono::steady_clock;
    JobContext load;
    const auto imported =
        take(importStep(std::string(RAY_CORPUS_DIR) + "/nist-pmi/nist_ctc_05_asme1_rd_geometry.stp", {}, load));
    // The largest solid of the file (as the corpus measurement takes it).
    Shape part;
    double largest = -1.0;
    for (const ImportedBody& body : imported.bodies)
        if (body.shape.count(SubKind::Solid) == 1 && volumeOf(body.shape) > largest) {
            largest = volumeOf(body.shape);
            part = body.shape;
        }
    REQUIRE_FALSE(part.isNull());
    const std::vector<testing::DirectEditCase> cases = take(testing::directEditCases(part));
    testing::waitForAbandonedEdits();
    const int baseline = testing::runningEditThreads();
    // Every fillet and blend run, cancelled after 2 s; and the fillet whose face trim used to ignore a cancel for
    // minutes (k1, port patch 0011) once more, cancelled after 20 s, when it is in that trim.
    std::vector<std::pair<const testing::DirectEditCase*, int>> runs;
    for (const testing::DirectEditCase& c : cases)
        if (c.edit == testing::DirectEdit::DeleteBlendRun || c.edit == testing::DirectEdit::DeleteFillet)
            runs.emplace_back(&c, 2);
    for (const testing::DirectEditCase& c : cases)
        if (c.edit == testing::DirectEdit::DeleteFillet && c.k == 1)
            runs.emplace_back(&c, 20);
    int measured = 0;
    for (const auto& [edit, delay] : runs) {
        const testing::DirectEditCase& c = *edit;
        CancelSource source;
        JobContext ctx{source.token(), {}};
        std::optional<Result<OpResult>> result;
        Clock::time_point returned;
        std::atomic<bool> done{false};
        std::thread caller([&] {
            result.emplace(deleteFaces(part, c.faces, ctx));
            returned = Clock::now();
            done.store(true);
        });
        // Only a call still running when it is cancelled says anything about cancelling.
        const Clock::time_point start = Clock::now();
        while (!done.load() && Clock::now() - start < std::chrono::seconds(delay))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool slow = !done.load();
        const Clock::time_point cancelled = Clock::now();
        source.cancel();
        caller.join();
        if (!slow)
            continue;
        ++measured;
        const double releaseMs = std::chrono::duration<double, std::milli>(returned - cancelled).count();
        while (testing::runningEditThreads() > baseline && Clock::now() - cancelled < std::chrono::seconds(30))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - cancelled).count();
        INFO(testing::directEditName(c.edit)
             << " k" << c.k << " cancelled after " << delay << " s: caller released after " << releaseMs
             << " ms, edit stopped after " << stopMs << " ms");
        REQUIRE(result.has_value());
        CHECK_FALSE(result->has_value());
        if (!result->has_value())
            CHECK(result->error().code == ErrorCode::Cancelled);
        CHECK(releaseMs < 100.0);
        CHECK(stopMs < 10000.0);
        std::printf("cancel %s k%d after %d s: caller released after %.1f ms, edit stopped after %.0f ms\n",
                    testing::directEditName(c.edit), c.k, delay, releaseMs, stopMs);
    }
    CHECK(measured > 0);
}

TEST_CASE("a fault inside a direct edit is contained and the next edit works", "[kernel][directedit]")
{
    const Shape plate = take(testing::makeHolePlate(1, 1));
    const std::vector<SubId> wall = facesOf(plate, SurfaceType::Cylinder);
    const std::uint64_t faultsBefore = testing::structuredExceptions();
    {
        testing::DirectEditFault fault(1);
        const Result<OpResult> broken = deleting(plate, {wall.front()});
        REQUIRE_FALSE(broken.has_value());
        CHECK(broken.error().code == ErrorCode::KernelFailure);
        CHECK(broken.error().message == "The geometry kernel failed on this edit. Nothing was changed.");
        REQUIRE_FALSE(broken.error().context.empty());
#if defined(_WIN32)
        CHECK(broken.error().context.front().find("structured exception 0xc0000005") != std::string::npos);
        CHECK(testing::structuredExceptions() > faultsBefore);
#else
        (void)faultsBefore;
#endif
    }
    const OpResult fine = take(deleting(plate, {wall.front()}));
    CHECK(close(volumeOf(fine.shape), 500.0, 1e-7));
}

namespace {

// A body written by kernel::serialize() into tests/unit/kernel/data.
Shape loadSavedBody(const char* name)
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

// vol(shape inside the box around `centre` with half-size `half`), through the public kernel.
double volumeNear(const Shape& shape, Vec3 centre, double half)
{
    JobContext ctx;
    const Shape box = take(makeBox({centre.x - half, centre.y - half, centre.z - half},
                                   {centre.x + half, centre.y + half, centre.z + half}));
    return volumeOf(take(boolean(BoolOp::Intersect, shape, box, ctx)).shape);
}

} // namespace

TEST_CASE("a small push/pull on a large imported body is judged by the volume near the face", "[kernel][directedit]")
{
    // NIST CTC 02, AP203 with PMI (47,098,068 mm3, 487 faces), as imported: its whole-body volume, integrated face by
    // face, is off by tens of mm3, so a correct 12.41 mm3 push and pull measured +2.2 and -36.1 mm3 and the S9 checker
    // called them wrong (T-080 round 2). The edit is right: inside a box around the face it moved exactly the prism.
    const Shape body = loadSavedBody("nist_ctc_02_ap203pmi_body.rbrep");
    REQUIRE(body.count(SubKind::Face) == 487);
    std::optional<SubId> face;
    for (std::uint32_t i = 0; i < body.count(SubKind::Face) && !face; ++i) {
        const SurfaceInfo info = take(surfaceInfo(body, SubId{SubKind::Face, i}));
        if (info.type == SurfaceType::Plane && close(info.area, 6.204985, 1e-6) &&
            close(info.centroid, Vec3{-151.5512, 1.5229, -14.9560}, 1e-5))
            face = SubId{SubKind::Face, i};
    }
    REQUIRE(face.has_value());
    const SurfaceInfo info = take(surfaceInfo(body, *face));
    const double prism = info.area * 2.0;
    const double before = volumeNear(body, info.centroid, 6.0);
    for (const double d : {2.0, -2.0}) {
        INFO("distance " << d);
        const OpResult out = take(pushing(body, {*face}, d));
        checkValidResult(out, body);
        const double change = volumeNear(out.shape, info.centroid, 6.0) - before;
        CHECK(close(change, d > 0 ? prism : -prism, 1e-6));
        // The moved face is the image of the old one, 2 mm along its outward normal.
        const std::vector<SubId> moved = modifiedTo(out, 0, *face);
        REQUIRE(moved.size() == 1);
        const FaceSample was = take(faceSample(body, *face));
        const Vec3 shifted{info.centroid.x + was.normal.x * d, info.centroid.y + was.normal.y * d,
                           info.centroid.z + was.normal.z * d};
        CHECK(close(take(surfaceInfo(out.shape, moved.front())).centroid, shifted, 1e-6));
    }
}
