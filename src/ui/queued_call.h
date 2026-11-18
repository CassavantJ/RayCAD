// Queued calls into the event loop of another thread (usually from a worker to the UI thread), in a form
// ThreadSanitizer can follow.
//
// QMetaObject::invokeMethod(context, f, Qt::QueuedConnection) on thread A moves `f` into a heap object, and
// QCoreApplication::postEvent hands that object to the thread of `context` through the posted-event queue, under the
// queue's lock; that thread runs `f` and then destroys it. The lock orders everything A wrote before posting (`f`, its
// captures, whatever they point to) before everything the receiving thread does with them. Qt is not built with
// TSan and its QMutex is a futex, so TSan does not see that lock. It sees an ordering only when the receiving thread
// was asleep and woke through the event dispatcher's wake-up descriptor (an intercepted write and read); when that
// thread takes the call without sleeping first (it was already sending posted events, for example inside
// QTest::keyClick), TSan reports its first reads of `f` as races with A's writes.
//
// invokeQueued makes the same call with `f` wrapped in QueuedHandOff. Under TSan the wrapper states Qt's ordering and
// nothing more: a release on the address of the heap copy as invokeMethod constructs it (after A's last write that
// the post orders), and an acquire on that address when the receiving thread runs the copy or destroys it unrun (a
// context deleted before the call), before its first access. What A writes after the post is not covered and is
// still checked. Without TSan the wrapper only forwards the call, so every build runs the same code.
#pragma once

#include <QtCore/QMetaObject>
#include <QtCore/QObject>

#include <type_traits>
#include <utility>

#if defined(__SANITIZE_THREAD__)
#define RAY_UI_TSAN_HANDOFF 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RAY_UI_TSAN_HANDOFF 1
#endif
#endif

#if defined(RAY_UI_TSAN_HANDOFF)
#include <sanitizer/tsan_interface.h>
#endif

namespace ray::ui {

namespace detail {

template <class F>
class QueuedHandOff {
public:
    explicit QueuedHandOff(F f) : f_(std::move(f)) {}
    // invokeMethod moves the wrapper into the object it posts: the last write of the posting thread to it.
    QueuedHandOff(QueuedHandOff&& other) noexcept(std::is_nothrow_move_constructible_v<F>) : f_(std::move(other.f_))
    {
#if defined(RAY_UI_TSAN_HANDOFF)
        __tsan_release(this);
#endif
    }
    QueuedHandOff(const QueuedHandOff&) = delete;
    QueuedHandOff& operator=(const QueuedHandOff&) = delete;
    QueuedHandOff& operator=(QueuedHandOff&&) = delete;
    ~QueuedHandOff()
    {
#if defined(RAY_UI_TSAN_HANDOFF)
        __tsan_acquire(this);
#endif
    }

    void operator()()
    {
#if defined(RAY_UI_TSAN_HANDOFF)
        __tsan_acquire(this);
#endif
        f_();
    }

private:
    F f_;
};

} // namespace detail

// Queues `f` to run on the thread of `context` (Qt::QueuedConnection, never inline); it is dropped unrun if `context`
// is deleted first. Returns invokeMethod's result.
template <class F>
bool invokeQueued(QObject* context, F&& f)
{
    using Fn = std::decay_t<F>;
    return QMetaObject::invokeMethod(context, detail::QueuedHandOff<Fn>(Fn(std::forward<F>(f))), Qt::QueuedConnection);
}

} // namespace ray::ui
