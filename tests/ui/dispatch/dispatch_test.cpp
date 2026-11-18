// Queued calls from a worker thread to the UI thread through Qt's event queue: QtUiDispatcher (IUiDispatcher of the
// application, which UiBridge::call, the job system's continuations and the app's notifications post through) and
// invokeQueued (queued_call.h).
//
//   - every closure a worker posts runs once, in order, on the UI thread, and sees what the worker wrote before
//     posting; a worker that waits for its closure the way UiBridge::call does gets its result
//   - closures dropped unrun (their context deleted first) are destroyed on the UI thread
//
// Under the linux-clang-tsan preset these tests must stay free of ThreadSanitizer reports. The UI thread drains with
// QCoreApplication::sendPostedEvents only, so no synchronisation TSan can see orders the two threads except the
// hand-off itself; the hidden [.tsan-selftest] case makes the same hand-off with a bare invokeMethod, and CTest
// requires TSan to report it (ray_ui_dispatch_tsan_selftest): proof that these tests would catch a hand-off TSan
// cannot follow.
#include "queued_call.h"
#include "scene_jobs.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QThread>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPosts = 200;

// Runs the UI thread's posted calls until `done` or 10 s. No event loop, no timers: nothing but the posted-event
// queue connects this thread to the worker.
template <class Done>
bool drainUntil(Done done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > 10000) {
            return false;
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        std::this_thread::yield();
    }
    return true;
}

// What a worker builds before it posts: heap data the UI thread reads and, at the end, frees.
struct Payload {
    int index = 0;
    std::vector<int> values;
    std::string text;
};

std::shared_ptr<Payload> makePayload(int i)
{
    auto p = std::make_shared<Payload>();
    p->index = i;
    p->values.assign(32, i);
    p->text = "payload " + std::to_string(i);
    return p;
}

bool matches(const Payload& p, int i)
{
    return p.index == i && p.values.size() == 32 && p.values.front() == i && p.values.back() == i &&
           p.text == "payload " + std::to_string(i);
}

} // namespace

TEST_CASE("closures a worker posts through QtUiDispatcher run in order on the UI thread", "[dispatch]")
{
    ray::ui::QtUiDispatcher dispatcher;
    QThread* const ui = QThread::currentThread();
    std::vector<int> seen; // UI thread only
    bool allOnUi = true;   // UI thread only
    std::thread worker([&] {
        for (int i = 0; i < kPosts; ++i) {
            auto payload = makePayload(i);
            dispatcher.post([payload, i, ui, &seen, &allOnUi] {
                allOnUi = allOnUi && QThread::currentThread() == ui;
                seen.push_back(matches(*payload, i) ? i : -1);
            });
        }
    });
    const bool drained = drainUntil([&] { return seen.size() == static_cast<std::size_t>(kPosts); });
    worker.join();
    REQUIRE(drained);
    CHECK(allOnUi);
    for (int i = 0; i < kPosts; ++i) {
        INFO(i);
        CHECK(seen[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("a worker waiting for its closure the way UiBridge::call does gets the result", "[dispatch]")
{
    // The shape of UiBridge::callErased: the closure captures a shared rendezvous and the work; the UI thread takes
    // the rendezvous' mutex (reading the captured pointer first), runs the work and marks it done; the worker waits
    // on that mutex and then reads the result.
    struct Rendezvous {
        std::mutex mutex;
        std::condition_variable changed;
        bool running = false;
        bool done = false;
    };
    ray::ui::QtUiDispatcher dispatcher;
    std::atomic<int> results{0};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        for (int i = 0; i < kPosts; ++i) {
            auto r = std::make_shared<Rendezvous>();
            auto slot = std::make_shared<int>(-1);
            std::function<void()> work = [slot, payload = makePayload(i), i] {
                *slot = matches(*payload, i) ? i : -2;
            };
            dispatcher.post([r, work = std::move(work)] {
                {
                    std::lock_guard<std::mutex> lock(r->mutex);
                    r->running = true;
                }
                work();
                {
                    std::lock_guard<std::mutex> lock(r->mutex);
                    r->done = true;
                }
                r->changed.notify_all();
            });
            std::unique_lock<std::mutex> lock(r->mutex);
            r->changed.wait(lock, [&] { return r->done; });
            if (*slot == i) {
                results.fetch_add(1, std::memory_order_relaxed);
            }
        }
        finished.store(true, std::memory_order_relaxed);
    });
    const bool drained = drainUntil([&] { return finished.load(std::memory_order_relaxed); });
    worker.join();
    REQUIRE(drained);
    CHECK(results.load() == kPosts);
}

TEST_CASE("invokeQueued runs a lambda from a worker on the context's thread", "[dispatch]")
{
    QObject context;
    std::vector<int> seen; // UI thread only
    int accepted = 0;      // worker only until joined
    std::thread worker([&] {
        for (int i = 0; i < kPosts; ++i) {
            std::vector<std::string> keys{"units.length", "view.tier", std::to_string(i)};
            if (ray::ui::invokeQueued(&context, [keys = std::move(keys), payload = makePayload(i), i, &seen] {
                    const bool ok = keys.size() == 3 && keys[2] == std::to_string(i) && matches(*payload, i);
                    seen.push_back(ok ? i : -1);
                })) {
                ++accepted;
            }
        }
    });
    const bool drained = drainUntil([&] { return seen.size() == static_cast<std::size_t>(kPosts); });
    worker.join();
    CHECK(accepted == kPosts);
    REQUIRE(drained);
    for (int i = 0; i < kPosts; ++i) {
        INFO(i);
        CHECK(seen[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("closures dropped unrun with their context are destroyed on the UI thread", "[dispatch]")
{
    // As when QtUiDispatcher goes with calls still queued (its context object is deleted). The worker gets the
    // context's address only, so that deleting it here needs no synchronisation with the worker (QtUiDispatcher
    // itself must outlive the workers that post to it).
    auto context = std::make_unique<QObject>();
    QObject* const target = context.get();
    int ran = 0; // UI thread only
    // Relaxed: only says that the worker is done posting, and orders nothing (so TSan sees no synchronisation).
    std::atomic<int> posted{0};
    std::thread worker([target, &ran, &posted] {
        for (int i = 0; i < kPosts; ++i) {
            std::function<void()> fn = [payload = makePayload(i), &ran] {
                ran += payload->index >= 0 ? 1 : 0;
            };
            ray::ui::invokeQueued(target, std::move(fn));
            posted.fetch_add(1, std::memory_order_relaxed);
        }
    });
    QElapsedTimer timer;
    timer.start();
    while (posted.load(std::memory_order_relaxed) < kPosts && timer.elapsed() < 10000) {
        std::this_thread::yield();
    }
    const bool allPosted = posted.load(std::memory_order_relaxed) == kPosts;
    if (!allPosted) {
        worker.join(); // the worker must not post to a deleted context
    }
    // Deleting the receiver removes its queued calls: each closure and its payload is destroyed here, unrun.
    context.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    if (worker.joinable()) {
        worker.join();
    }
    REQUIRE(allPosted);
    CHECK(ran == 0);
}

// Hidden: races on purpose, as far as TSan can tell. The same hand-off as the first test case with a bare
// QMetaObject::invokeMethod; under TSan, CTest's ray_ui_dispatch_tsan_selftest passes only if TSan reports it.
TEST_CASE("TSan reports a bare queued invokeMethod from a worker", "[.tsan-selftest]")
{
    QObject context;
    std::vector<int> seen; // UI thread only
    std::thread worker([&] {
        for (int i = 0; i < kPosts; ++i) {
            std::function<void()> fn = [payload = makePayload(i), i, &seen] {
                seen.push_back(matches(*payload, i) ? i : -1);
            };
            QMetaObject::invokeMethod(&context, std::move(fn), Qt::QueuedConnection);
        }
    });
    const bool drained = drainUntil([&] { return seen.size() == static_cast<std::size_t>(kPosts); });
    worker.join();
    CHECK(drained);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
