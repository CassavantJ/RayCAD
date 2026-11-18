// Format migration (PLAN.md 4.4, task T-012): a fabricated version 0 file is upgraded at open and keeps a .bak; a
// failing, throwing, missing or unverifiable step leaves the original untouched; a newer file is never modified.
//
// The "v0" format is invented for the test: version 1 without the derived and thumbs tables. No real version 0 was
// ever released.
#include "file.h"
#include "migrate.h"
#include "raw_db.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <random>
#include <set>
#include <stdexcept>

using namespace storetest;
using ray::ErrorCode;
using ray::Status;
using ray::store::IDocumentFile;
using ray::store::Migration;
using ray::store::MigrationCtx;
using ray::store::OpenInfo;
using ray::store::OpenMode;
using ray::store::SqlValue;

namespace {

// Every registered step is forgotten when the test ends.
struct Steps {
    Steps() { ray::store::detail::clearMigrations(); }
    ~Steps() { ray::store::detail::clearMigrations(); }
};

std::string filename(const std::string& utf8Path)
{
    const auto p =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()));
    const std::u8string name = p.filename().u8string();
    return std::string(reinterpret_cast<const char*>(name.data()), name.size());
}

std::set<std::string> filesIn(const TempDir& dir)
{
    std::set<std::string> out;
    const auto root =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(dir.dir().data()), dir.dir().size()));
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::u8string name = entry.path().filename().u8string();
        out.insert(std::string(reinterpret_cast<const char*>(name.data()), name.size()));
    }
    return out;
}

// A version 0 file holding one saved state; returns that state's hash.
std::string makeV0(const std::string& path)
{
    std::mt19937_64 rng(404);
    const ray::doc::DocState state = makeState(rng, 5, 100);
    {
        auto file = IDocumentFile::create(path);
        REQUIRE(file);
        IDocumentFile::SaveRequest request;
        request.state = std::make_shared<const ray::doc::DocState>(state);
        request.message = "saved as v0";
        ray::JobContext ctx;
        REQUIRE((*file)->save(request, ctx));
    }
    RawDb raw(path);
    REQUIRE(raw.exec("DROP TABLE derived; DROP TABLE thumbs; PRAGMA user_version = 0") == SQLITE_OK);
    return state.stateHash().hex();
}

// The real 0 -> 1 step of the invented format: create the two tables, note where the file came from.
Migration v0ToV1()
{
    return {0, 1, [](MigrationCtx& ctx) -> Status {
                if (ctx.from != 0 || ctx.to != 1) {
                    return tl::unexpected<ray::Error>(ray::Error{ErrorCode::Internal, "wrong versions", {}});
                }
                for (const std::string& statement : ray::store::detail::schemaStatements()) {
                    if (statement.find("TABLE derived") != std::string::npos ||
                        statement.find("TABLE thumbs") != std::string::npos) {
                        if (auto st = ctx.exec(statement); !st) {
                            return st;
                        }
                    }
                }
                auto rows = ctx.query("SELECT value FROM meta WHERE key = ?1", {SqlValue(std::string("file_id"))});
                if (!rows || rows->size() != 1 || !std::holds_alternative<std::string>((*rows)[0][0])) {
                    return tl::unexpected<ray::Error>(ray::Error{ErrorCode::Internal, "no file id", {}});
                }
                return ctx.exec("INSERT INTO meta (key, value) VALUES (?1, ?2)",
                                {SqlValue(std::string("migrated_from")), SqlValue(std::int64_t{0})});
            }};
}

} // namespace

TEST_CASE("a version 0 file is upgraded at open and the original is kept as a .bak", "[store][migrate]")
{
    Steps steps;
    TempDir dir;
    const std::string path = dir.path("old part ü.raycad");
    const std::string hash = makeV0(path);
    const std::string original = readFile(path);
    ray::store::registerMigration(v0ToV1());
    // Invalid or second steps are ignored: the first valid one stays.
    ray::store::registerMigration({0, 1, [](MigrationCtx&) -> Status {
                                       return tl::unexpected<ray::Error>(ray::Error{ErrorCode::Internal, "second", {}});
                                   }});
    ray::store::registerMigration({1, 2, [](MigrationCtx&) -> Status { return {}; }});
    ray::store::registerMigration({0, 2, [](MigrationCtx&) -> Status { return {}; }});
    ray::store::registerMigration({0, 1, nullptr});
    CHECK(ray::store::detail::canMigrate(0));
    CHECK_FALSE(ray::store::detail::canMigrate(-1));

    {
        OpenInfo info{};
        auto file = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
        INFO((file ? std::string() : file.error().message));
        REQUIRE(file);
        CHECK(info.migrated);
        CHECK(info.formatVersion == 0);
        auto head = (*file)->loadHead();
        REQUIRE(head);
        CHECK((*head)->stateHash().hex() == hash);
    }
    const std::string bak = dir.path("old part ü.v0.bak");
    CHECK(ray::store::detail::backupPathFor(path, 0) == bak);
    REQUIRE(exists(bak));
    CHECK(readFile(bak) == original);
    CHECK(filesIn(dir) == std::set<std::string>{filename(path), filename(bak)});
    {
        RawDb raw(path);
        CHECK(raw.text("PRAGMA user_version") == "1");
        CHECK(raw.text("PRAGMA integrity_check") == "ok");
        CHECK(raw.text("SELECT value FROM meta WHERE key = 'migrated_from'") == "0");
        CHECK(raw.text("SELECT count(*) FROM sqlite_master WHERE name IN ('derived', 'thumbs')") == "2");
    }
    // Opened again: nothing more to do.
    OpenInfo again{};
    REQUIRE(IDocumentFile::open(path, OpenMode::ReadWrite, &again));
    CHECK_FALSE(again.migrated);
    CHECK(again.formatVersion == 1);

    // A second upgrade of another copy never overwrites the first backup.
    removeDocument(path);
    makeV0(path);
    REQUIRE(IDocumentFile::open(path, OpenMode::ReadWrite));
    CHECK(readFile(bak) == original);
    CHECK(exists(dir.path("old part ü.v0.2.bak")));
}

TEST_CASE("a read-only open upgrades only its private copy", "[store][migrate]")
{
    Steps steps;
    TempDir dir;
    const std::string path = dir.path("read only v0.raycad");
    const std::string hash = makeV0(path);
    const std::string original = readFile(path);
    ray::store::registerMigration(v0ToV1());
    OpenInfo info{};
    auto file = IDocumentFile::open(path, OpenMode::ReadOnly, &info);
    INFO((file ? std::string() : file.error().message));
    REQUIRE(file);
    CHECK(info.migrated);
    CHECK(info.formatVersion == 0);
    auto head = (*file)->loadHead();
    REQUIRE(head);
    CHECK((*head)->stateHash().hex() == hash);
    file->reset();
    CHECK(readFile(path) == original);
    CHECK(filesIn(dir) == std::set<std::string>{filename(path)});
}

TEST_CASE("a migration that fails leaves the original file untouched", "[store][migrate]")
{
    TempDir dir;
    const std::string path = dir.path("fails to upgrade.raycad");
    makeV0(path);
    const std::string original = readFile(path);
    const auto expectUntouched = [&](ErrorCode code) {
        OpenInfo info{};
        auto rw = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
        REQUIRE_FALSE(rw);
        CHECK(rw.error().code == code);
        CHECK(readFile(path) == original);
        CHECK(filesIn(dir) == std::set<std::string>{filename(path)}); // no .bak, no working copy, no lock file
    };

    SECTION("no step is registered")
    {
        Steps steps;
        expectUntouched(ErrorCode::FormatError);
    }
    SECTION("the step reports an error after changing the copy")
    {
        Steps steps;
        ray::store::registerMigration({0, 1, [](MigrationCtx& ctx) -> Status {
                                           if (auto st = ctx.exec("DELETE FROM objects; DROP TABLE commits"); !st) {
                                               return st;
                                           }
                                           return tl::unexpected<ray::Error>(
                                               ray::Error{ErrorCode::KernelFailure, "step failed on purpose", {}});
                                       }});
        expectUntouched(ErrorCode::KernelFailure);
    }
    SECTION("the step throws")
    {
        Steps steps;
        ray::store::registerMigration(
            {0, 1, [](MigrationCtx&) -> Status { throw std::runtime_error("thrown on purpose"); }});
        expectUntouched(ErrorCode::Internal);
    }
    SECTION("the step runs bad SQL")
    {
        Steps steps;
        ray::store::registerMigration({0, 1, [](MigrationCtx& ctx) -> Status {
                                           return ctx.exec("UPDATE no_such_table SET x = ?1", {SqlValue(1.5)});
                                       }});
        expectUntouched(ErrorCode::IoError);
    }
    SECTION("the upgraded copy does not verify")
    {
        Steps steps;
        ray::store::registerMigration({0, 1, [](MigrationCtx&) -> Status { return {}; }}); // forgets the tables
        expectUntouched(ErrorCode::FormatError);
    }
}

TEST_CASE("a file with a newer format is refused and never modified, even with migrations registered",
          "[store][migrate][format]")
{
    Steps steps;
    ray::store::registerMigration(v0ToV1());
    TempDir dir;
    const std::string path = dir.path("from the future.raycad");
    makeV0(path);
    {
        RawDb raw(path);
        REQUIRE(raw.exec("PRAGMA user_version = 99") == SQLITE_OK);
    }
    const std::string bytes = readFile(path);
    for (const OpenMode mode : {OpenMode::ReadWrite, OpenMode::ReadOnly}) {
        auto file = IDocumentFile::open(path, mode);
        REQUIRE_FALSE(file);
        CHECK(file.error().code == ErrorCode::FormatTooNew);
        CHECK_THAT(file.error().message, Catch::Matchers::ContainsSubstring("99"));
        CHECK(readFile(path) == bytes);
        CHECK(filesIn(dir) == std::set<std::string>{filename(path)});
    }
}
