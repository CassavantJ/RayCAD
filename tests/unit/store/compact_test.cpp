// IDocumentFile::compact (PLAN.md 4.5, task T-012): keeps every ref, named version and reachable commit within the
// age policy, drops unreachable commits and objects, shrinks the file; a kill during compaction never leaves an
// unreadable file (20 kills in the normal run, 100 with the hidden tag [.compactkill100]).
#include "compact.h"
#include "file.h"
#include "file_util.h"
#include "raw_db.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <iostream>
#include <map>
#include <set>

using namespace storetest;
using ray::ErrorCode;
using ray::Hash256;
using ray::doc::DocState;
using ray::store::Commit;
using ray::store::IDocumentFile;
using ray::store::ObjectKind;
using ray::store::OpenMode;

namespace {

constexpr std::int64_t kDayMs = 86'400'000;

Commit save(IDocumentFile& file, const DocState& state, const std::string& message,
            std::vector<std::pair<ObjectKind, std::vector<std::byte>>> derived = {},
            std::map<Hash256, Hash256> index = {})
{
    IDocumentFile::SaveRequest request;
    request.state = std::make_shared<const DocState>(state);
    request.message = message;
    request.author = "compact test";
    request.derived = std::move(derived);
    request.derivedIndex = std::move(index);
    ray::JobContext ctx;
    auto commit = file.save(request, ctx);
    REQUIRE(commit);
    return *commit;
}

// An import feature whose props name `blob` as the BREP of its one body.
ray::doc::Feature importFeature(std::mt19937_64& rng, const Hash256& blob)
{
    ray::doc::Feature f;
    f.id = ray::FeatureId{ray::Uuid::fromString(randomUuid(rng))};
    f.type = "import";
    f.name = "Import part.step";
    f.propsJson = "{\"bodies\":[{\"blob\":\"" + blob.hex() + "\",\"color\":[1,1,1,1],\"id\":\"" + randomUuid(rng) +
                  "\",\"name\":\"Body\"}],\"fileHash\":\"" + randomHex(rng, 64) +
                  "\",\"format\":\"step\",\"sourceFile\":\"part.step\"}";
    return f;
}

void changeSome(std::mt19937_64& rng, DocState& state, std::size_t payload)
{
    for (int i = 0; i < 3; ++i) {
        auto& feature = state.features[rng() % state.features.size()];
        if (feature.type != "import") {
            feature.propsJson = "{\"payload\":\"" + randomHex(rng, payload) + "\"}";
        }
    }
}

std::vector<Hash256> objectHashes(const std::string& path)
{
    RawDb raw(path, SQLITE_OPEN_READONLY);
    std::vector<Hash256> out;
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.db, "SELECT hash FROM objects ORDER BY hash", -1, &stmt, nullptr) == SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Hash256 h;
        std::memcpy(h.bytes.data(), sqlite3_column_blob(stmt, 0), 32);
        out.push_back(h);
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace

TEST_CASE("compact keeps refs, named versions and reachable history and drops the rest", "[store][compact]")
{
    TempDir dir;
    const std::string path = dir.path("to compact ü.raycad");
    std::mt19937_64 rng(1212);
    auto created = IDocumentFile::create(path);
    REQUIRE(created);
    auto& file = *created;

    // main: c0 .. c9. c1 alone names brep0; c9 (and the tag's c3) name brep1.
    const auto brep0 = randomBytes(rng, 64 * 1024);
    const auto brep1 = randomBytes(rng, 64 * 1024);
    REQUIRE(file->put(ObjectKind::Brep, brep0));
    REQUIRE(file->put(ObjectKind::Brep, brep1));
    DocState state = makeState(rng, 6, 2000);
    std::vector<Commit> main;
    std::map<Hash256, std::string> stateOf; // commit -> state hash
    Hash256 onlyInC1;                       // a feature object only c1 has
    const Hash256 meshInput{};
    for (int i = 0; i < 10; ++i) {
        changeSome(rng, state, 2000);
        DocState saved = state;
        if (i == 1) {
            saved.features.push_back(importFeature(rng, hashOf(brep0)));
            onlyInC1 = ray::doc::serializeState(saved).features.back().first;
        }
        if (i == 3 || i == 9) {
            saved.features.push_back(importFeature(rng, hashOf(brep1)));
        }
        // Every save replaces the cached mesh of one input, orphaning the last one, and adds a thumbnail.
        auto mesh = randomBytes(rng, 32 * 1024);
        const Hash256 meshHash = hashOf(mesh);
        std::vector<std::pair<ObjectKind, std::vector<std::byte>>> derived;
        derived.emplace_back(ObjectKind::Mesh, std::move(mesh));
        derived.emplace_back(ObjectKind::Thumbnail, randomBytes(rng, 1024));
        main.push_back(save(*file, saved, "main " + std::to_string(i), std::move(derived), {{meshInput, meshHash}}));
        stateOf[main.back().id] = saved.stateHash().hex();
    }
    REQUIRE(file->setRef({"tags/Release \xC3\xBC", main[3].id}));
    // A side branch that is moved back afterwards: its two commits become unreachable.
    REQUIRE(file->setRef({"heads/side", main[5].id}));
    REQUIRE(file->loadHead("side"));
    DocState side = state;
    changeSome(rng, side, 50000);
    const Commit s1 = save(*file, side, "side 1");
    changeSome(rng, side, 50000);
    const Commit s2 = save(*file, side, "side 2");
    REQUIRE(file->setRef({"heads/side", main[5].id}));
    REQUIRE(file->loadHead("main"));
    // Objects nothing refers to.
    std::vector<Hash256> garbage;
    for (int i = 0; i < 20; ++i) {
        auto bytes = randomBytes(rng, 100 * 1024);
        auto put = file->put(ObjectKind::Attachment, bytes);
        REQUIRE(put);
        garbage.push_back(*put);
    }
    const auto sizeBefore = fileSize(path);

    // Within the age limit: only the unreachable side commits and the unreferenced objects go.
    ray::JobContext ctx;
    ray::store::detail::CompactStats stats;
    REQUIRE(ray::store::detail::compactAsOf(*file, 90, ray::store::detail::unixNowMs(), ctx, &stats));
    CHECK(stats.commitsDropped == 2);
    const std::int64_t objectsBefore = stats.objectsKept + stats.objectsDropped;
    CHECK(fileSize(path) < sizeBefore);
    for (const Hash256& g : garbage) {
        CHECK_FALSE(file->has(g));
    }
    CHECK(file->loadCommit(s1.id).error().code == ErrorCode::NotFound);
    CHECK(file->loadCommit(s2.id).error().code == ErrorCode::NotFound);
    auto log = file->log("main", 0);
    REQUIRE(log);
    REQUIRE(log->size() == 10);
    for (const Commit& c : *log) {
        auto loaded = file->loadCommit(c.id);
        REQUIRE(loaded);
        CHECK((*loaded)->stateHash().hex() == stateOf[c.id]);
    }
    CHECK(file->has(hashOf(brep0)));
    CHECK(file->has(hashOf(brep1)));
    auto cached = file->cachedDerived(meshInput);
    REQUIRE(cached);
    REQUIRE(*cached);
    CHECK(file->has(**cached)); // the derived index keeps its object; the meshes it replaced are gone
    CHECK(stats.objectsDropped >= static_cast<std::int64_t>(garbage.size() + 9 + 10 + 4));

    // 200 days later: unnamed microversions that no ref points at are pruned. Kept: main's head, the tag, side's head.
    REQUIRE(ray::store::detail::compactAsOf(*file, 90, ray::store::detail::unixNowMs() + 200 * kDayMs, ctx, &stats));
    CHECK(stats.commitsKept == 3);
    log = file->log("main", 0);
    REQUIRE(log);
    REQUIRE(log->size() == 1); // the history before the head was pruned
    CHECK(log->front().id == main[9].id);
    CHECK(file->loadCommit(main[8].id).error().code == ErrorCode::NotFound);
    for (const int kept : {3, 5, 9}) {
        auto loaded = file->loadCommit(main[static_cast<std::size_t>(kept)].id);
        REQUIRE(loaded);
        CHECK((*loaded)->stateHash().hex() == stateOf[main[static_cast<std::size_t>(kept)].id]);
    }
    auto refs = file->refs();
    REQUIRE(refs);
    CHECK(refs->size() == 3);
    CHECK_FALSE(file->has(hashOf(brep0))); // only the pruned c1 named it
    CHECK_FALSE(file->has(onlyInC1));
    CHECK(file->has(hashOf(brep1)));
    file.reset();
    CHECK(static_cast<std::int64_t>(objectHashes(path).size()) < objectsBefore);
    CHECK(ray::store::detail::verifyCopy(path));
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.text("SELECT count(*) FROM thumbs") == "3");
    CHECK(raw.text("SELECT count(*) FROM commits") == "3");
    CHECK(raw.text("PRAGMA integrity_check") == "ok");
}

TEST_CASE("compact refuses a read-only file and a negative age, and can be cancelled", "[store][compact]")
{
    TempDir dir;
    const std::string path = dir.path("refuses.raycad");
    std::mt19937_64 rng(1213);
    auto file = IDocumentFile::create(path);
    REQUIRE(file);
    save(**file, makeState(rng, 2, 10), "one");
    ray::JobContext ctx;
    CHECK((*file)->compact(-1, ctx).error().code == ErrorCode::InvalidArgument);
    auto readOnly = IDocumentFile::open(path, OpenMode::ReadOnly);
    REQUIRE(readOnly);
    CHECK((*readOnly)->compact(90, ctx).error().code == ErrorCode::Locked);
    ray::CancelSource cancel;
    cancel.cancel();
    ray::JobContext cancelled{cancel.token(), {}};
    CHECK((*file)->compact(90, cancelled).error().code == ErrorCode::Cancelled);
    CHECK((*file)->compact(0, ctx));
    CHECK((*file)->loadHead());
}

namespace {

struct Template {
    std::string path;
    std::string head;                        // head state hash
    std::map<std::string, std::string> refs; // name -> commit id
    std::size_t objects = 0;
};

Template buildTemplate(const TempDir& dir)
{
    Template t;
    t.path = dir.path("template.raycad");
    std::mt19937_64 rng(1214);
    auto file = IDocumentFile::create(t.path);
    REQUIRE(file);
    DocState state = makeState(rng, 8, 20000);
    Commit tagged;
    for (int i = 0; i < 25; ++i) {
        changeSome(rng, state, 200000);
        const Commit c = save(**file, state, "save " + std::to_string(i));
        if (i == 10) {
            tagged = c;
        }
        t.head = state.stateHash().hex();
    }
    REQUIRE((*file)->setRef({"tags/Kept", tagged.id}));
    for (int i = 0; i < 40; ++i) {
        REQUIRE((*file)->put(ObjectKind::Attachment, randomBytes(rng, 512 * 1024)));
    }
    auto refs = (*file)->refs();
    REQUIRE(refs);
    for (const auto& ref : *refs) {
        t.refs[ref.name] = ref.commit.hex();
    }
    file->reset();
    t.objects = objectHashes(t.path).size();
    return t;
}

void runCompactKillTest(int kills)
{
    useTestLockTiming();
    TempDir dir;
    const Template t = buildTemplate(dir);
    const std::string templateBytes = readFile(t.path);
    const std::string doc = dir.path("killed while compacting \xC3\xB8.raycad");
    const std::string acks = dir.path("compact acks.txt");

    // How long one compaction takes here, so the kills land inside it.
    writeFile(doc, templateBytes);
    std::int64_t compactMs = 0;
    {
        auto file = IDocumentFile::open(doc, OpenMode::ReadWrite);
        REQUIRE(file);
        ray::JobContext ctx;
        const auto t0 = nowMs();
        REQUIRE((*file)->compact(0, ctx));
        compactMs = nowMs() - t0;
    }
    removeDocument(doc);

    std::mt19937_64 rng(1215);
    int inside = 0, compacted = 0, untouched = 0, hot = 0;
    const auto t0 = nowMs();
    for (int k = 0; k < kills; ++k) {
        removeDocument(doc);
        writeFile(doc, templateBytes);
        removeFile(acks);
        const int days = rng() % 2 == 0 ? 0 : 90;
        {
            Process helper = Process::spawnHelper({"compact", doc, acks, std::to_string(days)});
            const auto deadline = nowMs() + 30000;
            while (readFile(acks).find('B') == std::string::npos && helper.running() && nowMs() < deadline) {
                sleepMs(1);
            }
            REQUIRE(readFile(acks).find('B') != std::string::npos);
            // Half of the kills anywhere in the first compaction, half late in it (the deletions and VACUUM come
            // after the reading).
            const auto span = static_cast<std::uint64_t>(compactMs * 13 / 10 + 2);
            const auto delay = k % 2 == 0 ? rng() % span : span / 2 + rng() % span;
            sleepMs(static_cast<int>(delay));
            INFO("kill " << k << ": the helper exited by itself");
            REQUIRE(helper.running());
            helper.kill();
        }
        const std::string log = readFile(acks);
        inside += log.find("A\n") == std::string::npos ? 1 : 0; // killed during the first compaction
        // A journal with a valid header and records: killed inside a write (the deletions or VACUUM), which the
        // next open rolls back.
        const std::string journal = readFile(doc + "-journal");
        static const unsigned char kMagic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
        if (journal.size() >= 12 && std::memcmp(journal.data(), kMagic, 8) == 0 &&
            (journal[8] | journal[9] | journal[10] | journal[11]) != 0) {
            ++hot;
        }

        INFO("kill " << k);
        REQUIRE(breakLockOfDeadWriter(doc));
        {
            auto file = IDocumentFile::open(doc, OpenMode::ReadWrite);
            INFO((file ? std::string() : file.error().message));
            REQUIRE(file);
            auto head = (*file)->loadHead();
            REQUIRE(head);
            CHECK((*head)->stateHash().hex() == t.head);
            auto refs = (*file)->refs();
            REQUIRE(refs);
            std::map<std::string, std::string> names;
            for (const auto& ref : *refs) {
                names[ref.name] = ref.commit.hex();
                auto state = (*file)->loadCommit(ref.commit);
                REQUIRE(state);
            }
            CHECK(names == t.refs);
            auto history = (*file)->log("main", 0);
            REQUIRE(history);
            REQUIRE_FALSE(history->empty());
        }
        // Looked at with plain SQLite only while the store has the file closed.
        {
            RawDb raw(doc, SQLITE_OPEN_READONLY);
            REQUIRE(raw.text("PRAGMA integrity_check") == "ok");
        }
        const std::vector<Hash256> objects = objectHashes(doc);
        {
            auto file = IDocumentFile::open(doc, OpenMode::ReadWrite);
            REQUIRE(file);
            for (const Hash256& h : objects) {
                REQUIRE((*file)->get(h)); // every object still decodes to its hash
            }
        }
        (objects.size() < t.objects ? compacted : untouched) += 1;
    }
    std::cout << "[store] compact kill test: " << kills << " kills, " << inside
              << " during the first compaction (one takes " << compactMs << " ms here, file "
              << templateBytes.size() / (1024 * 1024) << " MiB), " << hot
              << " left a hot journal (killed inside the deletions or VACUUM), " << compacted << " found compacted, "
              << untouched << " found as before, 0 unreadable files, " << (nowMs() - t0) / 1000.0 << " s\n";
}

} // namespace

TEST_CASE("killing compaction never leaves an unreadable file (20 kills)", "[store][compact][kill]")
{
    runCompactKillTest(20);
}

TEST_CASE("killing compaction never leaves an unreadable file (100 kills)", "[.compactkill100]")
{
    runCompactKillTest(100);
}
