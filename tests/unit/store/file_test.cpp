// The .raycad container: schema, settings, objects, commits, refs, save transaction, format version (T-011).
#include "file.h"
#include "lock.h"
#include "raw_db.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>

using namespace storetest;
using ray::ErrorCode;
using ray::Hash256;
using ray::store::Commit;
using ray::store::IDocumentFile;
using ray::store::ObjectKind;
using ray::store::OpenInfo;
using ray::store::OpenMode;
using ray::store::Ref;

namespace {

std::string squeeze(const std::string& sql)
{
    std::string out;
    bool space = false;
    for (const char c : sql) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out.push_back(' ');
        }
        space = false;
        out.push_back(c);
    }
    return out;
}

IDocumentFile::SaveRequest request(const ray::doc::DocState& state, std::string message)
{
    IDocumentFile::SaveRequest r;
    r.state = std::make_shared<const ray::doc::DocState>(state);
    r.message = std::move(message);
    r.author = "test author";
    return r;
}

std::unique_ptr<IDocumentFile> createFile(const std::string& path)
{
    auto file = IDocumentFile::create(path);
    REQUIRE(file);
    return std::move(*file);
}

} // namespace

TEST_CASE("the schema is PLAN.md section 4.1, with objects and thumbs as rowid tables", "[store][schema]")
{
    TempDir dir;
    const std::string path = dir.path("Bracket \xC3\xA9 v1.raycad");
    createFile(path).reset();

    // The text of 4.1, comments removed, except that `objects` and `thumbs` are ordinary rowid tables: SQLite reads
    // the whole of a large row each time a key lookup compares against it in a WITHOUT ROWID table, which made
    // loadHead on a 500 MB file take 0.8 to 1.9 s instead of 5 ms (see the T-011 report).
    const std::vector<std::string> expected = {
        "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        "CREATE TABLE objects (hash BLOB PRIMARY KEY, kind INTEGER NOT NULL, raw_size INTEGER NOT NULL, data BLOB NOT "
        "NULL)",
        "CREATE TABLE commits (id BLOB PRIMARY KEY, root BLOB NOT NULL, author TEXT, time_ms INTEGER NOT NULL, message "
        "TEXT, named INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID",
        "CREATE TABLE commit_parents (commit_id BLOB NOT NULL, ord INTEGER NOT NULL, parent BLOB NOT NULL, PRIMARY KEY "
        "(commit_id, ord)) WITHOUT ROWID",
        "CREATE TABLE refs (name TEXT PRIMARY KEY, commit_id BLOB NOT NULL)",
        "CREATE TABLE derived (input_hash BLOB PRIMARY KEY, object_hash BLOB NOT NULL, kernel_version TEXT NOT NULL) "
        "WITHOUT ROWID",
        "CREATE TABLE thumbs (commit_id BLOB PRIMARY KEY, png BLOB NOT NULL)",
    };
    RawDb raw(path);
    const auto tables = raw.rows("SELECT name, sql FROM sqlite_master WHERE type = 'table' ORDER BY rowid");
    REQUIRE(tables.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(squeeze(tables[i][1]) == expected[i]);
    }
    // Only the automatic indexes of the primary keys exist besides the tables.
    CHECK(raw.text("SELECT count(*) FROM sqlite_master WHERE type NOT IN ('table', 'index')") == "0");
    CHECK(raw.text("SELECT count(*) FROM sqlite_master WHERE type = 'index' AND sql IS NOT NULL") == "0");
    CHECK(raw.text("PRAGMA application_id") == std::to_string(0x52415943));
    CHECK(raw.text("PRAGMA user_version") == "1");
    CHECK(raw.text("PRAGMA journal_mode") == "delete");
    CHECK(raw.text("SELECT count(*) FROM meta WHERE key IN ('file_id', 'created_by')") == "2");
    CHECK(raw.text("SELECT length(value) FROM meta WHERE key = 'file_id'") == "36");
    // The private schema list is the same text.
    REQUIRE(ray::store::detail::schemaStatements().size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(squeeze(ray::store::detail::schemaStatements()[i]) == expected[i]);
    }
}

TEST_CASE("a writer connection uses the section 4.2 settings and holds the write lock from open", "[store][lock]")
{
    TempDir dir;
    const std::string path = dir.path("settings.raycad");
    createFile(path).reset();

    OpenInfo info{};
    auto file = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
    REQUIRE(file);
    CHECK_FALSE(info.openedReadOnlyBecauseLocked);
    CHECK(info.formatVersion == 1);
    CHECK_FALSE(info.lock);
    CHECK(ray::store::detail::connectionSettings(**file) == "journal_mode=delete synchronous=2 locking_mode=exclusive");
    CHECK(ray::store::detail::snapshotPath(**file).empty());
    CHECK(exists(path + ".lock"));

    // No save has happened yet, and still nobody else can read or write the database: the exclusive lock was
    // taken at open (S10 found it is otherwise only taken at the first write).
    {
        RawDb raw(path);
        sqlite3_busy_timeout(raw.db, 0);
        CHECK(raw.exec("SELECT count(*) FROM refs") == SQLITE_BUSY);
        CHECK(raw.exec("BEGIN IMMEDIATE") == SQLITE_BUSY);
    }
    file->reset();
    CHECK_FALSE(exists(path + ".lock"));
    CHECK_FALSE(exists(path + "-journal")); // deleted on a clean close
    RawDb raw(path);
    CHECK(raw.exec("SELECT count(*) FROM refs") == SQLITE_OK);
}

TEST_CASE("a saved state reloads with the same stateHash", "[store][commit]")
{
    TempDir dir;
    const std::string path = dir.path("round trip.raycad");
    std::mt19937_64 rng(1);
    ray::doc::DocState state = makeState(rng, 12, 300);
    state.features[3].suppressed = true;
    state.features[5].name = "Hole \"M6\" \xC3\xA9 \xE6\xB8\xAC";
    ray::JobContext ctx;
    Commit first;
    {
        auto file = createFile(path);
        CHECK(file->loadHead().error().code == ErrorCode::NotFound);
        auto saved = file->save(request(state, "first"), ctx);
        REQUIRE(saved);
        first = *saved;
        CHECK(saved->root == state.stateHash());
        CHECK(saved->parents.empty());
        CHECK(saved->author == "test author");
        CHECK(file->has(saved->root));
        auto head = file->loadHead();
        REQUIRE(head);
        CHECK((*head)->stateHash() == state.stateHash());
    }
    auto file = IDocumentFile::open(path, OpenMode::ReadWrite);
    REQUIRE(file);
    auto head = (*file)->loadHead("main");
    REQUIRE(head);
    CHECK((*head)->stateHash() == state.stateHash());
    REQUIRE((*head)->features.size() == state.features.size());
    CHECK((*head)->features[5].name == state.features[5].name);
    CHECK((*head)->features[3].suppressed);

    // A second save is a child of the first; the unchanged features are not stored again.
    state.features[0].propsJson = "{\"distance\":\"3 mm\"}";
    auto second = (*file)->save(request(state, "second"), ctx);
    REQUIRE(second);
    REQUIRE(second->parents.size() == 1);
    CHECK(second->parents[0] == first.id);
    auto log = (*file)->log("main", 0);
    REQUIRE(log);
    REQUIRE(log->size() == 2);
    CHECK((*log)[0].id == second->id);
    CHECK((*log)[0].message == "second");
    CHECK((*log)[1].id == first.id);
    auto old = (*file)->loadCommit(first.id);
    REQUIRE(old);
    CHECK((*old)->stateHash() == first.root);
    CHECK((*file)->loadCommit(Hash256{}).error().code == ErrorCode::NotFound);
    file->reset();

    RawDb raw(path);
    // 1 manifest + 12 features, then 1 manifest + 1 changed feature.
    CHECK(raw.text("SELECT count(*) FROM objects") == "15");
    CHECK(raw.text("SELECT count(*) FROM commits") == "2");
    CHECK(raw.text("SELECT value FROM meta WHERE key = 'units'") == "metric");
}

TEST_CASE("save is one transaction and a failed save leaves nothing behind", "[store][commit]")
{
    TempDir dir;
    const std::string path = dir.path("transaction.raycad");
    std::mt19937_64 rng(2);
    const ray::doc::DocState state = makeState(rng, 20, 200);
    ray::JobContext ctx;
    auto file = createFile(path);

    auto req = request(state, "with derived data");
    auto mesh = randomBytes(rng, 5000);
    const Hash256 input = hashOf(randomBytes(rng, 16));
    req.derivedIndex[input] = hashOf(mesh);
    req.derived.emplace_back(ObjectKind::Mesh, mesh);
    req.derived.emplace_back(ObjectKind::Thumbnail, randomBytes(rng, 300));
    const auto before = ray::store::detail::committedTransactions(*file);
    auto saved = file->save(req, ctx);
    REQUIRE(saved);
    // 20 features, a manifest, two derived objects, a derived row, the commit, the ref and the units: one COMMIT.
    CHECK(ray::store::detail::committedTransactions(*file) - before == 1);
    auto cached = file->cachedDerived(input);
    REQUIRE(cached);
    REQUIRE(cached->has_value());
    CHECK(**cached == hashOf(mesh));
    auto back = file->get(**cached);
    REQUIRE(back);
    CHECK(*back == mesh);

    // A save whose derived index points at an object that is not stored fails after its objects were written:
    // all of it is rolled back.
    ray::doc::DocState changed = state;
    changed.features[7].propsJson = "{\"distance\":\"9 mm\"}";
    auto bad = request(changed, "bad");
    bad.derived.emplace_back(ObjectKind::Mesh, randomBytes(rng, 4000));
    bad.derivedIndex[hashOf(randomBytes(rng, 16))] = hashOf(randomBytes(rng, 16));
    const auto beforeBad = ray::store::detail::committedTransactions(*file);
    auto failed = file->save(bad, ctx);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::InvalidArgument);
    CHECK(ray::store::detail::committedTransactions(*file) == beforeBad);
    CHECK_FALSE(file->has(changed.stateHash()));
    auto head = file->loadHead();
    REQUIRE(head);
    CHECK((*head)->stateHash() == state.stateHash());

    // A cancelled save writes nothing either.
    ray::CancelSource cancel;
    cancel.cancel();
    ray::JobContext cancelled{cancel.token(), {}};
    auto stopped = file->save(request(changed, "cancelled"), cancelled);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::Cancelled);
    file.reset();

    RawDb raw(path);
    CHECK(raw.text("SELECT count(*) FROM commits") == "1");
    CHECK(raw.text("SELECT count(*) FROM objects") == "23");
    CHECK(raw.text("SELECT count(*) FROM thumbs") == "1");
    CHECK(raw.text("SELECT count(*) FROM derived") == "1");
}

TEST_CASE("objects are content-addressed, compressed and verified on read", "[store][objects]")
{
    TempDir dir;
    const std::string path = dir.path("objects.raycad");
    std::mt19937_64 rng(3);
    auto file = createFile(path);
    const std::string text = randomHex(rng, 100000);
    const auto bytes = std::as_bytes(std::span<const char>(text.data(), text.size()));
    auto hash = file->put(ObjectKind::Json, bytes);
    REQUIRE(hash);
    CHECK(*hash == hashOf(bytes));
    auto again = file->put(ObjectKind::Json, bytes);
    REQUIRE(again);
    CHECK(*again == *hash);
    auto empty = file->put(ObjectKind::Attachment, {});
    REQUIRE(empty);
    auto emptyBack = file->get(*empty);
    REQUIRE(emptyBack);
    CHECK(emptyBack->empty());
    const auto random = randomBytes(rng, 300000);
    auto randomHash = file->put(ObjectKind::Brep, random);
    REQUIRE(randomHash);
    CHECK_FALSE(file->has(Hash256{}));
    CHECK(file->get(Hash256{}).error().code == ErrorCode::NotFound);
    file.reset();

    {
        RawDb raw(path);
        CHECK(raw.text("SELECT count(*) FROM objects") == "3");
        // Hex text compresses about 2:1; raw_size is the uncompressed length.
        CHECK(raw.text("SELECT raw_size FROM objects WHERE kind = 1") == "100000");
        CHECK(std::stoll(raw.text("SELECT length(data) FROM objects WHERE kind = 1")) < 60000);
        // Flip one byte in the middle of the stored (compressed) text object.
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw.db, "SELECT data FROM objects WHERE kind = 1", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        std::string data(static_cast<const char*>(sqlite3_column_blob(stmt, 0)),
                         static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
        sqlite3_finalize(stmt);
        data[data.size() / 2] = static_cast<char>(data[data.size() / 2] ^ 0x01);
        REQUIRE(sqlite3_prepare_v2(raw.db, "UPDATE objects SET data = ?1 WHERE kind = 1", -1, &stmt, nullptr) ==
                SQLITE_OK);
        sqlite3_bind_blob(stmt, 1, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    // Flip one byte of the random object directly in the file's bytes, outside SQLite.
    std::string fileBytes = readFile(path);
    const std::string needle(reinterpret_cast<const char*>(random.data()) + 150000, 32);
    const std::size_t at = fileBytes.find(needle);
    REQUIRE(at != std::string::npos);
    fileBytes[at + 7] = static_cast<char>(fileBytes[at + 7] ^ 0x40);
    writeFile(path, fileBytes);

    auto reopened = IDocumentFile::open(path, OpenMode::ReadWrite);
    REQUIRE(reopened);
    auto textBack = (*reopened)->get(*hash);
    REQUIRE_FALSE(textBack);
    CHECK(textBack.error().code == ErrorCode::FormatError);
    auto randomBack = (*reopened)->get(*randomHash);
    REQUIRE_FALSE(randomBack);
    CHECK(randomBack.error().code == ErrorCode::FormatError);
    CHECK((*reopened)->get(*empty));
}

TEST_CASE("refs, branches and named versions", "[store][commit]")
{
    TempDir dir;
    const std::string path = dir.path("refs.raycad");
    std::mt19937_64 rng(4);
    ray::doc::DocState state = makeState(rng, 3, 50);
    ray::JobContext ctx;
    auto file = createFile(path);
    auto c1 = file->save(request(state, "one"), ctx);
    REQUIRE(c1);
    state.features[0].name = "changed";
    auto c2 = file->save(request(state, "two"), ctx);
    REQUIRE(c2);

    REQUIRE(file->setRef({"tags/Release A", c1->id}));
    REQUIRE(file->setRef({"heads/experiment", c1->id}));
    CHECK(file->setRef({"tags/", c1->id}).error().code == ErrorCode::InvalidArgument);
    CHECK(file->setRef({"main", c1->id}).error().code == ErrorCode::InvalidArgument);
    CHECK(file->setRef({"tags/x", Hash256{}}).error().code == ErrorCode::NotFound);
    auto refs = file->refs();
    REQUIRE(refs);
    REQUIRE(refs->size() == 3);
    CHECK((*refs)[0].name == "heads/experiment");
    CHECK((*refs)[1].name == "heads/main");
    CHECK((*refs)[1].commit == c2->id);
    CHECK((*refs)[2].name == "tags/Release A");
    auto log = file->log("main", 0);
    REQUIRE(log);
    REQUIRE(log->size() == 2);
    CHECK((*log)[1].named);
    CHECK_FALSE((*log)[0].named);
    auto one = file->log("main", 1);
    REQUIRE(one);
    CHECK(one->size() == 1);

    // Saving after loading a branch head extends that branch.
    auto exp = file->loadHead("experiment");
    REQUIRE(exp);
    CHECK((*exp)->stateHash() == c1->root);
    state.features[1].name = "on the branch";
    auto c3 = file->save(request(state, "three"), ctx);
    REQUIRE(c3);
    REQUIRE(c3->parents.size() == 1);
    CHECK(c3->parents[0] == c1->id);
    auto branchLog = file->log("experiment", 0);
    REQUIRE(branchLog);
    CHECK(branchLog->size() == 2);
    CHECK(file->loadHead("missing").error().code == ErrorCode::NotFound);

    // Moving the tag away clears the named flag of its old commit.
    REQUIRE(file->setRef({"tags/Release A", c2->id}));
    auto after = file->log("main", 0);
    REQUIRE(after);
    CHECK((*after)[0].named);
    CHECK_FALSE((*after)[1].named);
    CHECK(file->compact(90, ctx)); // T-012: nothing to drop here, every commit is recent
}

TEST_CASE("a file with a newer format version is refused and left unchanged", "[store][format]")
{
    TempDir dir;
    const std::string path = dir.path("future.raycad");
    createFile(path).reset();
    {
        RawDb raw(path);
        REQUIRE(raw.exec("PRAGMA user_version = 99") == SQLITE_OK);
    }
    const std::string bytes = readFile(path);
    OpenInfo info{};
    auto rw = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
    REQUIRE_FALSE(rw);
    CHECK(rw.error().code == ErrorCode::FormatTooNew);
    CHECK_THAT(rw.error().message, Catch::Matchers::ContainsSubstring("99"));
    auto ro = IDocumentFile::open(path, OpenMode::ReadOnly);
    REQUIRE_FALSE(ro);
    CHECK(ro.error().code == ErrorCode::FormatTooNew);
    CHECK(readFile(path) == bytes);
    CHECK_FALSE(exists(path + ".lock"));
    CHECK_FALSE(exists(path + "-journal"));
}

TEST_CASE("create refuses an existing file and open refuses a missing or foreign one", "[store][format]")
{
    TempDir dir;
    const std::string path = dir.path("exists.raycad");
    writeFile(path, "not a database");
    const auto created = IDocumentFile::create(path);
    REQUIRE_FALSE(created);
    CHECK(created.error().code == ErrorCode::InvalidArgument);
    CHECK(readFile(path) == "not a database");
    CHECK(IDocumentFile::open(dir.path("missing.raycad"), OpenMode::ReadWrite).error().code == ErrorCode::NotFound);
    auto foreign = IDocumentFile::open(path, OpenMode::ReadWrite);
    REQUIRE_FALSE(foreign);
    CHECK(foreign.error().code == ErrorCode::FormatError);
    CHECK_FALSE(exists(path + ".lock"));
    {
        RawDb raw(dir.path("plain sqlite.raycad"), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        REQUIRE(raw.exec("CREATE TABLE t (x)") == SQLITE_OK);
    }
    auto plain = IDocumentFile::open(dir.path("plain sqlite.raycad"), OpenMode::ReadWrite);
    REQUIRE_FALSE(plain);
    CHECK(plain.error().code == ErrorCode::FormatError);
}

TEST_CASE("a second opener in the same process gets the lock owner and a read-only verified copy", "[store][lock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string path = dir.path("Shared \xE6\xB8\xAC folder doc.raycad");
    std::mt19937_64 rng(5);
    const ray::doc::DocState state = makeState(rng, 5, 100);
    ray::JobContext ctx;
    const auto t0 = std::chrono::system_clock::now();
    auto writer = createFile(path);
    REQUIRE(writer->save(request(state, "shared"), ctx));

    OpenInfo info{};
    auto reader = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
    REQUIRE(reader);
    CHECK(info.openedReadOnlyBecauseLocked);
    REQUIRE(info.lock);
    CHECK(info.lock->user == ray::store::detail::currentUserName());
    CHECK(info.lock->host == ray::store::detail::currentHostName());
    CHECK(info.lock->pid == ray::store::detail::currentProcessId());
    const auto since = std::chrono::milliseconds(info.lock->sinceUnixMs);
    CHECK(since >=
          std::chrono::duration_cast<std::chrono::milliseconds>(t0.time_since_epoch()) - std::chrono::seconds(2));
    CHECK(info.lock->heartbeatAgeSec < 1.0);
    CHECK_FALSE(info.lock->stale);
    CHECK_FALSE(info.lock->ownerProvablyDead);
    const std::string copy = ray::store::detail::snapshotPath(**reader);
    CHECK_FALSE(copy.empty());
    CHECK(exists(copy));
    auto head = (*reader)->loadHead();
    REQUIRE(head);
    CHECK((*head)->stateHash() == state.stateHash());
    CHECK((*reader)->save(request(state, "not allowed"), ctx).error().code == ErrorCode::Locked);
    CHECK((*reader)->put(ObjectKind::Json, {}).error().code == ErrorCode::Locked);
    CHECK((*reader)->setRef({"tags/x", Hash256{}}).error().code == ErrorCode::Locked);
    CHECK(ray::store::breakStaleLock(path).error().code == ErrorCode::Locked);

    // An explicit read-only open reports the holder but not "because locked".
    OpenInfo roInfo{};
    auto ro = IDocumentFile::open(path, OpenMode::ReadOnly, &roInfo);
    REQUIRE(ro);
    CHECK_FALSE(roInfo.openedReadOnlyBecauseLocked);
    CHECK(roInfo.lock);

    // The writer is unaffected.
    REQUIRE(writer->save(request(makeState(rng, 2, 10), "after"), ctx));
    reader->reset();
    ro->reset();
    CHECK_FALSE(exists(copy));
    writer.reset();
    CHECK_FALSE(exists(path + ".lock"));
    // With nobody holding it, read-only still opens a verified copy.
    OpenInfo freeInfo{};
    auto free = IDocumentFile::open(path, OpenMode::ReadOnly, &freeInfo);
    REQUIRE(free);
    CHECK_FALSE(freeInfo.lock);
    CHECK_FALSE(ray::store::detail::snapshotPath(**free).empty());
    CHECK((*free)->loadHead());
}

TEST_CASE("verifyCopy rejects a damaged copy", "[store][snapshot]")
{
    TempDir dir;
    const std::string path = dir.path("damaged.raycad");
    std::mt19937_64 rng(6);
    ray::JobContext ctx;
    {
        auto file = createFile(path);
        for (int i = 0; i < 5; ++i) {
            REQUIRE(file->save(request(makeState(rng, 30, 2000), "s" + std::to_string(i)), ctx));
        }
    }
    REQUIRE(ray::store::detail::verifyCopy(path));
    const std::string original = readFile(path);
    const std::string bad = dir.path("damaged copy.raycad");

    // A copy cut short, as a copy racing a growing file would be: SQLite's integrity check fails.
    writeFile(bad, original.substr(0, original.size() / 2));
    auto truncated = ray::store::detail::verifyCopy(bad);
    REQUIRE_FALSE(truncated);
    CHECK(truncated.error().code == ErrorCode::FormatError);

    // One flipped byte inside the head's manifest object: SQLite sees nothing wrong, the hash check does.
    std::string manifest;
    {
        RawDb raw(path, SQLITE_OPEN_READONLY);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw.db,
                                   "SELECT o.data FROM refs r JOIN commits c ON c.id = r.commit_id "
                                   "JOIN objects o ON o.hash = c.root WHERE r.name = 'heads/main'",
                                   -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        manifest.assign(static_cast<const char*>(sqlite3_column_blob(stmt, 0)),
                        static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    REQUIRE(manifest.size() > 64);
    const std::size_t at = original.find(manifest.substr(manifest.size() / 2 - 16, 32));
    REQUIRE(at != std::string::npos);
    std::string flipped = original;
    flipped[at + 5] = static_cast<char>(flipped[at + 5] ^ 0x10);
    writeFile(bad, flipped);
    auto verdict = ray::store::detail::verifyCopy(bad);
    REQUIRE_FALSE(verdict);
    CHECK(verdict.error().code == ErrorCode::FormatError);
}

TEST_CASE("loadHead on a 20 MB file is fast", "[store][bench]")
{
    TempDir dir;
    const std::string path = dir.path("twenty.raycad");
    std::mt19937_64 rng(7);
    ray::JobContext ctx;
    const ray::doc::DocState state = makeState(rng, 50, 400);
    {
        auto file = createFile(path);
        auto req = request(state, "big");
        for (int i = 0; i < 20; ++i) {
            auto blob = randomBytes(rng, 1u << 20);
            req.derivedIndex[hashOf(randomBytes(rng, 8))] = hashOf(blob);
            req.derived.emplace_back(ObjectKind::Brep, std::move(blob));
        }
        REQUIRE(file->save(req, ctx));
    }
    CHECK(fileSize(path) > 20u * 1000 * 1000);
    const auto t0 = nowMs();
    auto file = IDocumentFile::open(path, OpenMode::ReadWrite);
    REQUIRE(file);
    const auto t1 = nowMs();
    auto head = (*file)->loadHead();
    const auto t2 = nowMs();
    REQUIRE(head);
    CHECK((*head)->stateHash() == state.stateHash());
    std::cout << "[store] 20 MB file: open " << (t1 - t0) << " ms, loadHead " << (t2 - t1) << " ms\n";
    CHECK(t2 - t1 < 200);
}
