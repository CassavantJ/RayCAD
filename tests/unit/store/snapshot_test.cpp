// Read-only access while another process saves (PLAN.md 4.3, S10): a counter-guarded copy that is verified before
// it is opened. A plain copy taken during a save is usually corrupt; the guarded one must never open bad.
#include "file.h"
#include "raw_db.h"
#include "store_test_util.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <iostream>

using namespace storetest;
using ray::ErrorCode;
using ray::store::IDocumentFile;
using ray::store::OpenInfo;
using ray::store::OpenMode;

namespace {

void waitForSaves(const std::string& acks, Process& writer, std::int64_t count)
{
    const auto end = nowMs() + 30000;
    for (;;) {
        const Acks a = readAcks(acks);
        if (a.acked != INT64_MIN && a.start != INT64_MIN && a.acked - a.start >= count) {
            return;
        }
        REQUIRE(nowMs() < end);
        REQUIRE(writer.running());
        sleepMs(10);
    }
}

struct Tally {
    int good = 0;
    int busy = 0;
};

// One read-only open during saving. A success must be a fully good copy with a head the writer really saved.
void openOnce(const std::string& doc, const std::string& acks, Tally& tally)
{
    const Acks before = readAcks(acks);
    OpenInfo info{};
    auto file = IDocumentFile::open(doc, OpenMode::ReadOnly, &info);
    const Acks after = readAcks(acks);
    if (!file) {
        INFO(file.error().message);
        REQUIRE(file.error().code == ErrorCode::Locked); // no idle moment in time: refused, never a bad copy
        ++tally.busy;
        return;
    }
    REQUIRE(info.lock);
    CHECK_FALSE(info.lock->stale);
    const std::string copy = ray::store::detail::snapshotPath(**file);
    REQUIRE_FALSE(copy.empty());
    {
        RawDb raw(copy, SQLITE_OPEN_READONLY);
        REQUIRE(raw.text("PRAGMA integrity_check") == "ok");
    }
    auto log = (*file)->log("main", 0);
    REQUIRE(log);
    REQUIRE_FALSE(log->empty());
    const std::int64_t head = saveNumber(log->front().message);
    CHECK(head >= before.acked);
    CHECK(head <= after.begun);
    auto state = (*file)->loadHead();
    REQUIRE(state);
    REQUIRE((*state)->stateHash() == log->front().root);
    for (std::size_t i = 0; i < log->size() && i < 5; ++i) {
        auto old = (*file)->loadCommit((*log)[i].id);
        REQUIRE(old);
        REQUIRE((*old)->stateHash() == (*log)[i].root);
    }
    ++tally.good;
}

} // namespace

TEST_CASE("read-only openers copy while another process saves and never open a bad copy",
          "[store][snapshot][wallclock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string doc = dir.path("being saved \xC3\xA5.raycad");
    const std::string acks = dir.path("acks.txt");

    // A writer that saves every 40 ms (far more often than a person does).
    ray::store::detail::setSnapshotPatienceMs(10'000);
    Tally paused;
    {
        Process writer = Process::spawnHelper({"writer", doc, acks, "40", "0", "7"});
        waitForSaves(acks, writer, 5);
        for (int i = 0; i < 25; ++i) {
            openOnce(doc, acks, paused);
        }
        writer.kill();
    }
    CHECK(paused.good >= 20);

    // A writer that saves with no pause at all: opens may be refused, but none may open a bad copy.
    ray::store::detail::setSnapshotPatienceMs(1'500);
    Tally nonstop;
    int naiveBad = 0;
    int naiveTotal = 0;
    removeFile(acks);
    {
        Process writer = Process::spawnHelper({"writer", doc, acks, "0", "0", "8"});
        waitForSaves(acks, writer, 3);
        for (int i = 0; i < 8; ++i) {
            openOnce(doc, acks, nonstop);
        }
        // For comparison: plain copies of the same file, checked with the same verification.
        const std::string naive = dir.path("naive copy.raycad");
        for (int i = 0; i < 30; ++i) {
            removeFile(naive);
            removeFile(naive + "-journal");
            std::error_code ec;
            std::filesystem::copy_file(
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(doc.data()), doc.size())),
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(naive.data()), naive.size())), ec);
            if (ec) {
                continue;
            }
            ++naiveTotal;
            naiveBad += ray::store::detail::verifyCopy(naive) ? 0 : 1;
            sleepMs(3);
        }
        writer.kill();
    }
    ray::store::detail::setSnapshotPatienceMs(30'000);
    std::cout << "[store] snapshot test: writer pausing 40 ms: " << paused.good << " verified copies opened, "
              << paused.busy << " refused; writer non-stop: " << nonstop.good << " opened, " << nonstop.busy
              << " refused; plain copies rejected by verification: " << naiveBad << " of " << naiveTotal << "\n";
}
