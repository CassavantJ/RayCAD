// The CAM input hash and the staleness engine (task T-032) on hand-made builds: boxes named as imported bodies, CAM
// state made by the cam.* commands, the engine generating with the test operation type on a real job system.
#include "engine_test_util.h"

#include "test_dispatcher.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

kernel::Shape box(double x, double y, double z)
{
    auto shape = kernel::makeBox(Vec3{0, 0, 0}, Vec3{x, y, z});
    REQUIRE(shape);
    return *shape;
}

doc::BuiltBody built(std::uint64_t feature, const kernel::Shape& shape)
{
    return doc::BuiltBody{shape, naming::nameImported(FeatureId{Uuid::fromString(id('f', feature))}, shape)};
}

// Three part bodies (b1, b2, b3) as built boxes, and a document that names them.
struct Fixture {
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;
    std::string topRef;    // the JSON of b1's top face
    std::string bottomRef; // and of its bottom face

    Fixture()
    {
        for (int n = 1; n <= 3; ++n) {
            state = exec(state, command("body.set", R"({"id":")" + id('b', n) + R"(","name":"Body )" +
                                                        std::to_string(n) + R"(","role":"part"})"));
        }
        auto b = std::make_shared<doc::BuildResult>();
        b->bodies[body('b', 1)] = built(1, box(40, 20, 10));
        b->bodies[body('b', 2)] = built(2, box(30, 30, 5));
        b->bodies[body('b', 3)] = built(3, box(10, 10, 10));
        build = b;
        const auto snap = doc::camSnapshot(state, build);
        const auto resolver = naming::makeResolver(snap.bodies);
        const kernel::Shape& a = build->bodies.at(body('b', 1)).shape;
        const auto top = findFace(a, [](const kernel::SurfaceInfo& s) { return s.centroid.z > 9.99; });
        const auto bottom = findFace(a, [](const kernel::SurfaceInfo& s) { return s.centroid.z < 0.01; });
        topRef = naming::toJson(resolver->makeRef(body('b', 1), top));
        bottomRef = naming::toJson(resolver->makeRef(body('b', 1), bottom));
        // Library items, a setup on b1 and an operation on its top face.
        state = exec(state, command("cam.library.embed",
                                    library(toolJson(1) + "," + toolJson(2, 8.0), holderJson(1), machineJson(1))));
        state = exec(state, setupAdd(1, "\"" + id('b', 1) + "\""));
        state = exec(state, opAdd(1, 1, 1, "[" + topRef + "]"));
    }

    doc::BuildResultPtr withBody(std::uint64_t n, const kernel::Shape& shape) const
    {
        auto b = std::make_shared<doc::BuildResult>(*build);
        b->bodies[body('b', n)] = built(n, shape);
        return b;
    }
};

Hash256 inputHash(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, std::uint64_t op = 1)
{
    const auto snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    auto hash = operationInputHash(snap, *resolver, opId(op));
    if (!hash) {
        FAIL("operationInputHash failed: " << hash.error().message);
    }
    return *hash;
}

Error inputHashFails(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, std::uint64_t op = 1)
{
    const auto snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    auto hash = operationInputHash(snap, *resolver, opId(op));
    if (hash) {
        FAIL("operationInputHash should have failed");
    }
    return hash.error();
}

doc::Command opEdit(const std::string& members)
{
    return command("cam.operation.edit", R"({"id":")" + id('c', 1) + "\"," + members + "}");
}

doc::Command setupEdit(const std::string& members)
{
    return command("cam.setup.edit", R"({"id":")" + id('a', 1) + "\"," + members + "}");
}

// An engine over the test operation type, on a job system whose continuations run when the test drains `ui`.
struct EngineEnv {
    ray::test::TestDispatcher ui;
    std::unique_ptr<IJobSystem> jobs = makeJobSystem(ui, 4);
    std::unique_ptr<ICamEngine> engine = makeCamEngine(*jobs, testOperations(), &doc::camSnapshot);

    EngineEnv()
    {
        REQUIRE(jobs);
        REQUIRE(engine);
        Gate::instance().reset();
    }
    ~EngineEnv()
    {
        Gate::instance().open = true;
        engine.reset();
        jobs.reset();
    }

    // Runs the UI queue until `done` holds (polling: some conditions change on a worker, with nothing posted).
    template <class Done>
    bool until(Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            ui.runPending();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }

    OpState statusOf(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, std::uint64_t op = 1)
    {
        const auto all = engine->status(state, build);
        const auto it = all.find(opId(op));
        REQUIRE(it != all.end());
        return it->second;
    }

    // Generates and waits for the result to be filed (the continuation on the UI thread).
    Result<std::shared_ptr<const Toolpath>> generate(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                                     std::uint64_t op = 1)
    {
        auto job = engine->generate(state, build, opId(op));
        std::optional<Result<std::shared_ptr<const Toolpath>>> result;
        job.then([&](Result<std::shared_ptr<const Toolpath>> r) { result = std::move(r); });
        REQUIRE(until([&] { return result.has_value(); }));
        return *result;
    }
};

} // namespace

TEST_CASE("an operation's input hash changes with its params, tool, cutting data, referenced geometry and stock",
          "[cam][engine][hash]")
{
    Fixture f;
    const Hash256 h0 = inputHash(f.state, f.build);
    CHECK(inputHash(f.state, f.build) == h0); // a pure function of the snapshot

    std::set<Hash256> seen{h0};
    const auto differs = [&](const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const char* what) {
        INFO(what);
        const Hash256 h = inputHash(state, build);
        CHECK(h != h0);
        CHECK(seen.insert(h).second); // and differently from every other change
    };
    differs(exec(f.state, opEdit(R"("params":{"mode":"plain","depth":2})")), f.build, "params");
    differs(exec(f.state, opEdit(R"("tool":{"tool":")" + id('d', 2) + R"("})")), f.build, "another tool");
    differs(exec(f.state, command("cam.library.embed", library(toolJson(1, 6.5)))), f.build,
            "the embedded copy of the same tool changed");
    differs(exec(f.state, opEdit(R"("tool":{"holder":")" + id('e', 1) + R"("})")), f.build, "a holder");
    differs(exec(f.state, opEdit(R"("tool":{"stickOut":30})")), f.build, "stick-out");
    differs(exec(f.state, opEdit(R"("cutting":{"rpm":9000})")), f.build, "spindle speed");
    differs(exec(f.state, opEdit(R"("cutting":{"feedCut":1500})")), f.build, "cutting feed");
    differs(exec(f.state, opEdit(R"("cutting":{"coolant":"flood"})")), f.build, "coolant");
    differs(exec(f.state, opEdit(R"("heights":{"bottom":-6})")), f.build, "heights");
    differs(exec(f.state, opEdit(R"("geometry":[)" + f.bottomRef + "]")), f.build, "other geometry");
    differs(exec(f.state, opEdit(R"("geometry":[])")), f.build, "no geometry");
    differs(exec(f.state, opEdit(R"("type":"test.other")")), f.build, "operation type");
    differs(exec(f.state, setupEdit(R"("stock":{"type":"box","zPos":2})")), f.build, "stock offsets");
    differs(exec(f.state, setupEdit(R"("stock":{"type":"solid","body":")" + id('b', 3) + R"("})")), f.build,
            "solid stock");
    differs(exec(f.state, setupEdit(R"("fixtureBodies":[")" + id('b', 3) + R"("])")), f.build, "a fixture");
    differs(exec(f.state, setupEdit(R"("wcs":{"origin":{"mode":"stockCorner","corner":3}})")), f.build, "WCS origin");
    differs(exec(f.state, setupEdit(R"("wcs":{"origin":{"mode":"stockCorner"},"zFace":)" + f.topRef + "}")), f.build,
            "WCS face");
    differs(exec(exec(f.state, command("cam.library.embed", library({}, {}, machineJson(2)))),
                 setupEdit(R"("machine":")" + id('9', 2) + "\"")),
            f.build, "machine");
    {
        std::string peck = machineJson(1);
        peck.insert(peck.find("\"rigidTapping\""), "\"peckClearance\":0.3,");
        differs(exec(f.state, command("cam.library.embed", library({}, {}, peck))), f.build,
                "the embedded machine's peck clearance");
    }
    // The part itself (here: the part is a different box; its top face is then gone, see the Broken test, so the op
    // without geometry shows the part's content counts).
    {
        const auto noGeometry = exec(f.state, opEdit(R"("geometry":[])"));
        const Hash256 before = inputHash(noGeometry, f.build);
        CHECK(inputHash(noGeometry, f.withBody(1, box(40, 20, 12))) != before);
    }
}

TEST_CASE("the input hash ignores unrelated bodies, the tier, names and G-code-only settings", "[cam][engine][hash]")
{
    Fixture f;
    const Hash256 h0 = inputHash(f.state, f.build);
    const auto same = [&](const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const char* what) {
        INFO(what);
        CHECK(inputHash(state, build) == h0);
    };
    same(f.state, f.withBody(2, box(31, 30, 5)), "an unrelated body changed");
    same(f.state, f.withBody(3, box(1, 1, 1)), "another unrelated body changed");
    same(exec(f.state, opEdit(R"("name":"Renamed")")), f.build, "operation name");
    same(exec(f.state, opEdit(R"("template":"shop-rule-7")")), f.build, "template id");
    same(exec(f.state, opEdit(R"("tool":{"number":12,"lengthOffset":12,"diameterOffset":32})")), f.build,
         "tool number and offset registers");
    same(exec(f.state, setupEdit(R"("name":"Op 10","programNumber":2002,"programComment":"FLIP","post":"fanuc")")),
         f.build, "setup name and program");
    same(exec(f.state, setupEdit(R"("wcs":{"origin":{"mode":"stockCorner","corner":0},"workOffset":2})")), f.build,
         "work offset");
    same(exec(f.state, command("cam.library.embed", library(toolJson(5, 3.0)))), f.build, "an unused tool");
    // A second setup with its own operation does not change the first one's.
    same(exec(exec(f.state, setupAdd(2, "\"" + id('b', 2) + "\"")), opAdd(2, 2, 2)), f.build, "another setup");

    // The tier: display tessellation at the Low and the High preset tolerances (PLAN.md 6.2) leaves the hash alone.
    // Two separate but equal shapes, each meshed at one tier before anything is hashed.
    const kernel::Shape low = box(40, 20, 10);
    const kernel::Shape high = box(40, 20, 10);
    JobContext ctx;
    REQUIRE(kernel::tessellate(low, kernel::MeshOptions{0.2, 20.0 * 3.14159265358979 / 180.0}, ctx));
    REQUIRE(kernel::tessellate(high, kernel::MeshOptions{0.01, 6.0 * 3.14159265358979 / 180.0}, ctx));
    const Hash256 atLow = inputHash(f.state, f.withBody(1, low));
    const Hash256 atHigh = inputHash(f.state, f.withBody(1, high));
    CHECK(atLow == atHigh);
    CHECK(atLow == h0);
}

TEST_CASE("the input hash is the same after the document is saved and read back", "[cam][engine][hash]")
{
    Fixture f;
    const doc::StateObjects objects = doc::serializeState(*f.state);
    std::map<Hash256, std::string> store(objects.features.begin(), objects.features.end());
    auto read = doc::deserializeState(objects.manifest, [&](const Hash256& h) -> Result<std::string> {
        const auto it = store.find(h);
        if (it == store.end()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing", {}});
        }
        return it->second;
    });
    REQUIRE(read);
    const auto reread = std::make_shared<const doc::DocState>(std::move(*read));
    CHECK(reread->stateHash() == f.state->stateHash());
    CHECK(inputHash(reread, f.build) == inputHash(f.state, f.build));
}

TEST_CASE("a broken reference makes the operation ReferenceBroken, and it cannot be generated", "[cam][engine]")
{
    Fixture f;
    EngineEnv env;
    // A reference whose name the model does not have.
    const std::string bogus =
        R"({"body":")" + id('b', 1) + R"(","kind":"face","names":[")" + std::string(64, '7') + R"("]})";
    const auto broken = exec(f.state, opEdit(R"("geometry":[)" + bogus + "]"));
    CHECK(inputHashFails(broken, f.build).code == ErrorCode::ReferenceBroken);
    OpState st = env.statusOf(broken, f.build);
    CHECK(st.status == OpStatus::ReferenceBroken);
    CHECK_THAT(st.message, ContainsSubstring("Geometry 1 of 'Op 1'"));
    auto job = env.engine->generate(broken, f.build, opId(1));
    auto result = job.wait();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::ReferenceBroken);
    CHECK(Gate::instance().runs == 0);

    // A model edit that changes the picked face: the imported names need their fingerprint, so the top face of a
    // taller box is not the face that was picked. Broken, never a guess.
    const auto taller = f.withBody(1, box(40, 20, 12));
    st = env.statusOf(f.state, taller);
    CHECK(st.status == OpStatus::ReferenceBroken);
    // A body that is not built at all.
    auto without = std::make_shared<doc::BuildResult>(*f.build);
    without->bodies.erase(body('b', 1));
    CHECK(env.statusOf(f.state, without).status == OpStatus::ReferenceBroken);
    // A WCS face that is broken breaks every operation of the setup.
    const auto wcsBroken = exec(f.state, setupEdit(R"("wcs":{"zFace":)" + bogus + "}"));
    CHECK(env.statusOf(wcsBroken, f.build).status == OpStatus::ReferenceBroken);
}

TEST_CASE("generation: Stale, then Generating on a worker, then UpToDate with the toolpath of that input hash",
          "[cam][engine]")
{
    Fixture f;
    EngineEnv env;
    const auto blocking = exec(f.state, opEdit(R"("params":{"mode":"block"})"));
    OpState st = env.statusOf(blocking, f.build);
    CHECK(st.status == OpStatus::Stale);
    CHECK(st.toolpath == nullptr);

    Gate::instance().open = false;
    auto job = env.engine->generate(blocking, f.build, opId(1));
    REQUIRE(env.until([] { return Gate::instance().waiting.load() == 1; }));
    CHECK(env.statusOf(blocking, f.build).status == OpStatus::Generating);
    // The same request again is the same job.
    auto again = env.engine->generate(blocking, f.build, opId(1));
    Gate::instance().open = true;
    auto tp = job.wait();
    REQUIRE(tp);
    REQUIRE(again.wait());
    CHECK(*again.wait() == *tp);
    // The continuation on the UI thread files it.
    REQUIRE(env.until([&] { return env.statusOf(blocking, f.build).status == OpStatus::UpToDate; }));
    st = env.statusOf(blocking, f.build);
    CHECK(st.status == OpStatus::UpToDate);
    REQUIRE(st.toolpath != nullptr);
    CHECK(st.toolpath == *tp);
    CHECK(st.toolpath->inputHash == inputHash(blocking, f.build));
    CHECK(st.toolpath->operation == opId(1));
    CHECK(st.toolpath->moves.size() == 2);
    CHECK(st.toolpath->warnings == std::vector<std::string>{"parts 1, selected 1"});
    CHECK(Gate::instance().runs == 1);

    // Up to date again without running the generator: from the cache.
    auto cached = env.generate(blocking, f.build);
    REQUIRE(cached);
    CHECK(*cached == *tp);
    CHECK(Gate::instance().runs == 1);

    // An unrelated model edit keeps it up to date; an edit of the operation makes it Stale (with the old toolpath
    // still shown); going back to the earlier snapshot (undo) finds the toolpath again.
    CHECK(env.statusOf(blocking, f.withBody(2, box(1, 2, 3))).status == OpStatus::UpToDate);
    const auto edited = exec(blocking, opEdit(R"("cutting":{"rpm":9999})"));
    st = env.statusOf(edited, f.build);
    CHECK(st.status == OpStatus::Stale);
    CHECK(st.toolpath == *tp);
    CHECK(env.statusOf(blocking, f.build).status == OpStatus::UpToDate);
}

TEST_CASE("a toolpath whose inputs changed while it was generated is not reported up to date", "[cam][engine]")
{
    Fixture f;
    EngineEnv env;
    const auto first = exec(f.state, opEdit(R"("params":{"mode":"block"})"));
    Gate::instance().open = false;
    auto job = env.engine->generate(first, f.build, opId(1));
    REQUIRE(env.until([] { return Gate::instance().waiting.load() == 1; }));
    // The user edits the operation while it runs.
    const auto second = exec(first, opEdit(R"("heights":{"clearance":30})"));
    CHECK(env.statusOf(second, f.build).status == OpStatus::Stale);
    Gate::instance().open = true;
    REQUIRE(job.wait());
    // It is kept for the inputs it was made from, and only for them.
    REQUIRE(env.until([&] { return env.statusOf(first, f.build).status == OpStatus::UpToDate; }));
    CHECK(env.statusOf(second, f.build).status == OpStatus::Stale);

    // A generation for the new inputs replaces (cancels) one still running for older inputs.
    const auto third = exec(second, opEdit(R"("heights":{"clearance":40})"));
    Gate::instance().open = false;
    auto older = env.engine->generate(second, f.build, opId(1));
    REQUIRE(env.until([] { return Gate::instance().waiting.load() == 1; }));
    auto newer = env.engine->generate(third, f.build, opId(1));
    auto olderResult = older.wait();
    REQUIRE_FALSE(olderResult);
    CHECK(olderResult.error().code == ErrorCode::Cancelled);
    CHECK(env.statusOf(third, f.build).status == OpStatus::Generating);
    Gate::instance().open = true;
    REQUIRE(newer.wait());
    REQUIRE(env.until([&] { return env.statusOf(third, f.build).status == OpStatus::UpToDate; }));
    CHECK(env.statusOf(second, f.build).status == OpStatus::Stale);
}

TEST_CASE("a cancelled generation leaves the operation Stale; a failed one is Failed until its inputs change",
          "[cam][engine]")
{
    Fixture f;
    EngineEnv env;
    const auto blocking = exec(f.state, opEdit(R"("params":{"mode":"block"})"));
    Gate::instance().open = false;
    auto job = env.engine->generate(blocking, f.build, opId(1));
    REQUIRE(env.until([] { return Gate::instance().waiting.load() == 1; }));
    CHECK(job.cancel());
    auto result = job.wait();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    REQUIRE(env.until([&] { return env.statusOf(blocking, f.build).status == OpStatus::Stale; }));
    CHECK(Gate::instance().runs == 1);
    Gate::instance().open = true;

    const auto failing = exec(f.state, opEdit(R"("params":{"mode":"fail"})"));
    auto failed = env.generate(failing, f.build);
    REQUIRE_FALSE(failed);
    OpState st = env.statusOf(failing, f.build);
    CHECK(st.status == OpStatus::Failed);
    CHECK(st.message == "Test generator failed");
    CHECK(env.statusOf(exec(failing, opEdit(R"("params":{"mode":"plain"})")), f.build).status == OpStatus::Stale);

    const auto invalid = exec(f.state, opEdit(R"("params":{"mode":"invalid"})"));
    REQUIRE_FALSE(env.generate(invalid, f.build));
    CHECK(env.statusOf(invalid, f.build).status == OpStatus::Failed);
    CHECK(env.statusOf(invalid, f.build).message == "Test validation failed");
}

TEST_CASE("status: Suppressed, unknown types, no build yet, and generateAllStale", "[cam][engine]")
{
    Fixture f;
    EngineEnv env;
    const auto suppressed =
        exec(f.state, command("cam.operation.suppress", R"({"id":")" + id('c', 1) + R"(","suppressed":true})"));
    CHECK(env.statusOf(suppressed, f.build).status == OpStatus::Suppressed);
    const auto unknown = exec(f.state, opEdit(R"("type":"no.such.type")"));
    OpState st = env.statusOf(unknown, f.build);
    CHECK(st.status == OpStatus::Failed);
    CHECK_THAT(st.message, ContainsSubstring("no.such.type"));
    st = env.statusOf(f.state, nullptr);
    CHECK(st.status == OpStatus::Stale);
    // No CAM state: no operations.
    CHECK(env.engine->status(std::make_shared<const doc::DocState>(), f.build).empty());

    // Two stale operations generate in parallel; a suppressed one is left alone.
    auto state = exec(f.state, setupAdd(2, "\"" + id('b', 2) + "\""));
    state = exec(state, opAdd(2, 2, 2));
    state = exec(state, opAdd(3, 2, 2, "[]", R"({"mode":"plain"})", R"(,"suppressed":true)"));
    env.engine->generateAllStale(state, f.build);
    REQUIRE(env.until([&] {
        const auto all = env.engine->status(state, f.build);
        return all.at(opId(1)).status == OpStatus::UpToDate && all.at(opId(2)).status == OpStatus::UpToDate;
    }));
    CHECK(env.statusOf(state, f.build, 3).status == OpStatus::Suppressed);
    CHECK(Gate::instance().runs == 2);
}

TEST_CASE("the operation registry keeps the first type of a name and lists them sorted", "[cam][engine]")
{
    struct Named final : IOperationType {
        explicit Named(std::string n) : name(std::move(n)) {}
        std::string name;
        std::string_view typeName() const override { return name; }
        std::string_view paramsSchemaJson() const override { return "{}"; }
        Status validate(const GenerateInput&) const override { return {}; }
        Result<Toolpath> generate(const GenerateInput&, JobContext&) const override { return Toolpath{}; }
    };
    OperationRegistry registry;
    registry.add(std::make_unique<Named>("pocket2d"));
    registry.add(std::make_unique<Named>("drill"));
    auto second = std::make_unique<Named>("drill");
    const IOperationType* secondPtr = second.get();
    registry.add(std::move(second));
    registry.add(nullptr);
    registry.add(std::make_unique<Named>(""));
    CHECK(registry.types() == std::vector<std::string_view>{"drill", "pocket2d"});
    REQUIRE(registry.find("drill") != nullptr);
    CHECK(registry.find("drill") != secondPtr);
    CHECK(registry.find("face") == nullptr);
}

TEST_CASE("the engine's setup resolution gives the part, the selected faces and the stock box", "[cam][engine]")
{
    Fixture f;
    const auto state = exec(f.state, setupEdit(R"("stock":{"type":"box","xNeg":1,"xPos":2,"zPos":3},)"
                                               R"("fixtureBodies":[")" +
                                               id('b', 3) + R"("])"));
    const auto snap = doc::camSnapshot(state, f.build);
    const auto resolver = naming::makeResolver(snap.bodies);
    const Setup& setup = snap.cam->setups.at(0);
    const Operation& op = snap.cam->operations.at(opId(1));
    JobContext ctx;
    auto geom = resolveSetupGeometry(GeometryRequest{snap, setup, op, *resolver}, ctx);
    REQUIRE(geom);
    CHECK(geom->partShapes.size() == 1);
    CHECK(geom->fixtureShapes.size() == 1);
    REQUIRE(geom->selected.size() == 1);
    CHECK(geom->selectedShapeIndex == std::vector<int>{0});
    auto info = kernel::surfaceInfo(geom->partShapes[0], geom->selected[0]);
    REQUIRE(info);
    CHECK(info->centroid.z > 9.99);
    // The part box (0..40, 0..20, 0..10) grown by the offsets, in the WCS: origin at stock corner 0, the top front
    // left corner (tests/unit/cam/setup_test.cpp covers the rest).
    CHECK(geom->stockBounds.min.x == 0.0);
    CHECK(geom->stockBounds.max.x == 43.0);
    CHECK(geom->stockBounds.min.z == -13.0);
    CHECK(geom->stockBounds.max.z == 0.0);
}
