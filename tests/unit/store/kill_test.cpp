// Kill during save (PLAN.md 4.2, S10): a writer process saves in a loop and is killed at a random moment; the file
// must always open, verify completely and hold either the last acknowledged save or the one that had begun.
// The normal run does 50 kills; the hidden tag [.kill500] does the 500 of the acceptance test.
#include "file.h"
#include "raw_db.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

using namespace storetest;
using ray::Hash256;
using ray::store::IDocumentFile;
using ray::store::OpenInfo;
using ray::store::OpenMode;

namespace {

int gNoFile = 0; // checks that found no document (the first create was killed)

struct Bounds {
    std::int64_t lo = -1; // the head may not be older than this ...
    std::int64_t hi = -1; // ... nor newer than this
};

// Folds one killed writer's log into the bounds: everything it acknowledged must survive, nothing it had not begun
// may appear.
void account(Bounds& bounds, const Acks& acks)
{
    if (acks.start == INT64_MIN) {
        return; // killed before it had opened the file
    }
    const std::int64_t top = std::max(acks.start, acks.acked == INT64_MIN ? -1 : acks.acked);
    const std::int64_t begun = acks.begun == INT64_MIN ? top : std::max(top, acks.begun);
    bounds.lo = std::max(bounds.lo, top);
    bounds.hi = std::max(bounds.hi, begun);
}

// "hot" if the journal has a valid header with records, as SQLite needs to roll back.
bool hotJournal(const std::string& doc)
{
    const std::string journal = readFile(doc + "-journal");
    static const unsigned char kMagic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    if (journal.size() < 12 || std::memcmp(journal.data(), kMagic, 8) != 0) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(journal.data());
    return (p[8] | p[9] | p[10] | p[11]) != 0; // the record count, big-endian
}

// Opens, walks and re-hashes everything; returns the head save number (-1 for no document or no commit).
std::int64_t checkDocument(const std::string& doc)
{
    if (!exists(doc)) {
        // Killed before the first create finished: no file, which is fine, and at most a stale lock and a
        // temporary ".creating-" file beside it. The lock must be breakable.
        auto broken = ray::store::breakStaleLock(doc);
        for (const auto end = nowMs() + 5000; !broken && nowMs() < end;) {
            sleepMs(50);
            broken = ray::store::breakStaleLock(doc);
        }
        REQUIRE(broken);
        REQUIRE_FALSE(exists(doc + ".lock"));
        ++gNoFile;
        return -1;
    }
    // The writer is dead. A lock file it died writing has no readable content; on POSIX that is only judged stale
    // after a short grace period.
    auto broken = ray::store::breakStaleLock(doc);
    for (const auto end = nowMs() + 5000; !broken && nowMs() < end;) {
        sleepMs(50);
        broken = ray::store::breakStaleLock(doc);
    }
    INFO((broken ? std::string() : broken.error().message));
    REQUIRE(broken);
    std::set<Hash256> reachable;
    std::int64_t head = -1;
    {
        OpenInfo info{};
        auto file = IDocumentFile::open(doc, OpenMode::ReadWrite, &info);
        INFO((file ? std::string() : file.error().message));
        REQUIRE(file);
        REQUIRE_FALSE(info.openedReadOnlyBecauseLocked);
        auto log = (*file)->log("main", 0);
        REQUIRE(log);
        for (std::size_t i = 0; i < log->size(); ++i) {
            const auto& commit = (*log)[i];
            const std::int64_t n = saveNumber(commit.message);
            if (i == 0) {
                head = n;
            }
            REQUIRE(n == head - static_cast<std::int64_t>(i));
            auto state = (*file)->loadCommit(commit.id);
            REQUIRE(state);
            REQUIRE((*state)->stateHash() == commit.root);
            reachable.insert(commit.root);
            for (const auto& [hash, bytes] : ray::doc::serializeState(**state).features) {
                reachable.insert(hash);
            }
        }
        if (!log->empty()) {
            REQUIRE(log->back().parents.empty());
            REQUIRE(saveNumber(log->back().message) == 0);
            auto loaded = (*file)->loadHead();
            REQUIRE(loaded);
            REQUIRE((*loaded)->stateHash() == log->front().root);
        }
    }
    std::vector<Hash256> stored;
    {
        RawDb raw(doc, SQLITE_OPEN_READONLY);
        REQUIRE(raw.text("PRAGMA integrity_check") == "ok");
        const auto readHashes = [&raw](const char* sql) {
            std::vector<Hash256> out;
            sqlite3_stmt* stmt = nullptr;
            REQUIRE(sqlite3_prepare_v2(raw.db, sql, -1, &stmt, nullptr) == SQLITE_OK);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                REQUIRE(sqlite3_column_bytes(stmt, 0) == 32);
                Hash256 h;
                std::memcpy(h.bytes.data(), sqlite3_column_blob(stmt, 0), 32);
                out.push_back(h);
            }
            sqlite3_finalize(stmt);
            return out;
        };
        stored = readHashes("SELECT hash FROM objects");
        for (const Hash256& h : readHashes("SELECT object_hash FROM derived")) {
            reachable.insert(h); // a derived index entry keeps its object
        }
    }
    // No orphan objects (a partly applied save would leave some), and every object decodes to its hash.
    REQUIRE(std::set<Hash256>(stored.begin(), stored.end()) == reachable);
    auto file = IDocumentFile::open(doc, OpenMode::ReadWrite);
    REQUIRE(file);
    for (const Hash256& h : stored) {
        REQUIRE((*file)->get(h));
    }
    return head;
}

void runKillTest(int iterations)
{
    useTestLockTiming();
    TempDir dir;
    const std::string doc = dir.path("killed while saving \xC3\xB8.raycad");
    const std::string acks = dir.path("acks.txt");
    std::mt19937_64 rng(20261118);
    Bounds bounds;
    int hot = 0;
    int extraKills = 0;
    int withSaves = 0;
    int inSave = 0;
    std::int64_t savesAcked = 0;
    const auto t0 = nowMs();
    gNoFile = 0;
    for (int i = 0; i < iterations; ++i) {
        if (fileSize(doc) > 24u * 1000 * 1000) {
            removeDocument(doc);
            bounds = Bounds{};
        }
        removeFile(acks);
        {
            Process writer = Process::spawnHelper({"writer", doc, acks, "0", "10", std::to_string(rng())});
            // Four kills in five are timed from the moment the writer has opened the file, so they land among its
            // saves; the rest from process start, so some land in start-up, lock breaking and recovery.
            if (rng() % 5 != 0) {
                const auto end = nowMs() + 10000;
                while (readAcks(acks).start == INT64_MIN && writer.running() && nowMs() < end) {
                    sleepMs(1);
                }
                INFO("iteration " << i << ": the writer did not manage to open the file");
                REQUIRE(readAcks(acks).start != INT64_MIN);
            }
            sleepMs(static_cast<int>(rng() % 201));
            // A writer that has exited by itself hit an error: that is a failure, not a kill.
            INFO("iteration " << i << ": the writer exited by itself");
            REQUIRE(writer.running());
            writer.kill();
        }
        const Acks a = readAcks(acks);
        account(bounds, a);
        if (a.start != INT64_MIN && a.acked != INT64_MIN && a.acked > a.start) {
            savesAcked += a.acked - a.start;
            ++withSaves;
        }
        if (a.begun != INT64_MIN && (a.acked == INT64_MIN || a.begun > a.acked)) {
            ++inSave; // killed between "began save n" and "save n returned"
        }
        hot += hotJournal(doc) ? 1 : 0;
        if (rng() % 5 == 0) {
            // A second writer killed very early: during lock breaking or hot-journal rollback.
            removeFile(acks);
            Process early = Process::spawnHelper({"writer", doc, acks, "0", "10", std::to_string(rng())});
            sleepMs(static_cast<int>(rng() % 26));
            early.kill();
            account(bounds, readAcks(acks));
            ++extraKills;
        }
        INFO("iteration " << i << ", bounds [" << bounds.lo << ", " << bounds.hi << "]");
        const std::int64_t head = checkDocument(doc);
        REQUIRE(head >= bounds.lo);
        REQUIRE(head <= bounds.hi);
        bounds.lo = bounds.hi = head;
    }
    std::cout << "[store] kill test: " << iterations << " kills (+" << extraKills << " early kills), " << inSave
              << " inside a save, " << hot << " left a hot journal, " << withSaves << " writers saved " << savesAcked
              << " times, " << gNoFile << " checks found no file yet (first create killed), 0 unreadable files, "
              << (nowMs() - t0) / 1000.0 << " s\n";
}

} // namespace

TEST_CASE("killing the writer during saves never leaves an unreadable file (50 kills)", "[store][kill]")
{
    runKillTest(50);
}

TEST_CASE("killing the writer during saves never leaves an unreadable file (500 kills)", "[.kill500]")
{
    runKillTest(500);
}
