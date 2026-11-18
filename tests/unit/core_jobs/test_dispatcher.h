// Test double for ray::IUiDispatcher (task T-003). post() may be called from any thread; the posted
// functions run only when a test drains the queue, on the thread that drains it, which plays the UI thread.
#pragma once

#include <ray/core.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace ray::test {

class TestDispatcher final : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(fn));
            ++posted_;
        }
        changed_.notify_all();
    }

    // Runs posted functions on the calling thread until done() is true or `timeout` has passed.
    // Returns done(). done() is only called on the calling thread.
    template <class Done>
    bool runUntil(Done done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!changed_.wait_until(lock, deadline, [this] { return !queue_.empty(); })) {
                    return done();
                }
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            fn();
        }
        return true;
    }

    // Runs everything queued now (and whatever that posts) on the calling thread. Returns how many ran.
    std::size_t runPending()
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

    std::size_t posted() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return posted_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
    std::size_t posted_ = 0;
};

} // namespace ray::test
