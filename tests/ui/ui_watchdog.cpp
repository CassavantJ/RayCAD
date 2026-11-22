// The UI-thread watchdog of the stall tests (ui_watchdog.h says what it measures and why).
#include "ui_watchdog.h"

#include <QtCore/QAbstractEventDispatcher>
#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QEvent>
#include <QtCore/QEventLoop>

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <intrin.h>
#include <tlhelp32.h>
#include <winternl.h>
#endif

namespace ray::test {

namespace {

#ifdef _WIN32
std::uint64_t threadCycles()
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

// The thread of this process whose description (Qt names its threads) is `name`, or 0.
DWORD threadNamed(const wchar_t* name)
{
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto describe = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (describe == nullptr)
        return 0;
    DWORD found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && found == 0; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
            continue;
        PWSTR text = nullptr;
        if (SUCCEEDED(describe(thread, &text)) && text != nullptr) {
            if (std::wstring_view(text) == name)
                found = entry.th32ThreadID;
            LocalFree(text);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}

struct States {
    int ui = -1; // KTHREAD_STATE: 1 Ready, 2 Running, 3 Standby, 5 Waiting, 7 DeferredReady
    int uiWaitReason = -1;
    int render = -1;
};

bool runnable(int state)
{
    return state == 1 || state == 3 || state == 7;
}

// The scheduler states of the UI thread and of the render thread. `buffer` is reused between calls.
bool sampleStates(DWORD ui, DWORD render, std::vector<unsigned char>& buffer, States& out)
{
    using Query = LONG(WINAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (query == nullptr)
        return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed);
        if (status == LONG(0xC0000004L)) { // STATUS_INFO_LENGTH_MISMATCH
            buffer.resize(std::size_t(needed) + 256 * 1024);
            continue;
        }
        if (status < 0)
            return false;
        const DWORD pid = GetCurrentProcessId();
        for (std::size_t at = 0;;) {
            const auto* p = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + at);
            if (DWORD(reinterpret_cast<ULONG_PTR>(p->UniqueProcessId)) == pid) {
                const auto* t = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(p + 1);
                for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
                    const DWORD tid = DWORD(reinterpret_cast<ULONG_PTR>(t[i].ClientId.UniqueThread));
                    if (tid == ui) {
                        out.ui = int(t[i].ThreadState);
                        out.uiWaitReason = int(t[i].WaitReason);
                    } else if (tid == render && render != 0) {
                        out.render = int(t[i].ThreadState);
                    }
                }
                return out.ui >= 0;
            }
            if (p->NextEntryOffset == 0)
                return false;
            at += p->NextEntryOffset;
        }
    }
    return false;
}
#endif

} // namespace

bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    if (predicate())
        return true;
    const QDeadlineTimer deadline(timeoutMs, Qt::PreciseTimer);
    QTimer wake; // an event at least every millisecond, so the wait for events below always ends
    wake.setTimerType(Qt::PreciseTimer);
    wake.setInterval(1);
    wake.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (predicate())
            return true;
    } while (!deadline.hasExpired());
    return predicate();
}

void waitMs(int ms)
{
    const QDeadlineTimer deadline(ms, Qt::PreciseTimer);
    static_cast<void>(waitFor([&] { return deadline.hasExpired(); }, ms + 1000));
}

double UiWatchdog::Gap::stallMs() const
{
    return measured ? std::min(wallMs, ranMs + waitedMs) : wallMs;
}

std::string UiWatchdog::Gap::describe() const
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << "stall " << stallMs() << " ms in a gap of " << wallMs << " ms at " << atMs << " ms";
    if (!measured)
        return out.str() + " (the wall-clock gap: the thread's own time is not measured here)";
    out << ": the UI thread ran " << ranMs << " ms and was seen blocked " << waitedMs << " ms (commonest wait reason "
        << waitReason << "); seen runnable but not run " << readyMs << " ms, waiting for "
        << (renderThread ? "the render thread while that was runnable but not run " : "a render thread not found ")
        << starvedMs << " ms, waiting for events " << idleMs << " ms; " << samples << " samples; " << events
        << " events, the longest stretch " << longestMs << " ms from a " << receiver << " event of type " << eventType;
    return out.str();
}

UiWatchdog::UiWatchdog()
{
    tick_.setTimerType(Qt::PreciseTimer);
    tick_.setInterval(2);
    connect(&tick_, &QTimer::timeout, this, [this] { onTick(); });
}

UiWatchdog::~UiWatchdog()
{
    stop();
}

void UiWatchdog::start()
{
    reset();
    lastCycles_ = 0;
    tscPerNs_ = 0;
    uiThreadId_ = 0;
    renderThreadId_.store(0);
#ifdef _WIN32
    // The thread cycle counter counts at the rate of the time-stamp counter: measured against the wall clock.
    {
        QElapsedTimer t;
        t.start();
        const std::uint64_t c0 = __rdtsc();
        while (t.nsecsElapsed() < 2000000) {
        }
        tscPerNs_ = double(__rdtsc() - c0) / double(t.nsecsElapsed());
    }
    lastCycles_ = threadCycles();
    uiThreadId_ = GetCurrentThreadId();
    stopWatcher_.store(false);
    watcher_ = std::thread([this] { watch(); });
#endif
    idle_.store(false);
    if (QAbstractEventDispatcher* d = QAbstractEventDispatcher::instance()) {
        blocking_ = connect(
            d, &QAbstractEventDispatcher::aboutToBlock, this, [this] { idle_.store(true); }, Qt::DirectConnection);
        awake_ =
            connect(d, &QAbstractEventDispatcher::awake, this, [this] { idle_.store(false); }, Qt::DirectConnection);
    }
    clock_.start();
    lastNs_ = 0;
    lastEventNs_ = 0;
    lastTickNs_.store(0);
    QCoreApplication::instance()->installEventFilter(this);
    tick_.start();
}

void UiWatchdog::stop()
{
    if (!tick_.isActive())
        return;
    tick_.stop();
    QCoreApplication::instance()->removeEventFilter(this);
    disconnect(blocking_);
    disconnect(awake_);
#ifdef _WIN32
    stopWatcher_.store(true);
    if (watcher_.joinable())
        watcher_.join();
#endif
}

void UiWatchdog::reset()
{
    std::lock_guard lock(mutex_);
    gaps_.clear();
    sampled_.clear();
    worstShort_ = Gap{};
    current_ = Gap{};
}

UiWatchdog::Gap UiWatchdog::complete(Gap g, std::uint64_t serial) const
{
    const auto it = sampled_.find(serial);
    if (it == sampled_.end())
        return g;
    const Sampled& s = it->second;
    g.readyMs = double(s.ready) * 1e-6;
    g.starvedMs = double(s.starved) * 1e-6;
    g.waitedMs = double(s.waited) * 1e-6;
    g.idleMs = double(s.idle) * 1e-6;
    g.samples = s.samples;
    g.measured = g.measured && s.failed == 0;
    int most = 0;
    for (const auto& [reason, n] : s.reasons) {
        if (n > most) {
            most = n;
            g.waitReason = reason;
        }
    }
    return g;
}

UiWatchdog::Gap UiWatchdog::worst() const
{
    std::lock_guard lock(mutex_);
    Gap out = worstShort_;
    for (const auto& [serial, g] : gaps_) {
        const Gap c = complete(g, serial);
        if (c.stallMs() > out.stallMs())
            out = c;
    }
    return out;
}

UiWatchdog::Gap UiWatchdog::worstWall() const
{
    std::lock_guard lock(mutex_);
    Gap out = worstShort_;
    for (const auto& [serial, g] : gaps_) {
        if (g.wallMs > out.wallMs)
            out = complete(g, serial);
    }
    return out;
}

bool UiWatchdog::eventFilter(QObject* watched, QEvent* event)
{
    const qint64 now = clock_.nsecsElapsed();
    const double d = double(now - lastEventNs_) * 1e-6;
    ++current_.events;
    if (d > current_.longestMs) {
        current_.longestMs = d;
        current_.receiver = lastReceiver_;
        current_.eventType = lastType_;
    }
    lastEventNs_ = now;
    lastReceiver_ = watched->metaObject()->className();
    lastType_ = int(event->type());
    return false;
}

void UiWatchdog::onTick()
{
    const qint64 now = clock_.nsecsElapsed();
    Gap g = current_;
    g.atMs = lastNs_ / 1000000;
    g.wallMs = double(now - lastNs_) * 1e-6;
#ifdef _WIN32
    const std::uint64_t c = threadCycles();
    g.ranMs = tscPerNs_ > 0 ? double(c - lastCycles_) / tscPerNs_ * 1e-6 : 0;
    lastCycles_ = c;
    g.measured = tscPerNs_ > 0;
#endif
    g.renderThread = renderThreadId_.load() != 0;
    {
        std::lock_guard lock(mutex_);
        if (g.wallMs >= 4)
            gaps_.emplace_back(gapSerial_, g);
        else if (g.stallMs() > worstShort_.stallMs())
            worstShort_ = g;
        ++gapSerial_;
        lastTickNs_.store(now);
    }
    lastNs_ = now;
    current_ = Gap{};
    lastEventNs_ = now;
}

// While a gap is open (no tick for 4 ms), samples the scheduler's state of the UI thread and of Qt Quick's render
// thread back to back (at most every half millisecond). Each sample's state stands for the time from the previous
// sample's start to its own, within one gap; the samples go to that gap, also when it has closed meanwhile (a sample
// takes a few ms). At the highest priority, so that the load it measures does not hold up the samples.
void UiWatchdog::watch()
{
#ifdef _WIN32
    renderThreadId_.store(threadNamed(L"QSGRenderThread")); // here, not on the UI thread: it takes a while
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    // A high-resolution timer: a plain sleep lasts a whole system timer tick (up to 15.6 ms).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::vector<unsigned char> buffer(4 * 1024 * 1024);
    qint64 prevNs = -1;
    std::uint64_t prevSerial = 0;
    while (!stopWatcher_.load()) {
        LARGE_INTEGER due{};
        due.QuadPart = -5000; // 0.5 ms from now, in 100 ns units
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        const qint64 begin = clock_.nsecsElapsed();
        if (begin - lastTickNs_.load() < 4000000) {
            prevNs = -1;
            continue;
        }
        std::uint64_t serial = 0;
        {
            std::lock_guard lock(mutex_);
            serial = gapSerial_;
        }
        const bool idle = idle_.load();
        States st;
        const bool ok = sampleStates(DWORD(uiThreadId_), DWORD(renderThreadId_.load()), buffer, st);
        std::lock_guard lock(mutex_);
        Sampled& s = sampled_[serial];
        ++s.samples;
        if (!ok) {
            ++s.failed;
        } else if (prevNs >= 0 && prevSerial == serial) {
            const qint64 span = begin - prevNs;
            if (runnable(st.ui)) {
                s.ready += span;
            } else if (st.ui == 2) {
                // running: the cycle counter has it
            } else if (idle && idle_.load()) { // waiting for events before and after the sample
                s.idle += span;
            } else if (runnable(st.render)) {
                s.starved += span;
            } else {
                s.waited += span;
                ++s.reasons[st.uiWaitReason];
            }
        }
        prevNs = begin;
        prevSerial = serial;
    }
    if (timer != nullptr)
        CloseHandle(timer);
#endif
}

} // namespace ray::test
