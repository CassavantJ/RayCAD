// The on-screen simulation's api layer (task T-074, ray_api/cam_sim_session.h) and the setup preview and states of a
// setup whose stock comes from an earlier one (ray_api/cam_setup.h, the T-075 leftover T-074 fixes):
//   - the timeline's arithmetic on a hand-made timeline: times and positions both ways, steps of one move, the ends;
//   - a real document with a seeded collision (fixture_verify_test.cpp's job: the vise's jaw 3 mm above the part, a
//     pocket whose holder overhangs the jaw): SimSession::run finds HolderHitsFixture, places it on the timeline, and a
//     seek to its place shows the tool tip at the event's position; the first display mesh is whole and within the
//     budget, a seek back to the start brings a mesh of the changed region only; the clean twin (the vise 5 mm lower)
//     has no event;
//   - a second setup, turned over, with its stock from the first: setupStates without the carried stock checks
//     everything but the stock (no "must be simulated first" error any more), previewSetup takes the carried stock from
//     ICamEngine::carriedStock and shows it as a mesh in the model frame, and SimSession runs that setup from it.
#include "engine_test_util.h"
#include "parametric_test_util.h"

#include <ray/cam.h>
#include <ray/sim.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_sim.h>
#include <ray_api/cam_sim_session.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
namespace dt = ray::doc::test;
namespace ct = ray::cam::test;
namespace cs = ray::api::cam_sim;

namespace {

const std::string kXY = R"({"principal":"xy"})";
const std::string kVise = std::string(RAY_SOURCE_DIR) + "/resources/library/fixtures/generic-vise-6in.step";

std::string toolItem()
{
    return R"({"schema":1,"id":")" + ct::id('d', 1) +
           R"(","name":"10 mm 3FL end mill","type":"flatEndMill","flutes":3,"diameter":10,"shankDiameter":10,)"
           R"("fluteLength":13,"shoulderLength":13,"overallLength":60,"defaultToolNumber":1})";
}

std::string machineItem()
{
    return R"({"schema":1,"id":")" + ct::id('9', 1) +
           R"(","name":"Mill","kind":"mill3","axes":[{"letter":"X","min":0,"max":762,"rapid":25400,"maxFeed":10000,)"
           R"("accel":3000},{"letter":"Y","min":0,"max":406,"rapid":25400,"maxFeed":10000,"accel":3000},)"
           R"({"letter":"Z","min":-508,"max":0,"rapid":25400,"maxFeed":10000,"accel":3000}],)"
           R"("spindle":{"maxRpm":12000},"toolChanger":{"capacity":20},)"
           R"("control":{"dialect":"generic-iso","defaultPost":"generic-iso","rigidTapping":true}})";
}

Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

struct Job {
    dt::RebuildEnv env{4};
    std::shared_ptr<cam::OperationRegistry> types = std::make_shared<cam::OperationRegistry>();
    std::unique_ptr<cam::ICamEngine> engine;
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;
    BodyId vise, jaw;

    Job()
    {
        cam::registerBuiltinOperationTypes(*types);
        cam::CamEngineOptions options;
        options.simulateSetup = api::cam_sim::makeSetupSimulator(*env.jobs); // T-075: as the app's engine has it
        engine = cam::makeCamEngine(*env.jobs, types, &doc::camSnapshot, std::move(options));
        REQUIRE(engine);
    }
    ~Job() { engine.reset(); }

    void run(const doc::Command& c) { state = ct::exec(state, c); }
    void rebuild()
    {
        build = env.rebuild(state);
        for (const doc::FeatureReport& r : build->reports) {
            INFO(r.message);
            REQUIRE(r.status == doc::FeatureStatus::Ok);
        }
    }
    naming::TopoRef ref(const BodyId& body, kernel::SubId sub) const
    {
        const doc::BuiltBody& b = build->bodies.at(body);
        auto resolver = naming::makeResolver({naming::ResolverBody{body, b.map, b.shape}});
        return resolver->makeRef(body, sub);
    }
    kernel::SubId face(const BodyId& body, Vec3 n, double along, double area = 0) const
    {
        const kernel::Shape& shape = build->bodies.at(body).shape;
        for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId sub{kernel::SubKind::Face, i};
            auto s = kernel::faceSample(shape, sub);
            auto info = kernel::surfaceInfo(shape, sub);
            if (!s || !info || info->type != kernel::SurfaceType::Plane) {
                continue;
            }
            const double at = info->centroid.x * n.x + info->centroid.y * n.y + info->centroid.z * n.z;
            if (dt::near(s->normal.x, n.x, 1e-9) && dt::near(s->normal.y, n.y, 1e-9) &&
                dt::near(s->normal.z, n.z, 1e-9) && dt::near(at, along, 1e-6) &&
                (area == 0 || dt::near(info->area, area, 1e-6))) {
                return sub;
            }
        }
        FAIL_CHECK("no such face");
        return {};
    }
    template <class T>
    Result<T> wait(JobHandle<T> handle)
    {
        std::optional<Result<T>> result;
        handle.then([&](Result<T> r) { result = std::move(r); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            env.ui.runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(result.has_value());
        env.ui.runPending();
        return std::move(*result);
    }
    std::shared_ptr<const cam::Toolpath> generate(const OperationId& op)
    {
        auto r = wait(engine->generate(state, build, op));
        if (!r) {
            FAIL("generation failed: " << r.error().message);
        }
        return *r;
    }
};

// fixture_verify_test.cpp's document: the part with its pocket, the vise (lowered by `viseZ` after its mate) and its
// jaw as fixtures, the pocket operation with a 10 mm end mill 14 mm out of a 30 mm holder.
void buildJob(Job& j, const std::string& viseZ)
{
    j.run(dt::addFeature("sketch", 2, dt::sketchProps(kXY, dt::SketchJson().rect(1, 0, 0, 100, 60))));
    j.run(dt::addFeature("extrude", 3, dt::extrudeProps(2, "20", "new", 'b')));
    j.run(dt::addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":20})"));
    j.run(dt::addFeature(
        "sketch", 5,
        dt::sketchProps(R"({"datum":")" + dt::uuid('f', 4) + R"("})", dt::SketchJson().rect(1, 20, 6, 80, 26))));
    j.run(dt::addFeature("extrude", 6, dt::extrudeProps(5, "12", "cut", 'b', R"(,"direction":"reverse")")));
    doc::ImportRequest request;
    request.feature = dt::featureId(7);
    request.path = kVise;
    request.role = doc::Body::Role::Fixture;
    JobContext ctx;
    auto prepared = doc::prepareImport(request, ctx);
    REQUIRE(prepared);
    for (const auto& [hash, bytes] : prepared->objects) {
        auto put = j.env.pool->put(bytes);
        REQUIRE(put);
    }
    j.run(prepared->command);
    for (const doc::Body& b : j.state->bodies) {
        if (b.name == "Generic 6 in vise") {
            j.vise = b.id;
        } else if (b.name == "Generic 6 in vise moving jaw") {
            j.jaw = b.id;
        }
    }
    REQUIRE(j.vise.value != Uuid{});
    REQUIRE(j.jaw.value != Uuid{});
    j.rebuild();
    const kernel::SubId fixedJaw = j.face(j.vise, {0, 1, 0}, 0, 152.4 * 44.45);
    const kernel::SubId front = j.face(ct::body('b'), {0, -1, 0}, 0);
    j.run(ct::command("feature.transform_body.add",
                      R"({"id":")" + dt::uuid('f', 8) + R"(","props":{"body":")" + j.vise.value.toString() +
                          R"(","mate":{"face":)" + naming::toJson(j.ref(j.vise, fixedJaw)) + R"(,"target":)" +
                          naming::toJson(j.ref(ct::body('b'), front)) + R"(},"z":")" + viseZ + R"("}})"));
    j.rebuild();
    const kernel::SubId movingJaw = j.face(j.jaw, {0, -1, 0}, -50.8);
    j.run(ct::command("feature.transform_body.add",
                      R"({"id":")" + dt::uuid('f', 9) + R"(","props":{"body":")" + j.jaw.value.toString() +
                          R"(","mate":{"face":)" + naming::toJson(j.ref(j.jaw, movingJaw)) + R"(,"target":)" +
                          naming::toJson(j.ref(j.vise, fixedJaw)) + R"(},"gap":"60 mm"}})"));
    j.rebuild();
    j.run(ct::command("cam.library.embed", ct::library(toolItem(), ct::holderJson(1), machineItem())));
    j.run(ct::setupAdd(1, "\"" + dt::uuid('b', 1) + "\"",
                       R"(,"fixtureBodies":[")" + j.vise.value.toString() + R"(",")" + j.jaw.value.toString() + "\"]"));
    const kernel::SubId floor = j.face(ct::body('b'), {0, 0, 1}, 8);
    j.run(ct::command("cam.operation.add",
                      R"({"id":")" + ct::id('c', 1) + R"(","setup":")" + ct::id('a', 1) +
                          R"(","type":"pocket2d","name":"Pocket","tool":{"tool":")" + ct::id('d', 1) +
                          R"(","holder":")" + ct::id('e', 1) +
                          R"(","stickOut":14},"cutting":{"rpm":8000,"feedCut":1200,"feedPlunge":400,"feedRamp":900},)"
                          R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},"geometry":[)" +
                          naming::toJson(j.ref(ct::body('b'), floor)) + R"(],"params":{"stepdown":4}})"));
}

cs::RunRequest requestFor(Job& j, const SetupId& setup, std::vector<std::shared_ptr<const cam::Toolpath>> paths,
                          double cell)
{
    cs::RunRequest r;
    r.state = j.state;
    r.build = j.build;
    r.setup = setup;
    r.toolpaths = std::move(paths);
    r.toolNumbers.assign(r.toolpaths.size(), 1);
    r.cellMm = cell;
    r.mesh.budget = 400'000;
    return r;
}

double distance(Vec3 a, Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST_CASE("the simulation timeline: times and positions both ways, steps of one move, the ends", "[sim][timeline]")
{
    // Operation 0: a 10 s tool change, then moves ending at 11, 13, 13 (no time) and 20 s. Operation 1: a 5 s tool
    // change from 20 s, then moves ending at 26 and 30 s.
    cs::Timeline t;
    cs::TimelineOperation a;
    a.startSec = 0;
    a.toolChangeSec = 10;
    a.moveEndSec = {11, 13, 13, 20};
    a.endSec = 20;
    a.firstMove = 0;
    cs::TimelineOperation b;
    b.startSec = 20;
    b.toolChangeSec = 5;
    b.moveEndSec = {26, 30};
    b.endSec = 30;
    b.firstMove = 4;
    t.ops = {a, b};
    t.totalSec = 30;
    t.moveCount = 6;

    const auto same = [](sim::TimelinePosition p, std::uint32_t op, std::uint32_t move, double t01) {
        INFO("op " << p.opIndex << " move " << p.moveIndex << " t " << p.t01);
        CHECK(p.opIndex == op);
        CHECK(p.moveIndex == move);
        CHECK(p.t01 == Catch::Approx(t01).margin(1e-12));
    };
    same(t.positionAt(0), 0, 0, 0);
    same(t.positionAt(5), 0, 0, 0); // in the tool change: before the first move
    same(t.positionAt(10.5), 0, 0, 0.5);
    same(t.positionAt(12), 0, 1, 0.5);
    same(t.positionAt(16.5), 0, 3, 0.5); // past the move of no time
    same(t.positionAt(22), 1, 0, 0);     // operation 1's tool change
    same(t.positionAt(28), 1, 1, 0.5);
    same(t.positionAt(31), 1, 1, 1); // clamped to the end
    CHECK(t.secondsAt({0, 1, 0.5}) == Catch::Approx(12));
    CHECK(t.secondsAt({1, 0, 0}) == Catch::Approx(20)); // an operation starts with its tool change
    CHECK(t.secondsAt({1, 1, 1}) == Catch::Approx(30));
    CHECK(t.secondsAt({5, 0, 0}) == Catch::Approx(30)); // past the end: the end
    for (double s : {0.0, 10.5, 12.0, 13.5, 26.0, 29.0, 30.0})
        CHECK(t.secondsAt(t.positionAt(s)) == Catch::Approx(s).margin(1e-9));

    // Steps: to the end of the move it is in, or of the next; back to its start, or the one before.
    same(t.step({0, 0, 0}, 1), 0, 0, 1);
    same(t.step({0, 1, 0.5}, 1), 0, 1, 1);
    same(t.step({0, 1, 1}, 1), 0, 2, 1);
    same(t.step({0, 3, 1}, 1), 1, 0, 1);    // across the operations
    same(t.step({0, 1, 0.5}, -1), 0, 0, 1); // the start of move 1 is the end of move 0
    same(t.step({1, 0, 1}, -1), 0, 3, 1);
    same(t.step({0, 0, 0.5}, -1), 0, 0, 0);
    same(t.step({0, 0, 0}, -1), 0, 0, 0); // stays at the start
    same(t.step({1, 1, 1}, 1), 1, 1, 1);  // stays at the end
    same(t.end(), 1, 1, 1);
    CHECK(t.movesDone({0, 2, 1}) == 3);
    CHECK(t.movesDone({1, 0, 0.3}) == 4);
    same(t.afterMoves(0), 0, 0, 0);
    same(t.afterMoves(5), 1, 0, 1);
    same(t.afterMoves(99), 1, 1, 1);
}

TEST_CASE("inside a canned cycle the time follows the cycle's own motion: rapids, the feed, the dwell, the retract",
          "[sim][timeline][cycle]")
{
    // From (0, 0, 20): a G82 at (10, 0), R 2, bottom -10, at 100 mm/min with a 0.5 s dwell, back to R (G99); then a
    // plain feed of 10 mm along X at 100 mm/min. No machine: rapids at cam.h's 10,000 mm/min on every axis.
    auto path = std::make_shared<cam::Toolpath>();
    path->startZ = 20'000'000;
    cam::Move drill{};
    drill.kind = cam::MoveKind::CycleDrill;
    drill.role = cam::MoveRole::Plunge;
    drill.cycleReturn = cam::CycleReturn::RPlane;
    drill.feedMicronPerMin = 100'000;
    drill.cycleDwellMs = 500;
    drill.x = 10'000'000;
    drill.z = -10'000'000;
    drill.cycleRetract = 2'000'000;
    drill.cycleClearance = 20'000'000;
    cam::Move feed{};
    feed.kind = cam::MoveKind::Linear;
    feed.role = cam::MoveRole::Cut;
    feed.feedMicronPerMin = 100'000;
    feed.x = 20'000'000;
    feed.z = 2'000'000;
    path->moves = {drill, feed};
    sim::SimProgram program{};
    program.ops = {nullptr};
    program.toolpaths = {path};
    program.modelFromWcs = Xform::identity();
    const cs::Timeline t = cs::buildTimeline(program, nullptr);
    REQUIRE(t.ops.size() == 1);
    const cs::TimelineOperation& op = t.ops[0];
    REQUIRE(op.moves() == 2);
    // The cycle: 10 mm of rapid across (0.06 s), 18 mm of rapid down to R (0.108 s), 12 mm of feed (7.2 s), the dwell
    // (0.5 s), 12 mm of rapid back to R (0.072 s): 52 mm in 7.94 s.
    CHECK(op.moveEndSec[0] == Catch::Approx(7.94).epsilon(1e-9));
    CHECK(op.moveEndSec[1] == Catch::Approx(13.94).epsilon(1e-9));
    REQUIRE(op.profileOf(0) != nullptr);
    CHECK(op.profileOf(1) == nullptr); // a plain feed: time even along it
    const auto at = [&](double sec) {
        return t.positionAt(sec);
    };
    // At R, after both rapids.
    CHECK(at(0.168).moveIndex == 0);
    CHECK(at(0.168).t01 == Catch::Approx(28.0 / 52).margin(1e-9));
    // Half way through the feed: 6 mm down from R (an even spread would put it 9.6 s along the 52 mm instead).
    CHECK(at(0.168 + 3.6).t01 == Catch::Approx(34.0 / 52).margin(1e-9));
    CHECK(t.secondsAt({0, 0, 34.0 / 52}) == Catch::Approx(3.768).margin(1e-9));
    // In the dwell: at the bottom all along it; the bottom's time is the dwell's start.
    CHECK(at(7.368 + 0.1).t01 == Catch::Approx(40.0 / 52).margin(1e-9));
    CHECK(at(7.368 + 0.4).t01 == Catch::Approx(40.0 / 52).margin(1e-9));
    CHECK(t.secondsAt({0, 0, 40.0 / 52}) == Catch::Approx(7.368).margin(1e-9));
    // Half way up the retract.
    CHECK(at(7.94 - 0.036).t01 == Catch::Approx(46.0 / 52).margin(1e-9));
    // Both ways, away from the dwell; the plain feed after it is even.
    for (double s : {0.03, 0.1, 0.168, 1.0, 5.0, 7.3, 7.9, 7.94})
        CHECK(t.secondsAt(t.positionAt(s)) == Catch::Approx(s).margin(1e-9));
    CHECK(at(7.94 + 3).moveIndex == 1);
    CHECK(at(7.94 + 3).t01 == Catch::Approx(0.5).margin(1e-9));
    // A G83 with pecks: the knots rise in time and in length, from (0, 0) to (1, 1).
    cam::Move peck = drill;
    peck.kind = cam::MoveKind::CyclePeck;
    peck.cyclePeck = 3'000'000;
    peck.cycleDwellMs = 0;
    path->moves = {peck};
    const cs::Timeline p = cs::buildTimeline(program, nullptr);
    const cs::MoveProfile* profile = p.ops[0].profileOf(0);
    REQUIRE(profile != nullptr);
    REQUIRE(profile->knots.size() > 6);
    CHECK(profile->knots.front() == std::pair<double, double>{0.0, 0.0});
    CHECK(profile->knots.back() == std::pair<double, double>{1.0, 1.0});
    for (std::size_t i = 1; i < profile->knots.size(); ++i) {
        CHECK(profile->knots[i].first >= profile->knots[i - 1].first);
        CHECK(profile->knots[i].second > profile->knots[i - 1].second);
    }
    for (double s = 0; s <= p.totalSec; s += p.totalSec / 37)
        CHECK(p.secondsAt(p.positionAt(s)) == Catch::Approx(s).margin(1e-9));
}

TEST_CASE("a seeded collision: the run finds it, places it on the timeline and shows the tool there; the twin is clean",
          "[sim][session][acceptance]")
{
    Job bad;
    buildJob(bad, "-9.225 mm");
    const auto path = bad.generate(ct::opId(1));
    REQUIRE(path->moves.size() > 10);
    const SetupId setup = bad.state->cam->setups.at(0).id;

    cs::SimSession session(*bad.env.jobs);
    JobContext ctx;
    cs::RunRequest request = requestFor(bad, setup, {path}, 0.2);
    // A Warning no check raises today, added the tests' way: it goes where the simulator's events go. At the end of the
    // first straight feed that moves.
    std::uint32_t wm = 1;
    while (wm + 1 < path->moves.size() &&
           !(path->moves[wm].kind == cam::MoveKind::Linear &&
             (path->moves[wm].x != path->moves[wm - 1].x || path->moves[wm].y != path->moves[wm - 1].y ||
              path->moves[wm].z != path->moves[wm - 1].z)))
        ++wm;
    const cam::Move& warned = path->moves[wm];
    const Vec3 warnedAt{warned.x / 1e6, warned.y / 1e6, warned.z / 1e6};
    request.addedEvents.push_back(sim::SimEvent{sim::EventKind::ExcessEngagement, sim::Severity::Warning, ct::opId(1),
                                                wm, warnedAt, 0, "Added by the test"});
    auto r = session.run(request, ctx);
    if (!r) {
        FAIL("the run failed: " << r.error().message);
    }
    const cs::RunResult& res = **r;
    // The toolpath to draw: the CAM workspace's backplot of it, in the model frame.
    REQUIRE(res.backplots.size() == 1);
    REQUIRE(res.backplots[0]);
    CHECK(res.backplots[0]->moveFirstVertex.size() == path->moves.size() + 1);
    // The added warning: among the events, in order, placed at the end of its move (its position is that move's end).
    std::optional<std::size_t> warning;
    for (std::size_t i = 0; i < res.events.size(); ++i) {
        if (res.events[i].severity == sim::Severity::Warning)
            warning = i;
    }
    REQUIRE(warning.has_value());
    CHECK(res.events[*warning].kind == sim::EventKind::ExcessEngagement);
    CHECK(res.events[*warning].message == "Added by the test");
    REQUIRE(res.places.size() == res.events.size());
    CHECK(res.places[*warning].moveIndex == wm);
    CHECK(res.places[*warning].t01 == Catch::Approx(1).margin(1e-6));
    CHECK(res.places[*warning].sec == Catch::Approx(res.timeline.secondsAt({0, wm, 1})));
    for (std::size_t i = 1; i < res.events.size(); ++i)
        CHECK(res.events[i].moveIndex >= res.events[i - 1].moveIndex);
    CHECK(res.setupName == "Setup 1");
    REQUIRE(res.ops.size() == 1);
    CHECK(res.ops[0] == ct::opId(1));
    CHECK(res.inputs[0] == path->inputHash);
    REQUIRE(res.tools.size() == 1);
    CHECK_FALSE(res.tools[0].holderProfile.empty());
    CHECK(res.cellMm == 0.2);
    // The timeline: every move, the CAM tree's estimate.
    CHECK(res.timeline.moveCount == path->moves.size());
    CHECK(res.timeline.scaledToEstimate);
    cam::CycleTimeOperation op{path.get(), 1};
    const lib::MachineDef& machine = bad.state->cam->machines.begin()->second.item;
    const cam::CycleTime estimate = cam::estimateCycleTime(std::span<const cam::CycleTimeOperation>(&op, 1), machine);
    CHECK(res.timeline.totalSec == Catch::Approx(estimate.totalSec).epsilon(1e-9));
    // The first display: the whole stock at the run's end, within the budget.
    REQUIRE(res.first.mesh);
    CHECK(res.first.full);
    CHECK(res.first.triangles > 0);
    CHECK(res.first.triangles <= 400'000);
    CHECK(res.first.position.opIndex == 0);
    CHECK(res.first.position.moveIndex == path->moves.size() - 1);
    CHECK(res.first.position.t01 == 1);

    // The collision, placed where the holder first meets the jaw.
    REQUIRE(res.events.size() == res.places.size());
    std::optional<std::size_t> hit;
    for (std::size_t i = 0; i < res.events.size(); ++i) {
        if (res.events[i].kind == sim::EventKind::HolderHitsFixture && !hit)
            hit = i;
    }
    REQUIRE(hit.has_value());
    const sim::SimEvent& e = res.events[*hit];
    const cs::EventPlace& place = res.places[*hit];
    CHECK(place.opIndex == 0);
    CHECK(place.moveIndex == e.moveIndex);
    CHECK(place.sec == Catch::Approx(res.timeline.secondsAt({0, place.moveIndex, place.t01})));
    CHECK(place.sec > 0);
    CHECK(place.sec <= res.timeline.totalSec);
    // Events come sorted by operation and move: their places keep that order from one move to the next (within one
    // move each sits where its own contact is).
    for (std::size_t i = 1; i < res.places.size(); ++i) {
        if (res.places[i].opIndex != res.places[i - 1].opIndex ||
            res.places[i].moveIndex != res.places[i - 1].moveIndex)
            CHECK(res.places[i].sec >= res.places[i - 1].sec - 1e-9);
    }

    // A seek there: the tool's tip at the event's position (model frame).
    auto f =
        session.seek({place.opIndex, place.moveIndex, place.t01}, cs::MeshAsk{res.first.version, false, 400'000, 0});
    REQUIRE(f);
    CHECK(f->position.opIndex == place.opIndex);
    CHECK(f->position.moveIndex == place.moveIndex);
    CHECK(f->position.t01 == Catch::Approx(place.t01));
    const Vec3 tip{f->toolPose.m[3], f->toolPose.m[7], f->toolPose.m[11]};
    const Vec3 want = apply(res.modelFromWcs, e.position);
    INFO("tip " << tip.x << " " << tip.y << " " << tip.z << ", event " << want.x << " " << want.y << " " << want.z);
    CHECK(distance(tip, want) < 0.05);
    // The stock changed (it went back in time): a region, not the whole, at most the budget.
    REQUIRE(f->mesh);
    CHECK_FALSE(f->full);
    REQUIRE(f->dirty.has_value());
    CHECK(f->triangles <= 400'000);
    CHECK(f->version != res.first.version);
    // The start: back where nothing is cut; again nothing changes.
    auto g = session.seek(res.timeline.start(), cs::MeshAsk{f->version, false, 400'000, 0});
    REQUIRE(g);
    auto h = session.seek(res.timeline.start(), cs::MeshAsk{g->version, false, 400'000, 0});
    REQUIRE(h);
    CHECK_FALSE(h->mesh);
    // A cap: a dirty region asked for during playback comes coarser.
    auto k = session.seek(res.timeline.end(), cs::MeshAsk{h->version, false, 400'000, 64});
    REQUIRE(k);
    REQUIRE(k->mesh);
    CHECK(k->coarse);
    // The whole stock again at the budget.
    auto all = session.refresh(cs::MeshAsk{k->version, true, 400'000, 0});
    REQUIRE(all);
    CHECK(all->full);
    CHECK(all->triangles > k->triangles);

    // The twin: the vise 5 mm lower; the same operation; nothing found.
    Job clear;
    buildJob(clear, "-14.225 mm");
    const auto twinPath = clear.generate(ct::opId(1));
    cs::SimSession twin(*clear.env.jobs);
    JobContext twinCtx;
    auto t = twin.run(requestFor(clear, clear.state->cam->setups.at(0).id, {twinPath}, 0.2), twinCtx);
    REQUIRE(t);
    for (const sim::SimEvent& ev : (*t)->events)
        UNSCOPED_INFO(static_cast<int>(ev.kind) << " move " << ev.moveIndex << ": " << ev.message);
    CHECK((*t)->events.empty());
}

TEST_CASE("a setup turned over with its stock from the first: states, the preview from the carried stock, the run",
          "[sim][session][carried]")
{
    Job j;
    buildJob(j, "-14.225 mm");
    // 1 mm of stock under the part, for setup 2 to face off.
    j.run(ct::command("cam.setup.edit", R"({"id":")" + ct::id('a', 1) + R"(","stock":{"type":"box","zNeg":1}})"));
    // Setup 2: the part turned over (its Z face is the part's bottom), the stock what setup 1 leaves; a facing pass.
    const std::string bottom = naming::toJson(j.ref(ct::body('b'), j.face(ct::body('b'), {0, 0, -1}, 0)));
    j.run(ct::setupAdd(2, "\"" + dt::uuid('b', 1) + "\"",
                       R"(,"stock":{"type":"fromSetup","setup":")" + ct::id('a', 1) + R"("},"wcs":{"zFace":)" + bottom +
                           R"(,"origin":{"mode":"stockCorner","corner":0},"workOffset":2})"));
    j.run(ct::command("cam.operation.add",
                      R"({"id":")" + ct::id('c', 2) + R"(","setup":")" + ct::id('a', 2) +
                          R"(","type":"face","name":"Face bottom","tool":{"tool":")" + ct::id('d', 1) +
                          R"(","holder":")" + ct::id('e', 1) +
                          R"(","stickOut":30},"cutting":{"rpm":6000,"feedCut":1000,"feedPlunge":300},)"
                          R"("heights":{"clearance":15,"retract":5,"feedPlane":2,"top":0,"bottom":0},)"
                          R"("geometry":[],"params":{"stepdown":1}})"));
    const SetupId one = j.state->cam->setups.at(0).id;
    const SetupId two = j.state->cam->setups.at(1).id;
    CHECK(api::cam_setup::carriedSetups(*j.state) == std::vector<SetupId>{two});

    // States: without the carried stock, everything of setup 2 but its stock is checked, and it resolves.
    JobContext ctx;
    const auto states = api::cam_setup::setupStates(j.state, j.build, ctx);
    REQUIRE(states.count(two) == 1);
    INFO((states.at(two) ? std::string() : states.at(two).error().message));
    CHECK(static_cast<bool>(states.at(two)));
    CHECK(static_cast<bool>(states.at(one)));
    // The preview needs the stock: without it, resolveSetup's error.
    auto without = api::cam_setup::previewSetup(j.state, j.build, two, ctx);
    REQUIRE_FALSE(without);
    CHECK_THAT(without.error().message, ContainsSubstring("simulated first"));

    // The stock from the engine (as the UI asks it: a job), then the preview and the states from it.
    (void)j.generate(ct::opId(1));
    auto carried = j.wait(j.engine->carriedStock(j.state, j.build, two));
    if (!carried) {
        FAIL("carriedStock failed: " << carried.error().message);
    }
    REQUIRE(*carried);
    auto preview = api::cam_setup::previewSetup(j.state, j.build, two, ctx, carried->get());
    if (!preview) {
        FAIL("previewSetup failed: " << preview.error().message);
    }
    CHECK(preview->stock.isNull());
    REQUIRE(preview->stockMesh);
    CHECK(preview->stockMesh->indices.size() == (*carried)->solid->indices.size());
    REQUIRE(preview->stockMesh->faces.size() == 1);
    // In the model frame: within the first setup's stock box moved back into the model.
    const cam::CarriedStock& stock = **carried;
    Box3 model{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}};
    for (std::size_t i = 0; i + 2 < preview->stockMesh->positions.size(); i += 3) {
        model.min.x = std::min<double>(model.min.x, preview->stockMesh->positions[i]);
        model.min.z = std::min<double>(model.min.z, preview->stockMesh->positions[i + 2]);
        model.max.x = std::max<double>(model.max.x, preview->stockMesh->positions[i]);
        model.max.z = std::max<double>(model.max.z, preview->stockMesh->positions[i + 2]);
    }
    const Vec3 lo = apply(stock.modelFromWcs, stock.bounds.min), hi = apply(stock.modelFromWcs, stock.bounds.max);
    CHECK(model.min.x >= std::min(lo.x, hi.x) - 1e-3);
    CHECK(model.max.x <= std::max(lo.x, hi.x) + 1e-3);
    CHECK(model.min.z >= std::min(lo.z, hi.z) - 1e-3);
    CHECK(model.max.z <= std::max(lo.z, hi.z) + 1e-3);
    api::cam_setup::CarriedStocks stocks{{two, *carried}};
    const auto full = api::cam_setup::setupStates(j.state, j.build, ctx, &stocks);
    CHECK(static_cast<bool>(full.at(two)));

    // The run of setup 2 from that stock.
    const auto facing = j.generate(ct::opId(2));
    cs::SimSession session(*j.env.jobs);
    cs::RunRequest request = requestFor(j, two, {facing}, 0.2);
    request.startStock = *carried;
    auto r = session.run(request, ctx);
    if (!r) {
        FAIL("the run of setup 2 failed: " << r.error().message);
    }
    CHECK((*r)->setup == two);
    REQUIRE((*r)->first.mesh);
    CHECK((*r)->first.triangles > 0);
    // Without the stock it is refused, saying why.
    cs::SimSession other(*j.env.jobs);
    auto refused = other.run(requestFor(j, two, {facing}, 0.2), ctx);
    REQUIRE_FALSE(refused);
    CHECK_THAT(refused.error().message, ContainsSubstring("earlier setup"));
}
