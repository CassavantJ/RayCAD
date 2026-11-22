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
    differs(exec(exec(f.state, command("cam.library.embed", library({}, {}, machineJson(2, 9000)))),
                 setupEdit(R"("machine":")" + id('9', 2) + "\"")),
            f.build, "machine");
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

namespace {
// The JSON with every occurrence of `from` replaced by `to`.
std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    REQUIRE(text.find(from) != std::string::npos);
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}
// The JSON object with members added in front.
std::string withMembers(const std::string& object, const std::string& members)
{
    return "{" + members + "," + object.substr(1);
}
} // namespace

TEST_CASE("the input hash takes the tool's and holder's geometry and the machine fields a generator reads",
          "[cam][engine][hash]")
{
    Fixture f;
    const doc::DocStatePtr withHolder = exec(f.state, opEdit(R"("tool":{"holder":")" + id('e', 1) + R"("})"));
    const Hash256 h0 = inputHash(withHolder, f.build);
    const auto embed = [&](const std::string& tools, const std::string& holders, const std::string& machines) {
        return exec(withHolder, command("cam.library.embed", library(tools, holders, machines)));
    };
    const auto same = [&](const doc::DocStatePtr& state, const char* what) {
        INFO(what);
        CHECK(inputHash(state, f.build) == h0);
    };
    const auto differs = [&](const doc::DocStatePtr& state, const char* what) {
        INFO(what);
        CHECK(inputHash(state, f.build) != h0);
    };
    // Not what a generator reads: none of these makes the operation stale.
    same(embed(replaced(toolJson(1), "Tool 1", "Renamed tool"), {}, {}), "tool renamed");
    same(embed(withMembers(toolJson(1), R"("vendor":"Acme","productId":"A-6","comment":"From the catalogue",)"
                                        R"("bodyMaterial":"HSS","threadSpec":"M6x1","aCoating":"TiAlN")"),
               {}, {}),
         "tool vendor, order number, comment, body material, thread designation and an unknown key");
    same(embed(replaced(toolJson(1), R"("defaultToolNumber":1)", R"("defaultToolNumber":9)"), {}, {}),
         "default tool number");
    same(embed(withMembers(toolJson(1), R"("presets":[{"material":"b0000000-0000-4000-8000-000000000099",)"
                                        R"("opClass":"rough","surfaceSpeed":250,"chipLoad":0.05,)"
                                        R"("maxRadialFraction":0.1,"maxAxialFraction":1,"rampAngle":2}])"),
               {}, {}),
         "cutting data of a material (presets): the operation has its own");
    same(embed({}, replaced(holderJson(1), "Holder 1", "Renamed holder"), {}), "holder renamed");
    same(embed({}, withMembers(holderJson(1), R"("vendor":"Acme","comment":"measured")"), {}),
         "holder vendor and comment");
    same(embed({}, {}, replaced(machineJson(1), "Mill 1", "Renamed mill")), "machine renamed");
    same(embed({}, {}, replaced(machineJson(1), R"("useG187":true)", R"("useG187":false)")),
         "a post property of the machine");
    same(embed({}, {},
               replaced(machineJson(1), R"("toolChanger":{"capacity":20})",
                        R"("toolChanger":{"capacity":40,"changeSeconds":2})")),
         "the tool changer");
    same(embed({}, {}, replaced(machineJson(1), R"("powerKw":7.5)", R"("powerKw":11,"torqueNm":80)")),
         "spindle power and torque (feeds are the operation's)");
    same(embed({}, {},
               replaced(replaced(machineJson(1), R"("dialect":"generic-iso")", R"("dialect":"fanuc")"),
                        R"("defaultPost":"generic-iso")", R"("defaultPost":"fanuc")")),
         "controller dialect and default post");
    same(embed({}, {}, withMembers(machineJson(1), R"("vendor":"Haas","model":"VF-2","verified":true)")),
         "vendor, model, verified");
    // The machine's peck clearance is read when a cycle is expanded into plain moves (post host, backplot), from the
    // machine as it is then; the toolpath does not change with it (its moves carry their own cyclePeckClearance).
    const auto withPeck =
        embed({}, {}, replaced(machineJson(1), R"("rigidTapping":true)", R"("peckClearance":0.3,"rigidTapping":true)"));
    same(withPeck, "the machine's peck clearance");
    // The document keeps it (the library's codec), for the post host and the backplot.
    CHECK(withPeck->cam->machines.at(MachineId{Uuid::fromString(id('9', 1))}).item.peckClearance == 0.3);
    // What a generator reads: each of these does.
    differs(embed(replaced(toolJson(1), R"("fluteLength":20)", R"("fluteLength":22)"), {}, {}), "flute length");
    differs(embed(replaced(toolJson(1), R"("flutes":3)", R"("flutes":2)"), {}, {}), "flutes");
    differs(embed(replaced(toolJson(1), R"("type":"flatEndMill")", R"("type":"ballEndMill")"), {}, {}), "tool type");
    differs(embed(withMembers(toolJson(1), R"("cornerRadius":0.5)"), {}, {}), "corner radius");
    differs(embed({}, replaced(holderJson(1), R"("upperDiameter":40)", R"("upperDiameter":42)"), {}), "holder segment");
    differs(embed({}, replaced(holderJson(1), R"("gaugeLength":40)", R"("gaugeLength":45)"), {}), "gauge length");
    differs(embed({}, {}, machineJson(1, 10000)), "maximum spindle speed");
    differs(embed({}, {}, replaced(machineJson(1), R"("max":500)", R"("max":600)")), "axis travel");
    differs(embed({}, {}, replaced(machineJson(1), R"("rigidTapping":true)", R"("rigidTapping":false)")),
            "rigid tapping");
}

TEST_CASE("a generator receives the items reduced to what the input hash covers", "[cam][engine][hash]")
{
    lib::Tool tool;
    tool.id = ToolId{Uuid::fromString(id('d', 1))};
    tool.name = "Tool";
    tool.vendor = "Acme";
    tool.comment = "c";
    tool.extraJson = R"({"x":1})";
    tool.threadSpec = "M6x1";
    tool.presets.push_back(lib::ToolPreset{});
    tool.defaultToolNumber = 4;
    tool.type = lib::ToolType::Tap;
    tool.diameter = 6;
    tool.threadPitch = 1;
    const lib::Tool seen = generatorTool(tool);
    CHECK(seen.id.value == Uuid{});
    CHECK(seen.name.empty());
    CHECK(seen.vendor.empty());
    CHECK(seen.comment.empty());
    CHECK(seen.extraJson.empty());
    CHECK(seen.threadSpec.empty());
    CHECK(seen.presets.empty());
    CHECK(seen.defaultToolNumber == 0);
    CHECK(seen.type == lib::ToolType::Tap);
    CHECK(seen.diameter == 6);
    CHECK(seen.threadPitch == 1);
    CHECK(seen.geometryHash() == tool.geometryHash());

    lib::Holder holder;
    holder.name = "H";
    holder.gaugeLength = 40;
    holder.segments = {{40, 30, 40}};
    CHECK(generatorHolder(holder).name.empty());
    CHECK(generatorHolder(holder).geometryHash() == holder.geometryHash());

    lib::MachineDef machine;
    machine.name = "M";
    machine.maxRpm = 12000;
    machine.spindlePowerKw = 7.5;
    machine.controlDialect = "fanuc";
    machine.postPropertyOverrides = {{"a", "1"}};
    machine.rigidTapping = true;
    machine.peckClearance = 0.3;
    machine.axes = {{'X', 0, 500, 1, 2, 3}};
    const lib::MachineDef m = generatorMachine(machine);
    CHECK(m.peckClearance == 0); // an expansion setting, not a generator input (cam.h operationInputHash)
    CHECK(m.name.empty());
    CHECK(m.spindlePowerKw == 0);
    CHECK(m.controlDialect.empty());
    CHECK(m.postPropertyOverrides.empty());
    CHECK(m.maxRpm == 12000);
    CHECK(m.rigidTapping);
    REQUIRE(m.axes.size() == 1);
    CHECK(m.axes[0].maxTravel == 500);
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

TEST_CASE("forget releases what the engine holds for closed operations; their toolpaths are found again by hash",
          "[cam][engine][forget]")
{
    Fixture f;
    ray::test::TestDispatcher ui;
    auto jobs = makeJobSystem(ui, 4);
    REQUIRE(jobs);
    CamEngineOptions options;
    options.cacheEntries = 1; // one toolpath kept by hash: a second generation pushes the first out of the cache
    auto engine = makeCamEngine(*jobs, testOperations(), &doc::camSnapshot, options);
    REQUIRE(engine);
    Gate::instance().reset();
    const auto until = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline && !done()) {
            ui.runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    };
    const auto generated = [&](const doc::DocStatePtr& state, std::uint64_t op) {
        auto job = engine->generate(state, f.build, opId(op));
        REQUIRE(job.wait());
        REQUIRE(until([&] { return engine->status(state, f.build).at(opId(op)).status == OpStatus::UpToDate; }));
    };
    const auto statusOf = [&](const doc::DocStatePtr& state, std::uint64_t op) {
        return engine->status(state, f.build).at(opId(op));
    };
    const auto state = exec(f.state, opAdd(2, 1, 1));
    const auto edited = exec(state, opEdit(R"("cutting":{"rpm":9999})")); // c1 with other inputs

    // c1's toolpath, then c2's: c1's leaves the one-entry cache, and only its "latest" entry (shown while stale) holds
    // it.
    generated(state, 1);
    std::weak_ptr<const Toolpath> first = statusOf(state, 1).toolpath;
    const std::vector<std::byte> firstBytes = first.lock()->serialize();
    generated(state, 2);
    CHECK(statusOf(state, 1).status == OpStatus::Stale);
    CHECK(statusOf(edited, 1).toolpath == first.lock()); // still shown
    REQUIRE_FALSE(first.expired());

    // Forgetting c1 (its document closed) releases it.
    const std::vector<OperationId> closing{opId(1)};
    engine->forget(closing);
    CHECK(first.expired());
    OpState st = statusOf(state, 1);
    CHECK(st.status == OpStatus::Stale);
    CHECK(st.toolpath == nullptr);
    // c2's toolpath stays in the cache by input hash: forgotten too, it is found again at once (a reopened document).
    const std::vector<OperationId> second{opId(2)};
    engine->forget(second);
    st = statusOf(state, 2);
    CHECK(st.status == OpStatus::UpToDate);
    REQUIRE(st.toolpath != nullptr);
    // c1 generates again, to the same toolpath: same input hash, same bytes.
    const int runs = Gate::instance().runs.load();
    generated(state, 1);
    CHECK(Gate::instance().runs.load() == runs + 1);
    CHECK(statusOf(state, 1).toolpath->serialize() == firstBytes);

    // A generation still running when its operation is forgotten is not filed as that operation's latest toolpath when
    // it ends (it goes into the cache under its input hash, as any result does).
    const auto blocking = exec(state, opEdit(R"("params":{"mode":"block"})"));
    Gate::instance().open = false;
    auto job = engine->generate(blocking, f.build, opId(1));
    REQUIRE(until([] { return Gate::instance().waiting.load() == 1; }));
    engine->forget(closing);
    Gate::instance().open = true;
    REQUIRE(job.wait());
    REQUIRE(until([&] { return statusOf(blocking, 1).status == OpStatus::UpToDate; }));
    st = statusOf(edited, 1);
    CHECK(st.status == OpStatus::Stale);
    CHECK(st.toolpath == nullptr);
    engine.reset();
    jobs.reset();
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
