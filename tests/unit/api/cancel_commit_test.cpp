// A document command, undo or redo that is cancelled reports Cancelled if and only if it did not change the document:
// the UI-thread hop that commits it closes the job's token first (AppImpl::pointOfNoReturn, CancelToken::close), so a
// cancel either wins before that and nothing changes, or is too late (JobHandle::cancel returns false) and the job
// reports the change. Cancels are placed exactly through the app's test hook around that close, and raced from
// another thread in the stress test.
#include "api_test_util.h"

#include "app_impl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using ray::ErrorCode;
using ray::api::Caller;
using ray::api::OpenDocument;
using ray::test::kWait;
using ray::test::makeTestApp;

namespace {

using Handle = ray::JobHandle<ray::api::ActionResult>;

// param.set of one parameter to `value`: a quick command that always changes the state when the value is new.
std::string setParam(int value)
{
    return R"({"id":"00000000-0000-4000-8000-0000000000aa","name":"p","expression":")" + std::to_string(value) +
           R"(","kind":"unitless"})";
}

ray::api::detail::AppImpl& implOf(ray::api::HeadlessApp& app)
{
    auto* impl = dynamic_cast<ray::api::detail::AppImpl*>(&app);
    REQUIRE(impl != nullptr);
    return *impl;
}

struct DocSnapshot {
    std::string hash;
    std::size_t undos = 0;
    std::size_t redos = 0;
};

DocSnapshot snapshot(const OpenDocument& od)
{
    return DocSnapshot{od.document->state()->stateHash().hex(), od.document->undoCount(), od.document->redoCount()};
}

// The invariant, for one finished job: Cancelled (and the cancel took effect) exactly when the document is unchanged.
void requireIff(const ray::Result<ray::api::ActionResult>& result, bool cancelTookEffect, const DocSnapshot& before,
                const DocSnapshot& after)
{
    const bool changed = after.hash != before.hash || after.undos != before.undos || after.redos != before.redos;
    INFO("result " << (result ? std::string("ok") : result.error().message) << ", cancel took effect "
                   << cancelTookEffect << ", document changed " << changed);
    REQUIRE(result.has_value() == changed);
    REQUIRE(cancelTookEffect == !result.has_value());
    if (!result) {
        REQUIRE(result.error().code == ErrorCode::Cancelled);
    }
}

// Where the cancel is placed, relative to the hop's close of the job's token.
enum class At { BeforeClose, AfterClose };

// Runs `action` with a cancel placed exactly at `at` inside its UI-thread hop, from the UI thread itself or (joined
// there) from another thread. Returns whether that cancel took effect.
bool runWithCancelAt(ray::api::HeadlessApp& app, const std::string& action, const std::string& args, At at,
                     bool fromOtherThread, ray::Result<ray::api::ActionResult>& result)
{
    Handle job;
    std::atomic<bool> fired{false};
    std::atomic<bool> took{false};
    implOf(app).setNoReturnHookForTest([&](bool after) {
        if (after != (at == At::AfterClose) || fired.exchange(true)) {
            return;
        }
        if (fromOtherThread) {
            std::thread other([&] { took = job.cancel(); });
            other.join();
        } else {
            took = job.cancel();
        }
    });
    job = app.actions().invoke(action, args, Caller::User);
    result = app.wait(job, kWait);
    implOf(app).setNoReturnHookForTest({});
    REQUIRE(fired.load()); // the hop reached its point of no return
    return took.load();
}

} // namespace

TEST_CASE("a document command cancelled around its commit: Cancelled if and only if the document did not change",
          "[api][cancel][close]")
{
    auto app = makeTestApp("");
    REQUIRE(app->newDocument(""));
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    int value = 1;
    REQUIRE(app->wait(app->actions().invoke("param.set", setParam(value++), Caller::User), kWait));

    const bool fromOtherThread = GENERATE(false, true);
    INFO("cancel from " << (fromOtherThread ? "another thread" : "the UI thread"));
    SECTION("(a) cancelled just before the close: Cancelled, nothing changed")
    {
        const DocSnapshot before = snapshot(*od);
        ray::Result<ray::api::ActionResult> r;
        const bool took = runWithCancelAt(*app, "param.set", setParam(value++), At::BeforeClose, fromOtherThread, r);
        REQUIRE_FALSE(r);
        requireIff(r, took, before, snapshot(*od));
    }
    SECTION("(b) cancelled between the close and the end of the commit: too late, committed and reported")
    {
        const DocSnapshot before = snapshot(*od);
        ray::Result<ray::api::ActionResult> r;
        const bool took = runWithCancelAt(*app, "param.set", setParam(value++), At::AfterClose, fromOtherThread, r);
        REQUIRE(r);
        requireIff(r, took, before, snapshot(*od));
        CHECK(snapshot(*od).undos == before.undos + 1);
    }
    SECTION("(c) cancelled after the job is done: too late, the result stands")
    {
        const DocSnapshot before = snapshot(*od);
        Handle job = app->actions().invoke("param.set", setParam(value++), Caller::User);
        auto r = app->wait(job, kWait);
        REQUIRE(r);
        bool took = true;
        if (fromOtherThread) {
            std::thread other([&] { took = job.cancel(); });
            other.join();
        } else {
            took = job.cancel();
        }
        requireIff(job.wait(), took, before, snapshot(*od));
    }
    SECTION("(d) cancelled before the hop could start (the UI thread busy): Cancelled, nothing changed")
    {
        const DocSnapshot before = snapshot(*od);
        Handle job = app->actions().invoke("param.set", setParam(value++), Caller::User);
        bool took = false;
        if (fromOtherThread) {
            std::thread other([&] { took = job.cancel(); });
            other.join();
        } else {
            took = job.cancel();
        }
        auto r = app->wait(job, kWait);
        REQUIRE_FALSE(r);
        requireIff(r, took, before, snapshot(*od));
    }
}

TEST_CASE("undo and redo cancelled around their point of no return keep the invariant too", "[api][cancel][close]")
{
    auto app = makeTestApp("");
    REQUIRE(app->newDocument(""));
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    REQUIRE(app->wait(app->actions().invoke("param.set", setParam(1), Caller::User), kWait));
    REQUIRE(app->wait(app->actions().invoke("param.set", setParam(2), Caller::User), kWait));

    for (const char* action : {"edit.undo", "edit.redo"}) {
        INFO(action);
        if (std::string(action) == "edit.redo") {
            REQUIRE(app->wait(app->actions().invoke("edit.undo", "{}", Caller::User), kWait));
        }
        for (const At at : {At::BeforeClose, At::AfterClose}) {
            const DocSnapshot before = snapshot(*od);
            ray::Result<ray::api::ActionResult> r;
            const bool took = runWithCancelAt(*app, action, "{}", at, true, r);
            CHECK(r.has_value() == (at == At::AfterClose));
            requireIff(r, took, before, snapshot(*od));
            if (r && at == At::AfterClose) {
                // Put it back for the next round.
                REQUIRE(app->wait(app->actions().invoke(std::string(action) == "edit.undo" ? "edit.redo" : "edit.undo",
                                                        "{}", Caller::User),
                                  kWait));
            }
        }
    }
}

TEST_CASE("opening a file cancelled around the point where it adds the document", "[api][cancel][close]")
{
    auto app = makeTestApp("");
    const bool after = GENERATE(false, true);
    INFO((after ? "cancelled just after the close" : "cancelled just before the close"));
    ray::JobHandle<OpenDocument*> job;
    std::atomic<bool> fired{false};
    std::atomic<bool> took{false};
    implOf(*app).setNoReturnHookForTest([&](bool hookAfter) {
        if (hookAfter == after && !fired.exchange(true)) {
            std::thread other([&] { took = job.cancel(); });
            other.join();
        }
    });
    job = app->open(ray::test::kernelData("box_hole.step"));
    auto r = app->wait(job, kWait);
    implOf(*app).setNoReturnHookForTest({});
    REQUIRE(fired.load());
    INFO((r ? std::string("opened") : r.error().message));
    CHECK(r.has_value() == after);
    CHECK(took.load() == !after);
    CHECK(app->documents().size() == (after ? 1u : 0u));
    if (!r) {
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("cancel racing document commands from another thread: Cancelled if and only if nothing changed",
          "[api][cancel][close][stress]")
{
    auto app = makeTestApp("");
    REQUIRE(app->newDocument(""));
    OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    // A few seconds at most (fewer rounds in a slow sanitizer build); every round checks the invariant.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    int cancelled = 0;
    int committed = 0;
    // One canceller thread for all rounds (no thread start in the timing): each round it waits `delay` after the
    // command was invoked, then cancels. The delays sweep 0 to 150 us, about the time one command takes in Release,
    // so cancels land before the command runs, while it is prepared, while its hop waits for the UI thread, inside
    // the commit and after the job ended.
    std::mutex mutex;
    std::condition_variable changed;
    Handle armed;                                 // guarded by mutex
    bool pending = false;                         // guarded: `armed` waits for its cancel
    bool quit = false;                            // guarded
    bool took = false;                            // guarded: what the last cancel returned
    std::chrono::steady_clock::time_point fireAt; // guarded
    std::thread canceller([&] {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return pending || quit; });
            if (quit) {
                return;
            }
            Handle job = armed;
            const auto at = fireAt;
            lock.unlock();
            while (std::chrono::steady_clock::now() < at) {
                // busy: a sleep would round the delay up to the timer resolution
            }
            const bool result = job.cancel();
            lock.lock();
            took = result;
            pending = false;
            changed.notify_all();
        }
    });
    // Stops and joins the canceller also when a REQUIRE below ends the test early.
    struct Stop {
        std::mutex& mutex;
        std::condition_variable& changed;
        bool& quit;
        std::thread& thread;
        ~Stop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                quit = true;
            }
            changed.notify_all();
            thread.join();
        }
    } stop{mutex, changed, quit, canceller};
    for (int round = 0; round < 10000 && std::chrono::steady_clock::now() < deadline; ++round) {
        const DocSnapshot before = snapshot(*od);
        const auto delay = std::chrono::microseconds((round * 37) % 150);
        Handle job = app->actions().invoke("param.set", setParam(1000 + round), Caller::User);
        {
            std::lock_guard<std::mutex> lock(mutex);
            armed = job;
            fireAt = std::chrono::steady_clock::now() + delay;
            pending = true;
        }
        changed.notify_all();
        auto r = app->wait(job, kWait);
        bool tookEffect = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return !pending; });
            tookEffect = took;
        }
        INFO("round " << round << ", cancel after " << delay.count() << " us");
        requireIff(r, tookEffect, before, snapshot(*od));
        (r ? committed : cancelled) += 1;
    }
    INFO("cancelled " << cancelled << ", committed " << committed);
    CHECK(cancelled + committed > 0);
}
