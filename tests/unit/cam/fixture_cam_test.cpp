// Fixtures are never machining geometry (task T-077), wherever geometry is chosen: the CAM commands refuse an operation
// whose geometry lies on a fixture, a setup that lists a fixture among its part bodies and a part body made a fixture
// while a setup machines it; src/cam's own setup resolution refuses a fixture part body or reference in a snapshot
// that slipped past the commands (a document saved before them); and templates applied to a document find no hole on
// a fixture, though the fixture has holes.
#include "engine_test_util.h"
#include "parametric_test_util.h"

#include <ray/api.h>
#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
namespace dt = ray::doc::test;
namespace ct = ray::cam::test;

namespace {

const std::string kXY = R"({"principal":"xy"})";

// The part: body 'b', a 60 x 40 x 15 block. The fixture: body 'c', a 100 x 20 x 10 bar below it, made a fixture.
struct Bodies {
    dt::RebuildEnv env;
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;

    Bodies()
    {
        state =
            ct::exec(state, dt::addFeature("sketch", 2, dt::sketchProps(kXY, dt::SketchJson().rect(1, 0, 0, 60, 40))));
        state = ct::exec(state, dt::addFeature("extrude", 3, dt::extrudeProps(2, "15", "new", 'b')));
        state = ct::exec(state,
                         dt::addFeature("sketch", 4, dt::sketchProps(kXY, dt::SketchJson().rect(1, -20, 10, 80, 30))));
        state = ct::exec(
            state, dt::addFeature("extrude", 5, dt::extrudeProps(4, "10", "new", 'c', R"(,"direction":"reverse")")));
        state = ct::exec(state, ct::command("body.set", R"({"id":")" + dt::uuid('c', 1) + R"(","role":"fixture"})"));
        build = env.rebuild(state);
        state = ct::exec(state, ct::command("cam.library.embed", ct::library(ct::toolJson(1), {}, ct::machineJson(1))));
    }
    // A face reference on body `body` (its top face).
    std::string topOf(char body, double z) const
    {
        const doc::BuiltBody& b = dt::bodyOf(*build, body);
        const kernel::SubId face = ct::findFace(b.shape, [&](const kernel::SurfaceInfo& s) {
            return s.type == kernel::SurfaceType::Plane && dt::near(s.centroid.z, z);
        });
        return dt::refJson(dt::refTo(*build, body, face));
    }
};

} // namespace

TEST_CASE("the CAM commands refuse a fixture as machining geometry or as a part body", "[cam][fixtures]")
{
    Bodies f;
    // A setup that lists the fixture as a part body.
    Error e = ct::applyFails(f.state, ct::setupAdd(1, "\"" + dt::uuid('c', 1) + "\""));
    CHECK_THAT(e.message, ContainsSubstring("fixture"));
    CHECK_THAT(e.message, ContainsSubstring("partBodies"));
    // The part as part, the fixture as fixture: fine.
    doc::DocStatePtr s = ct::exec(
        f.state, ct::setupAdd(1, "\"" + dt::uuid('b', 1) + "\"", R"(,"fixtureBodies":[")" + dt::uuid('c', 1) + "\"]"));
    // An operation on the part's top: fine. On the fixture's top: refused, naming the fixture.
    s = ct::exec(s, ct::opAdd(1, 1, 1, "[" + f.topOf('b', 15) + "]"));
    e = ct::applyFails(s, ct::opAdd(2, 1, 1, "[" + f.topOf('c', 0) + "]"));
    CHECK_THAT(e.message, ContainsSubstring("geometry[0]"));
    CHECK_THAT(e.message, ContainsSubstring("fixtures are never machining geometry"));
    // The same through an edit of the existing operation.
    e = ct::applyFails(s, ct::command("cam.operation.edit", R"({"id":")" + ct::id('c', 1) + R"(","geometry":[)" +
                                                                f.topOf('b', 15) + "," + f.topOf('c', 0) + "]}"));
    CHECK_THAT(e.message, ContainsSubstring("geometry[1]"));
    CHECK_THAT(e.message, ContainsSubstring("fixture"));
    // A fixture-role body that a setup does not list at all is refused as a fixture too (not merely "not a part body").
    doc::DocStatePtr bare = ct::exec(f.state, ct::setupAdd(1, "\"" + dt::uuid('b', 1) + "\""));
    e = ct::applyFails(bare, ct::opAdd(2, 1, 1, "[" + f.topOf('c', 0) + "]"));
    CHECK_THAT(e.message, ContainsSubstring("fixtures are never machining geometry"));
    // A part body a setup machines cannot become a fixture.
    e = ct::applyFails(s, ct::command("body.set", R"({"id":")" + dt::uuid('b', 1) + R"(","role":"fixture"})"));
    CHECK_THAT(e.message, ContainsSubstring("role"));
    CHECK_THAT(e.message, ContainsSubstring("Setup 1"));
}

TEST_CASE("src/cam refuses a fixture part body or a geometry reference on a fixture that slipped past the commands",
          "[cam][fixtures]")
{
    Bodies f;
    // A document as one saved before the commands refused it: the fixture body is a part body and an operation
    // machines its top. Built by hand, past the commands.
    doc::DocStatePtr s = ct::exec(f.state, ct::setupAdd(1, "\"" + dt::uuid('b', 1) + "\""));
    s = ct::exec(s, ct::opAdd(1, 1, 1, "[" + f.topOf('b', 15) + "]"));
    auto legacy = std::make_shared<doc::DocState>(*s);
    auto cam = std::make_shared<cam::CamState>(*legacy->cam);
    cam->setups[0].partBodies.push_back(ct::body('c'));
    cam::Operation& op = cam->operations.at(ct::opId(1));
    naming::TopoRef fixtureTop = dt::refTo(*f.build, 'c', [&] {
        const doc::BuiltBody& b = dt::bodyOf(*f.build, 'c');
        return ct::findFace(b.shape, [](const kernel::SurfaceInfo& i) {
            return i.type == kernel::SurfaceType::Plane && dt::near(i.centroid.z, 0);
        });
    }());
    legacy->cam = cam;
    const doc::DocStatePtr legacyState = legacy;
    const cam::CamSnapshot snap = doc::camSnapshot(legacyState, f.build);
    REQUIRE(snap.documentFixtures == std::vector<BodyId>{ct::body('c')});
    auto resolver = naming::makeResolver(snap.bodies);
    JobContext ctx;
    {
        const cam::GeometryRequest request{snap, cam->setups[0], op, *resolver};
        auto geom = cam::resolveSetupGeometry(request, ctx);
        REQUIRE_FALSE(geom);
        CHECK(geom.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(geom.error().message, ContainsSubstring("is a fixture: fixtures are never machined"));
    }
    // Without the fixture among the part bodies, a reference on it is refused for what it is.
    cam->setups[0].partBodies.pop_back();
    op.geometry.push_back(fixtureTop);
    {
        const cam::GeometryRequest request{snap, cam->setups[0], op, *resolver};
        auto geom = cam::resolveSetupGeometry(request, ctx);
        REQUIRE_FALSE(geom);
        CHECK(geom.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(geom.error().message, ContainsSubstring("Geometry 2"));
        CHECK_THAT(geom.error().message, ContainsSubstring("fixtures are never machining geometry"));
    }
    // The setup's own fixture list counts too, for a body whose document role is part.
    cam::CamSnapshot partRole = snap;
    partRole.documentFixtures.clear();
    cam->setups[0].fixtureBodies = {ct::body('c')};
    {
        const cam::GeometryRequest request{partRole, cam->setups[0], op, *resolver};
        auto geom = cam::resolveSetupGeometry(request, ctx);
        REQUIRE_FALSE(geom);
        CHECK_THAT(geom.error().message, ContainsSubstring("fixtures are never machining geometry"));
    }
    // And the operation on the part alone resolves, the fixture carried as a collision body.
    op.geometry.pop_back();
    {
        const cam::GeometryRequest request{snap, cam->setups[0], op, *resolver};
        auto geom = cam::resolveSetupGeometry(request, ctx);
        REQUIRE(geom);
        CHECK(geom->partShapes.size() == 1);
        CHECK(geom->fixtureShapes.size() == 1);
    }
}

namespace {

constexpr std::chrono::milliseconds kWait = std::chrono::minutes(5);
constexpr double kIn = 25.4;

std::string invoke(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, api::Caller::User), kWait);
    if (!done) {
        FAIL(action << " failed: " << done.error().message);
    }
    return done->resultJson;
}

void run(api::HeadlessApp& app, const doc::Command& c)
{
    invoke(app, c.name, c.argsJson);
}

// Two counterbored through holes in a plate: the part (body b, 6 x 4 x 0.75 in) or the fixture (body c, a plate
// beside it).
void plateWithHoles(api::HeadlessApp& app, std::uint64_t n, char body, double x0)
{
    const double L = 6 * kIn, W = 4 * kIn, T = 0.75 * kIn;
    run(app, dt::addFeature("sketch", n,
                            dt::sketchProps(R"({"principal":"xy"})", dt::SketchJson().rect(1, x0, 0, x0 + L, W))));
    run(app, dt::addFeature("extrude", n + 1, dt::extrudeProps(n, dt::num(T), "new", body)));
    run(app, dt::addFeature("datum_plane", n + 2, R"({"base":{"principal":"xy"},"offset":)" + dt::num(T) + "}"));
    run(app, dt::addFeature(
                 "sketch", n + 3,
                 dt::sketchProps(R"({"datum":")" + dt::uuid('f', n + 2) + R"("})",
                                 dt::SketchJson().point(1, x0 + L * 0.25, W * 0.5).point(2, x0 + L * 0.75, W * 0.5))));
    run(app, dt::addFeature("hole", n + 4,
                            R"({"sketch":")" + dt::uuid('f', n + 3) + R"(","target":")" + dt::uuid(body, 1) +
                                R"(","kind":"counterbore","diameter":)" + dt::num(0.266 * kIn) +
                                R"(,"end":"through","counterbore":{"diameter":)" + dt::num(0.44 * kIn) +
                                R"(,"depth":)" + dt::num(0.26 * kIn) + "}}"));
}

} // namespace

TEST_CASE("templates applied to a document never pick a hole on a fixture", "[cam][fixtures][templates]")
{
    const std::filesystem::path user =
        std::filesystem::temp_directory_path() / "raycad-fixture-tests" / Uuid::generate().toString();
    std::filesystem::create_directories(user);
    {
        api::AppOptions options;
        options.workerThreads = 4;
        options.userDataDir = user.string();
        options.builtinLibraryDir = std::string(RAY_SOURCE_DIR) + "/resources/library";
        auto app = api::makeHeadlessApp(std::move(options));
        REQUIRE(app != nullptr);
        invoke(*app, "file.new", "{}");
        plateWithHoles(*app, 2, 'b', 0);
        plateWithHoles(*app, 10, 'c', 200); // the fixture: the same holes, 200 mm along X
        run(*app, ct::command("body.set", R"({"id":")" + dt::uuid('c', 1) + R"(","role":"fixture"})"));
        api::OpenDocument* od = app->active();
        REQUIRE(od != nullptr);
        REQUIRE(app->waitForBuild(*od, kWait));
        std::string machine, machineId, material, materialId;
        for (const lib::MachineDef& m : app->libraries()->machines()) {
            if (m.controlDialect == "haas-ngc") {
                machine = lib::toJson(m);
                machineId = m.id.value.toString();
            }
        }
        for (const lib::Material& m : app->libraries()->materials()) {
            if (m.name == "6061-T6") {
                material = lib::toJson(m);
                materialId = m.id.value.toString();
            }
        }
        REQUIRE_FALSE(machine.empty());
        REQUIRE_FALSE(material.empty());
        const std::string setup = ct::id('a', 1);
        invoke(*app, "cam.setup.add",
               R"({"id":")" + setup + R"(","name":"Op 1","partBodies":[")" + dt::uuid('b', 1) +
                   R"("],"fixtureBodies":[")" + dt::uuid('c', 1) + R"("],"machine":")" + machineId +
                   R"(","material":")" + materialId + R"(","library":{"machines":[)" + machine + R"(],"materials":[)" +
                   material + "]}}");
        const std::string preview =
            invoke(*app, "cam.template.preview", R"({"setup":")" + setup + R"(","set":"Drilled holes"})");
        INFO(preview);
        CHECK_THAT(preview, ContainsSubstring("\"holes\":2"));
        invoke(*app, "cam.template.apply", R"({"setup":")" + setup + R"(","set":"Drilled holes"})");
        const doc::DocStatePtr state = od->document->state();
        REQUIRE(state->cam != nullptr);
        REQUIRE_FALSE(state->cam->setups.at(0).operations.empty());
        std::size_t refs = 0;
        for (const OperationId& id : state->cam->setups.at(0).operations) {
            for (const naming::TopoRef& r : state->cam->operations.at(id).geometry) {
                CHECK(r.body == ct::body('b'));
                ++refs;
            }
        }
        CHECK(refs > 0);
    }
    std::error_code ec;
    std::filesystem::remove_all(user, ec);
}
