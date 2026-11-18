// The recovery journal (PLAN.md 4.2, task T-012) in one process: replay of commands, undos and redos, snapshot
// records, torn and damaged records, journals that must not be applied, journals in use, objects, append speed.
// A "crash" here drops the journal, document and file without discard(); journal_kill_test.cpp kills real processes.
#include "file.h"
#include "journal.h"
#include "store_test_util.h"
#include "test_dispatcher.h"

#include <ray/store.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace storetest;
using ray::ErrorCode;
using ray::Hash256;
using ray::doc::DocState;
using ray::doc::DocStatePtr;
using ray::store::IDocumentFile;
using ray::store::IRecoveryJournal;
using ray::store::JournalRecord;
using ray::store::OpenMode;

namespace {

struct Env {
    TempDir dir;
    ray::test::TestDispatcher ui;
    Env() { ray::store::detail::setRecoveryDir(dir.path("recovery ø dir")); }
    ~Env() { ray::store::detail::setRecoveryDir(""); }
    IRecoveryJournal::DocumentFactory factory()
    {
        return [this](DocStatePtr initial) {
            return ray::doc::makeDocument(ui, nullptr, std::move(initial));
        };
    }
};

// A document file with a first save, the document on it and its journal.
struct Session {
    std::string path;
    std::unique_ptr<IDocumentFile> file;
    std::unique_ptr<ray::doc::Document> doc;
    std::unique_ptr<IRecoveryJournal> journal;

    void crash()
    {
        journal.reset(); // closed without discard(): left for recovery
        doc.reset();
        file.reset();
    }
    std::string hash() const { return doc->state()->stateHash().hex(); }
};

Session startSession(Env& env, const std::string& name, std::mt19937_64& rng, bool save = true)
{
    Session s;
    s.path = env.dir.path(name);
    auto created = IDocumentFile::create(s.path);
    REQUIRE(created);
    s.file = std::move(*created);
    DocStatePtr base = std::make_shared<const DocState>(makeState(rng, 3, 40));
    if (save) {
        IDocumentFile::SaveRequest request;
        request.state = base;
        request.message = "first";
        ray::JobContext ctx;
        REQUIRE(s.file->save(request, ctx));
    }
    s.doc = ray::doc::makeDocument(env.ui, nullptr, base);
    auto journal = IRecoveryJournal::start(*s.file, s.path, base);
    INFO((journal ? std::string() : journal.error().message));
    REQUIRE(journal);
    s.journal = std::move(*journal);
    return s;
}

std::unique_ptr<IDocumentFile> reopen(const std::string& path)
{
    auto file = IDocumentFile::open(path, OpenMode::ReadWrite);
    REQUIRE(file);
    return std::move(*file);
}

IRecoveryJournal::Pending pendingFor(const IDocumentFile& file)
{
    auto pending = IRecoveryJournal::findPendingFor(file);
    REQUIRE(pending);
    return std::move(*pending);
}

std::string headHash(IDocumentFile& file)
{
    auto head = file.loadHead();
    REQUIRE(head);
    return (*head)->stateHash().hex();
}

void flipByte(const std::string& path, std::uint64_t offset)
{
    std::string bytes = readFile(path);
    REQUIRE(offset < bytes.size());
    bytes[static_cast<std::size_t>(offset)] = static_cast<char>(bytes[static_cast<std::size_t>(offset)] ^ 0x5a);
    writeFile(path, bytes);
}

} // namespace

TEST_CASE("replay reproduces the state and the undo and redo stacks after a crash", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1201);
    Session s = startSession(env, "replay me ü.raycad", rng);
    for (int i = 0; i < 300; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    }
    const std::string hash = s.hash();
    const std::size_t undo = s.doc->undoCount();
    const std::size_t redo = s.doc->redoCount();
    const auto fileId = ray::store::detail::fileIdOf(*s.file);
    s.crash();

    const auto all = IRecoveryJournal::findPending();
    REQUIRE(all.size() == 1);
    CHECK(all[0].fileId == fileId);
    CHECK(all[0].filePath == s.path);
    CHECK(all[0].records.size() == 300);
    CHECK_FALSE(all[0].damagedTail);
    CHECK(all[0].baseCommit.has_value());
    // The journal's own thread checked steps as they went (the newest at each moment; some are skipped).
    CHECK(std::count_if(all[0].records.begin(), all[0].records.end(),
                        [](const JournalRecord& r) { return r.stateAfter.has_value(); }) > 0);
    CHECK(std::any_of(all[0].records.begin(), all[0].records.end(),
                      [](const JournalRecord& r) { return r.kind == JournalRecord::Kind::Undo; }));
    CHECK(std::any_of(all[0].records.begin(), all[0].records.end(),
                      [](const JournalRecord& r) { return r.kind == JournalRecord::Kind::Redo; }));

    auto file = reopen(s.path);
    auto recovered = IRecoveryJournal::recover(all[0], *file, env.factory());
    INFO((recovered ? std::string() : recovered.error().message));
    REQUIRE(recovered);
    CHECK(recovered->applied == 300);
    CHECK_FALSE(recovered->stopped);
    CHECK(recovered->document->state()->stateHash().hex() == hash);
    CHECK(recovered->document->undoCount() == undo);
    CHECK(recovered->document->redoCount() == redo);
    CHECK(recovered->modified == (hash != headHash(*file)));

    // The continued journal holds the replayed steps plus new ones: a second crash loses nothing either.
    s.file = std::move(file);
    s.doc = std::move(recovered->document);
    s.journal = std::move(recovered->journal);
    for (int i = 0; i < 50; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    }
    const std::string hash2 = s.hash();
    const std::size_t undo2 = s.doc->undoCount();
    s.crash();
    file = reopen(s.path);
    auto again = IRecoveryJournal::recover(pendingFor(*file), *file, env.factory());
    REQUIRE(again);
    CHECK(again->applied == 350);
    CHECK(again->document->state()->stateHash().hex() == hash2);
    CHECK(again->document->undoCount() == undo2);

    // After a save the journal is empty, so nothing is pending; discard() removes it.
    IDocumentFile::SaveRequest request;
    request.state = again->document->state();
    ray::JobContext ctx;
    auto saved = file->save(request, ctx);
    REQUIRE(saved);
    REQUIRE(again->journal->markSaved(*saved));
    const std::string journalPath = ray::store::detail::journalPathFor(fileId);
    CHECK(exists(journalPath));
    again->journal.reset();
    CHECK_FALSE(IRecoveryJournal::findPendingFor(*file));
    auto restarted = IRecoveryJournal::start(*file, s.path, request.state);
    REQUIRE(restarted);
    REQUIRE((*restarted)->discard());
    CHECK_FALSE(exists(journalPath));
    CHECK((*restarted)->append({"feature.remove", "{}", ""}, request.state).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("undo and redo past the last save are recorded as snapshots and replay exactly", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1202);
    Session s = startSession(env, "past the save.raycad", rng);
    for (int i = 0; i < 4; ++i) {
        const auto command = randomCommand(rng, *s.doc->state());
        auto executed = s.doc->execute(command);
        REQUIRE(executed);
        REQUIRE(s.journal->append(command, *executed));
    }
    // Save: the journal now starts from here, but the document's undo stack still goes back four steps.
    IDocumentFile::SaveRequest request;
    request.state = s.doc->state();
    ray::JobContext ctx;
    auto saved = s.file->save(request, ctx);
    REQUIRE(saved);
    REQUIRE(s.journal->markSaved(*saved));
    s.doc->markClean();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(s.doc->undo());
        REQUIRE(s.journal->appendUndo(s.doc->state()));
    }
    REQUIRE(s.doc->redo());
    REQUIRE(s.journal->appendRedo(s.doc->state()));
    REQUIRE(s.doc->undo()); // still history from before the base: a snapshot again
    REQUIRE(s.journal->appendUndo(s.doc->state()));
    const auto command = randomCommand(rng, *s.doc->state());
    auto executed = s.doc->execute(command);
    REQUIRE(executed);
    REQUIRE(s.journal->append(command, *executed));
    REQUIRE(s.doc->undo()); // after the base: replayed as an undo
    REQUIRE(s.journal->appendUndo(s.doc->state()));
    const std::string hash = s.hash();
    s.crash();

    auto file = reopen(s.path);
    const auto pending = pendingFor(*file);
    REQUIRE(pending.records.size() == 7);
    CHECK(pending.records[0].kind == JournalRecord::Kind::Snapshot);
    CHECK(pending.records[3].kind == JournalRecord::Kind::Snapshot);
    CHECK(pending.records[4].kind == JournalRecord::Kind::Snapshot);
    CHECK(pending.records[5].kind == JournalRecord::Kind::Execute);
    CHECK(pending.records[6].kind == JournalRecord::Kind::Undo);
    auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
    REQUIRE(recovered);
    CHECK(recovered->document->state()->stateHash().hex() == hash);
    CHECK(recovered->document->canRedo());
    CHECK(recovered->modified);
}

TEST_CASE("steps made while a save ran survive markSaved", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1203);
    Session s = startSession(env, "saving in the background.raycad", rng);
    for (int i = 0; i < 3; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    }
    IDocumentFile::SaveRequest request;
    request.state = s.doc->state(); // the save job takes this snapshot ...
    for (int i = 0; i < 3; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0); // ... while the user goes on working
    }
    ray::JobContext ctx;
    auto saved = s.file->save(request, ctx);
    REQUIRE(saved);
    REQUIRE(s.journal->markSaved(*saved));
    const std::string hash = s.hash();
    REQUIRE(hash != request.state->stateHash().hex());
    s.crash();
    auto file = reopen(s.path);
    const auto pending = pendingFor(*file);
    REQUIRE(pending.records.size() == 1);
    CHECK(pending.records[0].kind == JournalRecord::Kind::Snapshot);
    auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
    REQUIRE(recovered);
    CHECK(recovered->document->state()->stateHash().hex() == hash);
    CHECK(recovered->modified);
}

TEST_CASE("a torn or damaged record ends the journal and everything before it replays", "[store][journal]")
{
    Env env;
    ray::store::detail::setJournalChecking(false); // one record per step, so the test knows which record is which
    struct Restore {
        ~Restore() { ray::store::detail::setJournalChecking(true); }
    } restore;
    std::mt19937_64 rng(1204);
    Session s = startSession(env, "torn tail.raycad", rng);
    std::vector<std::string> hashes; // after each step
    for (int i = 0; i < 30; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
        hashes.push_back(s.hash());
    }
    const std::string journalPath = ray::store::detail::journalPathFor(ray::store::detail::fileIdOf(*s.file));
    s.crash();
    const std::string original = readFile(journalPath);
    const auto ends = ray::store::detail::journalRecordEnds(journalPath);
    REQUIRE(ends.size() == 31); // the header and 30 steps
    REQUIRE(ends.back() == original.size());

    // Each damage, and how many steps must still replay.
    struct Damage {
        const char* what;
        std::function<void()> apply;
        std::size_t intact;
    };
    const std::uint64_t lastStart = ends[29];
    const std::uint64_t lastLength = ends[30] - lastStart;
    std::vector<Damage> damages = {
        {"torn inside the last record's frame",
         [&] { writeFile(journalPath, original.substr(0, static_cast<std::size_t>(lastStart + 7))); }, 29},
        {"torn inside the last record's payload",
         [&] {
             writeFile(journalPath,
                       original.substr(0, static_cast<std::size_t>(lastStart + 16 + rng() % (lastLength - 16))));
         },
         29},
        {"a flipped bit in the last payload",
         [&] { flipByte(journalPath, lastStart + 16 + rng() % (lastLength - 16)); }, 29},
        {"a flipped bit in the last length", [&] { flipByte(journalPath, lastStart + 1); }, 29},
        {"garbage after the last record", [&] { writeFile(journalPath, original + "\x10\x00\x00garbage!"); }, 30},
        {"a flipped bit in record 11", [&] { flipByte(journalPath, ends[10] + 20); }, 10},
    };
    for (const Damage& damage : damages) {
        INFO(damage.what);
        writeFile(journalPath, original);
        damage.apply();
        auto file = reopen(s.path);
        const auto pending = pendingFor(*file);
        CHECK(pending.records.size() == damage.intact);
        CHECK(pending.damagedTail);
        auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
        REQUIRE(recovered);
        CHECK(recovered->applied == damage.intact);
        CHECK(recovered->document->state()->stateHash().hex() == hashes[damage.intact - 1]);
        recovered->journal.reset();
        // The continued journal was rewritten without the damage.
        const auto rewritten = IRecoveryJournal::findPendingFor(*file);
        REQUIRE(rewritten);
        CHECK(rewritten->records.size() == damage.intact);
        CHECK_FALSE(rewritten->damagedTail);
    }

    // A damaged header: the journal is not a journal any more, so nothing is offered.
    writeFile(journalPath, original);
    flipByte(journalPath, 8 + 16 + 3);
    auto file = reopen(s.path);
    CHECK_FALSE(IRecoveryJournal::findPendingFor(*file));
    CHECK(IRecoveryJournal::findPending().empty());
}

TEST_CASE("a step that replays to another state stops the replay before it", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1205);
    Session s = startSession(env, "diverges.raycad", rng);
    std::vector<std::string> hashes;
    std::vector<std::size_t> undos, redos;
    for (int i = 0; i < 20; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
        hashes.push_back(s.hash());
        undos.push_back(s.doc->undoCount());
        redos.push_back(s.doc->redoCount());
    }
    s.crash();
    auto file = reopen(s.path);
    auto pending = pendingFor(*file);
    pending.records[8].stateAfter = Hash256{}; // as if the command now did something else
    auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
    REQUIRE(recovered);
    CHECK(recovered->applied == 8);
    REQUIRE(recovered->stopped);
    CHECK(recovered->stopped->code == ErrorCode::FormatError);
    CHECK(recovered->document->state()->stateHash().hex() == hashes[7]);
    CHECK(recovered->document->undoCount() == undos[7]);
    CHECK(recovered->document->redoCount() == redos[7]); // the bad step is not left on a stack either
}

TEST_CASE("a journal is never applied to another document or to a later save", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1206);
    Session a = startSession(env, "document a.raycad", rng);
    for (int i = 0; i < 10; ++i) {
        randomStep(rng, *a.doc, *a.journal, nullptr, 0);
    }
    a.crash();
    Session b = startSession(env, "document b.raycad", rng);
    REQUIRE(b.journal->discard());
    b.journal.reset();
    {
        auto fileA = reopen(a.path);
        const auto pendingA = pendingFor(*fileA);
        fileA.reset();
        const std::string headB = headHash(*b.file);
        const auto logB = b.file->log("main", 0);
        auto wrong = IRecoveryJournal::recover(pendingA, *b.file, env.factory());
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().code == ErrorCode::InvalidArgument);
        CHECK(headHash(*b.file) == headB);
        CHECK(b.file->log("main", 0)->size() == logB->size());
    }

    // A later save that is not a state the journal reached (another session, or a restored copy): not applied, the
    // file and the journal stay as they are.
    {
        auto fileA = reopen(a.path);
        IDocumentFile::SaveRequest other;
        other.state = std::make_shared<const DocState>(makeState(rng, 2, 10));
        ray::JobContext ctx;
        REQUIRE(fileA->save(other, ctx));
        const std::string head = headHash(*fileA);
        const auto pending = pendingFor(*fileA);
        auto older = IRecoveryJournal::recover(pending, *fileA, env.factory());
        REQUIRE_FALSE(older);
        CHECK(older.error().code == ErrorCode::InvalidArgument);
        CHECK(headHash(*fileA) == head);
        CHECK(pendingFor(*fileA).records.size() == 10);
        REQUIRE(IRecoveryJournal::discard(pending));
        CHECK_FALSE(IRecoveryJournal::findPendingFor(*fileA));
    }

    // The save landed but markSaved did not: the journal is accepted, continues from the new head, and the document
    // is clean.
    Session c = startSession(env, "saved then crashed.raycad", rng);
    for (int i = 0; i < 10; ++i) {
        randomStep(rng, *c.doc, *c.journal, nullptr, 0);
    }
    const std::string hashC = c.hash();
    const std::size_t undoC = c.doc->undoCount();
    IDocumentFile::SaveRequest request;
    request.state = c.doc->state();
    ray::JobContext ctx;
    REQUIRE(c.file->save(request, ctx));
    c.crash();
    auto fileC = reopen(c.path);
    auto landed = IRecoveryJournal::recover(pendingFor(*fileC), *fileC, env.factory());
    INFO((landed ? std::string() : landed.error().message));
    REQUIRE(landed);
    CHECK(landed->document->state()->stateHash().hex() == hashC);
    CHECK(landed->document->undoCount() == undoC);
    CHECK_FALSE(landed->modified);
    CHECK_FALSE(landed->document->dirty());
    landed->journal.reset();
    CHECK_FALSE(IRecoveryJournal::findPendingFor(*fileC)); // continued from the head: nothing left to replay
}

TEST_CASE("a journal in use is not offered, not discarded and not started twice", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1207);
    Session s = startSession(env, "in use.raycad", rng);
    for (int i = 0; i < 5; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    }
    CHECK(IRecoveryJournal::findPending().empty());
    CHECK_FALSE(IRecoveryJournal::findPendingFor(*s.file));
    auto head = s.file->loadHead();
    REQUIRE(head);
    auto second = IRecoveryJournal::start(*s.file, s.path, *head);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == ErrorCode::Locked);
    IRecoveryJournal::Pending fake;
    fake.journalPath = ray::store::detail::journalPathFor(ray::store::detail::fileIdOf(*s.file));
    CHECK(IRecoveryJournal::discard(fake).error().code == ErrorCode::Locked);

    // Left behind: a new session must not overwrite it, and gets going again once it is discarded.
    s.journal.reset();
    auto blocked = IRecoveryJournal::start(*s.file, s.path, *head);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == ErrorCode::InvalidArgument);
    REQUIRE(IRecoveryJournal::discard(pendingFor(*s.file)));
    CHECK(IRecoveryJournal::start(*s.file, s.path, *head));

    // A state that is not the saved head cannot start a journal; neither can a read-only document.
    CHECK(IRecoveryJournal::start(*s.file, s.path, s.doc->state()).error().code == ErrorCode::InvalidArgument);
    auto readOnly = IDocumentFile::open(s.path, OpenMode::ReadOnly);
    REQUIRE(readOnly);
    CHECK(IRecoveryJournal::start(**readOnly, s.path, *head).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("objects the unsaved steps need come back with the journal", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1208);
    Session s = startSession(env, "objects.raycad", rng);
    const auto brep = randomBytes(rng, 300 * 1024);
    const Hash256 hash = hashOf(brep);
    CHECK(s.journal->appendObject(Hash256{}, brep).error().code == ErrorCode::InvalidArgument);
    REQUIRE(s.journal->appendObject(hash, brep));
    randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    s.crash();
    auto file = reopen(s.path);
    const auto pending = pendingFor(*file);
    REQUIRE(pending.objects.size() == 1);
    CHECK(pending.objects[0].first == hash);
    CHECK(pending.objects[0].second == brep);
    auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
    REQUIRE(recovered);
    recovered->journal.reset();
    const auto continued = pendingFor(*file);
    REQUIRE(continued.objects.size() == 1); // still there until a save holds them
    CHECK(continued.objects[0].second == brep);
}

TEST_CASE("a document never saved is recovered from its journal alone", "[store][journal]")
{
    Env env;
    std::mt19937_64 rng(1209);
    Session s = startSession(env, "never saved.raycad", rng, false);
    for (int i = 0; i < 25; ++i) {
        randomStep(rng, *s.doc, *s.journal, nullptr, 0);
    }
    const std::string hash = s.hash();
    s.crash();
    auto file = reopen(s.path);
    const auto pending = pendingFor(*file);
    CHECK_FALSE(pending.baseCommit);
    CHECK(pending.records.front().kind == JournalRecord::Kind::Snapshot);
    auto recovered = IRecoveryJournal::recover(pending, *file, env.factory());
    REQUIRE(recovered);
    CHECK(recovered->document->state()->stateHash().hex() == hash);
    CHECK(recovered->modified);
}

namespace {

// Times append() after 1,000 random commands on a mid-sized part and checks the median against `limitMs`.
void appendBench(double limitMs)
{
    Env env;
    std::mt19937_64 rng(1210);
    Session s = startSession(env, "fast append.raycad", rng);
    // A mid-sized part: 60 features with about 1 KB of properties each.
    for (int i = 0; i < 60; ++i) {
        const auto command = ray::doc::Command{
            "feature.add",
            "{\"id\":\"" + randomUuid(rng) + "\",\"type\":\"extrude\",\"name\":\"F" + std::to_string(i) +
                "\",\"props\":{\"distance\":\"10 mm\",\"note\":\"" + randomHex(rng, 1000) + "\"}}",
            "Add"};
        auto executed = s.doc->execute(command);
        REQUIRE(executed);
        REQUIRE(s.journal->append(command, *executed));
    }
    std::vector<double> ms, hashMs;
    for (int i = 0; i < 1000; ++i) {
        const auto command = randomCommand(rng, *s.doc->state());
        auto executed = s.doc->execute(command);
        if (!executed) {
            continue;
        }
        if (i % 50 == 0 || hashMs.empty()) {
            const auto h0 = std::chrono::steady_clock::now();
            (void)(*executed)->stateHash();
            hashMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - h0).count());
        }
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(s.journal->append(command, *executed));
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(hashMs.begin(), hashMs.end());
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2];
    const double p99 = ms[ms.size() * 99 / 100];
    std::cout << "[store] journal append (" << s.doc->state()->features.size() << " features): median " << median
              << " ms, p99 " << p99 << " ms, max " << ms.back() << " ms over " << ms.size()
              << " appends; the state hash its own thread checks takes " << hashMs[hashMs.size() / 2]
              << " ms (median)\n";
    CHECK(median < limitMs);
}

} // namespace

// The normal run allows for slow CI disks; the hidden tag holds the budget of PLAN.md 4.2 (measure it on the target).
TEST_CASE("append is fast enough to run after every command", "[store][journal][bench]")
{
    appendBench(10.0);
}

TEST_CASE("append takes under 2 ms (median)", "[.journalbench]")
{
    appendBench(2.0);
}
