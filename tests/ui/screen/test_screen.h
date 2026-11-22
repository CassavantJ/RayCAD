// RAY_TEST_SCREEN: every window a test shows goes on one secondary screen, never on the primary one (README, "Which
// tests to run"). Opt-in: unset (or empty), nothing here does anything, and the tests behave exactly as without it.
//
// The value names a screen by its platform name: on Windows the display device name, DISPLAY3 (or \\.\DISPLAY3,
// any case), elsewhere QScreen::name() (the output name, such as DP-1). A name, not an index: it stays with the same
// monitor output when monitors are added, removed or rearranged and whatever order they are enumerated in, and Qt's
// own names are no better on Windows (two monitors of the same model are "XF270HU (1)" and "XF270HU (2)" in
// enumeration order). A name that matches no screen, or names the primary screen, stops the test process before its
// first test case with a message listing the screens.
//
// In a test process with a QGuiApplication (test_screen_listener.cpp, linked into every Qt Quick test):
//   - Qt's primary screen becomes the named one (QWindowSystemInterface::handlePrimaryScreenChanged; Windows is not
//     told). A new QWindow starts on the primary screen, so the windows the process creates are laid out on the
//     named screen before they are created: the application centres its main window on window->screen() (and a
//     test that places a window itself reads QGuiApplication::primaryScreen()), and popups, menus and dialogs go
//     over their transient parent. A window with no position at all is laid out by Qt on the screen at 0,0, the real
//     primary one: the tests' own probe windows call placeBeforeShow.
//   - When a top-level native window is created (still hidden: before it is first shown), one that is not on the
//     named screen after all, or is larger than it and hangs over an edge facing the primary screen, is moved
//     (placeFrame): onto the screen, back inside that edge (it overhangs on the other side instead). The summary
//     counts these; only the controls gallery at 200 % (a 1440x900 window on a screen of 1280x720) needs it here.
//   - Self-check: every top-level window of the process (Qt's, and on Windows any other, such as a native dialog) is
//     checked when it is shown, moved or resized, and on Windows also sampled every 20 ms. One whose frame touches
//     the primary screen fails the test case that is running, and the process.
// raycad.exe started by a test: waitForChild watches its windows the same way; childWindowGeometry is a window
// rectangle on the named screen to save in its settings, which it restores (no product change needed).
#pragma once

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>

#include <optional>
#include <string>
#include <vector>

class QProcess;
class QScreen;
class QWindow;

namespace ray::test::screen {

inline constexpr const char* kVariable = "RAY_TEST_SCREEN";

// A screen as the selection sees it.
struct ScreenId {
    std::string name;        // the platform name (Windows: \\.\DISPLAY3)
    std::string description; // for messages: Qt's name, geometry, device pixel ratio
    bool primary = false;
};

struct Selection {
    bool requested = false; // the variable is set and not empty
    int index = -1;         // the named screen, when requested and usable
    std::string error;      // why not, when requested and not usable
};

// The screen `value` (the variable's value; nullopt when unset) names among `screens`. Unset or empty: not requested.
// Names compare without case and without a leading \\.\ on either side.
Selection selectScreen(const std::optional<std::string>& value, const std::vector<ScreenId>& screens);

// The top-left a window frame `frame` is moved to so that it is on `target` and does not overhang the edges of
// `target` that face `primary` (all in one coordinate system). A frame whose centre is not on the target is centred
// on it first. Then along each axis on which the primary screen's centre lies beyond an edge of the target, the
// frame is pushed back inside that edge. Unchanged when it is on the target and does not overhang those edges.
QPoint placeFrame(const QRect& frame, const QRect& target, const QRect& primary);

// The named screen once the listener has set it up; null when the variable is unset (or in a process without one).
QScreen* testScreen();

// With a test screen, centres `window` on it; for a window a test makes itself with no position of its own (Qt
// would lay it out on the screen at 0,0, the real primary one, whatever Qt's primary screen is). Call before it is
// first shown. Does nothing without a test screen, or once the window's native window exists.
void placeBeforeShow(QWindow& window);

// Called by the listener. install() reads the variable and, when it names a screen, sets everything above up; false
// with `error` when it names none. The others are no-ops unless install() set something up.
bool install(std::string& error);
void testCaseStarting(const std::string& name);
void testCaseEnded();
// The summary line; false if a window was on the primary screen outside any test case (so nothing failed for it).
bool finish(std::string& summary);

// A rectangle for the main window of a raycad.exe a test starts, of `size` and centred on the named screen's
// available area, in Qt's coordinates there (as AppHost saves and restores it); nullopt when the variable is unset.
std::optional<QRect> childWindowGeometry(QSize size);

// As process.waitForFinished(timeoutMs). With a test screen it also checks the process's top-level windows every
// 20 ms meanwhile (Windows) and fails the running test case if one touches the primary screen.
bool waitForChild(QProcess& process, int timeoutMs);

} // namespace ray::test::screen
