// The action registry: schema checks with the path of the bad field, the confirmation rule, deterministic search,
// document commands as actions, undo/redo through actions, jobs and cancellation (task T-017).
#include "api_test_util.h"

#include "schema.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::api::ActionInfo;
using ray::api::ActionResult;
using ray::api::Caller;
using ray::test::kWait;
using ray::test::makeTestApp;
using ray::test::TempDir;

namespace {

ActionInfo testAction(std::string id, bool gcode, bool longRunning = false)
{
    ActionInfo info;
    info.id = std::move(id);
    info.title = "Test action";
    info.category = "Test";
    info.description = "A test action";
    info.argsSchemaJson =
        R"({"type":"object","additionalProperties":false,"properties":{"n":{"type":"integer","default":1}}})";
    info.changesToolpathsOrGcode = gcode;
    info.longRunning = longRunning;
    return info;
}

std::string newDocId(ray::api::HeadlessApp& app)
{
    auto done = app.wait(app.actions().invoke("file.new", "", Caller::User), kWait);
    REQUIRE(done);
    auto doc = app.active();
    REQUIRE(doc != nullptr);
    return doc->id;
}

} // namespace

TEST_CASE("invalid arguments are rejected by the schema with the path of the bad field", "[api][actions]")
{
    TempDir dir;
    auto app = makeTestApp("");
    auto& actions = app->actions();

    auto wrongType = app->wait(actions.invoke("file.saveAs", R"({"path":5})", Caller::User), kWait);
    REQUIRE_FALSE(wrongType);
    CHECK(wrongType.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(wrongType.error().message, ContainsSubstring("/path") && ContainsSubstring("string"));

    auto missing = app->wait(actions.invoke("file.open", "{}", Caller::User), kWait);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(missing.error().message, ContainsSubstring("/path") && ContainsSubstring("required"));

    auto unknown = app->wait(actions.invoke("file.save", R"({"bogus":true})", Caller::User), kWait);
    REQUIRE_FALSE(unknown);
    CHECK_THAT(unknown.error().message, ContainsSubstring("/bogus"));

    auto badEnum = app->wait(actions.invoke("file.import", R"({"path":"x.step","heal":"some"})", Caller::User), kWait);
    REQUIRE_FALSE(badEnum);
    CHECK_THAT(badEnum.error().message, ContainsSubstring("/heal"));

    // Nested: a colour channel out of range in body.set names the array index.
    newDocId(*app);
    auto nested = app->wait(
        actions.invoke("body.set", R"({"id":"00000000-0000-4000-8000-000000000001","color":[0,0,2,1]})", Caller::User),
        kWait);
    REQUIRE_FALSE(nested);
    CHECK(nested.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(nested.error().message, ContainsSubstring("/color/2"));

    auto notJson = app->wait(actions.invoke("file.open", "{path:", Caller::User), kWait);
    REQUIRE_FALSE(notJson);
    CHECK(notJson.error().code == ErrorCode::InvalidArgument);

    auto notObject = app->wait(actions.invoke("file.open", "[1]", Caller::User), kWait);
    REQUIRE_FALSE(notObject);
    CHECK(notObject.error().code == ErrorCode::InvalidArgument);

    auto none = app->wait(actions.invoke("no.such.action", "{}", Caller::User), kWait);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
}

TEST_CASE("the schema subset reports the first bad value as a JSON Pointer", "[api][actions][schema]")
{
    namespace json = ray::api::json;
    const auto schema = json::parse(R"({"type":"object","required":["a"],"properties":{
        "a":{"type":"array","items":{"type":"object","properties":{"b/c":{"type":"integer","minimum":0}}}},
        "s":{"type":"string","minLength":2,"pattern":"^x"},"e":{"enum":["p","q"]},
        "n":{"anyOf":[{"type":"null"},{"type":"number","exclusiveMaximum":3}]}},"additionalProperties":false})");
    REQUIRE(schema);
    const auto check = [&](const char* text) {
        auto v = json::parse(text);
        REQUIRE(v);
        return ray::api::schema::validate(*schema, *v);
    };
    CHECK(check(R"({"a":[{"b/c":1}],"s":"xy","e":"p","n":null})"));
    CHECK(check(R"({"a":[],"n":2.5})"));
    auto e1 = check(R"({"a":[{"b/c":1},{"b/c":-1}]})");
    REQUIRE_FALSE(e1);
    CHECK_THAT(e1.error().message, ContainsSubstring("/a/1/b~1c"));
    auto e2 = check(R"({"a":[{"b/c":1.5}]})");
    REQUIRE_FALSE(e2);
    CHECK_THAT(e2.error().message, ContainsSubstring("integer"));
    CHECK_FALSE(check(R"({"a":[],"s":"x"})"));
    CHECK_FALSE(check(R"({"a":[],"s":"yy"})"));
    CHECK_FALSE(check(R"({"a":[],"e":"r"})"));
    CHECK_FALSE(check(R"({"a":[],"n":3})"));
    auto e3 = check(R"({})");
    REQUIRE_FALSE(e3);
    CHECK_THAT(e3.error().message, ContainsSubstring("/a"));
}

TEST_CASE("defaults fill missing arguments; an empty argument string means {}", "[api][actions]")
{
    auto app = makeTestApp("");
    std::atomic<int> seen{-1};
    REQUIRE(app->actions().add(testAction("test.echo", false), [&](std::string_view args, Caller, ray::JobContext&) {
        seen = args == R"({"n":1})" ? 1 : 0;
        return ray::Result<ActionResult>(ActionResult{std::string(args), {}});
    }));
    auto done = app->wait(app->actions().invoke("test.echo", "", Caller::User), kWait);
    REQUIRE(done);
    CHECK(seen == 1);
    CHECK(done->resultJson == R"({"n":1})");

    // Registering the same id twice, an empty id or a bad schema fails.
    auto noop = [](std::string_view, Caller, ray::JobContext&) {
        return ray::Result<ActionResult>(ActionResult{});
    };
    CHECK_FALSE(app->actions().add(testAction("test.echo", false), noop));
    CHECK_FALSE(app->actions().add(testAction("", false), noop));
    ActionInfo bad = testAction("test.bad", false);
    bad.argsSchemaJson = "[1,2]";
    CHECK_FALSE(app->actions().add(bad, noop));
    CHECK_FALSE(app->actions().add(testAction("test.nofn", false), nullptr));
}

TEST_CASE("automation needs confirmation for actions that change toolpaths or G-code", "[api][actions][confirm]")
{
    auto app = makeTestApp("");
    std::atomic<int> runs{0};
    REQUIRE(app->actions().add(testAction("test.post", true), [&](std::string_view, Caller, ray::JobContext&) {
        ++runs;
        return ray::Result<ActionResult>(ActionResult{"{}", {}});
    }));
    int hookCalls = 0;
    bool answer = false;
    std::string lastArgs;
    app->setConfirmHook([&](const ActionInfo& info, std::string_view args, std::string_view summary) {
        ++hookCalls;
        lastArgs = std::string(args);
        CHECK(info.id == "test.post");
        CHECK_FALSE(summary.empty());
        return answer;
    });

    SECTION("an agent is declined when the hook says no, and nothing runs")
    {
        auto r = app->wait(app->actions().invoke("test.post", R"({"n":7})", Caller::Agent), kWait);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(hookCalls == 1);
        CHECK(lastArgs == R"({"n":7})");
        CHECK(runs == 0);
    }
    SECTION("a script runs after the hook says yes")
    {
        answer = true;
        auto r = app->wait(app->actions().invoke("test.post", "{}", Caller::Script), kWait);
        REQUIRE(r);
        CHECK(hookCalls == 1);
        CHECK(runs == 1);
    }
    SECTION("the user is never asked")
    {
        auto r = app->wait(app->actions().invoke("test.post", "{}", Caller::User), kWait);
        REQUIRE(r);
        CHECK(hookCalls == 0);
        CHECK(runs == 1);
    }
    SECTION("without a hook automation is declined")
    {
        app->setConfirmHook({});
        auto r = app->wait(app->actions().invoke("test.post", "{}", Caller::Agent), kWait);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(runs == 0);
    }
    SECTION("actions that do not change toolpaths need no confirmation")
    {
        auto r = app->wait(app->actions().invoke("app.version", "{}", Caller::Agent), kWait);
        REQUIRE(r);
        CHECK(hookCalls == 0);
    }
}

TEST_CASE("search ranks deterministically", "[api][actions][search]")
{
    std::vector<std::vector<std::string>> firstRun;
    const std::vector<std::string> queries = {"sav", "save", "open", "undo", "exp",
                                              "stl", "feat", "fadd", "  ",   "zzzq"};
    for (int run = 0; run < 2; ++run) {
        auto app = makeTestApp("");
        std::vector<std::vector<std::string>> ids;
        for (const auto& q : queries) {
            std::vector<std::string> list;
            for (const auto& a : app->actions().search(q, 0)) {
                list.push_back(a.id);
            }
            ids.push_back(list);
        }
        if (run == 0) {
            firstRun = ids;
        } else {
            CHECK(ids == firstRun);
        }
    }
    // Fixed expectations, so a ranking change between builds or platforms is noticed.
    REQUIRE(firstRun[0].size() >= 2);
    CHECK(firstRun[0][0] == "file.save");
    CHECK(firstRun[0][1] == "file.saveAs");
    CHECK(firstRun[1][0] == "file.save");
    CHECK(firstRun[2][0] == "file.open");
    CHECK(firstRun[3][0] == "edit.undo");
    CHECK(firstRun[4][0] == "file.exportStl");
    CHECK(firstRun[5][0] == "file.exportStl");
    CHECK(std::find(firstRun[7].begin(), firstRun[7].end(), "feature.add") != firstRun[7].end());
    CHECK(firstRun[9].empty());

    auto app = makeTestApp("");
    const auto all = app->actions().all();
    CHECK(firstRun[8].size() == all.size()); // a blank query lists everything
    CHECK(app->actions().search("sav", 1).size() == 1);
    CHECK(std::is_sorted(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.id < b.id; }));
}

TEST_CASE("the built-in actions are registered and every document command is an action", "[api][actions]")
{
    auto app = makeTestApp("");
    std::set<std::string> ids;
    for (const auto& a : app->actions().all()) {
        ids.insert(a.id);
    }
    for (const char* id :
         {"app.about", "app.version", "file.new", "file.open", "file.save", "file.saveAs", "file.close", "file.import",
          "file.exportStep", "file.exportIges", "file.exportStl", "edit.undo", "edit.redo"}) {
        CHECK(ids.count(id) == 1);
    }
    const auto& commands = app->commandTypes();
    REQUIRE_FALSE(commands.names().empty());
    namespace json = ray::api::json;
    for (const std::string& name : commands.names()) {
        INFO(name);
        auto info = app->actions().find(name);
        REQUIRE(info);
        CHECK(info->changesDocument);
        CHECK(info->changesToolpathsOrGcode == commands.find(name)->changesToolpathsOrGcode());
        // The action takes the command's arguments, plus "document".
        auto actionSchema = json::parse(info->argsSchemaJson);
        auto commandSchema = json::parse(commands.find(name)->argsSchemaJson());
        REQUIRE(actionSchema);
        REQUIRE(commandSchema);
        const auto* props = actionSchema->find("properties");
        REQUIRE(props);
        CHECK(props->find("document") != nullptr);
        if (const auto* own = commandSchema->find("properties")) {
            for (const auto& [key, value] : own->asObject()) {
                CHECK(props->find(key) != nullptr);
            }
        }
    }
    auto about = app->wait(app->actions().invoke("app.about", "", Caller::User), kWait);
    REQUIRE(about);
    CHECK_THAT(about->resultJson, ContainsSubstring(RAY_VERSION_STRING));
}

TEST_CASE("document commands, undo and redo through actions restore the state hashes", "[api][actions]")
{
    auto app = makeTestApp("");
    const std::string docId = newDocId(*app);
    auto& actions = app->actions();
    auto* od = app->findDocument(docId);
    REQUIRE(od);
    const auto h0 = od->document->state()->stateHash();

    auto set = app->wait(actions.invoke("param.set",
                                        R"({"id":"00000000-0000-4000-8000-0000000000aa","name":"width",)"
                                        R"("expression":"2 in","kind":"length"})",
                                        Caller::User),
                         kWait);
    REQUIRE(set);
    const auto h1 = od->document->state()->stateHash();
    CHECK(h1 != h0);
    CHECK_THAT(set->resultJson, ContainsSubstring(h1.hex()));
    // "document" picks the document explicitly.
    auto edit = app->wait(actions.invoke("param.set",
                                         R"({"id":"00000000-0000-4000-8000-0000000000aa","expression":"3 in",)"
                                         R"("document":")" +
                                             docId + R"("})",
                                         Caller::User),
                          kWait);
    REQUIRE(edit);
    const auto h2 = od->document->state()->stateHash();
    CHECK(h2 != h1);

    REQUIRE(app->wait(actions.invoke("edit.undo", "{}", Caller::User), kWait));
    CHECK(od->document->state()->stateHash() == h1);
    REQUIRE(app->wait(actions.invoke("edit.undo", "", Caller::User), kWait));
    CHECK(od->document->state()->stateHash() == h0);
    auto nothing = app->wait(actions.invoke("edit.undo", "", Caller::User), kWait);
    REQUIRE_FALSE(nothing);
    CHECK(nothing.error().code == ErrorCode::InvalidArgument);
    REQUIRE(app->wait(actions.invoke("edit.redo", "", Caller::User), kWait));
    REQUIRE(app->wait(actions.invoke("edit.redo", "", Caller::User), kWait));
    CHECK(od->document->state()->stateHash() == h2);

    // A command the document rejects changes nothing and reports why.
    auto rejected = app->wait(
        actions.invoke("feature.remove", R"({"id":"00000000-0000-4000-8000-0000000000bb"})", Caller::User), kWait);
    REQUIRE_FALSE(rejected);
    CHECK(od->document->state()->stateHash() == h2);

    auto wrongDoc = app->wait(actions.invoke("edit.undo", R"({"document":"nope"})", Caller::User), kWait);
    REQUIRE_FALSE(wrongDoc);
    CHECK(wrongDoc.error().code == ErrorCode::NotFound);
}

TEST_CASE("long-running actions run as jobs with progress and cancel, never blocking the caller",
          "[api][actions][jobs]")
{
    auto app = makeTestApp("");
    std::atomic<bool> started{false};
    REQUIRE(app->actions().add(testAction("test.long", false, true),
                               [&](std::string_view, Caller, ray::JobContext& ctx) -> ray::Result<ActionResult> {
                                   started = true;
                                   for (int i = 0; i < 100000; ++i) {
                                       ctx.progress.set(0.5, "Working");
                                       if (auto st = ctx.cancel.check(); !st) {
                                           return tl::unexpected<ray::Error>(st.error());
                                       }
                                       std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   }
                                   return ActionResult{"{}", {}};
                               }));
    int jobEvents = 0;
    app->subscribe([&](const ray::api::AppEvent& e) {
        if (e.kind == ray::api::AppEvent::Kind::JobsChanged) {
            ++jobEvents;
        }
    });
    const auto before = std::chrono::steady_clock::now();
    auto job = app->actions().invoke("test.long", "{}", Caller::User);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    REQUIRE(app->runUntil(
        [&] {
            auto jobs = app->activeJobs();
            return started.load() && jobs.size() == 1 && jobs[0].progress >= 0.5;
        },
        kWait));
    const auto jobs = app->activeJobs();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].title == "Test action");
    CHECK(jobs[0].stage == "Working");
    app->cancelJob(jobs[0].id);
    auto result = app->wait(job, kWait);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    REQUIRE(app->runUntil([&] { return app->activeJobs().empty(); }, kWait));
    app->runPending();
    CHECK(jobEvents >= 2);
}

TEST_CASE("shutting the app down with a job running is clean", "[api][actions][jobs]")
{
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    {
        auto app = makeTestApp("");
        REQUIRE(app->actions().add(testAction("test.forever", false, true),
                                   [&](std::string_view, Caller, ray::JobContext& ctx) -> ray::Result<ActionResult> {
                                       started = true;
                                       while (!ctx.cancel.cancelled()) {
                                           std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                       }
                                       finished = true;
                                       return tl::unexpected<ray::Error>(ray::Error{ErrorCode::Cancelled, "", {}});
                                   }));
        // A UI-thread action queued behind it, and a job waiting for the UI thread that never comes.
        (void)app->actions().invoke("test.forever", "{}", Caller::User);
        (void)app->actions().invoke("file.new", "{}", Caller::User);
        (void)app->open(ray::test::kernelData("box_hole.step"));
        REQUIRE(app->runUntil([&] { return started.load(); }, kWait));
    }
    CHECK(finished);
}
