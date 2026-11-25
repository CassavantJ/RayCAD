// Direct edits (task T-080): deleteFaces, offsetFaces, resizeHole, unifySameDomain and blendRun on bodies with known
// volumes; history; refusals and their messages; immutability; determinism; cancel and abandon; fault containment.
// The success rates on the S9 set are in directedit_corpus_test.cpp.
#include <catch2/catch_test_macros.hpp>

#include "history_test_util.h"
#include "kernel_test_util.h"

#include <directedit_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
