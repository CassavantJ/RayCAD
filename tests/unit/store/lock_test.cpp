// The lock file of PLAN.md 4.3: content, heartbeat, save counter, second opener, stale lock, breaking races.
#include "lock.h"
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
namespace detail = ray::store::detail;

namespace {

void makeDocument(const std::string& path)
{
    std::mt19937_64 rng(11);
    auto file = IDocumentFile::create(path);
    REQUIRE(file);
    IDocumentFile::SaveRequest request;
    request.state = std::make_shared<const ray::doc::DocState>(makeState(rng, 4, 40));
    request.message = "save 0";
    ray::JobContext ctx;
    REQUIRE((*file)->save(request, ctx));
}

// Waits for a file the helper writes atomically; returns its content.
std::string waitForFile(const std::string& path, Process& process, int timeoutMs = 30000)
{
    const auto end = nowMs() + timeoutMs;
    for (;;) {
        if (exists(path)) {
            // Written beside and renamed into place, so it is complete; a virus scanner may still hold it a moment.
            std::string content = readFile(path);
            if (!content.empty()) {
                return content;
            }
        }
        REQUIRE(nowMs() < end);
        REQUIRE(process.running());
        sleepMs(5);
    }
}

} // namespace

TEST_CASE("lock file content round-trips and a torn read is detected", "[store][lock]")
{
    detail::LockFields f;
    f.user = "someone \xC3\xBC\nwith a newline";
    f.host = "a-host";
    f.pid = 4242;
    f.sinceUnixMs = 1'700'000'000'000;
    f.heartbeatUnixMs = 1'700'000'030'000;
    f.beat = 1;
    f.saving = true;
    f.saveCount = 17;
    const std::string bytes = detail::encodeLock(f);
    CHECK(bytes.size() == detail::kLockFileBytes);
    const auto back = detail::decodeLock(bytes);
    REQUIRE(back);
    CHECK(back->user == "someone \xC3\xBC?with a newline");
    CHECK(back->host == "a-host");
    CHECK(back->pid == 4242);
    CHECK(back->sinceUnixMs == f.sinceUnixMs);
    CHECK(back->saving);
    CHECK(back->saveCount == 17);
    for (const std::size_t at : {std::size_t{3}, std::size_t{40}, bytes.find("saves=") + 6}) {
        std::string torn = bytes;
        torn[at] = static_cast<char>(torn[at] ^ 0x01);
        CHECK_FALSE(detail::decodeLock(torn));
    }
    CHECK_FALSE(detail::decodeLock(""));
}

TEST_CASE("the writer's lock file carries owner, heartbeat and the save counter", "[store][lock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string path = dir.path("counter \xC3\xA4.raycad");
    auto created = IDocumentFile::create(path);
    REQUIRE(created);
    auto& file = *created;
    auto state = detail::readLock(path);
    REQUIRE(state);
    REQUIRE(state->readable);
    CHECK(state->fields.user == detail::currentUserName());
    CHECK(state->fields.host == detail::currentHostName());
    CHECK(state->fields.pid == detail::currentProcessId());
    CHECK_FALSE(state->fields.saving);
    CHECK_FALSE(state->ownerDead);
    CHECK_FALSE(state->stale);
    const std::int64_t count0 = state->fields.saveCount;
    const std::int64_t beat0 = state->fields.beat;

    std::mt19937_64 rng(12);
    ray::JobContext ctx;
    IDocumentFile::SaveRequest request;
    request.state = std::make_shared<const ray::doc::DocState>(makeState(rng, 3, 30));
    for (int i = 1; i <= 3; ++i) {
        REQUIRE(file->save(request, ctx));
        auto after = detail::readLock(path);
        REQUIRE(after);
        CHECK(after->fields.saveCount == count0 + i);
        CHECK_FALSE(after->fields.saving);
    }
    // Every write transaction counts, not only saves.
    REQUIRE(file->put(ray::store::ObjectKind::Attachment, randomBytes(rng, 10)));
    CHECK(detail::readLock(path)->fields.saveCount == count0 + 4);
    // An object that is already stored needs no transaction.
    REQUIRE(file->put(ray::store::ObjectKind::Attachment, {}));
    REQUIRE(file->put(ray::store::ObjectKind::Attachment, {}));
    CHECK(detail::readLock(path)->fields.saveCount == count0 + 5);

    sleepMs(static_cast<int>(kTestHeartbeatMs * 4));
    auto later = detail::readLock(path);
    REQUIRE(later);
    CHECK(later->fields.beat >= beat0 + 2);
    CHECK(later->ageSec < static_cast<double>(kTestStaleMs) / 1000.0);
    CHECK_FALSE(later->stale);
}

TEST_CASE("a second process sees the holder; a killed holder's lock is stale and is broken by rename", "[store][lock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string path = dir.path("held by a child \xE6\xB8\xAC.raycad");
    makeDocument(path);
    const std::string ready = dir.path("ready");
    Process holder = Process::spawnHelper({"hold", path, ready, "1"});
    REQUIRE(waitForFile(ready, holder) == "rw");

    OpenInfo info{};
    auto second = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
    REQUIRE(second);
    CHECK(info.openedReadOnlyBecauseLocked);
    REQUIRE(info.lock);
    CHECK(info.lock->pid == holder.pid());
    // Same machine, so the names the helper read from the OS are this process's.
    CHECK(info.lock->user == detail::currentUserName());
    CHECK(info.lock->host == detail::currentHostName());
    CHECK(info.lock->sinceUnixMs > 0);
    CHECK_FALSE(info.lock->stale);
    CHECK_FALSE(info.lock->ownerProvablyDead);
    CHECK((*second)->loadHead());
    second->reset();
    CHECK(ray::store::breakStaleLock(path).error().code == ErrorCode::Locked);
    CHECK(exists(path + ".lock"));

    auto live1 = detail::readLock(path);
    sleepMs(static_cast<int>(kTestHeartbeatMs * 4));
    auto live2 = detail::readLock(path);
    REQUIRE(live1);
    REQUIRE(live2);
    CHECK(live2->fields.beat > live1->fields.beat);
    CHECK_FALSE(live2->stale);

    holder.kill();
    auto dead = detail::readLock(path);
    REQUIRE(dead);
    CHECK(dead->ownerDead); // at once, without waiting for the heartbeat to age
    CHECK(dead->stale);
    OpenInfo info2{};
    auto stillLocked = IDocumentFile::open(path, OpenMode::ReadWrite, &info2);
    REQUIRE(stillLocked);
    CHECK(info2.openedReadOnlyBecauseLocked);
    REQUIRE(info2.lock);
    CHECK(info2.lock->stale);
    CHECK(info2.lock->ownerProvablyDead);
    stillLocked->reset();

    REQUIRE(ray::store::breakStaleLock(path));
    CHECK_FALSE(exists(path + ".lock"));
    CHECK(ray::store::breakStaleLock(path)); // nothing left to break
    OpenInfo info3{};
    auto owner = IDocumentFile::open(path, OpenMode::ReadWrite, &info3);
    REQUIRE(owner);
    CHECK_FALSE(info3.openedReadOnlyBecauseLocked);
    REQUIRE((*owner)->loadHead());
    std::mt19937_64 rng(13);
    IDocumentFile::SaveRequest request;
    request.state = std::make_shared<const ray::doc::DocState>(makeState(rng, 2, 10));
    ray::JobContext ctx;
    CHECK((*owner)->save(request, ctx));
    // No renamed lock files are left behind.
    owner->reset();
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(
             std::u8string(reinterpret_cast<const char8_t*>(dir.dir().data()), dir.dir().size())))) {
        const std::string name = reinterpret_cast<const char*>(entry.path().filename().u8string().c_str());
        CHECK(name.find(".broken-") == std::string::npos);
        CHECK(name.find(".probe-") == std::string::npos);
    }
}

TEST_CASE("a live owner whose heartbeat stops goes stale but still guards the file", "[store][lock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string path = dir.path("silent.raycad");
    makeDocument(path);
    const std::string ready = dir.path("ready");
    Process holder = Process::spawnHelper({"hold", path, ready, "0"});
    REQUIRE(waitForFile(ready, holder) == "rw");
    sleepMs(static_cast<int>(kTestStaleMs + 600));
    auto state = detail::readLock(path);
    REQUIRE(state);
    CHECK(state->stale);
    CHECK_FALSE(state->ownerDead);
    CHECK(state->ageSec > static_cast<double>(kTestStaleMs) / 1000.0);
    auto broken = ray::store::breakStaleLock(path);
#ifdef _WIN32
    // The owner's handle denies delete sharing: the file cannot be renamed while the owner process lives.
    REQUIRE_FALSE(broken);
    CHECK(broken.error().code == ErrorCode::Locked);
#else
    // POSIX cannot stop the rename (the plan allows breaking a lock older than the limit) ...
    REQUIRE(broken);
#endif
    // ... and either way SQLite's own exclusive lock, taken at the owner's open, still keeps a second writer out.
    OpenInfo info{};
    auto opened = IDocumentFile::open(path, OpenMode::ReadWrite, &info);
    if (opened) {
        CHECK(info.openedReadOnlyBecauseLocked);
    } else {
        CHECK(opened.error().code == ErrorCode::Locked);
    }
}

TEST_CASE("two processes breaking the same stale lock: exactly one gets write access", "[store][lock]")
{
    useTestLockTiming();
    TempDir dir;
    const std::string path = dir.path("race.raycad");
    makeDocument(path);
    int wins = 0;
    for (int round = 0; round < 10; ++round) {
        const std::string tag = std::to_string(round);
        const std::string ready = dir.path("ready" + tag);
        Process holder = Process::spawnHelper({"hold", path, ready, "1"});
        REQUIRE(waitForFile(ready, holder) == "rw");
        holder.kill(); // leaves a stale lock with a dead owner
        REQUIRE(exists(path + ".lock"));

        const std::string go = dir.path("go" + tag);
        const std::string done = dir.path("done" + tag);
        const std::string resultA = dir.path("a" + tag);
        const std::string resultB = dir.path("b" + tag);
        Process a = Process::spawnHelper({"race", path, go, resultA, done});
        Process b = Process::spawnHelper({"race", path, go, resultB, done});
        sleepMs(150); // both are spinning on the go file
        writeFile(go, "go");
        const std::string ra = waitForFile(resultA, a, 60000);
        const std::string rb = waitForFile(resultB, b, 60000);
        const int rw = (ra.rfind("rw", 0) == 0 ? 1 : 0) + (rb.rfind("rw", 0) == 0 ? 1 : 0);
        const int ro = (ra.rfind("ro", 0) == 0 ? 1 : 0) + (rb.rfind("ro", 0) == 0 ? 1 : 0);
        INFO("round " << round << ": " << ra << " / " << rb);
        CHECK(rw == 1);
        CHECK(ro == 1);
        wins += rw;
        writeFile(done, "done");
        CHECK(a.wait() == 0);
        CHECK(b.wait() == 0);
        CHECK_FALSE(exists(path + ".lock"));
    }
    CHECK(wins == 10);
}
