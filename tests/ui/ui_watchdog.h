// The UI-thread watchdog of the stall tests ([wallclock]): PLAN.md 7, the UI thread never blocks for long.
//
// A 2 ms timer on the UI thread; the time between two of its ticks is a gap, time in which the event loop did not
// come round. On a loaded machine (other builds and tests beside it: the normal case on a developer's machine, and CI
// runners are slower still) the wall-clock length of a gap also measures the machine: time in which the UI thread
// was ready to run and Windows ran something else, or in which it waited for the render thread and Windows did not
// run that. So on Windows a gap's stall is what the UI thread itself did in it:
//   - the time it ran, from its cycle counter (QueryThreadCycleTime; exact, where the thread times only advance every
//     15.6 ms), and
//   - the time it was seen blocked: while a gap is open a watcher thread samples the scheduler's state of the UI
//     thread and of Qt Quick's render thread (each sample takes a few ms on a busy machine; a wait shorter than that
//     can go unseen). A wait counts unless the UI thread was waiting for events in its event loop (between the event
//     dispatcher's aboutToBlock and awake: idle, the tick just came late) or the render thread was runnable and not
//     run (the threaded render loop's sync, where the UI thread waits for the render thread, on a starved render
//     thread). A wait for a lock, a worker or a file counts, and so does a busy render thread the UI thread waits
//     for.
// Time the UI thread was runnable and not run does not count. Elsewhere the stall is the whole wall-clock gap.
//
// QTest::qWait and QTest::qWaitFor sleep the UI thread for up to 10 ms between their passes of the event loop. In a
// measured stretch that sleep was part of the worst gap (a 10 ms sleep lasts a tick or two of the system timer,
// 15.6 or 31.2 ms, and longer on a loaded machine): the stall tests wait there with waitFor and waitMs below, which
// wait for events in the event loop instead.
//
// The report names the event that the longest stretch inside the gap began with (an application-wide event filter),
// so a failure says what held the thread.
#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ray::test {

// As QTest::qWaitFor and QTest::qWait, without sleeping the UI thread: between passes it waits in the event loop for
// the next event (a 1 ms timer makes sure there is one).
bool waitFor(const std::function<bool()>& predicate, int timeoutMs);
void waitMs(int ms);

class UiWatchdog : public QObject {
public:
    struct Gap {
        qint64 atMs = 0;       // when it began, from start()
        double wallMs = 0;     // the wall-clock gap
        bool measured = false; // the thread's own time is known (else the stall is the wall-clock gap)
        double ranMs = 0;      // the UI thread ran
        double waitedMs = 0;   // it was seen blocked
        int waitReason = -1;   // the commonest KWAIT_REASON of those samples
        double readyMs = 0;    // it was seen runnable and not run
        double starvedMs = 0;  // it was seen waiting while the render thread was runnable and not run
        double idleMs = 0;     // it was seen waiting for events in the event loop
        int samples = 0;       // thread-state samples taken in the gap
        bool renderThread = false;
        // The longest stretch between two event deliveries inside the gap, and the event that began it.
        double longestMs = 0;
        const char* receiver = "";
        int eventType = 0;
        int events = 0;

        // The time the UI thread was busy or blocked: what the tests judge.
        double stallMs() const;
        std::string describe() const;
    };

    UiWatchdog();
    ~UiWatchdog() override;

    void start();
    void stop();
    // Starts a new worst: only the gaps from here on count.
    void reset();
    // The gap with the worst stall since start() or reset(), and the longest wall-clock gap (for the report).
    Gap worst() const;
    Gap worstWall() const;
    double worstStallMs() const { return worst().stallMs(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // What the samples saw in one gap, in ns.
    struct Sampled {
        qint64 ready = 0, starved = 0, waited = 0, idle = 0;
        int samples = 0, failed = 0;
        std::map<int, int> reasons; // of the samples that saw it blocked
    };
    void onTick();
    void watch();
    Gap complete(Gap g, std::uint64_t serial) const; // with what the samples saw; under the lock

    QTimer tick_;
    QElapsedTimer clock_;
    qint64 lastNs_ = 0;
    qint64 lastEventNs_ = 0;
    const char* lastReceiver_ = "";
    int lastType_ = 0;
    Gap current_;
    std::uint64_t lastCycles_ = 0;
    double tscPerNs_ = 0;
    QMetaObject::Connection blocking_, awake_;

    // The watcher thread (Windows).
    unsigned long uiThreadId_ = 0;
    std::atomic<unsigned long> renderThreadId_{0};
    std::thread watcher_;
    std::atomic<bool> stopWatcher_{false};
    std::atomic<qint64> lastTickNs_{0};
    std::atomic<bool> idle_{false}; // the UI thread waits for events (from the dispatcher's aboutToBlock to awake)

    mutable std::mutex mutex_;                        // guards the four below
    std::uint64_t gapSerial_ = 0;                     // of the gap that is open
    std::vector<std::pair<std::uint64_t, Gap>> gaps_; // the gaps of 4 ms or more, with their serials
    std::map<std::uint64_t, Sampled> sampled_;        // by gap serial
    Gap worstShort_;                                  // the worst of the shorter gaps (never sampled)
};

} // namespace ray::test
