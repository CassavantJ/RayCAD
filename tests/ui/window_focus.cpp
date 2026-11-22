// The keyboard focus for a test window (window_focus.h says why).
#include "window_focus.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QtTest/QTest>

namespace ray::test {

bool focusWindow(QWindow* window, int waitMs)
{
    if (window == nullptr)
        return false;
    if (window->isActive())
        return true;
    window->requestActivate();
    if (QTest::qWaitForWindowActive(window, waitMs))
        return true;
    QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
        window, Qt::ActiveWindowFocusReason);
    QCoreApplication::processEvents();
    return QGuiApplication::focusWindow() == window;
}

} // namespace ray::test
