// Kill and replay (PLAN.md 4.2, task T-012): a helper process journals N random steps (commands, undos, redos and,
// in half the trials, saves) and is killed, either once it has finished or at a random moment in the middle. The
// journal is then replayed onto the file it left; the state must be the one the helper had acknowledged (or, for a
// kill between a step and its acknowledgement, possibly that step's). The normal run does 20 trials; the hidden tag
// [.journal200] does the 200 of the acceptance test.
#include "journal.h"
#include "store_test_util.h"
#include "test_dispatcher.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace storetest;
using ray::store::IDocumentFile;
using ray::store::IRecoveryJournal;
using ray::store::OpenMode;

namespace {

struct HelperLog {
    std::string startHash;            // "R"
    std::map<int, std::string> begun; // "B <i> <hash>"
    int acked = -1;                   // last "A <i> ..."
    std::size_t undo = 0, redo = 0;   // at the last "A"
    int saves = 0;                    // at the last "A"
    bool done = false;                // "D"
};

HelperLog readLog(const std::string& path)
{
    HelperLog log;
    const std::string text = readFile(path);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            break; // cut short by the kill
        }
        std::istringstream line(text.substr(pos, end - pos));
        pos = end + 1;
        char tag = 0;
        line >> tag;
        if (tag == 'R') {
            line >> log.startHash;
        } else if (tag == 'B') {
            int i = 0;
            std::string hash;
            line >> i >> hash;
            log.begun[i] = hash;
        } else if (tag == 'A') {
            line >> log.acked >> log.undo >> log.redo >> log.saves;
        } else if (tag == 'D') {
            log.done = true;
        }
    }
    return log;
}

void runJournalKillTest(int trials)
{
    useTestLockTiming();
    TempDir dir;
    const std::string recovery = dir.path("recovery \xC3\xB8");
    ray::store::detail::setRecoveryDir(recovery);
    ray::test::TestDispatcher ui;
    const IRecoveryJournal::DocumentFactory factory = [&ui](ray::doc::DocStatePtr initial) {
        return ray::doc::makeDocument(ui, nullptr, std::move(initial));
    };
    std::mt19937_64 rng(20261118u + static_cast<unsigned>(trials));
    int midway = 0, inFlight = 0, replayed = 0, nothingPending = 0, stacksChecked = 0;
    std::size_t records = 0;
    const auto t0 = nowMs();
    for (int t = 0; t < trials; ++t) {
        const std::string doc = dir.path("trial " + std::to_string(t) + " \xC3\xA9.raycad");
        const std::string acks = dir.path("acks " + std::to_string(t) + ".txt");
        const int steps = 1 + static_cast<int>(rng() % 80);
        const int savePercent = t % 2 == 0 ? 0 : 8;
        const bool killMidway = rng() % 4 == 0;
        {
            Process helper = Process::spawnHelper({"journal", recovery, doc, acks, std::to_string(rng()),
                                                   std::to_string(steps), std::to_string(savePercent)});
            const auto deadline = nowMs() + 60000;
            while (readLog(acks).startHash.empty() && helper.running() && nowMs() < deadline) {
                sleepMs(1);
            }
            INFO("trial " << t << ": the helper did not start its journal");
            REQUIRE_FALSE(readLog(acks).startHash.empty());
            if (killMidway) {
                sleepMs(static_cast<int>(rng() % static_cast<unsigned>(steps * 2 + 1)));
            } else {
                while (!readLog(acks).done && helper.running() && nowMs() < deadline) {
                    sleepMs(2);
                }
            }
            INFO("trial " << t << ": the helper exited by itself");
            REQUIRE(helper.running());
            helper.kill();
        }
        const HelperLog log = readLog(acks);
        if (!killMidway) {
            REQUIRE(log.done);
        }
        // The state may be the acknowledged one, or the one of a step that had begun but was not acknowledged.
        std::set<std::string> allowed;
        allowed.insert(log.acked >= 0 ? log.begun.at(log.acked) : log.startHash);
        if (const auto next = log.begun.find(log.acked + 1); next != log.begun.end()) {
            allowed.insert(next->second);
            ++inFlight;
        }
        midway += killMidway ? 1 : 0;

        INFO("trial " << t << ": " << steps << " steps, saves " << savePercent << "%, killed "
                      << (killMidway ? "midway" : "after the last step") << ", acknowledged " << log.acked);
        REQUIRE(breakLockOfDeadWriter(doc));
        auto file = IDocumentFile::open(doc, OpenMode::ReadWrite);
        INFO((file ? std::string() : file.error().message));
        REQUIRE(file);
        std::string got;
        if (auto pending = IRecoveryJournal::findPendingFor(**file)) {
            records += pending->records.size();
            auto recovered = IRecoveryJournal::recover(*pending, **file, factory);
            INFO((recovered ? std::string() : recovered.error().message));
            REQUIRE(recovered);
            CHECK_FALSE(recovered->stopped);
            got = recovered->document->state()->stateHash().hex();
            if (!killMidway && log.saves == 0) {
                // No save: the journal holds the whole session, so the undo and redo stacks are the same too.
                CHECK(recovered->document->undoCount() == log.undo);
                CHECK(recovered->document->redoCount() == log.redo);
                ++stacksChecked;
            }
            REQUIRE(recovered->journal->discard());
            ++replayed;
        } else {
            auto head = (*file)->loadHead();
            REQUIRE(head);
            got = (*head)->stateHash().hex();
            ++nothingPending;
        }
        CHECK(allowed.count(got) == 1);
        ui.runPending();
        file->reset();
        removeDocument(doc);
    }
    ray::store::detail::setRecoveryDir("");
    std::cout << "[store] journal kill test: " << trials << " trials (" << midway << " killed midway, " << inFlight
              << " with a step in flight), " << replayed << " replayed (" << records << " records), " << nothingPending
              << " with nothing pending (killed right after a save), stacks compared in " << stacksChecked
              << ", 0 mismatches, " << (nowMs() - t0) / 1000.0 << " s\n";
}

} // namespace

TEST_CASE("a killed session replays to the state it had (20 trials)", "[store][journal][kill]")
{
    runJournalKillTest(20);
}

TEST_CASE("a killed session replays to the state it had (200 trials)", "[.journal200]")
{
    runJournalKillTest(200);
}
