// The app session: import, save with the imported BREP objects, reopen without the source file, locks, errors,
// several documents, recent files, export and events (task T-017).
#include "api_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <map>
#include <set>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::Hash256;
using ray::api::Caller;
using ray::api::OpenDocument;
using ray::test::kWait;
using ray::test::makeTestApp;
using ray::test::TempDir;

namespace {

std::map<ray::BodyId, Hash256> bodyHashes(const OpenDocument& od)
{
    std::map<ray::BodyId, Hash256> out;
    auto build = od.document->build();
    REQUIRE(build);
    for (const auto& [id, body] : build->bodies) {
        REQUIRE_FALSE(body.shape.isNull());
        out[id] = body.shape.contentHash();
    }
    return out;
}

void requireBuiltOk(const OpenDocument& od)
{
    auto build = od.document->build();
    REQUIRE(build);
    for (const auto& r : build->reports) {
        INFO(r.message);
        CHECK((r.status == ray::doc::FeatureStatus::Ok || r.status == ray::doc::FeatureStatus::Warning));
    }
    CHECK_FALSE(build->bodies.empty());
}

// Imports `source` (copied into `dir` first), saves, then reopens in a new app with the copy deleted.
void roundTrip(const std::string& source, const TempDir& dir)
{
    const std::string copy = dir.file("source" + ray::test::fsPath(source).extension().string());
    std::filesystem::copy_file(ray::test::fsPath(source), ray::test::fsPath(copy));
    const std::string saved = dir.file("part.raycad");
    Hash256 stateHash;
    std::map<ray::BodyId, Hash256> hashes;
    std::string description;
    {
        auto app = makeTestApp(dir.file("user"));
        auto od = app->wait(app->open(copy), kWait);
        REQUIRE(od);
        REQUIRE(app->waitForBuild(**od, kWait));
        requireBuiltOk(**od);
        CHECK((*od)->document->dirty());
        CHECK((*od)->path.empty());
        stateHash = (*od)->document->state()->stateHash();
        hashes = bodyHashes(**od);
        REQUIRE(app->wait(app->save(**od, saved), kWait));
        CHECK_FALSE((*od)->document->dirty());
        CHECK((*od)->path == saved);
        CHECK((*od)->file != nullptr);
        description = ray::api::describeDocumentJson(*(*od)->document->state(), *(*od)->document->build());
        // A second save of an unchanged document works too and writes no object twice.
        REQUIRE(app->wait(app->save(**od), kWait));
    }
    std::filesystem::remove(ray::test::fsPath(copy));
    REQUIRE_FALSE(std::filesystem::exists(ray::test::fsPath(copy)));
    {
        auto app = makeTestApp(dir.file("user"));
        auto od = app->wait(app->open(saved), kWait);
        REQUIRE(od);
        REQUIRE(app->waitForBuild(**od, kWait));
        requireBuiltOk(**od);
        CHECK((*od)->document->state()->stateHash() == stateHash);
        CHECK(bodyHashes(**od) == hashes);
        CHECK_FALSE((*od)->document->dirty());
        CHECK_FALSE((*od)->readOnly);
        CHECK(ray::api::describeDocumentJson(*(*od)->document->state(), *(*od)->document->build()) == description);
        CHECK_THAT(description, ContainsSubstring("contentHash"));
    }
}

} // namespace

TEST_CASE("a STEP file imports, saves and reopens without the STEP file, with equal hashes", "[api][session]")
{
    TempDir dir;
    SECTION("small STEP")
    {
        roundTrip(ray::test::kernelData("box_hole.step"), dir);
    }
    SECTION("an assembly")
    {
        roundTrip(ray::test::kernelData("assembly_mm.step"), dir);
    }
    SECTION("IGES")
    {
        roundTrip(ray::test::kernelData("box_hole.igs"), dir);
    }
    SECTION("a larger corpus file")
    {
        roundTrip(ray::test::corpusFile("nist-pmi/nist_ctc_04_asme1_rd_geometry.stp"), dir);
    }
}

TEST_CASE("opening a missing, corrupt or unsupported file gives a clear error", "[api][session]")
{
    TempDir dir;
    auto app = makeTestApp("");
    auto missing = app->wait(app->open(dir.file("nope.raycad")), kWait);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_FALSE(missing.error().message.empty());

    const std::string corrupt = dir.file("corrupt.raycad");
    ray::test::writeBytes(corrupt, std::string(4096, 'x'));
    auto bad = app->wait(app->open(corrupt), kWait);
    REQUIRE_FALSE(bad);
    CHECK((bad.error().code == ErrorCode::FormatError || bad.error().code == ErrorCode::IoError));
    CHECK_FALSE(bad.error().message.empty());

    auto missingStep = app->wait(app->open(dir.file("nope.step")), kWait);
    REQUIRE_FALSE(missingStep);
    CHECK(missingStep.error().code == ErrorCode::NotFound);

    auto garbage = app->wait(app->open(ray::test::kernelData("garbage.step")), kWait);
    REQUIRE_FALSE(garbage);
    CHECK_FALSE(garbage.error().message.empty());

    auto other = app->wait(app->open(dir.file("notes.txt")), kWait);
    REQUIRE_FALSE(other);
    CHECK(other.error().code == ErrorCode::InvalidArgument);
    CHECK(app->documents().empty());
    CHECK(app->active() == nullptr);
}

TEST_CASE("a file locked by another session opens read-only and names the lock holder", "[api][session][lock]")
{
    TempDir dir;
    const std::string path = dir.file("shared.raycad");
    {
        auto app = makeTestApp("");
        auto od = app->newDocument("");
        REQUIRE(od);
        REQUIRE(app->wait(app->actions().invoke("param.set",
                                                R"({"id":"00000000-0000-4000-8000-0000000000aa","name":"w",)"
                                                R"("expression":"1 in","kind":"length"})",
                                                Caller::User),
                          kWait));
        REQUIRE(app->wait(app->save(**od, path), kWait));
    }
    // Another session (here: the store, in this process) holds the write lock.
    ray::store::OpenInfo holderInfo{};
    auto holder = ray::store::IDocumentFile::open(path, ray::store::OpenMode::ReadWrite, &holderInfo);
    REQUIRE(holder);
    REQUIRE_FALSE(holderInfo.openedReadOnlyBecauseLocked);

    auto app = makeTestApp("");
    auto od = app->wait(app->open(path), kWait);
    REQUIRE(od);
    CHECK((*od)->readOnly);
    REQUIRE((*od)->lock);
    CHECK((*od)->lock->pid > 0);
    CHECK((*od)->lock->sinceUnixMs > 0);
    REQUIRE(app->waitForBuild(**od, kWait));
    CHECK((*od)->document->state()->parameters.size() == 1);

    // Saving in place is refused; saving as a new file works and leaves read-only mode.
    auto inPlace = app->wait(app->save(**od), kWait);
    REQUIRE_FALSE(inPlace);
    CHECK(inPlace.error().code == ErrorCode::Locked);
    // Replacing the locked file is refused too.
    auto replace = app->wait(app->save(**od, path, true), kWait);
    REQUIRE_FALSE(replace);
    const std::string copy = dir.file("mine.raycad");
    REQUIRE(app->wait(app->save(**od, copy), kWait));
    CHECK_FALSE((*od)->readOnly);
    CHECK_FALSE((*od)->lock);
    CHECK((*od)->path == copy);
    holder->reset();
}

TEST_CASE("two documents can be open at once", "[api][session]")
{
    TempDir dir;
    auto app = makeTestApp(dir.file("user"));
    std::vector<ray::api::AppEvent> events;
    const int sub = app->subscribe([&](const ray::api::AppEvent& e) { events.push_back(e); });
    CHECK(sub != 0);
    auto a = app->newDocument("");
    REQUIRE(a);
    auto b = app->wait(app->open(ray::test::kernelData("box_hole.step")), kWait);
    REQUIRE(b);
    CHECK(*a != *b);
    CHECK(app->documents().size() == 2);
    CHECK(app->active() == *b);
    CHECK((*a)->title != (*b)->title);
    CHECK((*b)->title == "box_hole");

    // An action can target either one.
    const std::string setA =
        R"({"id":"00000000-0000-4000-8000-0000000000aa","name":"a","expression":"1","kind":"unitless","document":")" +
        (*a)->id + R"("})";
    REQUIRE(app->wait(app->actions().invoke("param.set", setA, Caller::User), kWait));
    CHECK((*a)->document->state()->parameters.size() == 1);
    CHECK((*b)->document->state()->parameters.empty());
    app->setActive(*a);
    CHECK(app->active() == *a);

    // Both save to their own files; reopening one of them only activates it.
    REQUIRE(app->wait(app->save(**a, dir.file("a.raycad")), kWait));
    REQUIRE(app->waitForBuild(**b, kWait));
    REQUIRE(app->wait(app->save(**b, dir.file("b.raycad")), kWait));
    auto again = app->wait(app->open(dir.file("a.raycad")), kWait);
    REQUIRE(again);
    CHECK(*again == *a);
    CHECK(app->documents().size() == 2);

    // Export from the imported one.
    const std::string stepOut = dir.file("out.step");
    const std::string stlOut = dir.file("out.stl");
    using ray::test::jsonString;
    REQUIRE(app->wait(app->actions().invoke("file.exportStep",
                                            "{\"path\":" + jsonString(stepOut) + ",\"document\":\"" + (*b)->id + "\"}",
                                            Caller::User),
                      kWait));
    CHECK(std::filesystem::file_size(ray::test::fsPath(stepOut)) > 0);
    REQUIRE(app->wait(app->actions().invoke("file.exportStl",
                                            "{\"path\":" + jsonString(stlOut) + ",\"document\":\"" + (*b)->id + "\"}",
                                            Caller::User),
                      kWait));
    CHECK(std::filesystem::file_size(ray::test::fsPath(stlOut)) > 0);
    auto emptyExport = app->wait(
        app->actions().invoke("file.exportStep",
                              "{\"path\":" + jsonString(dir.file("x.step")) + ",\"document\":\"" + (*a)->id + "\"}",
                              Caller::User),
        kWait);
    REQUIRE_FALSE(emptyExport);
    CHECK(emptyExport.error().code == ErrorCode::InvalidArgument);

    // Recent files, most recent first, and persisted for the next session.
    const auto recent = app->recentFiles();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].path == dir.file("a.raycad"));
    CHECK(recent[1].path == dir.file("b.raycad"));
    CHECK(recent[0].thumbnailPng.empty());

    // Closing: a dirty document is refused by the action without discardChanges.
    const std::string setA2 =
        R"({"id":"00000000-0000-4000-8000-0000000000aa","expression":"2","document":")" + (*a)->id + R"("})";
    REQUIRE(app->wait(app->actions().invoke("param.set", setA2, Caller::User), kWait));
    auto refused =
        app->wait(app->actions().invoke("file.close", R"({"document":")" + (*a)->id + R"("})", Caller::User), kWait);
    REQUIRE_FALSE(refused);
    CHECK_THAT(refused.error().message, ContainsSubstring("unsaved"));
    const std::string aId = (*a)->id;
    REQUIRE(app->wait(
        app->actions().invoke("file.close", R"({"discardChanges":true,"document":")" + aId + R"("})", Caller::User),
        kWait));
    CHECK(app->documents().size() == 1);
    CHECK(app->findDocument(aId) == nullptr);
    REQUIRE(app->close(**b));
    CHECK(app->documents().empty());
    CHECK(app->active() == nullptr);

    app->runPending();
    std::set<ray::api::AppEvent::Kind> kinds;
    for (const auto& e : events) {
        kinds.insert(e.kind);
    }
    using K = ray::api::AppEvent::Kind;
    for (K k : {K::DocumentOpened, K::DocumentClosed, K::ActiveDocumentChanged, K::DocumentSaved, K::JobsChanged,
                K::RecentFilesChanged}) {
        CHECK(kinds.count(k) == 1);
    }
    app->unsubscribe(sub);
    app.reset();

    auto next = makeTestApp(dir.file("user"));
    const auto persisted = next->recentFiles();
    REQUIRE(persisted.size() == 2);
    CHECK(persisted[0].path == dir.file("a.raycad"));
}

TEST_CASE("save rules: no name, other extensions, existing files and overwrite", "[api][session]")
{
    TempDir dir;
    auto app = makeTestApp("");
    auto od = app->newDocument("");
    REQUIRE(od);
    CHECK((*od)->title == "Untitled 1");
    CHECK_FALSE(app->newDocument("no-such-template"));

    auto noName = app->wait(app->save(**od), kWait);
    REQUIRE_FALSE(noName);
    CHECK(noName.error().code == ErrorCode::InvalidArgument);
    auto wrongExt = app->wait(app->save(**od, dir.file("x.step")), kWait);
    REQUIRE_FALSE(wrongExt);
    CHECK(wrongExt.error().code == ErrorCode::InvalidArgument);

    // No extension: .raycad is added.
    REQUIRE(app->wait(app->save(**od, dir.file("plain")), kWait));
    CHECK((*od)->path == dir.file("plain.raycad"));
    CHECK((*od)->title == "plain");

    auto second = app->newDocument("");
    REQUIRE(second);
    const std::string other = dir.file("other.raycad");
    REQUIRE(app->wait(app->save(**second, other), kWait));
    REQUIRE(app->close(**second));
    auto exists = app->wait(app->save(**od, other), kWait);
    REQUIRE_FALSE(exists);
    CHECK(exists.error().code == ErrorCode::InvalidArgument);
    // Overwriting works for a RayCAD file nobody has open, not for one open in this session.
    REQUIRE(app->wait(app->save(**od, other, true), kWait));
    CHECK((*od)->path == other);
    auto third = app->newDocument("");
    REQUIRE(third);
    auto openHere = app->wait(app->save(**third, other, true), kWait);
    REQUIRE_FALSE(openHere);
    CHECK(openHere.error().code == ErrorCode::Locked);
    // A file that is not a RayCAD document is never replaced.
    const std::string text = dir.file("text.raycad");
    ray::test::writeBytes(text, "hello");
    auto notOurs = app->wait(app->save(**third, text, true), kWait);
    REQUIRE_FALSE(notOurs);
    CHECK(std::filesystem::file_size(ray::test::fsPath(text)) == 5);
}

TEST_CASE("import into an open document is one undo step", "[api][session]")
{
    TempDir dir;
    auto app = makeTestApp("");
    auto od = app->newDocument("");
    REQUIRE(od);
    const Hash256 empty = (*od)->document->state()->stateHash();
    REQUIRE(app->wait(app->importFile(**od, ray::test::kernelData("box_hole.step")), kWait));
    CHECK((*od)->document->state()->features.size() == 1);
    REQUIRE(app->waitForBuild(**od, kWait));
    requireBuiltOk(**od);
    REQUIRE(app->wait(app->actions().invoke("edit.undo", "", Caller::User), kWait));
    CHECK((*od)->document->state()->stateHash() == empty);

    // file.import with an explicit role and name, through the action.
    std::string args = "{\"path\":" + ray::test::jsonString(ray::test::kernelData("box_hole.step")) +
                       R"(,"role":"fixture","name":"Vise"})";
    auto imported = app->wait(app->actions().invoke("file.import", args, Caller::User), kWait);
    REQUIRE(imported);
    const auto state = (*od)->document->state();
    REQUIRE(state->features.size() == 1);
    CHECK(state->features[0].name == "Vise");
    REQUIRE_FALSE(state->bodies.empty());
    CHECK(state->bodies[0].role == ray::doc::Body::Role::Fixture);

    // A document busy with a job cannot close.
    auto job = app->save(**od, dir.file("busy.raycad"));
    auto closeWhileSaving = app->close(**od);
    REQUIRE(app->wait(job, kWait));
    REQUIRE_FALSE(closeWhileSaving);
    CHECK(closeWhileSaving.error().code == ErrorCode::Locked);
    app->runPending();
    CHECK(app->close(**od));
}

TEST_CASE("shutting down while a STEP import runs is clean", "[api][session]")
{
    for (int delayMs : {0, 50, 300}) {
        auto app = makeTestApp("");
        auto job = app->open(ray::test::corpusFile("nist-pmi/nist_ctc_04_asme1_rd_geometry.stp"));
        app->runUntil([] { return false; }, std::chrono::milliseconds(delayMs));
        app.reset();
        CHECK(job.done());
    }
}
