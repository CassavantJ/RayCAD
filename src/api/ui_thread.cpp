#include "ui_thread.h"

#include <algorithm>

namespace ray::api::detail {

void QueueDispatcher::post(std::function<void()> fn)
{
    if (!fn) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(fn));
    }
    changed_.notify_all();
}

bool QueueDispatcher::runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Wake up now and then even without work: done() may depend on other threads (a job finishing).
            const auto wakeAt = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
            changed_.wait_until(lock, wakeAt, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    lock.unlock();
                    return done();
                }
                continue;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
    }
    return true;
}

std::size_t QueueDispatcher::runPending()
{
    std::size_t ran = 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return ran;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
        ++ran;
    }
}

namespace {
struct Rendezvous {
    enum class Phase { Pending, Running, Done, Abandoned };
    std::mutex mutex;
    std::condition_variable changed;
    Phase phase = Phase::Pending;
};
} // namespace

Status UiBridge::callErased(std::function<void()> fn, const CancelToken& cancel)
{
    const auto cancelled = [] {
        return tl::unexpected<Error>(Error{ErrorCode::Cancelled, "Cancelled", {}});
    };
    if (shutdown_.load() || cancel.cancelled()) {
        return cancelled();
    }
    auto r = std::make_shared<Rendezvous>();
    dispatcher_.post([r, fn = std::move(fn)]() {
        {
            std::lock_guard<std::mutex> lock(r->mutex);
            if (r->phase == Rendezvous::Phase::Abandoned) {
                return;
            }
            r->phase = Rendezvous::Phase::Running;
        }
        fn(); // never throws: call() wraps it
        {
            std::lock_guard<std::mutex> lock(r->mutex);
            r->phase = Rendezvous::Phase::Done;
        }
        r->changed.notify_all();
    });
    std::unique_lock<std::mutex> lock(r->mutex);
    for (;;) {
        if (r->phase == Rendezvous::Phase::Done) {
            return {};
        }
        if (r->phase == Rendezvous::Phase::Pending && (cancel.cancelled() || shutdown_.load())) {
            r->phase = Rendezvous::Phase::Abandoned;
            return cancelled();
        }
        r->changed.wait_for(lock, std::chrono::milliseconds(10));
    }
}

} // namespace ray::api::detail
