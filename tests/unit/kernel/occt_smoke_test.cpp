// OCCT smoke test (task T-001). No OCCT header is included here: only src/kernel may do that.
#include <catch2/catch_test_macros.hpp>

#include <occt_smoke.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <delayimp.h>

namespace {
std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// DLLs this executable loaded through its own delay-load imports, in lower case.
std::set<std::string>& delayLoadedDlls()
{
    static std::set<std::string> dlls;
    return dlls;
}

FARPROC WINAPI onDelayLoad(unsigned event, PDelayLoadInfo info)
{
    if (event == dliNotePreLoadLibrary && info != nullptr && info->szDll != nullptr)
        delayLoadedDlls().insert(lower(info->szDll));
    return nullptr;
}
} // namespace

// The delay-load helper in delayimp.lib calls this hook before it loads each DLL.
extern "C" const PfnDliHook __pfnDliNotifyHook2 = onDelayLoad;
#endif

using ray::kernel::detail::occtSmoke;
using ray::kernel::detail::occtToolkits;
using ray::kernel::detail::occtVersion;

TEST_CASE("the kernel is built against the pinned OCCT version", "[kernel][occt]")
{
    REQUIRE(occtVersion() == RAY_EXPECTED_OCCT_VERSION);
}

TEST_CASE("the smoke calls into every linked OCCT toolkit", "[kernel][occt]")
{
    const std::vector<std::string> toolkits = occtToolkits();
    REQUIRE(toolkits.size() >= 20);
    const std::set<std::string> expected(toolkits.begin(), toolkits.end());
    REQUIRE(expected.size() == toolkits.size());

#if defined(_WIN32)
    // Delay-loading: nothing from OCCT is in the process before the first call.
    REQUIRE(delayLoadedDlls().empty());
    for (const std::string& toolkit : toolkits) {
        INFO(toolkit << ".dll is loaded before any kernel call, so it is not delay-loaded");
        REQUIRE(GetModuleHandleA((toolkit + ".dll").c_str()) == nullptr);
    }
#endif

    const auto result = occtSmoke();
    if (!result.has_value())
        FAIL(result.error().message);
    const std::set<std::string> exercised(result->begin(), result->end());
    REQUIRE(exercised == expected);

#if defined(_WIN32)
    // Every toolkit was resolved through this executable's own delay-load imports, and nothing else was:
    // the list in src/kernel/CMakeLists.txt is exactly what the code calls.
    std::set<std::string> expectedDlls;
    for (const std::string& toolkit : toolkits)
        expectedDlls.insert(lower(toolkit) + ".dll");
    REQUIRE(delayLoadedDlls() == expectedDlls);
#endif
}
