// Crash recovery in the app session: a killed session's changes are offered, recovered with the same state hash
// (imported objects included) or discarded; saved and cleanly closed documents leave nothing; read-only documents
// have no journal (task T-017 follow-up, store.h IRecoveryJournal).
#include "api_test_util.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::api::Caller;
using ray::test::kWait;
using ray::test::makeTestApp;

namespace {

struct Reached {
    std::string stateHash;
    std::string description;
};

// Runs the helper until it wrote what it reached, then kills it (no destructor runs: like a crash).
Reached runAndKill(const std::vector<std::string>& args, const std::string& out)
{
    auto process = storetest::Process::spawnHelper(args);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    while (!storetest::exists(out)) {
        REQUIRE(process.running());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    process.kill();
    const std::string text = storetest::readFile(out);
    const auto nl = text.find('\n');
    REQUIRE(nl != std::string::npos);
    Reached r{text.substr(0, nl), text.substr(nl + 1)};
    while (!r.description.empty() && (r.description.back() == '\n' || r.description.back() == '\r')) {
        r.description.pop_back();
    }
    return r;
}

std::string describe(const ray::api::OpenDocument& od)
{
    return ray::api::describeDocumentJson(*od.document->state(), *od.document->build());
}

// A saved document with one parameter.
void makeDocument(const std::string& userData, const std::string& path)
{
    auto app = makeTestApp(userData);
    auto od = app->newDocument("");
    REQUIRE(od);
    REQUIRE(app->wait(
        app->actions().invoke(
            "param.set",
            R"({"id":"00000000-0000-4000-8000-0000000000a0","name":"base","expression":"1","kind":"unitless"})",
            Caller::User),
        kWait));
    REQUIRE(app->wait(app->save(**od, path), kWait));
    REQUIRE(app->close(**od));
    CHECK(app->pendingRecoveries().empty());
}

} // namespace

TEST_CASE("a killed session's changes are recovered with the same state hash", "[api][recovery]")
{
    storetest::TempDir dir;
    const std::string userData = dir.path("user");
    const std::string doc = dir.path("part.raycad");
    const std::string step = ray::test::kernelData("box_hole.step");
    makeDocument(userData, doc);

    const Reached before = runAndKill({"edit", userData, doc, step, dir.path("reached.txt")}, dir.path("reached.txt"));

    auto app = makeTestApp(userData);
    auto pending = app->pendingRecoveries();
    REQUIRE(pending.size() == 1);
    CHECK(ray::test::fsPath(pending[0].filePath) == ray::test::fsPath(doc));
    CHECK_FALSE(pending[0].unsaved);
    CHECK(pending[0].title == "part");
    CHECK(pending[0].steps >= 6);
    CHECK(pending[0].lastWriteMs > 0);

    // A plain open refuses until the changes are recovered or discarded.
    auto refused = app->wait(app->open(doc), kWait);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Locked);
    CHECK_THAT(refused.error().message, ContainsSubstring("recover"));

    auto od = app->wait(app->recover(pending[0]), kWait);
    REQUIRE(od);
    REQUIRE(app->waitForBuild(**od, kWait));
    CHECK((*od)->document->state()->stateHash().hex() == before.stateHash);
    CHECK(describe(**od) == before.description); // the imported body came back from the journal's objects
    CHECK_THAT(before.description, ContainsSubstring("contentHash"));
    CHECK((*od)->document->dirty());
    CHECK((*od)->path == doc);
    CHECK((*od)->journal != nullptr);
    CHECK((*od)->warnings.empty());
    CHECK(app->pendingRecoveries().empty()); // this session holds the journal now

    // Undo and redo still work on the replayed history.
    REQUIRE(app->wait(app->actions().invoke("edit.undo", "", Caller::User), kWait));
    REQUIRE(app->wait(app->actions().invoke("edit.redo", "", Caller::User), kWait));
    CHECK((*od)->document->state()->stateHash().hex() == before.stateHash);

    // Saved and closed cleanly: nothing is offered any more, and the file reopens at the recovered state.
    REQUIRE(app->wait(app->save(**od), kWait));
    CHECK_FALSE((*od)->document->dirty());
    REQUIRE(app->close(**od));
    CHECK(app->pendingRecoveries().empty());
    app.reset();
    auto next = makeTestApp(userData);
    CHECK(next->pendingRecoveries().empty());
    auto reopened = next->wait(next->open(doc), kWait);
    REQUIRE(reopened);
    REQUIRE(next->waitForBuild(**reopened, kWait));
    CHECK((*reopened)->document->state()->stateHash().hex() == before.stateHash);
    CHECK(describe(**reopened) == before.description);
}

TEST_CASE("a never-saved document's changes are recovered untitled, or discarded", "[api][recovery]")
{
    storetest::TempDir dir;
    const std::string userData = dir.path("user");
    const std::string step = ray::test::kernelData("box_hole.step");

    SECTION("recover")
    {
        const Reached before = runAndKill({"new", userData, step, dir.path("reached.txt")}, dir.path("reached.txt"));
        auto app = makeTestApp(userData);
        auto pending = app->pendingRecoveries();
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].unsaved);
        CHECK(pending[0].title == "Untitled");
        auto od = app->wait(app->recover(pending[0]), kWait);
        REQUIRE(od);
        REQUIRE(app->waitForBuild(**od, kWait));
        CHECK((*od)->path.empty());
        CHECK_THAT((*od)->title, ContainsSubstring("recovered"));
        CHECK((*od)->document->state()->stateHash().hex() == before.stateHash);
        CHECK(describe(**od) == before.description);
        // Save As makes it a real document; the backing file goes away.
        const std::string saved = dir.path("rescued.raycad");
        REQUIRE(app->wait(app->save(**od, saved), kWait));
        CHECK((*od)->path == saved);
        REQUIRE(app->close(**od));
        CHECK(app->pendingRecoveries().empty());
        CHECK(std::filesystem::is_empty(ray::test::fsPath(userData) / "unsaved"));
    }
    SECTION("discard")
    {
        (void)runAndKill({"new", userData, step, dir.path("reached.txt")}, dir.path("reached.txt"));
        auto app = makeTestApp(userData);
        auto pending = app->pendingRecoveries();
        REQUIRE(pending.size() == 1);
        REQUIRE(app->discardRecovery(pending[0]));
        CHECK(app->pendingRecoveries().empty());
        CHECK_FALSE(std::filesystem::exists(ray::test::fsPath(pending[0].filePath)));
        auto again = app->discardRecovery(pending[0]);
        REQUIRE_FALSE(again);
        CHECK(again.error().code == ErrorCode::NotFound);
        app.reset();
        CHECK(makeTestApp(userData)->pendingRecoveries().empty());
    }
}

TEST_CASE("discarding a saved document's journal lets it open normally", "[api][recovery]")
{
    storetest::TempDir dir;
    const std::string userData = dir.path("user");
    const std::string doc = dir.path("part.raycad");
    makeDocument(userData, doc);
    (void)runAndKill({"edit", userData, doc, ray::test::kernelData("box_hole.step"), dir.path("reached.txt")},
                     dir.path("reached.txt"));
    auto app = makeTestApp(userData);
    auto pending = app->pendingRecoveries();
    REQUIRE(pending.size() == 1);
    REQUIRE(app->discardRecovery(pending[0]));
    CHECK(app->pendingRecoveries().empty());
    // The dead session's lock is stale; the store breaks it after it went stale.
    REQUIRE(storetest::breakLockOfDeadWriter(doc));
    auto od = app->wait(app->open(doc), kWait);
    REQUIRE(od);
    REQUIRE(app->waitForBuild(**od, kWait));
    CHECK((*od)->document->state()->parameters.size() == 1); // only what was saved
    CHECK((*od)->journal != nullptr);
}

TEST_CASE("journals follow the document's life: journalled, kept, discarded, none when read-only", "[api][recovery]")
{
    ray::test::TempDir dir;
    const std::string userData = dir.file("user");
    const std::string doc = dir.file("doc.raycad");
    makeDocument(userData, doc);
    const std::string edit = R"({"id":"00000000-0000-4000-8000-0000000000a0","expression":"7"})";

    SECTION("a read-write open journals; a clean close leaves nothing")
    {
        auto app = makeTestApp(userData);
        auto od = app->wait(app->open(doc), kWait);
        REQUIRE(od);
        CHECK((*od)->journal != nullptr);
        REQUIRE(app->wait(app->actions().invoke("param.set", edit, Caller::User), kWait));
        REQUIRE(app->close(**od));
        CHECK(app->pendingRecoveries().empty());
    }
    SECTION("the app's destructor keeps unsaved changes for the next start")
    {
        std::string hash;
        {
            auto app = makeTestApp(userData);
            auto od = app->wait(app->open(doc), kWait);
            REQUIRE(od);
            REQUIRE(app->wait(app->actions().invoke("param.set", edit, Caller::User), kWait));
            hash = (*od)->document->state()->stateHash().hex();
            auto fresh = app->newDocument(""); // unchanged: nothing to keep
            REQUIRE(fresh);
            CHECK((*fresh)->journal == nullptr);
        }
        auto app = makeTestApp(userData);
        auto pending = app->pendingRecoveries();
        REQUIRE(pending.size() == 1);
        auto od = app->wait(app->recover(pending[0]), kWait);
        REQUIRE(od);
        CHECK((*od)->document->state()->stateHash().hex() == hash);
    }
    SECTION("a read-only document has no journal")
    {
        ray::store::OpenInfo info{};
        auto holder = ray::store::IDocumentFile::open(doc, ray::store::OpenMode::ReadWrite, &info);
        REQUIRE(holder);
        auto app = makeTestApp(userData);
        auto od = app->wait(app->open(doc), kWait);
        REQUIRE(od);
        CHECK((*od)->readOnly);
        CHECK((*od)->journal == nullptr);
        REQUIRE(app->wait(app->actions().invoke("param.set", edit, Caller::User), kWait));
        CHECK((*od)->journal == nullptr);
        app.reset();
        holder->reset();
        CHECK(makeTestApp(userData)->pendingRecoveries().empty());
    }
    SECTION("a never-saved document journals from its first change into a backing file")
    {
        auto app = makeTestApp(userData);
        auto od = app->newDocument("");
        REQUIRE(od);
        CHECK((*od)->journal == nullptr);
        REQUIRE(app->wait(
            app->actions().invoke(
                "param.set",
                R"({"id":"00000000-0000-4000-8000-0000000000b0","name":"x","expression":"1","kind":"unitless"})",
                Caller::User),
            kWait));
        CHECK((*od)->journal != nullptr);
        CHECK((*od)->path.empty());
        CHECK_FALSE(std::filesystem::is_empty(ray::test::fsPath(userData) / "unsaved"));
        REQUIRE(app->close(**od));
        CHECK(std::filesystem::is_empty(ray::test::fsPath(userData) / "unsaved"));
        CHECK(app->pendingRecoveries().empty());
    }
    SECTION("no user data folder: no journals")
    {
        auto app = makeTestApp("");
        auto od = app->wait(app->open(doc), kWait);
        REQUIRE(od);
        CHECK((*od)->journal == nullptr);
        CHECK(app->pendingRecoveries().empty());
    }
}
