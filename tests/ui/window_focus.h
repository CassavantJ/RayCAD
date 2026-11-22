// The keyboard focus for a test window, whether or not the desktop grants it (tests/ui/window_focus.cpp).
//
// Keys a test sends with QTest go to the window's focus item, and window shortcuts (Ctrl+K, Esc on the palette) only
// fire in Qt's focus window. Qt learns which window that is from the platform; Windows gives the foreground to a
// window of a background process only when no other program holds it, and on a busy desktop one does (another test's
// window, from another worktree's ctest that the "desktop" resource lock does not cover). A test that needs the focus
// then neither got it nor could tell.
#pragma once

class QWindow;

namespace ray::test {

// Asks for `window` to be activated and waits up to `waitMs` for the desktop to do it; if it does not, tells Qt that
// the window has the focus (QWindowSystemInterface, as the platform plugin would), so keys and window shortcuts reach
// it all the same. Nothing changes on the desktop itself. True if Qt's focus window is `window` afterwards.
bool focusWindow(QWindow* window, int waitMs = 1000);

} // namespace ray::test
