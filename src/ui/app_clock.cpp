#include "app_clock.h"

#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#elif defined(__linux__)
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#endif

namespace ray::ui {
namespace {

// Fallback: the first call is time zero.
double msSinceFirstCall()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

#if defined(_WIN32)
unsigned long long ticks(const FILETIME& ft)
{
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
#endif

} // namespace

double msSinceProcessStart()
{
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) {
        return msSinceFirstCall();
    }
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return static_cast<double>(ticks(now) - ticks(created)) / 10000.0; // 100 ns units
#elif defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) {
        return msSinceFirstCall();
    }
    struct timeval now{};
    gettimeofday(&now, nullptr);
    const struct timeval& start = info.kp_proc.p_starttime;
    return static_cast<double>(now.tv_sec - start.tv_sec) * 1000.0 +
           static_cast<double>(now.tv_usec - start.tv_usec) / 1000.0;
#elif defined(__linux__)
    // Field 22 of /proc/self/stat is the start time in clock ticks after boot (10 ms resolution).
    std::ifstream stat("/proc/self/stat");
    std::string line;
    if (!std::getline(stat, line)) {
        return msSinceFirstCall();
    }
    const auto paren = line.rfind(')'); // the command name may contain spaces
    if (paren == std::string::npos) {
        return msSinceFirstCall();
    }
    std::istringstream fields(line.substr(paren + 2));
    std::string field;
    for (int index = 3; index <= 22 && fields >> field; ++index) {
        if (index == 22) {
            const double startTicks = std::stod(field);
            timespec boot{};
            clock_gettime(CLOCK_BOOTTIME, &boot);
            const double nowMs = static_cast<double>(boot.tv_sec) * 1000.0 + static_cast<double>(boot.tv_nsec) / 1e6;
            return nowMs - startTicks * 1000.0 / static_cast<double>(sysconf(_SC_CLK_TCK));
        }
    }
    return msSinceFirstCall();
#else
    return msSinceFirstCall();
#endif
}

} // namespace ray::ui
