// IJobSystem::submit and JobHandle (task T-003).
#include <catch2/catch_test_macros.hpp>

#include "test_dispatcher.h"

#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using ray::test::TestDispatcher;

namespace {

// Spins until `flag` is set or five seconds pass.
bool waitFor(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    return flag.load();
}

// A job that runs until it is cancelled.
std::function<ray::Result<int>(ray::JobContext&)> untilCancelled(std::atomic<bool>& started)
{
    return [&started](ray::JobContext& ctx) -> ray::Result<int> {
        started = true;
        while (!ctx.cancel.cancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        return 42; // the job system turns this into Cancelled
    };
}

} // namespace

TEST_CASE("a job returns its value through wait", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    CHECK(jobs->workerCount() == 2);
    ray::JobHandle<int> handle =
        jobs->submit<int>({"answer"}, [](ray::JobContext&) -> ray::Result<int> { return 6 * 7; });
    CHECK(handle.valid());
    ray::Result<int> result = handle.wait();
    REQUIRE(result.has_value());
    CHECK(*result == 42);
    CHECK(handle.done());
    CHECK(handle.wait().value() == 42); // the result can be read again
}

TEST_CASE("an error result is passed through unchanged", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    auto handle = jobs->submit<int>({"fails"}, [](ray::JobContext&) -> ray::Result<int> {
        return tl::unexpected<ray::Error>(ray::Error{ray::ErrorCode::NotFound, "missing", {"inner"}});
    });
    const ray::Result<int> result = handle.wait();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ray::ErrorCode::NotFound);
    CHECK(result.error().message == "missing");
}

TEST_CASE("then continuations run on the dispatcher thread", "[jobs][then]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    const std::thread::id uiThread = std::this_thread::get_id();

    SECTION("attached before the job finishes")
    {
        std::atomic<bool> release{false};
        std::thread::id workerThread;
        auto handle = jobs->submit<std::string>({"text"}, [&](ray::JobContext&) -> ray::Result<std::string> {
            workerThread = std::this_thread::get_id();
            while (!release.load()) {
                std::this_thread::sleep_for(100us);
            }
            return std::string("done");
        });
        bool ran = false;
        handle.then([&](ray::Result<std::string> result) {
            CHECK(std::this_thread::get_id() == uiThread);
            REQUIRE(result.has_value());
            CHECK(*result == "done");
            ran = true;
        });
        release = true;
        CHECK(dispatcher.runUntil([&] { return ran; }));
        CHECK(workerThread != uiThread); // read after the continuation: the job has finished
    }
    SECTION("attached after the job finished")
    {
        auto handle = jobs->submit<int>({"quick"}, [](ray::JobContext&) -> ray::Result<int> { return 7; });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!handle.done() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(100us);
        }
        REQUIRE(handle.done());
        int calls = 0;
        handle.then([&](ray::Result<int> result) {
            CHECK(std::this_thread::get_id() == uiThread);
            CHECK(result.value() == 7);
            ++calls;
        });
        CHECK(calls == 0); // posted, not run inside then()
        handle.then([&](ray::Result<int> result) {
            CHECK(result.value() == 7);
            ++calls;
        });
        CHECK(dispatcher.runUntil([&] { return calls == 2; }));
    }
    SECTION("from many jobs at once")
    {
        constexpr int kJobs = 200;
        int seen = 0;
        long long sum = 0;
        std::vector<ray::JobHandle<int>> handles;
        for (int i = 0; i < kJobs; ++i) {
            handles.push_back(jobs->submit<int>({"many"}, [i](ray::JobContext&) -> ray::Result<int> { return i; }));
            handles.back().then([&](ray::Result<int> result) {
                CHECK(std::this_thread::get_id() == uiThread);
                sum += result.value();
                ++seen;
            });
        }
        CHECK(dispatcher.runUntil([&] { return seen == kJobs; }));
        CHECK(sum == static_cast<long long>(kJobs) * (kJobs - 1) / 2);
    }
}

TEST_CASE("jobs submitted from several threads all complete", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::atomic<int> ran{0};
    std::vector<std::thread> threads;
    std::mutex handlesMutex;
    std::vector<ray::JobHandle<int>> handles;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const ray::JobPriority priority = static_cast<ray::JobPriority>((t + i) % 3);
                auto handle =
                    jobs->submit<int>({"from thread", priority}, [&ran, i](ray::JobContext&) -> ray::Result<int> {
                        ++ran;
                        return i;
                    });
                std::lock_guard<std::mutex> lock(handlesMutex);
                handles.push_back(std::move(handle));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (auto& handle : handles) {
        CHECK(handle.wait().has_value());
    }
    CHECK(ran.load() == kThreads * kPerThread);
}

TEST_CASE("a cancelled job returns Cancelled within 100 ms", "[jobs][cancel]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    for (int round = 0; round < 50; ++round) {
        std::atomic<bool> started{false};
        auto handle = jobs->submit<int>({"long", static_cast<ray::JobPriority>(round % 3)}, untilCancelled(started));
        REQUIRE(waitFor(started));
        const auto t0 = std::chrono::steady_clock::now();
        handle.cancel();
        const ray::Result<int> result = handle.wait();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        INFO("round " << round);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ray::ErrorCode::Cancelled);
        CHECK(elapsed < 100ms);
    }
}

TEST_CASE("a job that uses check() stops with Cancelled", "[jobs][cancel]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    std::atomic<bool> started{false};
    auto handle = jobs->submit<int>({"checks"}, [&started](ray::JobContext& ctx) -> ray::Result<int> {
        started = true;
        for (;;) {
            if (ray::Status status = ctx.cancel.check(); !status) {
                return tl::unexpected<ray::Error>(status.error());
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    REQUIRE(waitFor(started));
    handle.cancel();
    CHECK(handle.wait().error().code == ray::ErrorCode::Cancelled);
}

TEST_CASE("a job cancelled before it starts never runs and is done at once", "[jobs][cancel]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1);
    std::atomic<bool> blockerStarted{false};
    auto blocker = jobs->submit<int>({"blocker"}, untilCancelled(blockerStarted));
    REQUIRE(waitFor(blockerStarted)); // the only worker is busy now

    std::atomic<bool> queuedRan{false};
    auto queued = jobs->submit<int>({"queued"}, [&queuedRan](ray::JobContext&) -> ray::Result<int> {
        queuedRan = true;
        return 1;
    });
    bool continued = false;
    queued.then([&](ray::Result<int> result) {
        CHECK(result.error().code == ray::ErrorCode::Cancelled);
        continued = true;
    });
    CHECK_FALSE(queued.done());
    queued.cancel();
    CHECK(queued.done());
    CHECK(queued.wait().error().code == ray::ErrorCode::Cancelled);
    CHECK(dispatcher.runUntil([&] { return continued; }));

    blocker.cancel();
    CHECK(blocker.wait().error().code == ray::ErrorCode::Cancelled);
    // The worker picks up the queued task now and must skip it.
    auto after = jobs->submit<int>({"after"}, [](ray::JobContext&) -> ray::Result<int> { return 2; });
    CHECK(after.wait().value() == 2);
    jobs.reset();
    CHECK_FALSE(queuedRan.load());
}

TEST_CASE("cancelling a finished job keeps its result", "[jobs][cancel]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    auto handle = jobs->submit<int>({"quick"}, [](ray::JobContext&) -> ray::Result<int> { return 5; });
    REQUIRE(handle.wait().value() == 5);
    handle.cancel();
    CHECK(handle.wait().value() == 5);
}

TEST_CASE("exceptions inside a job become Internal and never escape", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);

    auto stdException =
        jobs->submit<int>({"throws"}, [](ray::JobContext&) -> ray::Result<int> { throw std::runtime_error("boom"); });
    const ray::Result<int> first = stdException.wait();
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == ray::ErrorCode::Internal);
    CHECK(first.error().message.find("boom") != std::string::npos);
    REQUIRE(first.error().context.size() == 1);
    CHECK(first.error().context[0] == "throws");

    auto otherException = jobs->submit<int>({"throws int"}, [](ray::JobContext&) -> ray::Result<int> { throw 7; });
    CHECK(otherException.wait().error().code == ray::ErrorCode::Internal);

    bool continued = false;
    otherException.then([&](ray::Result<int> result) {
        CHECK(result.error().code == ray::ErrorCode::Internal);
        continued = true;
    });
    CHECK(dispatcher.runUntil([&] { return continued; }));

    // An empty function is a job that cannot run, not a crash.
    auto empty = jobs->submit<int>({"empty"}, std::function<ray::Result<int>(ray::JobContext&)>{});
    CHECK(empty.wait().error().code == ray::ErrorCode::Internal);

    // The job system still works afterwards.
    CHECK(jobs->submit<int>({"after"}, [](ray::JobContext&) -> ray::Result<int> { return 1; }).wait().value() == 1);
}

TEST_CASE("destroying the job system with jobs running is clean", "[jobs][shutdown]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    constexpr int kRunning = 2;
    constexpr int kQueued = 50;
    std::vector<std::atomic<bool>> started(kRunning);
    std::vector<ray::JobHandle<int>> handles;
    int continuations = 0;
    for (int i = 0; i < kRunning; ++i) {
        handles.push_back(jobs->submit<int>({"running"}, untilCancelled(started[static_cast<std::size_t>(i)])));
    }
    // One job ignores cancellation for a while; destruction waits for it.
    std::atomic<bool> stubbornStarted{false};
    std::atomic<bool> stubbornFinished{false};
    handles.push_back(jobs->submit<int>({"stubborn"}, [&](ray::JobContext& ctx) -> ray::Result<int> {
        stubbornStarted = true;
        while (!ctx.cancel.cancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(50ms);
        stubbornFinished = true;
        return 3;
    }));
    // A job that submits more work while the system shuts down (the system is alive until its destructor
    // has waited for this job).
    ray::IJobSystem* system = jobs.get();
    std::atomic<bool> spawnerStarted{false};
    std::atomic<int> childCode{-1};
    handles.push_back(jobs->submit<int>({"spawner"}, [&, system](ray::JobContext& ctx) -> ray::Result<int> {
        spawnerStarted = true;
        while (!ctx.cancel.cancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        auto child = system->submit<int>({"child"}, [](ray::JobContext&) -> ray::Result<int> { return 9; });
        childCode = static_cast<int>(child.wait().error().code);
        return 0;
    }));
    for (auto& flag : started) {
        REQUIRE(waitFor(flag));
    }
    REQUIRE(waitFor(stubbornStarted));
    REQUIRE(waitFor(spawnerStarted));
    // All four workers are busy; these stay queued.
    std::atomic<int> queuedRan{0};
    for (int i = 0; i < kQueued; ++i) {
        handles.push_back(jobs->submit<int>({"queued"}, [&queuedRan](ray::JobContext&) -> ray::Result<int> {
            ++queuedRan;
            return 1;
        }));
    }
    for (auto& handle : handles) {
        handle.then([&continuations](ray::Result<int>) { ++continuations; });
    }

    const auto t0 = std::chrono::steady_clock::now();
    jobs.reset();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(childCode.load() == static_cast<int>(ray::ErrorCode::Cancelled));
    CHECK(elapsed < 2s);

    for (std::size_t i = 0; i < handles.size(); ++i) {
        INFO("job " << i);
        CHECK(handles[i].done());
    }
    for (int i = 0; i < kRunning; ++i) {
        CHECK(handles[static_cast<std::size_t>(i)].wait().error().code == ray::ErrorCode::Cancelled);
    }
    CHECK(stubbornFinished.load());
    CHECK(handles[kRunning].wait().error().code == ray::ErrorCode::Cancelled); // its 3 is discarded
    CHECK(queuedRan.load() == 0);
    // Every continuation was posted before the system went away.
    dispatcher.runPending();
    CHECK(continuations == static_cast<int>(handles.size()));

    // A handle outliving its job system still works; new continuations are dropped.
    CHECK(handles[0].wait().error().code == ray::ErrorCode::Cancelled);
    handles[0].then([](ray::Result<int>) { FAIL("posted after the job system was destroyed"); });
    CHECK(dispatcher.runPending() == 0);
}

TEST_CASE("progress of a job reaches the dispatcher thread, final 1.0 before the result", "[jobs][progress]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    const std::thread::id uiThread = std::this_thread::get_id();
    std::atomic<bool> go{false};
    auto handle = jobs->submit<int>({"progress"}, [&go](ray::JobContext& ctx) -> ray::Result<int> {
        while (!go.load()) {
            std::this_thread::sleep_for(100us);
        }
        ray::Progress first = ctx.progress.sub(0.5);
        ray::Progress second = ctx.progress.sub(0.5);
        for (int i = 0; i <= 100; ++i) {
            first.set(i / 100.0, "first");
            std::this_thread::sleep_for(1ms);
        }
        for (int i = 0; i < 100; ++i) {
            second.set(i / 100.0, "second"); // never reaches 1.0; the job system reports it at the end
            std::this_thread::sleep_for(1ms);
        }
        return 1;
    });
    std::vector<double> fractions;
    bool finished = false;
    bool progressAfterResult = false;
    handle.onProgress([&](double fraction, std::string) {
        CHECK(std::this_thread::get_id() == uiThread);
        progressAfterResult = progressAfterResult || finished;
        fractions.push_back(fraction);
    });
    handle.then([&](ray::Result<int> result) {
        CHECK(result.value() == 1);
        finished = true;
    });
    go = true;
    REQUIRE(dispatcher.runUntil([&] { return finished; }));
    dispatcher.runPending();
    REQUIRE_FALSE(fractions.empty());
    CHECK(fractions.back() == 1.0);
    CHECK_FALSE(progressAfterResult);
    // About 200 ms of work at 30 Hz at most; generous for a slow machine, far below the 201 set() calls.
    CHECK(fractions.size() < 100);
}

TEST_CASE("wait inside a job on a job that has not started runs it inline", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1); // the outer job holds the only worker
    auto outer = jobs->submit<int>({"outer"}, [&jobs](ray::JobContext&) -> ray::Result<int> {
        auto inner = jobs->submit<int>({"inner"}, [](ray::JobContext&) -> ray::Result<int> { return 20; });
        return inner.wait().value() + 1;
    });
    CHECK(outer.wait().value() == 21);
}

TEST_CASE("a deferred job runs only through runDeferred, on the caller's thread", "[jobs][defer]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    std::atomic<int> ran{0};
    std::thread::id ranOn;
    auto handle = jobs->defer<int>({"deferred"}, [&](ray::JobContext&) -> ray::Result<int> {
        ranOn = std::this_thread::get_id();
        ++ran;
        return 5;
    });
    bool continued = false;
    handle.then([&](ray::Result<int> r) { continued = r.has_value() && *r == 5; });
    // Nothing runs it: no worker picks it up.
    std::this_thread::sleep_for(50ms);
    dispatcher.runPending();
    CHECK(ran.load() == 0);
    CHECK_FALSE(handle.done());
    CHECK_FALSE(continued);

    // wait() from another thread does not run it; it ends when runDeferred() has run it here.
    std::atomic<bool> waited{false};
    std::atomic<int> waitedValue{0};
    std::thread waiter([&] {
        const ray::Result<int> r = handle.wait();
        waitedValue = r ? *r : -1;
        waited = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK(ran.load() == 0);
    CHECK_FALSE(waited.load());
    handle.runDeferred();
    waiter.join();
    CHECK(ran.load() == 1);
    CHECK(ranOn == std::this_thread::get_id());
    CHECK(handle.done());
    CHECK(waited.load());
    CHECK(waitedValue.load() == 5);
    handle.runDeferred(); // done: does nothing
    CHECK(ran.load() == 1);
    REQUIRE(dispatcher.runUntil([&] { return continued; }));
}

TEST_CASE("a deferred job cancelled before it runs ends at once and never runs", "[jobs][defer]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1);
    std::atomic<int> ran{0};
    auto handle = jobs->defer<int>({"deferred"}, [&](ray::JobContext&) -> ray::Result<int> {
        ++ran;
        return 1;
    });
    bool cancelled = false;
    handle.then([&](ray::Result<int> r) { cancelled = !r && r.error().code == ray::ErrorCode::Cancelled; });
    handle.cancel();
    CHECK(handle.done()); // at once, without a worker or a runDeferred()
    handle.runDeferred();
    CHECK(ran.load() == 0);
    CHECK(handle.wait().error().code == ray::ErrorCode::Cancelled);
    REQUIRE(dispatcher.runUntil([&] { return cancelled; }));
}

TEST_CASE("a deferred job run after its job system is gone completes as cancelled", "[jobs][defer]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1);
    std::atomic<int> ran{0};
    auto handle = jobs->defer<int>({"deferred"}, [&](ray::JobContext&) -> ray::Result<int> {
        ++ran;
        return 1;
    });
    jobs.reset();
    handle.runDeferred();
    CHECK(handle.done());
    CHECK(ran.load() == 0);
    CHECK(handle.wait().error().code == ray::ErrorCode::Cancelled);
}

TEST_CASE("results that cannot be copied are moved out", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    auto handle = jobs->submit<std::unique_ptr<int>>(
        {"move only"}, [](ray::JobContext&) -> ray::Result<std::unique_ptr<int>> { return std::make_unique<int>(3); });
    ray::Result<std::unique_ptr<int>> result = handle.wait();
    REQUIRE(result.has_value());
    REQUIRE(*result != nullptr);
    CHECK(**result == 3);
}

TEST_CASE("an empty JobHandle is harmless", "[jobs]")
{
    ray::JobHandle<int> handle;
    CHECK_FALSE(handle.valid());
    CHECK(handle.done());
    handle.cancel();
    handle.then([](ray::Result<int>) { FAIL("an empty handle has no result"); });
    CHECK(handle.wait().error().code == ray::ErrorCode::InvalidArgument);
}

TEST_CASE("workerCount follows setWorkerCount and jobs keep running", "[jobs]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher);
    CHECK(jobs->workerCount() >= 1);
    jobs->setWorkerCount(3);
    CHECK(jobs->workerCount() == 3);
    jobs->setWorkerCount(0);
    CHECK(jobs->workerCount() == 1);
    for (const ray::JobPriority priority :
         {ray::JobPriority::Interactive, ray::JobPriority::Normal, ray::JobPriority::Background}) {
        auto handle =
            jobs->submit<int>({"after resize", priority}, [](ray::JobContext&) -> ray::Result<int> { return 4; });
        CHECK(handle.wait().value() == 4);
    }
}
