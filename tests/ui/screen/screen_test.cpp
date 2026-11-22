// RAY_TEST_SCREEN (test_screen.h): which screen a value names, where a window too large for it goes, and that with
// the variable unset nothing is set up. No QGuiApplication and no window: this runs anywhere.
#include "test_screen.h"

#include <QtCore/QtEnvironmentVariables>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace ray::test::screen;

namespace {

// This machine's arrangement (Windows device names).
std::vector<ScreenId> screens()
{
    return {{"\\\\.\\DISPLAY1", "XF270HU (1), 3840,-1064 1707x960", false},
            {"\\\\.\\DISPLAY2", "PG32UCDM, 0,0 2560x1440", true},
            {"\\\\.\\DISPLAY3", "XF270HU (2), 3840,376 1707x960", false}};
}

} // namespace

TEST_CASE("unset or empty, no screen is requested", "[screen]")
{
    for (const std::optional<std::string>& value : {std::optional<std::string>(), std::optional<std::string>("")}) {
        const Selection s = selectScreen(value, screens());
        CHECK_FALSE(s.requested);
        CHECK(s.index == -1);
        CHECK(s.error.empty());
    }
    // Also with no screens at all (a process without a display).
    CHECK_FALSE(selectScreen(std::nullopt, {}).requested);
}

TEST_CASE("a screen is named by its device name, with or without \\\\.\\ and in any case", "[screen]")
{
    for (const char* value : {"DISPLAY3", "\\\\.\\DISPLAY3", "display3", "\\\\.\\Display3"}) {
        INFO(value);
        const Selection s = selectScreen(std::string(value), screens());
        CHECK(s.requested);
        CHECK(s.error.empty());
        CHECK(s.index == 2);
    }
    CHECK(selectScreen(std::string("DISPLAY1"), screens()).index == 0);
    // Elsewhere the names are Qt's (X11 output names).
    const std::vector<ScreenId> x11{{"DP-1", "", true}, {"HDMI-A-1", "", false}};
    CHECK(selectScreen(std::string("HDMI-A-1"), x11).index == 1);
    // A prefix or a suffix of a name is not the name.
    CHECK(selectScreen(std::string("DISPLAY"), screens()).index == -1);
    CHECK(selectScreen(std::string("DISPLAY33"), screens()).index == -1);
}

TEST_CASE("a name of no screen, or of the primary one, is an error that lists the screens", "[screen]")
{
    Selection s = selectScreen(std::string("DISPLAY7"), screens());
    CHECK(s.requested);
    CHECK(s.index == -1);
    INFO(s.error);
    CHECK(s.error.find("RAY_TEST_SCREEN=DISPLAY7 names no screen") != std::string::npos);
    CHECK(s.error.find("\\\\.\\DISPLAY1: XF270HU (1)") != std::string::npos);
    CHECK(s.error.find("\\\\.\\DISPLAY2 (primary)") != std::string::npos);

    s = selectScreen(std::string("DISPLAY2"), screens());
    CHECK(s.requested);
    CHECK(s.index == -1);
    INFO(s.error);
    CHECK(s.error.find("names the primary screen") != std::string::npos);
    CHECK(s.error.find("\\\\.\\DISPLAY3: XF270HU (2)") != std::string::npos);

    // A single screen is the primary one: nothing can be named.
    CHECK_FALSE(selectScreen(std::string("DP-1"), {{"DP-1", "", true}}).error.empty());
}

TEST_CASE("a window too large for the screen is pushed back inside the edges that face the primary screen", "[screen]")
{
    // Native pixels: the primary screen on the left, the named one to its right.
    const QRect primary(0, 0, 3840, 2160);
    const QRect target(3840, 376, 2560, 1440);
    // Wider than the screen and centred on it: it hangs over both side edges; the left one faces the primary screen.
    // Taller too: neither the top nor the bottom edge faces the primary screen (whose centre is level with the
    // screen), so it may hang over them.
    CHECK(placeFrame(QRect(3680, 300, 2880, 1600), target, primary) == QPoint(3840, 300));
    // Inside the edges facing the primary screen: unchanged, wherever else it hangs over.
    CHECK(placeFrame(QRect(4000, 300, 2880, 1600), target, primary) == QPoint(4000, 300));
    CHECK(placeFrame(QRect(4000, 500, 1000, 800), target, primary) == QPoint(4000, 500));
    // The primary screen on the right: the right edge faces it.
    const QRect left(-2560, 0, 2560, 1440);
    CHECK(placeFrame(QRect(-2700, 100, 2880, 1000), left, primary) == QPoint(-2880, 100));
    // The primary screen below: the bottom edge faces it.
    const QRect above(0, -1440, 2560, 1440);
    CHECK(placeFrame(QRect(100, -1400, 1000, 1500), above, primary) == QPoint(100, -1500));
    // The primary screen above and to the left: both of those edges.
    const QRect belowRight(3840, 2160, 2560, 1440);
    CHECK(placeFrame(QRect(3800, 2100, 1000, 800), belowRight, primary) == QPoint(3840, 2160));
}

TEST_CASE("a window laid out on another screen is centred on the named one", "[screen]")
{
    const QRect primary(0, 0, 3840, 2160);
    const QRect target(3840, 376, 2560, 1440);
    // A small window Qt centred on the primary screen.
    CHECK(placeFrame(QRect(1861, 951, 198, 152), target, primary) == QPoint(3840 + 1181, 376 + 644));
    // Larger than the screen: centred, then pushed back inside the edge facing the primary screen.
    CHECK(placeFrame(QRect(500, 180, 2880, 1800), target, primary) == QPoint(3840, 376 - 180));
    // Mostly on the screen (its centre is): only the edge rule applies.
    CHECK(placeFrame(QRect(3700, 400, 1000, 800), target, primary) == QPoint(3840, 400));
}

TEST_CASE("with the variable unset, nothing is set up and the helpers do what they would without it", "[screen]")
{
    const QByteArray saved = qgetenv(kVariable);
    const bool wasSet = qEnvironmentVariableIsSet(kVariable);
    qunsetenv(kVariable);
    std::string error;
    CHECK(install(error)); // without a QGuiApplication too: nothing to do
    CHECK(error.empty());
    CHECK(testScreen() == nullptr);
    CHECK_FALSE(childWindowGeometry(QSize(1440, 900)).has_value());
    std::string summary;
    CHECK(finish(summary));
    CHECK(summary.empty());
    // Empty counts as unset.
    qputenv(kVariable, QByteArray());
    CHECK(install(error));
    CHECK(testScreen() == nullptr);
    // Set, in a process without a QGuiApplication: an error, not a silent fallback.
    qputenv(kVariable, "DISPLAY3");
    CHECK_FALSE(install(error));
    CHECK(error.find("no QGuiApplication") != std::string::npos);
    CHECK(testScreen() == nullptr);
    if (wasSet)
        qputenv(kVariable, saved);
    else
        qunsetenv(kVariable);
}
