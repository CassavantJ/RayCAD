// The CAM commands as actions (task T-032): registered from the document command registry with their argument
// schemas, run through IActionRegistry like every document command, undone and redone with edit.undo / edit.redo,
// and saved and reopened with the document.
#include "engine_test_util.h"

#include <ray/api.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam::test;
using ray::api::Caller;

namespace {

constexpr std::chrono::milliseconds kWait = std::chrono::minutes(5);

class TempDir {
public:
    TempDir()
    {
        path_ = std::filesystem::temp_directory_path() / "raycad-cam-tests" / Uuid::generate().toString();
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string file(const std::string& name) const
    {
        const std::u8string s = (path_ / name).u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }

private:
    std::filesystem::path path_;
};

std::unique_ptr<api::HeadlessApp> makeApp(const std::string& userDataDir)
{
    api::AppOptions options;
    options.workerThreads = 4;
    options.userDataDir = userDataDir;
    auto app = api::makeHeadlessApp(std::move(options));
    REQUIRE(app != nullptr);
    return app;
}

std::string invoke(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, Caller::User), kWait);
    if (!done) {
        FAIL(action << " failed: " << done.error().message);
    }
    return done->resultJson;
}

std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

} // namespace

TEST_CASE("the CAM commands are actions with argument schemas", "[cam][engine][api]")
{
    auto app = makeApp("");
    std::map<std::string, api::ActionInfo> byId;
    for (const auto& info : app->actions().all()) {
        byId[info.id] = info;
    }
    for (const char* name : {"cam.setup.add", "cam.setup.edit", "cam.setup.remove", "cam.setup.reorder",
                             "cam.operation.add", "cam.operation.edit", "cam.operation.remove", "cam.operation.reorder",
                             "cam.operation.suppress", "cam.library.embed"}) {
        INFO(name);
        REQUIRE(byId.count(name) == 1);
        const api::ActionInfo& info = byId[name];
        CHECK(info.category == "CAM");
        CHECK(info.changesDocument);
        CHECK(info.changesToolpathsOrGcode);
        CHECK_THAT(info.argsSchemaJson, ContainsSubstring("\"document\"") && ContainsSubstring("\"id\""));
        CHECK(info.title.find("Cam") == std::string::npos); // a real title, not the generated one
    }
    CHECK_THAT(byId["cam.operation.add"].argsSchemaJson, ContainsSubstring("\"cutting\"") &&
                                                             ContainsSubstring("\"heights\"") &&
                                                             ContainsSubstring("\"library\""));
    CHECK_THAT(byId["cam.setup.add"].argsSchemaJson, ContainsSubstring("\"stock\"") && ContainsSubstring("\"wcs\""));

    // Automation must ask: a script's CAM change without a confirmation hook that says yes changes nothing.
    app->setConfirmHook([](const api::ActionInfo&, std::string_view, std::string_view) { return false; });
    invoke(*app, "file.new", "{}");
    auto refused = app->wait(app->actions().invoke("cam.library.embed", library(toolJson(1)), Caller::Script), kWait);
    REQUIRE_FALSE(refused);
    CHECK(app->active()->document->state()->cam == nullptr);
}

TEST_CASE("a setup and an operation made through actions undo, redo, save and reopen with the document",
          "[cam][engine][api]")
{
    TempDir dir;
    const std::string saved = dir.file("part.raycad");
    const std::string body = id('b', 1);
    Hash256 stateHash;
    {
        auto app = makeApp(dir.file("user"));
        invoke(*app, "file.new", "{}");
        // A 40 x 20 x 10 block.
        const std::string sketch =
            R"({"entities":[{"id":"e0000000-0000-4000-8000-000000000001","type":"point","x":0,"y":0},)"
            R"({"id":"e0000000-0000-4000-8000-000000000002","type":"point","x":40,"y":0},)"
            R"({"id":"e0000000-0000-4000-8000-000000000003","type":"point","x":40,"y":20},)"
            R"({"id":"e0000000-0000-4000-8000-000000000004","type":"point","x":0,"y":20},)"
            R"({"id":"e0000000-0000-4000-8000-000000000005","type":"line","a":"e0000000-0000-4000-8000-000000000001","b":"e0000000-0000-4000-8000-000000000002"},)"
            R"({"id":"e0000000-0000-4000-8000-000000000006","type":"line","a":"e0000000-0000-4000-8000-000000000002","b":"e0000000-0000-4000-8000-000000000003"},)"
            R"({"id":"e0000000-0000-4000-8000-000000000007","type":"line","a":"e0000000-0000-4000-8000-000000000003","b":"e0000000-0000-4000-8000-000000000004"},)"
            R"({"id":"e0000000-0000-4000-8000-000000000008","type":"line","a":"e0000000-0000-4000-8000-000000000004","b":"e0000000-0000-4000-8000-000000000001"}],)"
            R"("constraints":[]})";
        invoke(*app, "feature.sketch.add",
               R"({"id":")" + id('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" + sketch + "}}");
        invoke(*app, "feature.extrude.add",
               R"({"id":")" + id('f', 2) + R"(","props":{"sketch":")" + id('f', 1) +
                   R"(","distance":"10 mm","operation":"new","target":")" + body + R"("}})");
        api::OpenDocument* od = app->active();
        REQUIRE(od != nullptr);
        REQUIRE(app->waitForBuild(*od, kWait));
        // The top face (index from the build), as a reference by model.reference.
        const auto build = od->document->build();
        const kernel::Shape& shape = build->bodies.begin()->second.shape;
        const auto top = findFace(shape, [](const kernel::SurfaceInfo& s) { return s.centroid.z > 9.99; });
        const std::string reference =
            invoke(*app, "model.reference",
                   R"({"body":")" + body + R"(","kind":"face","index":)" + std::to_string(top.index) + "}");
        const auto refAt = reference.find("\"ref\":");
        REQUIRE(refAt != std::string::npos);
        const std::string ref = reference.substr(refAt + 6, reference.size() - refAt - 7); // the object, to the last }
        REQUIRE(ref.front() == '{');

        const auto hashNow = [&] {
            return od->document->state()->stateHash();
        };
        const Hash256 h0 = hashNow();
        invoke(*app, "cam.setup.add",
               R"({"id":")" + id('a', 1) + R"(","name":"Op 1","partBodies":[")" + body + R"("],"machine":")" +
                   id('9', 1) + R"(","library":)" + library({}, {}, machineJson(1)) + "}");
        const Hash256 h1 = hashNow();
        invoke(*app, "cam.operation.add",
               R"({"id":")" + id('c', 1) + R"(","setup":")" + id('a', 1) +
                   R"(","type":"drill","name":"Spot","tool":{"tool":")" + id('d', 1) + R"("},"cutting":)" + kCutting +
                   R"(,"heights":)" + kHeights + R"(,"geometry":[)" + ref + R"(],"library":)" + library(toolJson(1)) +
                   "}");
        const Hash256 h2 = hashNow();
        CHECK(h0 != h1);
        CHECK(h1 != h2);
        const auto& cam = *od->document->state()->cam;
        REQUIRE(cam.operations.size() == 1);
        CHECK(cam.operations.begin()->second.geometry.size() == 1);
        CHECK(cam.tools.size() == 1);
        CHECK(od->document->undoCommand()->label == "New Operation Spot");

        // A bad argument is refused by the command, with the field named.
        auto bad =
            app->wait(app->actions().invoke("cam.operation.edit",
                                            R"({"id":")" + id('c', 1) + R"(","cutting":{"rpm":-5}})", Caller::User),
                      kWait);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == ErrorCode::InvalidArgument);

        // Undo and redo, with the model command before them on the same stack.
        invoke(*app, "edit.undo", "{}");
        CHECK(hashNow() == h1);
        invoke(*app, "edit.undo", "{}");
        CHECK(hashNow() == h0);
        CHECK(od->document->state()->cam == nullptr);
        invoke(*app, "edit.undo", "{}"); // the extrude
        CHECK(od->document->state()->features.size() == 1);
        for (int i = 0; i < 3; ++i) {
            invoke(*app, "edit.redo", "{}");
        }
        CHECK(hashNow() == h2);

        invoke(*app, "file.saveAs", R"({"path":)" + jsonString(saved) + "}");
        stateHash = hashNow();
        REQUIRE(app->close(*od));
    }
    {
        auto app = makeApp(dir.file("user"));
        invoke(*app, "file.open", R"({"path":)" + jsonString(saved) + "}");
        api::OpenDocument* od = app->active();
        REQUIRE(od != nullptr);
        CHECK(od->document->state()->stateHash() == stateHash);
        REQUIRE(od->document->state()->cam != nullptr);
        const auto& cam = *od->document->state()->cam;
        REQUIRE(cam.setups.size() == 1);
        CHECK(cam.setups[0].name == "Op 1");
        CHECK(cam.operations.at(opId(1)).name == "Spot");
        CHECK(cam.machines.size() == 1);
    }
}
