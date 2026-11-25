// CAM operations through the direct-edit features (task T-082): an operation's picked faces follow a direct edit of the
// part through the edit's ShapeHistory like through any other feature. An unrelated edit leaves them resolving Exact
// (postable without review; the operation goes Stale because the part changed, never ReferenceBroken); deleting the
// face an operation machines makes it ReferenceBroken; resizing a referenced hole keeps its reference Exact.
#include "engine_test_util.h"

#include "parametric_test_util.h" // the doc tests' sketch and extrude builders (tests/unit/doc)

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <thread>

using namespace ray;
using namespace ray::cam::test;
using ray::cam::OpStatus;

namespace {

namespace dt = ray::doc::test;

} // namespace

TEST_CASE("CAM references follow a direct edit: Exact through an unrelated one, ReferenceBroken when their face goes",
          "[cam][engine][document][directedit]")
{
    // A 60 x 30 x 10 plate (body b) with hole A (radius 3 at (15, 15)) and hole B (radius 4 at (45, 15)).
    dt::RebuildEnv env;
    auto state = std::make_shared<const doc::DocState>();
    const std::string xy = R"({"principal":"xy"})";
    state = exec(state,
                 dt::addFeature(
                     "sketch", 2,
                     dt::sketchProps(
                         xy, dt::SketchJson().rect(1, 0, 0, 60, 30).circleAt(10, 15, 15, 3).circleAt(12, 45, 15, 4))));
    state = exec(state, dt::addFeature("extrude", 3, dt::extrudeProps(2, "10", "new", 'b')));
    const auto build = env.rebuild(state);
    const BodyId b = dt::bodyId('b');
    const kernel::Shape& shape = build->bodies.at(b).shape;
    const auto snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    const auto wall = [&](double r) {
        return findFace(shape, [&](const kernel::SurfaceInfo& s) {
            return s.type == kernel::SurfaceType::Cylinder && std::abs(s.radius - r) < 1e-6;
        });
    };
    const naming::TopoRef wallA = resolver->makeRef(b, wall(3));
    const naming::TopoRef wallB = resolver->makeRef(b, wall(4));
    const naming::TopoRef front =
        resolver->makeRef(b, findFace(shape, [](const kernel::SurfaceInfo& s) {
                              return s.type == kernel::SurfaceType::Plane && std::abs(s.centroid.y) < 1e-6;
                          }));
    // Operation 1 machines hole A, operation 2 the front face.
    state = exec(state, command("cam.library.embed", library(toolJson(1), {}, machineJson(1))));
    state = exec(state, setupAdd(1, "\"" + b.value.toString() + "\""));
    state = exec(state, opAdd(1, 1, 1, "[" + naming::toJson(wallA) + "]"));
    state = exec(state, opAdd(2, 1, 1, "[" + naming::toJson(front) + "]"));
    const auto camState = state;

    Gate::instance().reset();
    auto engine = cam::makeCamEngine(*env.jobs, testOperations(), &doc::camSnapshot);
    REQUIRE(engine);
    const auto statuses = [&](const doc::DocStatePtr& s, const doc::BuildResultPtr& built) {
        std::map<std::uint64_t, OpStatus> out;
        for (const auto& [opIdValue, st] : engine->status(s, built)) {
            for (std::uint64_t n = 1; n <= 2; ++n) {
                if (opIdValue == opId(n)) {
                    out[n] = st.status;
                }
            }
        }
        return out;
    };
    const auto settle = [&](const doc::DocStatePtr& s, const doc::BuildResultPtr& built) {
        engine->generateAllStale(s, built);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            env.ui.runPending();
            bool busy = false;
            for (const auto& [n, st] : statuses(s, built)) {
                busy = busy || st == OpStatus::Generating || st == OpStatus::Stale;
            }
            if (!busy) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    // The references as CAM resolves them on a build.
    const auto resolved = [&](const doc::DocStatePtr& s, const doc::BuildResultPtr& built, const naming::TopoRef& ref) {
        return naming::makeResolver(doc::camSnapshot(s, built).bodies)->resolve(ref);
    };
    const std::map<std::uint64_t, OpStatus> upToDate{{1, OpStatus::UpToDate}, {2, OpStatus::UpToDate}};
    settle(camState, build);
    REQUIRE(statuses(camState, build) == upToDate);

    // Hole B deleted: nothing either operation machines. Both references stay Exact and postable without review; the
    // operations are Stale (the part changed) and regenerate.
    const auto deletedB =
        exec(camState, dt::addFeature("delete_face", 4, R"({"faces":[)" + naming::toJson(wallB) + "]}"));
    const auto builtB = env.rebuild(deletedB);
    REQUIRE(dt::reportOf(*builtB, dt::featureId(4)).status == doc::FeatureStatus::Ok);
    for (const naming::TopoRef* ref : {&wallA, &front}) {
        const naming::Resolved r = resolved(deletedB, builtB, *ref);
        CHECK(r.state == naming::ResolveState::Exact);
        CHECK(r.postableWithoutReview());
    }
    CHECK(statuses(deletedB, builtB) == std::map<std::uint64_t, OpStatus>{{1, OpStatus::Stale}, {2, OpStatus::Stale}});
    settle(deletedB, builtB);
    CHECK(statuses(deletedB, builtB) == upToDate);

    // Hole A resized (the face operation 1 machines): its reference stays Exact, on the new wall.
    const auto resizedA =
        exec(camState, dt::addFeature("resize_hole", 4, R"({"face":)" + naming::toJson(wallA) + R"(,"diameter":8})"));
    const auto builtA = env.rebuild(resizedA);
    REQUIRE(dt::reportOf(*builtA, dt::featureId(4)).status == doc::FeatureStatus::Ok);
    const naming::Resolved r = resolved(resizedA, builtA, wallA);
    CHECK(r.state == naming::ResolveState::Exact);
    REQUIRE_FALSE(r.elements.empty());
    auto info = kernel::surfaceInfo(builtA->bodies.at(b).shape, r.elements.front());
    REQUIRE(info);
    CHECK(std::abs(info->radius - 4.0) < 1e-6);
    CHECK(statuses(resizedA, builtA).at(1) == OpStatus::Stale);

    // Hole A deleted: operation 1's face is gone, so it is ReferenceBroken (it cannot be posted); operation 2 is not.
    const auto deletedA =
        exec(camState, dt::addFeature("delete_face", 4, R"({"faces":[)" + naming::toJson(wallA) + "]}"));
    const auto builtDeletedA = env.rebuild(deletedA);
    REQUIRE(dt::reportOf(*builtDeletedA, dt::featureId(4)).status == doc::FeatureStatus::Ok);
    CHECK(resolved(deletedA, builtDeletedA, wallA).state == naming::ResolveState::Broken);
    const auto st = statuses(deletedA, builtDeletedA);
    CHECK(st.at(1) == OpStatus::ReferenceBroken);
    CHECK(st.at(2) == OpStatus::Stale);
    engine.reset();
}
