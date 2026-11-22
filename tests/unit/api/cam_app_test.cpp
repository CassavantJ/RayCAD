// The CAM modules joined in the app: the operation registry, the CAM engine, the library stack and the post engine
// of an app made by makeHeadlessApp, driven through actions as the UI and scripts drive them.
#include "api_test_util.h"

#include <ray/api.h>
#include <ray_api/cam_setup.h>
#include <ray_api/settings.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

namespace ray::lib::detail {
// Defined in src/lib/stack.cpp (declared in its private library_internal.h): blocks background library reads in tests.
void setBackgroundReadHook(std::function<void(const std::string& dir, const CancelToken& cancel)> hook);
} // namespace ray::lib::detail

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

// "test.schema" (test only): params with a range, a choice and defaults, no other keys. Generates nothing.
class TestSchemaType final : public cam::IOperationType {
public:
    std::string_view typeName() const override { return "test.schema"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","additionalProperties":false,"properties":{)"
               R"("stepover":{"type":"number","exclusiveMinimum":0,"maximum":1,"default":0.4},)"
               R"("direction":{"enum":["climb","conventional"],"default":"climb"},)"
               R"("passes":{"type":"integer","minimum":1}}})";
    }
    Status validate(const cam::GenerateInput&) const override { return {}; }
    Result<cam::Toolpath> generate(const cam::GenerateInput&, JobContext&) const override
    {
        return tl::unexpected(Error{ErrorCode::Internal, "not used", {}});
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
    app->operationTypes()->add(std::make_unique<TestSchemaType>());
    return app;
}

// The error of an action that must fail.
Error invokeFails(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = app.wait(app.actions().invoke(action, args, Caller::User), kWait);
    if (done) {
        FAIL(action << " should have failed");
    }
    return done.error();
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

TEST_CASE("the app's post list has the shipped posts of the built-in library", "[api][cam][library]")
{
    // T-040 / T-041: the built-in library is copied beside the programs (src/api/CMakeLists.txt), posts included; the
    // setup panel lists the posts of every library root with postStems.
    const auto stems = api::cam_setup::postStems({RAY_BUILTIN_LIBRARY_COPY});
    for (const char* post : {"fanuc", "generic-iso", "haas-classic", "haas-ngc"}) {
        INFO(post);
        CHECK(std::find(stems.begin(), stems.end(), post) != stems.end());
    }
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
    const auto allRead = [&stack] {
        const auto now = stack.roots();
        return std::none_of(now.begin(), now.end(), [](const lib::LibraryStack::RootInfo& r) { return r.loading; });
    };
    REQUIRE(app->runUntil(allRead, kWait)); // the user library is read on a worker (within the start-up wait, usually)
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
    REQUIRE(app->runUntil([&] { return stack.roots().size() == 3 && allRead(); }, kWait));
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
    REQUIRE(app->runUntil([&] { return stack.roots().back().dir == elsewhere && allRead(); }, kWait));
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

TEST_CASE("operation params are checked against the type's schema, with its defaults filled in", "[api][cam][params]")
{
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    writeLibrary(builtin, "Built-in");
    auto app = makeCamApp("", builtin);
    invoke(*app, "file.new", "{}");
    makeBlock(*app);
    addSetupAndOperation(*app, 1, R"({"mode":"plain"})"); // test.gen: any object
    const auto opArgs = [](int op, const std::string& type, const std::string& paramsMember) {
        return R"({"id":")" + uuid('c', op) + R"(","setup":")" + uuid('a', 1) + R"(","type":")" + type +
               R"(","name":"Op )" + std::to_string(op) + R"(","tool":{"tool":")" + uuid('d', 1) + R"("},"cutting":)" +
               kCutting + R"(,"heights":)" + kHeights + paramsMember + "}";
    };
    const auto paramsOf = [&](int op) {
        return app->active()
            ->document->state()
            ->cam->operations.at(OperationId{Uuid::fromString(uuid('c', op))})
            .paramsJson;
    };
    const std::string before = app->active()->document->state()->stateHash().hex();

    // Out of range, not one of the choices, the wrong type, an unknown key: refused, naming the value, and the
    // document is unchanged.
    Error e = invokeFails(*app, "cam.operation.add", opArgs(2, "test.schema", R"(,"params":{"stepover":2})"));
    CHECK(e.code == ErrorCode::InvalidArgument);
    CHECK_THAT(e.message, ContainsSubstring("test.schema"));
    CHECK_THAT(e.message, ContainsSubstring("/params/stepover: must be at most 1"));
    e = invokeFails(*app, "cam.operation.add", opArgs(2, "test.schema", R"(,"params":{"direction":"sideways"})"));
    CHECK_THAT(e.message, ContainsSubstring("/params/direction: must be one of"));
    e = invokeFails(*app, "cam.operation.add", opArgs(2, "test.schema", R"(,"params":{"passes":1.5})"));
    CHECK_THAT(e.message, ContainsSubstring("/params/passes: must be an integer"));
    e = invokeFails(*app, "cam.operation.add", opArgs(2, "test.schema", R"(,"params":{"mode":"plain"})"));
    CHECK_THAT(e.message, ContainsSubstring("/params/mode: is not a known argument"));
    CHECK(app->active()->document->state()->stateHash().hex() == before);

    // Params left out (all of them, or some) take the schema's defaults, stored in the document.
    invoke(*app, "cam.operation.add", opArgs(2, "test.schema", ""));
    CHECK(paramsOf(2) == R"({"direction":"climb","stepover":0.4})");
    invoke(*app, "cam.operation.add", opArgs(3, "test.schema", R"(,"params":{"passes":3,"stepover":0.25})"));
    CHECK(paramsOf(3) == R"({"direction":"climb","passes":3,"stepover":0.25})");

    // An edit with params is checked and completed the same way (params are replaced whole).
    e = invokeFails(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 2) + R"(","params":{"stepover":0}})");
    CHECK_THAT(e.message, ContainsSubstring("/params/stepover: must be greater than 0"));
    invoke(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 2) + R"(","params":{"direction":"conventional"}})");
    CHECK(paramsOf(2) == R"({"direction":"conventional","stepover":0.4})");
    // An edit of something else is not a params check; a change of type checks the params it keeps.
    invoke(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 2) + R"(","name":"Renamed"})");
    e = invokeFails(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 1) + R"(","type":"test.schema"})");
    CHECK_THAT(e.message, ContainsSubstring("/params/mode: is not a known argument"));
    invoke(*app, "cam.operation.edit", R"({"id":")" + uuid('c', 1) + R"(","type":"test.schema","params":{}})");
    CHECK(paramsOf(1) == R"({"direction":"climb","stepover":0.4})");

    // Not an object at all.
    e = invokeFails(*app, "cam.operation.add", opArgs(4, "test.schema", R"(,"params":[1])"));
    CHECK_THAT(e.message, ContainsSubstring("/params: must be an object"));
    // A type the app does not have is not checked (the engine reports it Failed).
    invoke(*app, "cam.operation.add", opArgs(4, "no.such.type", R"(,"params":{"anything":[1,2]})"));
    CHECK(paramsOf(4) == R"({"anything":[1,2]})");
    CHECK(statusOf(*app, 4).status == cam::OpStatus::Failed);
}
TEST_CASE("closing a document makes the engine forget its operations, unless another open copy has them",
          "[api][cam][forget]")
{
    Seen::get().reset();
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    writeLibrary(builtin, "Built-in");
    const std::string a = dir.file("a.raycad");
    const std::string b = dir.file("b.raycad");
    auto app = makeCamApp("", builtin);
    invoke(*app, "file.new", "{}");
    makeBlock(*app);
    addSetupAndOperation(*app, 1, R"({"mode":"plain"})");
    invoke(*app, "file.saveAs", R"({"path":)" + jsonString(a) + "}");
    REQUIRE(app->close(*app->active()));
    std::filesystem::copy_file(ray::test::fsPath(a), ray::test::fsPath(b)); // a second file, same operation ids
    const auto open = [&](const std::string& path) {
        invoke(*app, "file.open", R"({"path":)" + jsonString(path) + "}");
        REQUIRE(app->waitForBuild(*app->active(), kWait));
        return app->active();
    };
    const auto editRpm = [&](int rpm) {
        invoke(*app, "cam.operation.edit",
               R"({"id":")" + uuid('c', 1) + R"(","cutting":{"rpm":)" + std::to_string(rpm) + "}}");
    };

    api::OpenDocument* first = open(a);
    const auto toolpath = generate(*app, 1);
    REQUIRE(Seen::get().runs.load() == 1);
    api::OpenDocument* second = open(b);
    CHECK(statusOf(*app, 1).status == cam::OpStatus::UpToDate); // the same inputs: found by input hash
    editRpm(9000);
    CHECK(statusOf(*app, 1).status == cam::OpStatus::Stale);
    CHECK(statusOf(*app, 1).toolpath == toolpath); // the operation's latest toolpath, shown while stale

    // Closing the first copy leaves the second one's entries alone.
    REQUIRE(app->close(*first));
    REQUIRE(app->active() == second);
    CHECK(statusOf(*app, 1).toolpath == toolpath);

    // Closing the last copy forgets them: reopened, an operation that is stale shows no old toolpath, and one whose
    // inputs are those of a cached toolpath finds it again by input hash without generating.
    REQUIRE(app->close(*second));
    open(a);
    CHECK(statusOf(*app, 1).status == cam::OpStatus::UpToDate);
    CHECK(statusOf(*app, 1).toolpath == toolpath);
    CHECK(Seen::get().runs.load() == 1);
    REQUIRE(app->close(*app->active()));
    open(a);
    editRpm(7000);
    const cam::OpState stale = statusOf(*app, 1);
    CHECK(stale.status == cam::OpStatus::Stale);
    CHECK(stale.toolpath == nullptr);
    CHECK(generate(*app, 1)->inputHash != toolpath->inputHash);
}
TEST_CASE("start-up does not wait for a slow shared library folder; it joins the stack when read",
          "[api][cam][library][background]")
{
    TempDir dir;
    const std::string builtin = dir.file("builtin");
    const std::string shop = dir.file("shop");
    const std::string user = dir.file("user");
    writeLibrary(builtin, "Built-in");
    std::filesystem::create_directories(ray::test::fsPath(shop));
    lib::Tool shopTool = libraryTool();
    shopTool.id = ToolId{Uuid::fromString(uuid('d', 2))};
    shopTool.name = "shop tool";
    {
        auto library = lib::ILibrary::open(shop);
        REQUIRE(library);
        REQUIRE((*library)->save(shopTool));
    }
    { // The shared folder goes into the persisted settings.
        auto app = makeCamApp(user, builtin);
        invoke(*app, "settings.set", R"({"key":"library.sharedFolders","value":[)" + jsonString(shop) + "]}");
    }
    std::atomic<bool> release{false};
    std::atomic<int> blocked{0};
    std::atomic<int> cancelled{0};
    lib::detail::setBackgroundReadHook([&](const std::string& folder, const CancelToken& cancel) {
        if (folder != shop) {
            return;
        }
        ++blocked;
        while (!release.load()) {
            if (cancel.cancelled()) {
                ++cancelled;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    {
        const auto start = std::chrono::steady_clock::now();
        auto app = makeCamApp(user, builtin);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10)); // not the share's (endless) read
        lib::LibraryStack& stack = *app->libraries();
        auto roots = stack.roots();
        REQUIRE(roots.size() == 3);
        CHECK(roots[0].open); // the built-in library, read at once
        CHECK(stack.tool(ToolId{Uuid::fromString(uuid('d', 1))}) != nullptr);
        CHECK(roots[1].dir == shop);
        CHECK(roots[1].loading);
        CHECK(stack.tool(shopTool.id) == nullptr);
        REQUIRE(app->runUntil([&] { return blocked.load() == 1; }, kWait));
        int calls = 0;
        stack.subscribe([&calls] { ++calls; });
        // When the read ends, the share's items appear and the listeners are told.
        release = true;
        REQUIRE(app->runUntil([&] { return stack.tool(shopTool.id) != nullptr; }, kWait));
        CHECK(calls >= 1);
        CHECK(stack.roots()[1].open);
        CHECK(stack.rootOf(lib::ItemKind::Tool, shopTool.id.value) == std::optional<std::size_t>(1));
    }
    // Shutting the app down while the share is being read: the read is cancelled and the destructor returns.
    release = false;
    blocked = 0;
    {
        auto app = makeCamApp(user, builtin);
        REQUIRE(app->runUntil([&] { return blocked.load() == 1; }, kWait));
        CHECK(app->libraries()->roots()[1].loading);
    }
    CHECK(cancelled.load() == 1);
    lib::detail::setBackgroundReadHook({});
}
