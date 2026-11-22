// The post thread and its CPU clock (engine.h). std::thread cannot choose its stack size, and the default differs
// (1 MB on Windows, 512 KB for secondary threads on macOS), so the thread is made with the platform API.
#include "engine.h"

#include <filesystem>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace ray::post {
namespace {

struct ThreadStart {
    const std::function<void()>* work;
};

#if defined(_WIN32)
unsigned __stdcall threadMain(void* arg)
{
    (*static_cast<ThreadStart*>(arg)->work)();
    return 0;
}
#else
void* threadMain(void* arg)
{
    (*static_cast<ThreadStart*>(arg)->work)();
    return nullptr;
}
#endif

} // namespace

bool runOnPostThread(std::size_t stackBytes, const std::function<void()>& work)
{
    ThreadStart start{&work};
#if defined(_WIN32)
    // STACK_SIZE_PARAM_IS_A_RESERVATION: the size is the reserved stack, committed as it is used.
    const std::uintptr_t handle = _beginthreadex(nullptr, static_cast<unsigned>(stackBytes), &threadMain, &start,
                                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0) {
        return false;
    }
    const auto thread = reinterpret_cast<HANDLE>(handle);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    pthread_attr_setstacksize(&attr, stackBytes);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, &threadMain, &start);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        return false;
    }
    pthread_join(thread, nullptr);
    return true;
#endif
}

double threadCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    auto seconds = [](const FILETIME& t) {
        const std::uint64_t ticks = (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; // 100 ns units
        return static_cast<double>(ticks) * 1.0e-7;
    };
    return seconds(kernel) + seconds(user);
#else
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) {
        return 0;
    }
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1.0e-9;
#endif
}

std::string postIdFromPath(std::string_view path)
{
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    const std::u8string name = p.filename().u8string();
    std::string file(reinterpret_cast<const char*>(name.data()), name.size());
    constexpr std::string_view kSuffix = ".post.js";
    if (file.size() > kSuffix.size() && file.ends_with(kSuffix)) {
        return file.substr(0, file.size() - kSuffix.size());
    }
    const std::u8string stem = p.stem().u8string();
    return std::string(reinterpret_cast<const char*>(stem.data()), stem.size());
}

} // namespace ray::post
