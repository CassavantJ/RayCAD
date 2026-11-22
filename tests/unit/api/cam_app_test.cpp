// The CAM modules joined in the app: the operation registry, the CAM engine, the library stack and the post engine
// of an app made by makeHeadlessApp, driven through actions as the UI and scripts drive them.
#include "api_test_util.h"

#include <ray/api.h>
#include <ray_api/settings.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using ray::api::Caller;
using ray::test::jsonString;
using ray::test::kWait;
using ray::test::TempDir;

namespace {

// What the test generator saw (it runs on workers).
struct Seen {
    std::atomic<int> runs{0};
    std::atomic<int> cancelled{0};
    std::atomic<bool> blocking{false};
    std::atomic<bool> toolNamed{false}; // a generator was handed a tool with a name (it must not be)
    static Seen& get()
    {
        static Seen seen;
        return seen;
    }
    void reset()
    {
        runs = 0;
        cancelled = 0;
        blocking = false;
        toolNamed = false;
    }
};

// "test.gen": one rapid and one feed move; {"mode":"block"} runs until it is cancelled.
class TestGenerator final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "test.gen"; }
    std::string_view paramsSchemaJson() const override { return R"({"type":"object"})"; }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput& in, JobContext& ctx) const override
    {
        Seen& seen = Seen::get();
        ++seen.runs;
        if (!in.tool.name.empty()) {
            seen.toolNamed = true;
        }
        if (in.op.paramsJson.find("\"block\"") != std::string::npos) {
            seen.blocking = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto st = ctx.cancel.check(); !st) {
                    ++seen.cancelled;
                    return tl::unexpected(st.error());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return tl::unexpected(Error{ErrorCode::Internal, "never cancelled", {}});
        }
        cam::Toolpath tp{};
        cam::Move rapid{};
        rapid.kind = cam::MoveKind::Rapid;
        rapid.role = cam::MoveRole::Approach;
        rapid.z = toNm(in.op.heights.clearance);
        cam::Move feed{};
        feed.kind = cam::MoveKind::Linear;
        feed.role = cam::MoveRole::Cut;
        feed.z = toNm(in.op.heights.bottom);
        feed.feedMicronPerMin = static_cast<std::int32_t>(in.op.cutting.feedCut * 1000.0);
        tp.moves = {rapid, feed};
        tp.stats.cutLengthMm = in.tool.diameter;
        return tp;
    }
};

std::string uuid(char kind, int n)
{
    std::string tail = std::to_string(n);
    return std::string(1, kind) + "0000000-0000-4000-8000-" + std::string(12 - tail.size(), '0') + tail;
}

lib::Tool libraryTool()
{
    lib::Tool t;
    t.id = ToolId{Uuid::fromString(uuid('d', 1))};
    t.name = "6 mm 3FL carbide";
    t.flutes = 3;
    t.diameter = 6;
    t.shankDiameter = 6;
    t.fluteLength = 18;
    t.shoulderLength = 22;
    t.overallLength = 57;
    t.bodyMaterial = "carbide";
    t.defaultToolNumber = 4;
    t.comment = "Generic end mill; nominal catalogue sizes";
    t.extraJson = R"({"coating":"AlTiN"})"; // a key of a newer RayCAD, kept all the way into the document
    return t;
}

lib::MachineDef libraryMachine()
{
    lib::MachineDef m;
    m.id = MachineId{Uuid::fromString(uuid('9', 1))};
    m.name = "Test mill";
    m.axes = {{'X', 0, 500, 20000, 10000, 0}, {'Y', 0, 400, 20000, 10000, 0}, {'Z', -400, 0, 20000, 10000, 0}};
    m.maxRpm = 12000;
    m.toolCapacity = 20;
    m.controlDialect = "generic-iso";
    m.defaultPost = "generic-iso";
    return m;
}

void writeLibrary(const std::string& dir, const std::string& name)
{
    std::filesystem::create_directories(ray::test::fsPath(dir));
    ray::test::writeBytes(dir + "/library.json", R"({"schema":1,"name":")" + name + R"("})");
    auto library = lib::ILibrary::open(dir);
    REQUIRE(library);
    REQUIRE((*library)->save(libraryTool()));
    REQUIRE((*library)->save(libraryMachine()));
}

std::unique_ptr<api::HeadlessApp> makeCamApp(const std::string& userDataDir, const std::string& builtin)
{
    api::AppOptions options;
    options.workerThreads = 4;
    options.userDataDir = userDataDir;
    options.builtinLibraryDir = builtin;
    auto app = api::makeHeadlessApp(std::move(options));
    REQUIRE(app != nullptr);
    REQUIRE(app->operationTypes() != nullptr);
    app->operationTypes()->add(std::make_unique<TestGenerator>());
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

// A 40 x 20 x 10 block as body b1 of the active document, built.
BodyId makeBlock(api::HeadlessApp& app)
{
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
    invoke(app, "feature.sketch.add",
           R"({"id":")" + uuid('f', 1) + R"(","props":{"plane":{"principal":"xy"},"sketch":)" + sketch + "}}");
    invoke(app, "feature.extrude.add",
           R"({"id":")" + uuid('f', 2) + R"(","props":{"sketch":")" + uuid('f', 1) +
               R"(","distance":"10 mm","operation":"new","target":")" + uuid('b', 1) + R"("}})");
    REQUIRE(app.waitForBuild(*app.active(), kWait));
    return BodyId{Uuid::fromString(uuid('b', 1))};
}

const std::string kCutting = R"({"rpm":8000,"feedCut":1200,"feedPlunge":300})";
const std::string kHeights = R"({"clearance":25,"retract":5,"feedPlane":2,"top":0,"bottom":-5})";

// A setup on b1 with the machine, and operation c<n> with the tool, both taken from the app's library stack.
void addSetupAndOperation(api::HeadlessApp& app, int op, const std::string& params)
{
    lib::LibraryStack& stack = *app.libraries();
    const lib::Tool* tool = stack.tool(ToolId{Uuid::fromString(uuid('d', 1))});
    const lib::MachineDef* machine = stack.machine(MachineId{Uuid::fromString(uuid('9', 1))});
    REQUIRE(tool != nullptr);
    REQUIRE(machine != nullptr);
    if (!app.active()->document->state()->cam) {
        invoke(app, "cam.setup.add",
               R"({"id":")" + uuid('a', 1) + R"(","name":"Op 1","partBodies":[")" + uuid('b', 1) + R"("],"machine":")" +
                   uuid('9', 1) + R"(","library":{"machines":[)" + lib::toJson(*machine) + "]}}");
    }
    invoke(app, "cam.operation.add",
           R"({"id":")" + uuid('c', op) + R"(","setup":")" + uuid('a', 1) + R"(","type":"test.gen","name":"Op )" +
               std::to_string(op) + R"(","tool":{"tool":")" + uuid('d', 1) + R"("},"cutting":)" + kCutting +
               R"(,"heights":)" + kHeights + R"(,"params":)" + params + R"(,"library":{"tools":[)" +
               lib::toJson(*tool) + "]}}");
}

cam::OpState statusOf(api::HeadlessApp& app, int op)
{
    api::OpenDocument* od = app.active();
    const auto all = app.cam()->status(od->document->state(), od->document->build());
    const auto it = all.find(OperationId{Uuid::fromString(uuid('c', op))});
    REQUIRE(it != all.end());
    return it->second;
}

// Generates operation c<n> of the active document and waits until the engine has filed the result.
std::shared_ptr<const cam::Toolpath> generate(api::HeadlessApp& app, int op)
{
    api::OpenDocument* od = app.active();
    auto job =
        app.cam()->generate(od->document->state(), od->document->build(), OperationId{Uuid::fromString(uuid('c', op))});
    auto result = app.wait(job, kWait);
    if (!result) {
        FAIL("generation failed: " << result.error().message);
    }
    REQUIRE(app.runUntil([&] { return statusOf(app, op).status == cam::OpStatus::UpToDate; }, kWait));
    return *result;
}

} // namespace

TEST_CASE("an app has its operation registry, CAM engine, library stack and post engine", "[api][cam]")
{
    TempDir dir;
    auto app = makeCamApp("", "");
    CHECK(app->cam() != nullptr);
    CHECK(app->libraries() != nullptr);
    CHECK(app->posts() != nullptr);
    CHECK(app->operationTypes()->find("test.gen") != nullptr);
    // Nothing persisted and no built-in library: an empty stack.
    CHECK(app->libraries()->roots().empty());

    // Explicit roots are the whole stack, whatever the settings; all writable.
    writeLibrary(dir.file("one"), "One");
    api::AppOptions options;
    options.userDataDir = dir.file("user");
    options.libraryRoots = {dir.file("one")};
    auto explicitApp = api::makeHeadlessApp(std::move(options));
    REQUIRE(explicitApp);
    const auto roots = explicitApp->libraries()->roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].name == "One");
    CHECK(roots[0].writable);
    CHECK(explicitApp->libraries()->tool(ToolId{Uuid::fromString(uuid('d', 1))}) != nullptr);
}

TEST_CASE("the default library stack: built-in, shared folders from the settings, then the user library",
          "[api][cam][library]")
{
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    const std::string shop = dir.file("shop");
    const std::string user = dir.file("user");
    writeLibrary(builtin, "Built-in");
    std::filesystem::create_directories(ray::test::fsPath(shop));
    auto app = makeCamApp(user, builtin);
    lib::LibraryStack& stack = *app->libraries();
    auto roots = stack.roots();
    REQUIRE(roots.size() == 2);
    CHECK(roots[0].dir == builtin);
    CHECK_FALSE(roots[0].writable); // the built-in library is read-only
    CHECK(roots[1].dir == ray::test::utf8(ray::test::fsPath(user) / "library"));
    CHECK(roots[1].open); // made by the app
    CHECK(roots[1].writable);
    CHECK(stack.rootOf(lib::ItemKind::Tool, Uuid::fromString(uuid('d', 1))) == std::optional<std::size_t>(0));

    // A shared folder in the settings joins the stack between the built-in and the user library.
    invoke(*app, "settings.set", R"({"key":"library.sharedFolders","value":[)" + jsonString(shop) + "]}");
    REQUIRE(app->runUntil([&] { return stack.roots().size() == 3; }, kWait));
    roots = stack.roots();
    CHECK(roots[1].dir == shop);
    CHECK(roots[1].writable);
    // Saving into the user library overrides the built-in copy.
    lib::Tool mine = libraryTool();
    mine.name = "my 6 mm";
    REQUIRE(stack.library(2) != nullptr);
    REQUIRE(stack.library(2)->save(mine));
    stack.remerge();
    CHECK(stack.tool(mine.id)->name == "my 6 mm");
    CHECK(stack.rootOf(lib::ItemKind::Tool, mine.id.value) == std::optional<std::size_t>(2));

    // Another user library folder from the settings.
    const std::string elsewhere = dir.file("elsewhere");
    std::filesystem::create_directories(ray::test::fsPath(elsewhere));
    invoke(*app, "settings.set", R"({"key":"library.userFolder","value":)" + jsonString(elsewhere) + "}");
    REQUIRE(app->runUntil([&] { return stack.roots().back().dir == elsewhere; }, kWait));
    CHECK(stack.tool(mine.id)->name == "6 mm 3FL carbide"); // the built-in one again
}

TEST_CASE("CAM through the app: embed from the library, generate, status, save and reopen", "[api][cam]")
{
    Seen::get().reset();
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    writeLibrary(builtin, "Built-in");
    const std::string saved = dir.file("part.raycad");
    Hash256 inputHash;
    {
        auto app = makeCamApp(dir.file("user"), builtin);
        invoke(*app, "file.new", "{}");
        makeBlock(*app);
        addSetupAndOperation(*app, 1, R"({"mode":"plain"})");
        api::OpenDocument* od = app->active();

        // The embedded copies are the library's items, byte for byte (comment and unknown keys included).
        const auto& cam = *od->document->state()->cam;
        const auto& tool = cam.tools.at(ToolId{Uuid::fromString(uuid('d', 1))});
        CHECK(cam::embeddedJson(tool) == lib::toJson(*app->libraries()->tool(tool.item.id)));
        CHECK(tool.item.comment == "Generic end mill; nominal catalogue sizes");
        CHECK(tool.item.extraJson == R"({"coating":"AlTiN"})");

        CHECK(statusOf(*app, 1).status == cam::OpStatus::Stale);
        const auto toolpath = generate(*app, 1);
        REQUIRE(toolpath != nullptr);
        CHECK(toolpath->moves.size() == 2);
        CHECK(toolpath->stats.cutLengthMm == 6);
        CHECK_FALSE(Seen::get().toolNamed.load()); // the generator saw the tool's geometry only
        CHECK(statusOf(*app, 1).toolpath == toolpath);

        // A rename of the operation or of its tool leaves it up to date; a change of the tool's shape does not.
        invoke(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 1) + R"(","name":"Renamed"})");
        CHECK(statusOf(*app, 1).status == cam::OpStatus::UpToDate);
        lib::Tool renamed = libraryTool();
        renamed.name = "renamed in the library";
        renamed.comment = "edited";
        invoke(*app, "cam.library.embed", R"({"tools":[)" + lib::toJson(renamed) + "]}");
        CHECK(statusOf(*app, 1).status == cam::OpStatus::UpToDate);
        lib::Tool longer = renamed;
        longer.fluteLength = 20;
        invoke(*app, "cam.library.embed", R"({"tools":[)" + lib::toJson(longer) + "]}");
        CHECK(statusOf(*app, 1).status == cam::OpStatus::Stale);
        // generateAllStale makes app jobs too.
        app->cam()->generateAllStale(od->document->state(), od->document->build());
        REQUIRE(app->runUntil([&] { return statusOf(*app, 1).status == cam::OpStatus::UpToDate; }, kWait));
        inputHash = statusOf(*app, 1).toolpath->inputHash;

        invoke(*app, "file.saveAs", R"({"path":)" + jsonString(saved) + "}");
        REQUIRE(app->close(*od));
    }
    {
        auto app = makeCamApp(dir.file("user"), builtin);
        invoke(*app, "file.open", R"({"path":)" + jsonString(saved) + "}");
        REQUIRE(app->waitForBuild(*app->active(), kWait));
        const auto& tool = app->active()->document->state()->cam->tools.at(ToolId{Uuid::fromString(uuid('d', 1))});
        CHECK(tool.item.extraJson == R"({"coating":"AlTiN"})");
        CHECK(tool.item.fluteLength == 20);
        // A new app holds no toolpath: Stale until generated, then up to date with the same input hash.
        CHECK(statusOf(*app, 1).status == cam::OpStatus::Stale);
        CHECK(generate(*app, 1)->inputHash == inputHash);
    }
}

TEST_CASE("closing a document cancels its generations; the app's destructor cancels and drains the rest", "[api][cam]")
{
    Seen::get().reset();
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    writeLibrary(builtin, "Built-in");
    {
        auto app = makeCamApp("", builtin);
        invoke(*app, "file.new", "{}");
        makeBlock(*app);
        addSetupAndOperation(*app, 1, R"({"mode":"block"})");
        api::OpenDocument* od = app->active();
        auto job = app->cam()->generate(od->document->state(), od->document->build(),
                                        OperationId{Uuid::fromString(uuid('c', 1))});
        REQUIRE(app->runUntil([] { return Seen::get().blocking.load(); }, kWait));
        CHECK(statusOf(*app, 1).status == cam::OpStatus::Generating);
        REQUIRE(app->close(*od));
        auto result = app->wait(job, kWait);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::Cancelled);
        CHECK(Seen::get().cancelled.load() == 1);
    }
    Seen::get().reset();
    {
        auto app = makeCamApp("", builtin);
        invoke(*app, "file.new", "{}");
        makeBlock(*app);
        addSetupAndOperation(*app, 1, R"({"mode":"block"})");
        api::OpenDocument* od = app->active();
        (void)app->cam()->generate(od->document->state(), od->document->build(),
                                   OperationId{Uuid::fromString(uuid('c', 1))});
        REQUIRE(app->runUntil([] { return Seen::get().blocking.load(); }, kWait));
    } // the destructor cancels the generation and waits for it
    CHECK(Seen::get().cancelled.load() == 1);
}
