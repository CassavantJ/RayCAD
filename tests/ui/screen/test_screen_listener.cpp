// RAY_TEST_SCREEN in a Qt Quick test process (test_screen.h): set up when the test run starts (the test's main has
// made its QGuiApplication by then, and no window exists yet), followed through every test case, summed up at the
// end. Linked into every test that shows a window (tests/CMakeLists.txt). Does nothing while the variable is unset.
//
// A value that names no usable screen stops the process before its first test case, with exit code 1 and a message
// that lists the screens. A window on the primary screen fails the test case that is running; one seen outside any
// test case fails the process (exit code 1) after the summary.
#include "test_screen.h"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

class TestScreen : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        std::string error;
        if (!ray::test::screen::install(error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            std::fflush(stderr);
            std::_Exit(1);
        }
    }
    void testCaseStarting(const Catch::TestCaseInfo& info) override { ray::test::screen::testCaseStarting(info.name); }
    void testCaseEnded(const Catch::TestCaseStats&) override { ray::test::screen::testCaseEnded(); }
    void testRunEnded(const Catch::TestRunStats&) override
    {
        std::string summary;
        const bool ok = ray::test::screen::finish(summary);
        if (!summary.empty()) {
            std::fprintf(ok ? stdout : stderr, "%s\n", summary.c_str());
            std::fflush(ok ? stdout : stderr);
        }
        if (!ok)
            std::_Exit(1);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(TestScreen)
