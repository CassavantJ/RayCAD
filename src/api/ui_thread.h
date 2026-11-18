// The UI thread as seen from src/api: the headless queue dispatcher, and the bridge that lets a worker run a short
// piece of work on the UI thread (creating a document, executing a command) and wait for it. Private to src/api.
#pragma once

#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ray::api::detail {

// A thread-safe queue: post() from any thread; the UI thread runs the functions in runUntil/runPending.
class QueueDispatcher final : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override;
    bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout);
    std::size_t runPending();

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
};

class UiBridge {
public:
    UiBridge(IUiDispatcher& dispatcher, std::thread::id uiThread) : dispatcher_(dispatcher), uiThread_(uiThread) {}

    bool onUiThread() const { return std::this_thread::get_id() == uiThread_; }

    // Runs `fn` on the UI thread and waits for it. On the UI thread itself it runs `fn` at once. If `cancel` is
    // cancelled (or shutdown() was called) before the UI thread started `fn`, `fn` never runs and the result is
    // Cancelled; once started, it is waited for (UI work is short).
    template <class T>
    Result<T> call(std::function<Result<T>()> fn, const CancelToken& cancel)
    {
        if (onUiThread()) {
            return guarded(fn);
        }
        auto slot = std::make_shared<std::optional<Result<T>>>();
        Status ran = callErased([slot, fn = std::move(fn)]() { slot->emplace(guarded(fn)); }, cancel);
        if (!ran) {
            return tl::unexpected<Error>(ran.error());
        }
        return std::move(**slot);
    }

    // After this, call() from a worker returns Cancelled without posting, and waits stop.
    void shutdown() noexcept { shutdown_.store(true); }

private:
    template <class T>
    static Result<T> guarded(const std::function<Result<T>()>& fn)
    {
        try {
            return fn();
        } catch (const std::bad_alloc&) {
            return tl::unexpected<Error>(Error{ErrorCode::Internal, "Out of memory", {}});
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(
                Error{ErrorCode::Internal, std::string("Unexpected failure: ") + e.what(), {}});
        } catch (...) {
            return tl::unexpected<Error>(Error{ErrorCode::Internal, "Unexpected failure", {}});
        }
    }

    Status callErased(std::function<void()> fn, const CancelToken& cancel);

    IUiDispatcher& dispatcher_;
    std::thread::id uiThread_;
    std::atomic<bool> shutdown_{false};
};

} // namespace ray::api::detail
