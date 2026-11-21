// Keeps the graphics driver's worker threads of a test process from starving on a busy machine (Windows). Linked
// into every test that draws with Direct3D 11: the render tests and the Qt Quick tests (tests/CMakeLists.txt).
//
// The NVIDIA Direct3D 11 driver (nvwgf2umx.dll) runs its worker threads at THREAD_PRIORITY_IDLE (48 of them in the
// render tests), and creating a device, its pipelines and a readback wait for them. Idle priority only runs on a
// processor that no normal-priority thread wants: with every processor busy (a parallel ctest, a build beside it)
// those threads did not run for seconds at a time. The tests make a device per test case, so they wait on those
// threads hundreds of times. Measured beside four naming blocks and two S4 suites (32 hardware threads), the same
// load each time: ray_render_tests (39 s alone, CPU time 0.6 of wall time) 247 s and 237 s as they were, 92 s and
// 87 s with this; it had timed out at 900 s in a parallel full run during a build. ray_ui_settings_tests (8 s
// alone) 210 s as it was, 16 s with this.
//
// While the tests run, a watcher raises every thread of this process that is at idle priority to normal priority,
// every 20 ms. Only the driver makes such threads in these processes; nothing a test checks depends on their
// priority, only how long the test waits for them. RAY_TEST_DRIVER_PRIORITY=keep leaves them alone (to measure).
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

namespace {

class DriverThreadPriority : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        char keep[8] = {};
        if (GetEnvironmentVariableA("RAY_TEST_DRIVER_PRIORITY", keep, sizeof(keep)) > 0 &&
            std::string_view(keep) == "keep")
            return;
        watcher_ = std::thread([this] {
            while (!stop_.load()) {
                raiseIdleThreads();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    void testRunEnded(const Catch::TestRunStats&) override
    {
        stop_.store(true);
        if (watcher_.joinable())
            watcher_.join();
    }

private:
    static void raiseIdleThreads()
    {
        const DWORD self = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != self)
                continue;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE,
                                       entry.th32ThreadID);
            if (thread == nullptr)
                continue;
            if (GetThreadPriority(thread) == THREAD_PRIORITY_IDLE)
                SetThreadPriority(thread, THREAD_PRIORITY_NORMAL);
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
    }

    std::thread watcher_;
    std::atomic<bool> stop_{false};
};

} // namespace

CATCH_REGISTER_LISTENER(DriverThreadPriority)
#endif
