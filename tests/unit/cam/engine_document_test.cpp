// CAM state inside the document (task T-032): the cam.* commands undo and redo with the rest of the document, the
// CAM state is saved and read back (older documents without it, newer ones, unknown keys), and a model edit rebuilt by
// the real rebuilder marks exactly the operations it affects Stale.
#include "engine_test_util.h"

#include "parametric_test_util.h" // the doc tests' sketch and extrude builders (tests/unit/doc)

#include "commands/cam_json.h"
#include "schema.h" // src/api's JSON Schema validator, to check the stored CAM objects against schemas/doc.cam.schema.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam::test;
using ray::cam::OpStatus;

namespace {

namespace dt = ray::doc::test;

using Objects = std::map<Hash256, std::string>;

Objects objectsOf(const doc::StateObjects& o)
{
    return Objects(o.features.begin(), o.features.end());
}

Result<doc::DocState> readBack(const std::string& manifest, const Objects& objects)
{
    return doc::deserializeState(manifest, [&](const Hash256& h) -> Result<std::string> {
        const auto it = objects.find(h);
        if (it == objects.end()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing object " + h.hex(), {}});
        }
        return it->second;
    });
}

Hash256 hashText(const std::string& s)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span<const char>(s.data(), s.size())));
    return h.finish();
}

std::string camHashOf(const std::string& manifest)
{
    const auto at = manifest.find("\"cam\":\"");
    REQUIRE(at != std::string::npos);
    return manifest.substr(at + 7, 64);
}

// Replaces the object `oldHex` by `bytes` everywhere: in the objects and in the manifest that names it.
void replaceObject(std::string& manifest, Objects& objects, const std::string& oldHex, const std::string& bytes)
{
    const Hash256 h = hashText(bytes);
    objects[h] = bytes;
    const auto at = manifest.find(oldHex);
    REQUIRE(at != std::string::npos);
    manifest.replace(at, 64, h.hex());
}

// Bodies 1 and 2 named in a document with a CAM state: two setups, an operation each.
doc::DocStatePtr camDocument()
{
    auto state = std::make_shared<const doc::DocState>();
    for (int n = 1; n <= 2; ++n) {
        state = exec(state, command("body.set", R"({"id":")" + id('b', n) + R"(","name":"B","role":"part"})"));
    }
    state = exec(state,
                 command("cam.library.embed", library(toolJson(1) + "," + toolJson(2), holderJson(1), machineJson(1))));
    state = exec(state, setupAdd(1, "\"" + id('b', 1) + "\"", R"(,"stock":{"type":"box","zPos":1.5})"));
    state = exec(state, setupAdd(2, "\"" + id('b', 2) + "\"",
                                 R"(,"stock":{"type":"fromSetup","setup":")" + id('a', 1) + R"("})"));
    state = exec(state, opAdd(1, 1, 1));
    state =
        exec(state, opAdd(2, 2, 2, "[]", R"({"mode":"plain","depth":3})", {}, R"(,"holder":")" + id('e', 1) + "\""));
    return state;
}

} // namespace

TEST_CASE("CAM commands undo and redo with the rest of the document", "[cam][engine][document]")
{
    ray::test::TestDispatcher ui;
    auto document = doc::makeDocument(ui);
    REQUIRE(document);
    const auto run = [&](const doc::Command& c) {
        auto result = document->execute(c);
        if (!result) {
            FAIL(c.name << " failed: " << result.error().message);
        }
        return (*result)->stateHash();
    };
    std::vector<Hash256> hashes{document->state()->stateHash()};
    hashes.push_back(run(command("body.set", R"({"id":")" + id('b', 1) + R"(","name":"Plate","role":"part"})")));
    hashes.push_back(run(command("cam.library.embed", library(toolJson(1), {}, machineJson(1)))));
    hashes.push_back(run(setupAdd(1, "\"" + id('b', 1) + "\"")));
    hashes.push_back(run(opAdd(1, 1, 1)));
    hashes.push_back(run(command("body.set", R"({"id":")" + id('b', 1) + R"(","name":"Plate 2"})")));
    hashes.push_back(run(command("cam.operation.edit", R"({"id":")" + id('c', 1) + R"(","cutting":{"rpm":4000}})")));
    hashes.push_back(run(command("cam.operation.remove", R"({"id":")" + id('c', 1) + R"("})")));
    hashes.push_back(run(command("cam.setup.remove", R"({"id":")" + id('a', 1) + R"("})")));
    CHECK(std::set<Hash256>(hashes.begin(), hashes.end()).size() == hashes.size());
    CHECK(document->undoCommand()->name == "cam.setup.remove");

    // Undo one step at a time back to the start, through model and CAM commands alike, then redo it all.
    for (std::size_t i = hashes.size() - 1; i > 0; --i) {
        REQUIRE(document->undo());
        CHECK(document->state()->stateHash() == hashes[i - 1]);
    }
    CHECK(document->state()->cam == nullptr);
    for (std::size_t i = 1; i < hashes.size(); ++i) {
        REQUIRE(document->redo());
        CHECK(document->state()->stateHash() == hashes[i]);
    }
    // Only the embedded items are left: still CAM state.
    REQUIRE(document->state()->cam != nullptr);
    CHECK(document->state()->cam->setups.empty());
    CHECK(document->state()->cam->tools.size() == 1);

    // The CAM state at the operation edit, read through the undo stack.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(document->undo());
    }
    const auto& cam = *document->state()->cam;
    REQUIRE(cam.operations.size() == 1);
    CHECK(cam.operations.at(opId(1)).cutting.spindleRpm == 4000);
    CHECK(cam.setups.at(0).operations == std::vector<OperationId>{opId(1)});

    // A command is a pure function of (snapshot, canonical arguments): its JSON form replays to the same state.
    const auto c = opAdd(9, 1, 1);
    auto json = doc::commandToJson(doc::Command{c.name, doc::canonicalJson(c.argsJson), c.label});
    REQUIRE(json);
    auto back = doc::commandFromJson(*json);
    REQUIRE(back);
    const auto before = document->state();
    CHECK(exec(before, *back)->stateHash() == exec(before, c)->stateHash());
}

TEST_CASE("cam commands check their arguments and change nothing when they fail", "[cam][engine][document]")
{
    auto state = camDocument();
    const auto fails = [&](const doc::Command& c, const char* text) {
        INFO(c.name << " " << c.argsJson);
        const Error e = applyFails(state, c);
        CHECK(e.code == ErrorCode::InvalidArgument);
        CHECK_THAT(e.message, ContainsSubstring(text));
    };
    fails(setupAdd(1, "\"" + id('b', 1) + "\""), "already used by a setup");
    fails(setupAdd(3, "\"" + id('b', 7) + "\""), "not in the document");
    fails(setupAdd(3, ""), "at least one body");
    fails(command("cam.setup.add", R"({"id":")" + id('a', 3) + R"(","name":"S","partBodies":[")" + id('b', 1) +
                                       R"("],"machine":")" + id('9', 5) + R"("})"),
          "not embedded");
    fails(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"fixtureBodies":[")" + id('b', 1) + R"("])"), "is a part body");
    fails(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"stock":{"type":"fromSetup","setup":")" + id('a', 9) + R"("})"),
          "earlier setup");
    fails(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"stock":{"type":"box","xNeg":-1})"), "'stock.xNeg'");
    fails(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"wcs":{"origin":{"mode":"modelPoint"}})"), "originVertex");
    fails(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"colour":"red")"), "'colour' is not an argument");
    fails(opAdd(9, 1, 7), "not embedded");
    fails(opAdd(1, 1, 1), "already used by an operation");
    fails(opAdd(9, 4, 1), "names no setup");
    fails(command("cam.operation.add", R"({"id":")" + id('c', 9) + R"(","setup":")" + id('a', 1) +
                                           R"(","type":"test.op","name":"X","tool":{"tool":")" + id('d', 1) +
                                           R"("},"cutting":{"rpm":0,"feedCut":1,"feedPlunge":1},"heights":)" +
                                           kHeights + "}"),
          "'cutting.rpm' must be greater than 0");
    fails(command("cam.operation.edit", R"({"id":")" + id('c', 1) + R"(","heights":{"retract":50}})"),
          "clearance >= retract");
    fails(command("cam.operation.edit", R"({"id":")" + id('c', 1) + R"(","cutting":{"speed":1}})"),
          "'cutting.speed' is not a known member");
    fails(command("cam.operation.edit", R"({"id":")" + id('c', 1) + R"("})"), "nothing to change");
    fails(command("cam.operation.edit", R"({"id":")" + id('c', 1) + R"(","geometry":[{"body":")" + id('b', 2) +
                                            R"(","kind":"face","names":[")" + std::string(64, '1') + R"("]}]})"),
          "not a part body");
    fails(command("cam.setup.remove", R"({"id":")" + id('a', 1) + R"("})"), "starts from");
    fails(command("cam.setup.reorder", R"({"id":")" + id('a', 2) + R"(","index":0})"), "would not come before it");
    fails(command("cam.library.embed", R"({"tools":[{"id":")" + id('d', 3) + R"(","name":"T"}]})"), "'tools[0]");
    fails(command("cam.library.embed", "{}"), "nothing to embed");

    // And what passes: moving an operation to the other setup, reordering, removing a setup with its operations.
    auto moved = exec(state, command("cam.operation.reorder",
                                     R"({"id":")" + id('c', 1) + R"(","index":0,"setup":")" + id('a', 2) + R"("})"));
    CHECK(moved->cam->setups[0].operations.empty());
    CHECK(moved->cam->setups[1].operations == std::vector<OperationId>{opId(1), opId(2)});
    auto removed =
        exec(exec(state, command("cam.setup.edit", R"({"id":")" + id('a', 2) + R"(","stock":{"type":"box"}})")),
             command("cam.setup.remove", R"({"id":")" + id('a', 2) + R"("})"));
    CHECK(removed->cam->setups.size() == 1);
    CHECK(removed->cam->operations.count(opId(2)) == 0);
    // Defaults of a new operation come from its embedded tool.
    const cam::Operation& op = state->cam->operations.at(opId(1));
    CHECK(op.tool.toolNumber == 1);
    CHECK(op.tool.lengthOffset == 1);
    CHECK(op.tool.stickOut == 25.0);
    CHECK(op.cutting.feedRamp == 1200.0);
    CHECK(op.cutting.coolant == cam::CuttingData::Coolant::Off);
    CHECK(state->cam->setups[0].postId == "generic-iso");
}

TEST_CASE("CAM state is saved with the document and read back; a document without it is written as before",
          "[cam][engine][document]")
{
    const auto state = camDocument();
    const doc::StateObjects objects = doc::serializeState(*state);
    // The manifest names the CAM object and one object per embedded item, after the (no) feature objects.
    CHECK_THAT(objects.manifest, ContainsSubstring("\"cam\":\"") && ContainsSubstring("\"embedded\":{"));
    CHECK(objects.features.size() == 1 + 2 + 1 + 1); // CAM object, two tools, a holder, a machine
    auto read = readBack(objects.manifest, objectsOf(objects));
    REQUIRE(read);
    CHECK(read->stateHash() == state->stateHash());
    REQUIRE(read->cam != nullptr);
    CHECK(read->cam->setups.size() == 2);
    CHECK(read->cam->operations.size() == 2);
    CHECK(read->cam->tools.at(ToolId{Uuid::fromString(id('d', 2))}).item.defaultToolNumber == 2);
    const auto& machine = read->cam->machines.at(MachineId{Uuid::fromString(id('9', 1))}).item;
    CHECK(machine.postPropertyOverrides.at("useG187") == "true");
    CHECK(machine.postPropertyOverrides.at("mode") == "fast");
    CHECK(std::holds_alternative<cam::FromSetupStock>(read->cam->setups[1].stock));
    CHECK(doc::serializeState(*read).manifest == objects.manifest);

    // A document without CAM state: no "cam" key, and the same bytes as an empty CAM state.
    auto plain = std::make_shared<doc::DocState>(*state);
    plain->cam = nullptr;
    const auto plainObjects = doc::serializeState(*plain);
    CHECK(plainObjects.manifest.find("\"cam\"") == std::string::npos);
    CHECK(plainObjects.manifest.find("\"embedded\"") == std::string::npos);
    CHECK(plainObjects.features.empty());
    auto empty = std::make_shared<doc::DocState>(*plain);
    empty->cam = std::make_shared<const cam::CamState>();
    CHECK(empty->stateHash() == plain->stateHash());
    auto plainRead = readBack(plainObjects.manifest, {});
    REQUIRE(plainRead);
    CHECK(plainRead->cam == nullptr);

    // The golden document of the doc tests (written before CAM existed) reads with no CAM state and keeps its hash.
    const std::string golden = RAY_DOC_GOLDEN_DIR;
    const auto slurp = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    };
    std::string manifest = slurp(golden + "/manifest.json");
    while (!manifest.empty() && (manifest.back() == '\n' || manifest.back() == '\r')) {
        manifest.pop_back();
    }
    Objects goldenObjects;
    for (int i = 0; i < 3; ++i) {
        std::string bytes = slurp(golden + "/feature-" + std::to_string(i) + ".json");
        while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
            bytes.pop_back();
        }
        goldenObjects[hashText(bytes)] = bytes;
    }
    auto old = readBack(manifest, goldenObjects);
    REQUIRE(old);
    CHECK(old->cam == nullptr);
    CHECK(doc::serializeState(*old).manifest == manifest);
}

TEST_CASE("a CAM object of a newer format is FormatTooNew; unknown keys are kept", "[cam][engine][document]")
{
    const auto state = camDocument();
    const doc::StateObjects objects = doc::serializeState(*state);
    const std::string camHex = camHashOf(objects.manifest);
    Objects all = objectsOf(objects);
    const std::string camBytes = all.at([&] {
        Hash256 h;
        for (const auto& [hash, bytes] : all) {
            if (hash.hex() == camHex) {
                h = hash;
            }
        }
        return h;
    }());

    SECTION("newer schema")
    {
        std::string manifest = objects.manifest;
        std::string newer = camBytes;
        const auto at = newer.rfind("\"schema\":1");
        REQUIRE(at != std::string::npos);
        newer.replace(at, 10, "\"schema\":2");
        replaceObject(manifest, all, camHex, newer);
        auto read = readBack(manifest, all);
        REQUIRE_FALSE(read);
        CHECK(read.error().code == ErrorCode::FormatTooNew);
    }
    SECTION("unknown keys in the CAM object, a setup, an operation's cutting data and a tool")
    {
        std::string manifest = objects.manifest;
        // Members whose names sort first, so the canonical form is the text with them put at the front.
        std::string edited = camBytes;
        REQUIRE(edited.front() == '{');
        edited.insert(1, R"("aFuture":{"x":1},)");
        const auto setup = edited.find(R"({"fixtureBodies")");
        REQUIRE(setup != std::string::npos);
        edited.insert(setup + 1, R"("aSetupKey":true,)");
        const auto cutting = edited.find(R"("cutting":{)");
        REQUIRE(cutting != std::string::npos);
        edited.insert(cutting + 11, R"("aCuttingKey":[1,2],)");
        REQUIRE(doc::canonicalJson(edited) == edited);
        replaceObject(manifest, all, camHex, edited);
        // And an embedded tool with a key of a newer library format, at its front and inside a nested object.
        std::string toolHex;
        std::string toolBytes;
        for (const auto& [hash, bytes] : objects.features) {
            if (bytes.find("\"flutes\"") != std::string::npos && toolHex.empty()) {
                toolHex = hash.hex();
                toolBytes = bytes;
            }
        }
        REQUIRE_FALSE(toolHex.empty());
        toolBytes.insert(1, R"("aCoating":"TiAlN",)");
        REQUIRE(doc::canonicalJson(toolBytes) == toolBytes);
        replaceObject(manifest, all, toolHex, toolBytes);
        auto read = readBack(manifest, all);
        REQUIRE(read);
        const auto again = doc::serializeState(*read);
        CHECK(camHashOf(again.manifest) == hashText(edited).hex());
        CHECK(again.manifest == manifest);
        CHECK(read->cam->extraJson == R"({"aFuture":{"x":1}})");
        CHECK(read->cam->tools.begin()->second.extraJson == R"({"aCoating":"TiAlN"})");
        CHECK(read->cam->setups[0].extraJson == R"({"aSetupKey":true})");
        // Stored again, every edited object comes back byte for byte.
        const Objects againObjects = objectsOf(again);
        CHECK(againObjects.count(hashText(edited)) == 1);
        CHECK(againObjects.count(hashText(toolBytes)) == 1);
    }
    SECTION("a damaged CAM object")
    {
        std::string manifest = objects.manifest;
        std::string damaged = camBytes;
        damaged.replace(damaged.find("\"operations\":["), 14, "\"operations\":[{\"id\":7},");
        replaceObject(manifest, all, camHex, damaged);
        auto read = readBack(manifest, all);
        REQUIRE_FALSE(read);
        CHECK(read.error().code == ErrorCode::FormatError);
    }
}

TEST_CASE("the stored CAM objects match schemas/doc.cam.schema.json", "[cam][engine][document][schema]")
{
    std::ifstream in(std::string(RAY_SCHEMA_DIR) + "/doc.cam.schema.json", std::ios::binary);
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    auto schemaFile = api::json::parse(text.str());
    REQUIRE(schemaFile);
    const auto state = camDocument();
    const doc::StateObjects objects = doc::serializeState(*state);
    const std::string camHex = camHashOf(objects.manifest);
    int checked = 0;
    for (const auto& [hash, bytes] : objects.features) {
        auto value = api::json::parse(bytes);
        REQUIRE(value);
        // Which kind of object: the CAM object by its hash, an item by its keys.
        const char* kind = hash.hex() == camHex                              ? "cam"
                           : bytes.find("\"flutes\"") != std::string::npos   ? "tool"
                           : bytes.find("\"segments\"") != std::string::npos ? "holder"
                           : bytes.find("\"spindle\"") != std::string::npos  ? "machine"
                                                                             : "material";
        // The CAM object is the schema itself; the library items are its $defs.
        const api::json::Value* defs = schemaFile->find("$defs");
        REQUIRE(defs != nullptr);
        const api::json::Value* schema = std::string(kind) == "cam" ? &*schemaFile : defs->find(kind);
        REQUIRE(schema != nullptr);
        INFO(kind << ": " << bytes);
        auto valid = api::schema::validate(*schema, *value);
        if (!valid) {
            FAIL_CHECK(valid.error().message);
        }
        ++checked;
    }
    CHECK(checked == 5);
}

TEST_CASE("a model edit marks exactly the operations it affects Stale", "[cam][engine][document]")
{
    // Two blocks from sketches and extrudes, rebuilt by the real rebuilder: body b (40 x 20 x 10) and body e.
    dt::RebuildEnv env;
    auto state = std::make_shared<const doc::DocState>();
    const std::string xy = R"({"principal":"xy"})";
    state = exec(state, dt::addFeature("sketch", 2, dt::sketchProps(xy, dt::SketchJson().rect(1, 0, 0, 40, 20))));
    state = exec(state, dt::addFeature("extrude", 3, dt::extrudeProps(2, "10", "new", 'b')));
    state = exec(state, dt::addFeature("sketch", 4, dt::sketchProps(xy, dt::SketchJson().rect(11, 100, 0, 130, 30))));
    state = exec(state, dt::addFeature("extrude", 5, dt::extrudeProps(4, "8", "new", 'e')));
    auto build = env.rebuild(state);
    for (const auto& r : build->reports) {
        INFO(r.message);
        REQUIRE(r.status == doc::FeatureStatus::Ok);
    }
    const BodyId b = dt::bodyId('b');
    const BodyId e = dt::bodyId('e');
    // The side face at y = 0 of each block (named from its sketch line, so it survives a height change).
    const auto snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    const auto sideOf = [&](const BodyId& body) {
        const kernel::Shape& shape = build->bodies.at(body).shape;
        const auto face = findFace(shape, [](const kernel::SurfaceInfo& s) {
            return s.type == kernel::SurfaceType::Plane && std::abs(s.centroid.y) < 1e-6;
        });
        return naming::toJson(resolver->makeRef(body, face));
    };
    const std::string sideB = sideOf(b);
    const std::string sideE = sideOf(e);
    state = exec(state, command("cam.library.embed", library(toolJson(1), {}, machineJson(1))));
    state = exec(state, setupAdd(1, "\"" + b.value.toString() + "\""));
    state = exec(state, setupAdd(2, "\"" + e.value.toString() + "\""));
    state = exec(state, opAdd(1, 1, 1, "[" + sideB + "]"));
    state = exec(state, opAdd(2, 2, 1, "[" + sideE + "]"));
    state = exec(state, opAdd(3, 1, 1)); // the whole part, no picked geometry
    const auto camState = state;

    Gate::instance().reset();
    auto engine = cam::makeCamEngine(*env.jobs, testOperations(), &doc::camSnapshot);
    REQUIRE(engine);
    const auto statuses = [&](const doc::DocStatePtr& s, const doc::BuildResultPtr& built) {
        std::map<std::uint64_t, OpStatus> out;
        for (const auto& [opIdValue, st] : engine->status(s, built)) {
            for (std::uint64_t n = 1; n <= 3; ++n) {
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
    const std::map<std::uint64_t, OpStatus> allUpToDate{
        {1, OpStatus::UpToDate}, {2, OpStatus::UpToDate}, {3, OpStatus::UpToDate}};
    settle(camState, build);
    REQUIRE(statuses(camState, build) == allUpToDate);

    // Taller block b: its operations go stale (the picked side face is still the same face, now taller), e's not.
    const auto tallerB = exec(camState, dt::editFeature("extrude", 3, R"({"distance":12})"));
    const auto builtTallerB = env.rebuild(tallerB);
    CHECK(statuses(tallerB, builtTallerB) ==
          std::map<std::uint64_t, OpStatus>{{1, OpStatus::Stale}, {2, OpStatus::UpToDate}, {3, OpStatus::Stale}});
    // The same for block e.
    const auto tallerE = exec(camState, dt::editFeature("extrude", 5, R"({"distance":9})"));
    const auto builtTallerE = env.rebuild(tallerE);
    CHECK(statuses(tallerE, builtTallerE) ==
          std::map<std::uint64_t, OpStatus>{{1, OpStatus::UpToDate}, {2, OpStatus::Stale}, {3, OpStatus::UpToDate}});
    // Regenerated, then the edit undone (the earlier snapshot and its rebuild): up to date again from the cache.
    settle(tallerB, builtTallerB);
    CHECK(statuses(tallerB, builtTallerB) == allUpToDate);
    const int runs = Gate::instance().runs;
    CHECK(statuses(camState, env.rebuild(camState)) == allUpToDate);
    CHECK(Gate::instance().runs == runs);
    // A reference counts by what it resolves to, not by the body it is on: with its WCS Z face on block e (which
    // setup 1 does not machine), setup 1's operations follow e's edit too, and only through that reference.
    const auto wcsOnE =
        exec(camState, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","wcs":{"zFace":)" + sideE + "}}"));
    settle(wcsOnE, build);
    REQUIRE(statuses(wcsOnE, build) == allUpToDate);
    const auto wcsOnETaller = exec(wcsOnE, dt::editFeature("extrude", 5, R"({"distance":9})"));
    CHECK(statuses(wcsOnETaller, env.rebuild(wcsOnETaller)) ==
          std::map<std::uint64_t, OpStatus>{{1, OpStatus::Stale}, {2, OpStatus::Stale}, {3, OpStatus::Stale}});
    // A model edit that breaks a picked face: removing the extrude of e (its sketch stays) leaves op 2 with nothing.
    const auto removedE = exec(camState, command("feature.remove", R"({"id":")" + dt::uuid('f', 5) + R"("})"));
    CHECK(statuses(removedE, env.rebuild(removedE)).at(2) == OpStatus::ReferenceBroken);
    engine.reset();
}
