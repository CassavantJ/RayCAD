// boolean and transformed: volumes, history coverage, no-op and failure detection, determinism (task T-094).
#include <catch2/catch_test_macros.hpp>

#include "history_test_util.h"
#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

double volumeOf(const Shape& shape)
{
    return take(massProps(shape)).volume;
}

OpResult run(BoolOp op, const Shape& target, const Shape& tool)
{
    JobContext ctx;
    return take(boolean(op, target, tool, ctx));
}

Error runFails(BoolOp op, const Shape& target, const Shape& tool)
{
    JobContext ctx;
    auto result = boolean(op, target, tool, ctx);
    REQUIRE_FALSE(result.has_value());
    INFO(result.error().message);
    CHECK(result.error().code == ErrorCode::KernelFailure);
    CHECK_FALSE(result.error().message.empty());
    return result.error();
}

Shape cylinder(Vec3 base, Vec3 axis, double radius, double height)
{
    return take(makeCylinder(base, axis, radius, height));
}

void checkValid(const OpResult& op)
{
    REQUIRE_FALSE(op.shape.isNull());
    CHECK(take(testing::isValidShape(op.shape)));
    CHECK(op.shape.count(SubKind::Solid) >= 1);
}

Xform moveBy(double dx, double dy, double dz)
{
    Xform x = Xform::identity();
    x.m[3] = dx;
    x.m[7] = dy;
    x.m[11] = dz;
    return x;
}

// Rotation about Z by `angle` then a move.
Xform rotateZ(double angle, double dx, double dy, double dz)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return Xform{{c, -s, 0, dx, s, c, 0, dy, 0, 0, 1, dz}};
}

} // namespace

TEST_CASE("cut, union and intersection volumes match closed form", "[kernel][boolean]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    SECTION("through hole")
    {
        const Shape tool = cylinder({50, 30, -5}, {0, 0, 1}, 8.0, 30.0);
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(orphans(op).empty());
        CHECK(op.warnings.empty());
        CHECK(close(volumeOf(op.shape), 120000.0 - kPi * 64.0 * 20.0, 1e-9));
        CHECK(op.shape.count(SubKind::Face) == 7);
        // The hole wall comes from the cylinder's side face.
        bool wallFromTool = false;
        for (std::uint32_t f = 0; f < tool.count(SubKind::Face); ++f)
            for (const SubId& r : modifiedTo(op, 1, SubId{SubKind::Face, f}))
                wallFromTool = wallFromTool || take(surfaceInfo(op.shape, r)).type == SurfaceType::Cylinder;
        CHECK(wallFromTool);
    }
    SECTION("overlapping boxes")
    {
        const Shape other = box({80, 40, 10}, {120, 80, 30});
        const double overlap = 20.0 * 20.0 * 10.0;
        const OpResult cut = run(BoolOp::Cut, block, other);
        const OpResult fuse = run(BoolOp::Union, block, other);
        const OpResult common = run(BoolOp::Intersect, block, other);
        for (const OpResult* op : {&cut, &fuse, &common}) {
            checkValid(*op);
            checkHistory(*op, {&block, &other});
            CHECK(orphans(*op).empty());
        }
        CHECK(close(volumeOf(cut.shape), 120000.0 - overlap, 1e-9));
        CHECK(close(volumeOf(fuse.shape), 120000.0 + 40.0 * 40.0 * 20.0 - overlap, 1e-9));
        CHECK(close(volumeOf(common.shape), overlap, 1e-9));
        CHECK(common.shape.count(SubKind::Face) == 6);
    }
}

TEST_CASE("coplanar faces: a flush pocket and a fuse that unifies faces", "[kernel][boolean]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    SECTION("pocket whose tool top is flush with the block top")
    {
        const Shape tool = box({20, 10, 12}, {50, 40, 20});
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(orphans(op).empty());
        CHECK(close(volumeOf(op.shape), 120000.0 - 30.0 * 30.0 * 8.0, 1e-9));
        CHECK(op.shape.count(SubKind::Face) == 11);
    }
    SECTION("notch flush with two sides")
    {
        const Shape tool = box({80, 0, 10}, {100, 60, 20});
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(orphans(op).empty());
        CHECK(close(volumeOf(op.shape), 120000.0 - 20.0 * 60.0 * 10.0, 1e-9));
        CHECK(op.shape.count(SubKind::Face) == 8);
    }
    SECTION("two boxes side by side fuse into one box")
    {
        const Shape right = box({100, 0, 0}, {150, 60, 20});
        const OpResult op = run(BoolOp::Union, block, right);
        checkValid(op);
        checkHistory(op, {&block, &right});
        CHECK(orphans(op).empty());
        CHECK(close(volumeOf(op.shape), 150.0 * 60.0 * 20.0, 1e-9));
        REQUIRE(op.shape.count(SubKind::Face) == 6);
        CHECK(op.shape.count(SubKind::Solid) == 1);
        // Four result faces are each the union of one face from each input.
        int merged = 0;
        for (std::uint32_t f = 0; f < 6; ++f) {
            int sources = 0;
            for (const HistoryEntry& e : op.history.entries)
                if (e.relation == HistoryEntry::Relation::Modified && e.source.kind == SubKind::Face)
                    for (const SubId& r : e.results)
                        sources += r == SubId{SubKind::Face, f} ? 1 : 0;
            merged += sources == 2 ? 1 : 0;
        }
        CHECK(merged == 4);
    }
}

TEST_CASE("a tool face a rounding error away from a body face leaves no sliver", "[kernel][boolean]")
{
    // The naming harness put a sketch plane through a single-precision mesh sample of the face y = 50.8, at
    // y = 50.7999992 (the float nearest 50.8), and a cut from there kept a slab 7.6e-7 mm thick; a user who sketches
    // on a picked point can do the same. The kernel treats faces closer than kernel.h's sliver width as one.
    const double faceY = 50.8;
    const double sketchY = static_cast<double>(static_cast<float>(faceY));
    REQUIRE(faceY - sketchY > 1e-7);
    REQUIRE(faceY - sketchY < 1e-6);
    const Shape block = box({0, 0, 0}, {30, faceY, 20});
    SECTION("a pocket cut down from just below the top face")
    {
        const Shape tool = box({10, sketchY - 8, 5}, {40, sketchY, 15}); // open on the x = 30 side
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(take(testing::sliverFaces(op.shape)).empty());
        CHECK(close(volumeOf(op.shape), 30.0 * faceY * 20.0 - 20.0 * 8.0 * 10.0, 1e-6));
        CHECK(op.shape.count(SubKind::Solid) == 1);
        CHECK(op.shape.count(SubKind::Face) == 10);
        REQUIRE(op.warnings.size() == 1);
        CHECK(op.warnings[0].find("0.0001 mm") != std::string::npos);
    }
    SECTION("a cut stopping just short of the far face")
    {
        const Shape tool = box({10, 1e-6, 5}, {20, 60, 15}); // through the top, 1e-6 mm short of y = 0
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(take(testing::sliverFaces(op.shape)).empty());
        CHECK(close(volumeOf(op.shape), 30.0 * faceY * 20.0 - 10.0 * faceY * 10.0, 1e-6));
        CHECK(op.warnings.size() == 1);
    }
    SECTION("a boss a rounding error short of the face it stands on")
    {
        const Shape tool = box({10, sketchY + 2e-6, 5}, {20, 60, 15}); // a gap of about 2.8e-6 mm
        JobContext ctx;
        auto result = boolean(BoolOp::Union, block, tool, ctx);
        // Separate solids are refused before any sliver check; the message must say so, not crash or join wrongly.
        if (result) {
            checkValid(*result);
            CHECK(result->shape.count(SubKind::Solid) == 1);
        } else {
            CHECK(result.error().code == ErrorCode::KernelFailure);
        }
    }
    SECTION("an exact cut is unchanged")
    {
        const Shape tool = box({10, faceY - 8, 5}, {40, faceY, 15});
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(take(testing::sliverFaces(op.shape)).empty());
        CHECK(op.shape.count(SubKind::Face) == 10);
    }
    SECTION("a skin the body already had is found, and not blamed on a tool elsewhere")
    {
        const Shape skinned = take(testing::makeSkinnedPocket(faceY - sketchY));
        const std::vector<SubId> slivers = take(testing::sliverFaces(skinned));
        CHECK(slivers.size() == 2); // the top face and the pocket's ceiling
        const Shape hole = cylinder({5, 25, -5}, {0, 0, 1}, 2.0, 30.0);
        const OpResult op = run(BoolOp::Cut, skinned, hole);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(take(testing::sliverFaces(op.shape)).size() == 2);
    }
    SECTION("the testing switch keeps the skin, as before the rule")
    {
        const Shape tool = box({10, sketchY - 8, 5}, {40, sketchY, 15});
        const testing::SliverMergeOff off;
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(op.shape.count(SubKind::Face) == 11);
        CHECK(take(testing::sliverFaces(op.shape)).size() == 2);
    }
    SECTION("a skin thicker than the sliver width is left alone")
    {
        const Shape tool = box({10, faceY - 8 - 2e-4, 5}, {40, faceY - 2e-4, 15});
        const OpResult op = run(BoolOp::Cut, block, tool);
        checkValid(op);
        CHECK(op.warnings.empty());
        CHECK(op.shape.count(SubKind::Face) == 11);
    }
}

TEST_CASE("tangent contacts give a valid result or a clear failure", "[kernel][boolean]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    SECTION("a hole tangent to a side face from inside")
    {
        const Shape tool = cylinder({8, 30, -5}, {0, 0, 1}, 8.0, 30.0); // touches x = 0 along a line
        JobContext ctx;
        // OCCT 8.0.1 gives a valid solid here (spike S4 saw a tangent hole fail BRepCheck later in a longer chain);
        // either outcome is allowed, a crash or an invalid success is not.
        auto result = boolean(BoolOp::Cut, block, tool, ctx);
        if (result) {
            checkValid(*result);
            checkHistory(*result, {&block, &tool});
            CHECK(close(volumeOf(result->shape), 120000.0 - kPi * 64.0 * 20.0, 1e-9));
        } else {
            CHECK(result.error().code == ErrorCode::KernelFailure);
            CHECK_FALSE(result.error().message.empty());
        }
    }
    SECTION("a cylinder touching the outside along a line removes nothing")
    {
        const Shape tool = cylinder({-8, 30, -5}, {0, 0, 1}, 8.0, 30.0);
        runFails(BoolOp::Cut, block, tool);
    }
    SECTION("a cylinder touching the outside along a line cannot be joined")
    {
        const Shape tool = cylinder({-8, 30, 0}, {0, 0, 1}, 8.0, 20.0);
        runFails(BoolOp::Union, block, tool);
    }
    SECTION("a box resting on the top face removes nothing")
    {
        const Shape tool = box({10, 10, 20}, {30, 30, 40});
        runFails(BoolOp::Cut, block, tool);
    }
    SECTION("a box resting on the top face joins along it")
    {
        const Shape tool = box({10, 10, 20}, {30, 30, 40});
        const OpResult op = run(BoolOp::Union, block, tool);
        checkValid(op);
        checkHistory(op, {&block, &tool});
        CHECK(orphans(op).empty());
        CHECK(close(volumeOf(op.shape), 120000.0 + 8000.0, 1e-9));
        CHECK(op.shape.count(SubKind::Solid) == 1);
    }
}

TEST_CASE("no-ops, empty results and separate pieces are reported", "[kernel][boolean]")
{
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const Shape far = box({200, 0, 0}, {210, 10, 10});
    const Shape inner = box({10, 10, 5}, {20, 20, 15});
    const Shape enclosing = box({-10, -10, -10}, {110, 70, 30});
    SECTION("cut by a tool that misses")
    {
        runFails(BoolOp::Cut, block, far);
    }
    SECTION("union with material inside the body")
    {
        runFails(BoolOp::Union, block, inner);
    }
    SECTION("union with a body far away")
    {
        runFails(BoolOp::Union, block, far);
    }
    SECTION("intersection with a body far away")
    {
        runFails(BoolOp::Intersect, block, far);
    }
    SECTION("intersection with a tool that encloses the body")
    {
        runFails(BoolOp::Intersect, block, enclosing);
    }
    SECTION("cut by a tool that encloses the body")
    {
        runFails(BoolOp::Cut, block, enclosing);
    }
    SECTION("union of a body with itself")
    {
        runFails(BoolOp::Union, block, block);
    }
    SECTION("cut of a body by itself")
    {
        runFails(BoolOp::Cut, block, block);
    }
    SECTION("a cut that splits the body into two solids")
    {
        const Shape wall = box({40, -5, -5}, {60, 65, 25});
        const OpResult op = run(BoolOp::Cut, block, wall);
        checkValid(op);
        checkHistory(op, {&block, &wall});
        CHECK(op.shape.count(SubKind::Solid) == 2);
        CHECK(close(volumeOf(op.shape), 120000.0 - 20.0 * 60.0 * 20.0, 1e-9));
        REQUIRE(op.warnings.size() == 1);
        CHECK(op.warnings[0].find("2 separate solids") != std::string::npos);
    }
    SECTION("an inner cavity is a valid cut")
    {
        const OpResult op = run(BoolOp::Cut, block, inner);
        checkValid(op);
        CHECK(close(volumeOf(op.shape), 120000.0 - 1000.0, 1e-9));
        CHECK(op.shape.count(SubKind::Solid) == 1);
    }
}

TEST_CASE("boolean arguments that are not solids are rejected", "[kernel][boolean][errors]")
{
    JobContext ctx;
    const Shape block = box({0, 0, 0}, {10, 10, 10});
    const Shape face = take(testing::makePlanarFace());
    for (const auto& [a, b] :
         {std::pair{Shape(), block}, std::pair{block, Shape()}, std::pair{face, block}, std::pair{block, face}}) {
        auto result = boolean(BoolOp::Cut, a, b, ctx);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(result.error().message.empty());
    }
    auto bad = boolean(static_cast<BoolOp>(7), block, block, ctx);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("a cancelled boolean returns Cancelled", "[kernel][boolean]")
{
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), Progress()};
    auto result = boolean(BoolOp::Cut, box({0, 0, 0}, {10, 10, 10}), box({5, 5, 5}, {15, 15, 15}), ctx);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
}

TEST_CASE("history covers every result face on the boolean test set", "[kernel][boolean][history]")
{
    JobContext ctx;
    const Shape plate = take(testing::makeHolePlate(2, 3));
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const SweepResult slotTool = take(extrude(profile(xyPlane(12.0), {slot(20, 70, 30, 6, 1, 100)}), 20.0, 0.0, ctx));
    // A ring of radii 10 to 14 about the vertical line through (50, 30), from z = 5 to 30.
    const SweepResult ring = take(
        revolve(profile(xzPlane(), {rectangle(60, 5, 64, 30, 1, 100)}), Vec3{50, 0, 0}, Vec3{0, 0, 1}, 2 * kPi, ctx));
    const Shape movedRing = take(transformed(ring.op.shape, moveBy(0, 30, 0))).shape;
    const Shape cross = cylinder({-5, 30, 10}, {1, 0, 0}, 4.0, 110.0);
    const Shape bore = cylinder({50, 30, -5}, {0, 0, 1}, 9.0, 40.0);
    const Shape plateBore = cylinder({10, 10, -1}, {0, 0, 1}, 4.0, 10.0); // overlaps four of the plate's holes
    const Shape plateCorner = box({-5, -5, 1}, {17, 12, 3});
    const Shape filleted = take(testing::makeFilletedBody());
    const Shape slab = box({-5, -5, 2}, {85, 55, 4});
    const Shape side = box({70, 10, 5}, {120, 40, 15});
    struct Case {
        const char* name;
        BoolOp op;
        const Shape* target;
        const Shape* tool;
    };
    const Case cases[] = {
        {"slot pocket", BoolOp::Cut, &block, &slotTool.op.shape},
        {"ring groove", BoolOp::Cut, &block, &movedRing},
        {"ring boss", BoolOp::Union, &block, &movedRing},
        {"cross hole", BoolOp::Cut, &block, &cross},
        {"bore", BoolOp::Cut, &block, &bore},
        {"bore through the hole plate", BoolOp::Cut, &plate, &plateBore},
        {"hole plate corner", BoolOp::Intersect, &plate, &plateCorner},
        {"filleted body cut in two", BoolOp::Cut, &filleted, &slab},
        {"filleted body slab", BoolOp::Intersect, &filleted, &slab},
        {"filleted body plus block", BoolOp::Union, &filleted, &side},
    };
    int checked = 0;
    for (const Case& c : cases) {
        INFO(c.name);
        auto result = boolean(c.op, *c.target, *c.tool, ctx);
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
        checkValid(*result);
        checkHistory(*result, {c.target, c.tool});
        const std::vector<SubId> missing = orphans(*result);
        if (!missing.empty()) {
            bool reported = false;
            for (const std::string& w : result->warnings)
                reported = reported || w.find("no recorded origin") != std::string::npos;
            CHECK(reported);
        }
        CHECK(missing.empty()); // OCCT 8.0.1 gave history for every boolean face in spike S4
        ++checked;
    }
    // A second cut on a cut body: history is per operation.
    const OpResult first = take(boolean(BoolOp::Cut, block, bore, ctx));
    const OpResult second = take(boolean(BoolOp::Cut, first.shape, cross, ctx));
    checkHistory(second, {&first.shape, &cross});
    CHECK(orphans(second).empty());
    CHECK(checked == 10);
}

TEST_CASE("an extruded tool's side faces can be followed through a cut", "[kernel][boolean][history]")
{
    JobContext ctx;
    const Shape block = box({0, 0, 0}, {100, 60, 20});
    const Profile p = profile(xyPlane(10.0), {slot(20, 70, 30, 6, 1, 100)});
    const SweepResult tool = take(extrude(p, 15.0, 0.0, ctx));
    const OpResult cut = take(boolean(BoolOp::Cut, block, tool.op.shape, ctx));
    // Every slot wall (side face of curve 1..4) ends up as a face of the pocket; the start cap becomes its floor.
    for (const SweepFaceOrigin& o : tool.origins) {
        const std::vector<SubId> to = modifiedTo(cut, 1, o.face);
        if (o.role == SweepFaceOrigin::Role::EndCap) {
            CHECK(to.empty()); // above the block
            continue;
        }
        REQUIRE(to.size() == 1);
        const SurfaceInfo info = take(surfaceInfo(cut.shape, to[0]));
        if (o.role == SweepFaceOrigin::Role::StartCap)
            CHECK(close(info.centroid.z, 10.0, 1e-12));
        else
            CHECK(close(info.centroid.z, 15.0, 1e-9));
    }
}

TEST_CASE("transformed moves rigidly and maps every sub-shape", "[kernel][transform]")
{
    const Shape body = take(testing::makeCutBody());
    const double volume = volumeOf(body);
    const OpResult moved = take(transformed(body, rotateZ(kPi / 6, 10, -20, 5)));
    checkValid(moved);
    checkHistory(moved, {&body});
    CHECK(orphans(moved).empty());
    CHECK(close(volumeOf(moved.shape), volume, 1e-9));
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid})
        CHECK(moved.shape.count(kind) == body.count(kind));
    for (const HistoryEntry& e : moved.history.entries) {
        CHECK(e.relation == HistoryEntry::Relation::Modified);
        CHECK(e.results.size() == 1);
    }
    // Vertex i goes where the transform says.
    const Xform x = rotateZ(kPi / 6, 10, -20, 5);
    for (const HistoryEntry& e : moved.history.entries) {
        if (e.source.kind != SubKind::Vertex)
            continue;
        const Vec3 p = take(vertexPoint(body, e.source));
        const Vec3 q = take(vertexPoint(moved.shape, e.results[0]));
        const Vec3 expected{x.m[0] * p.x + x.m[1] * p.y + x.m[2] * p.z + x.m[3],
                            x.m[4] * p.x + x.m[5] * p.y + x.m[6] * p.z + x.m[7],
                            x.m[8] * p.x + x.m[9] * p.y + x.m[10] * p.z + x.m[11]};
        CHECK(close(q, expected, 1e-12));
    }
    const Box3 shifted = bounds(take(transformed(box({0, 0, 0}, {1, 2, 3}), moveBy(5, 6, 7))).shape);
    CHECK(close(shifted.min, Vec3{5, 6, 7}, 1e-12));
    CHECK(close(shifted.max, Vec3{6, 8, 10}, 1e-12));
    // A moved body works as a boolean tool.
    const Shape block = box({0, 0, 0}, {10, 10, 10});
    const OpResult cut = run(BoolOp::Cut, block, take(transformed(block, moveBy(5, 5, 5))).shape);
    CHECK(close(volumeOf(cut.shape), 1000.0 - 125.0, 1e-9));
}

TEST_CASE("transformed returns the input for the identity", "[kernel][transform]")
{
    const Shape body = take(testing::makeCutBody());
    const OpResult same = take(transformed(body, Xform::identity()));
    CHECK(same.shape.contentHash() == body.contentHash());
    CHECK(same.warnings.empty());
    checkHistory(same, {&body});
    std::size_t entries = 0;
    for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid}) {
        CHECK(same.shape.count(kind) == body.count(kind));
        entries += body.count(kind);
    }
    REQUIRE(same.history.entries.size() == entries);
    for (const HistoryEntry& e : same.history.entries) {
        CHECK(e.relation == HistoryEntry::Relation::Modified);
        CHECK(e.inputIndex == 0);
        REQUIRE(e.results.size() == 1);
        CHECK(e.results[0] == e.source);
    }
}

TEST_CASE("transformed rejects scaling and mirroring", "[kernel][transform][errors]")
{
    const Shape block = box({0, 0, 0}, {10, 10, 10});
    Xform scale = Xform::identity();
    scale.m[0] = 2.0;
    Xform mirror = Xform::identity();
    mirror.m[0] = -1.0;
    Xform skew = Xform::identity();
    skew.m[1] = 0.5;
    Xform nan = moveBy(std::nan(""), 0, 0);
    for (const Xform& bad : {scale, mirror, skew, nan}) {
        auto result = transformed(block, bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(result.error().message.empty());
    }
    auto none = transformed(Shape(), moveBy(1, 0, 0));
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::KernelFailure);
}

namespace {

// A fixed chain of every T-094 operation. Returns the content hash of every intermediate result and the history,
// as bytes; empty on any error. No Catch2 assertions (safe on worker threads).
std::vector<unsigned char> pipeline()
{
    std::vector<unsigned char> out;
    const auto fail = [&] {
        out.clear();
        return out;
    };
    const auto addHash = [&](const Shape& s) {
        const Hash256 h = s.contentHash();
        const auto* bytes = reinterpret_cast<const unsigned char*>(&h);
        out.insert(out.end(), bytes, bytes + sizeof(h));
    };
    const auto addHistory = [&](const ShapeHistory& history) {
        for (const HistoryEntry& e : history.entries) {
            out.push_back(static_cast<unsigned char>(e.relation));
            out.push_back(static_cast<unsigned char>(e.inputIndex));
            out.push_back(static_cast<unsigned char>(e.source.kind));
            appendBits(out, static_cast<double>(e.source.index));
            for (const SubId& r : e.results)
                appendBits(out, static_cast<double>(r.index) + 0.25 * static_cast<double>(r.kind));
        }
    };
    JobContext ctx;
    const Profile plate = profile(
        xyPlane(), {rectangle(0, 0, 120, 80, 1, 100), circleLoop({20, 20}, 6, 11, 101), slot(50, 90, 50, 8, 21, 102)});
    auto base = extrude(plate, 25.0, 0.0, ctx);
    if (!base)
        return fail();
    addHash(base->op.shape);
    for (const SweepFaceOrigin& o : base->origins)
        appendBits(out, static_cast<double>(o.face.index * 1000 + o.curveTag));
    auto ring =
        revolve(profile(xzPlane(), {rectangle(4, 10, 9, 40, 1, 100)}), Vec3{0, 0, 0}, Vec3{0, 0, 1}, 2 * kPi, ctx);
    if (!ring)
        return fail();
    auto placed = transformed(ring->op.shape, rotateZ(0.3, 30, 60, 0));
    if (!placed)
        return fail();
    addHash(placed->shape);
    auto cut = boolean(BoolOp::Cut, base->op.shape, placed->shape, ctx);
    if (!cut)
        return fail();
    addHash(cut->shape);
    addHistory(cut->history);
    auto boss = extrude(profile(xyPlane(25.0), {circleLoop({100, 20}, 10, 1, 100)}), 10.0, 0.05, ctx);
    if (!boss)
        return fail();
    auto fused = boolean(BoolOp::Union, cut->shape, boss->op.shape, ctx);
    if (!fused)
        return fail();
    addHash(fused->shape);
    addHistory(fused->history);
    auto trim = makeBox({-1, -1, -1}, {115, 70, 30});
    if (!trim)
        return fail();
    auto common = boolean(BoolOp::Intersect, fused->shape, *trim, ctx);
    if (!common)
        return fail();
    addHash(common->shape);
    addHistory(common->history);
    return out;
}

} // namespace

TEST_CASE("the result check of a chain of operations agrees with a full BRepCheck", "[kernel][boolean][blend]")
{
    // Every result of the kernel is marked validated, so the next operation on it checks only the faces it changed
    // (boolean_internal.h INCREMENTAL CHECK). RAYCAD_KERNEL_VALIDATE_BOTH makes the kernel run the full check too and
    // count disagreements.
#if defined(_WIN32)
    _putenv_s("RAYCAD_KERNEL_VALIDATE_BOTH", "1");
#else
    setenv("RAYCAD_KERNEL_VALIDATE_BOTH", "1", 1);
#endif
    const std::uint64_t before = testing::validationDisagreements();
    Shape body = box({0, 0, 0}, {100, 60, 20});
    JobContext ctx;
    for (int i = 0; i < 6; ++i) {
        const double x = 10.0 + 14.0 * i;
        body = take(boolean(BoolOp::Cut, body, cylinder({x, 20, -5}, {0, 0, 1}, 4.0, 30.0), ctx)).shape;
        body = take(boolean(BoolOp::Cut, body, box({x - 5, 35, 12}, {x + 5, 70, 25}), ctx)).shape;
        body = take(boolean(BoolOp::Union, body, box({x - 2, -10, 0}, {x + 2, 0, 8}), ctx)).shape;
        CHECK(take(testing::isValidShape(body)));
    }
    // Blends on the result, including sizes OCCT cannot build (the size search makes invalid trial results).
    for (std::uint32_t e = 0; e < body.count(SubKind::Edge); e += 37) {
        const SubId edge{SubKind::Edge, e};
        for (const double size : {0.5, 3.0, 12.0}) {
            auto filleted = fillet(body, {&edge, 1}, size, ctx);
            auto chamfered = chamfer(body, {&edge, 1}, size, ctx);
            if (filleted)
                CHECK(take(testing::isValidShape(filleted->shape)));
            if (chamfered)
                CHECK(take(testing::isValidShape(chamfered->shape)));
        }
    }
#if defined(_WIN32)
    _putenv_s("RAYCAD_KERNEL_VALIDATE_BOTH", "");
#else
    unsetenv("RAYCAD_KERNEL_VALIDATE_BOTH");
#endif
    CHECK(testing::validationDisagreements() == before);
}

TEST_CASE("sweeps and booleans are deterministic over repeated runs and 8 threads", "[kernel][boolean][threads]")
{
    const std::vector<unsigned char> reference = pipeline();
    REQUIRE_FALSE(reference.empty());
    for (int run = 0; run < 3; ++run)
        CHECK(pipeline() == reference);
    constexpr int kThreads = 8;
    std::vector<std::vector<unsigned char>> results(kThreads);
    runOnThreads(kThreads, [&results](int t) {
        for (int run = 0; run < 2; ++run)
            results[static_cast<std::size_t>(t)] = pipeline();
    });
    for (const auto& r : results)
        CHECK(r == reference);
}

TEST_CASE("8 threads cutting the same two shapes at once leave them untouched", "[kernel][boolean][threads]")
{
    // Non-destructive mode: OCCT must not write tolerances or curves into the caller's immutable shapes.
    const Shape block = take(testing::makeCutBody());
    const Shape tool = cylinder({20, 30, -5}, {0, 0, 1}, 6.0, 40.0);
    const Hash256 blockHash = block.contentHash();
    const Hash256 toolHash = tool.contentHash();
    const std::vector<std::byte> blockBytes = serialize(block);
    const Hash256 expected = run(BoolOp::Cut, block, tool).shape.contentHash();
    constexpr int kThreads = 8;
    std::vector<Hash256> hashes(kThreads);
    std::vector<int> ok(kThreads, 0);
    runOnThreads(kThreads, [&](int t) {
        JobContext ctx;
        for (int run = 0; run < 3; ++run) {
            auto result = boolean(BoolOp::Cut, block, tool, ctx);
            if (!result)
                return;
            hashes[static_cast<std::size_t>(t)] = result->shape.contentHash();
        }
        ok[static_cast<std::size_t>(t)] = 1;
    });
    for (int t = 0; t < kThreads; ++t) {
        CHECK(ok[static_cast<std::size_t>(t)] == 1);
        CHECK(hashes[static_cast<std::size_t>(t)] == expected);
    }
    // contentHash() is cached, so compare fresh serialisations with the hashes taken before.
    CHECK(serialize(block) == blockBytes);
    const std::vector<std::byte> toolBytes = serialize(tool);
    CHECK(Shape(take(deserialize(toolBytes))).contentHash() == toolHash);
    CHECK(Shape(take(deserialize(serialize(block)))).contentHash() == blockHash);
}
