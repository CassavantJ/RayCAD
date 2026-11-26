// Makes a Debug-CRT assertion fail the test process at once instead of blocking it (MSVC Debug builds). Linked into the
// test programs that run the geometry kernel (tests/CMakeLists.txt).
//
// OpenCASCADE's Standard_ASSERT macros report through _CrtDbgReport(_CRT_ASSERT, ...) when _DEBUG is defined, and the
// Debug CRT's default for an assertion is a modal message box. A test process has no one to press its buttons, so it
// waited forever with no CPU use: ray_naming_oracle_tests stopped inside its first randomised sequence in the
// windows-msvc-debug CI job until ctest killed it at 1200 s (task T-080 round 4: BRepTools_History::AddModified
// asserting on a boolean that had been built twice). Release builds compile those reports out, so only Debug showed it.
//
// When the test run starts, a CRT report hook is installed for assertions and errors: it prints the report and the
// test case that was running to stderr and ends the process with exit code 3, which ctest counts as a failure. Off
// MSVC Debug this file compiles to nothing.
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string& runningTest()
{
    static std::string name;
    return name;
}

int __cdecl failOnReport(int type, char* message, int* returnValue)
{
    if (type != _CRT_ASSERT && type != _CRT_ERROR)
        return 0; // warnings: the CRT's own handling
    std::fprintf(stderr, "\nDebug CRT %s (it would have opened a modal report box) in test case \"%s\":\n%s\n",
                 type == _CRT_ASSERT ? "assertion" : "error", runningTest().c_str(), message ? message : "");
    std::fflush(stderr);
    if (returnValue != nullptr)
        *returnValue = 0;
    std::_Exit(3);
}

class DebugAssertFails : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, failOnReport);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no message box either
    }
    void testCaseStarting(const Catch::TestCaseInfo& info) override { runningTest() = info.name; }
};

} // namespace

CATCH_REGISTER_LISTENER(DebugAssertFails)
#endif
