// Keeps the real mouse pointer out of the window tests (Windows). Linked into every test that shows a Qt Quick window
// (tests/CMakeLists.txt).
//
// The tests drive their windows with Qt's own event injection (QTest, QWindowSystemInterface, sendEvent), never with
// the system pointer. But Windows also sends a test window the messages of the real pointer whenever it rests over
// it: a person using the desktop, or a pointer that was left where a test window then opens. Qt turns those into
// mouse and hover events like any other, so the result of a test depended on where its owner's mouse was. Seen: the
// viewport answers a hover with a pick, and its first pick creates the picking pass's render target and pipelines
// (11 RHI resources, made once and kept; bounded, not a leak), so the glass tests' resource count after 200 resizes
// was 45 against 34 at the start whenever the pointer rested on the window.
//
// While the tests run, a native event filter drops every real pointer message (mouse, non-client mouse, wheel,
// pointer, touch, gesture) sent to a window of this process, before Qt sees it. Qt's own injection does not go
// through native messages, so every event a test sends arrives as before. RAY_TEST_REAL_POINTER=keep lets the real
// pointer through (to try a test window by hand). Compiles to nothing off Windows.
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#ifdef _WIN32
#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>

#include <windows.h>

#include <cstdio>
#include <string_view>

namespace {

// The messages a pointing device sends a window.
bool pointerMessage(UINT m)
{
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) // move, buttons, wheels
        return true;
    if (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) // the same over the frame
        return true;
    if (m >= 0x0245 && m <= 0x0257) // WM_POINTERUPDATE .. WM_POINTERROUTEDRELEASED (pen, touch, mouse in pointer)
        return true;
    switch (m) {
    case WM_MOUSEHOVER:
    case WM_MOUSELEAVE:
    case WM_NCMOUSEHOVER:
    case WM_NCMOUSELEAVE:
    case WM_TOUCH:
    case WM_GESTURE:
        return true;
    default:
        return false;
    }
}

class RealPointerFilter final : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
            return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg == nullptr || !pointerMessage(msg->message))
            return false;
        ++dropped;
        if (result != nullptr)
            *result = 0;
        return true;
    }
    long long dropped = 0;
};

class RealPointer : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        char keep[8] = {};
        if (GetEnvironmentVariableA("RAY_TEST_REAL_POINTER", keep, sizeof(keep)) > 0 &&
            std::string_view(keep) == "keep")
            return;
        if (QCoreApplication::instance() == nullptr)
            return;
        filter_ = new RealPointerFilter;
        QCoreApplication::instance()->installNativeEventFilter(filter_);
    }
    void testRunEnded(const Catch::TestRunStats&) override
    {
        if (filter_ == nullptr)
            return;
        if (QCoreApplication::instance() != nullptr)
            QCoreApplication::instance()->removeNativeEventFilter(filter_);
        if (filter_->dropped > 0) {
            std::printf("real pointer: %lld messages of the system pointer to this process's windows ignored\n",
                        filter_->dropped);
            std::fflush(stdout);
        }
        delete filter_;
        filter_ = nullptr;
    }

private:
    RealPointerFilter* filter_ = nullptr;
};

} // namespace

CATCH_REGISTER_LISTENER(RealPointer)
#endif
