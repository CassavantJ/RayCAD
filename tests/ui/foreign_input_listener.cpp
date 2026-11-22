// Keeps the rest of the desktop out of the window tests (Windows): the real mouse pointer, and other programs taking
// the activation. Linked into every test that shows a Qt Quick window (tests/CMakeLists.txt).
//
// The tests drive their windows with Qt's own event injection (QTest, QWindowSystemInterface, sendEvent), never with
// the system's input. But Windows also tells a test window about the desktop around it, and Qt acts on that:
//   - The real pointer. Whenever it rests over a test window (a person using the desktop, or a pointer left where a
//     test window then opens), the window gets its messages, and Qt turns them into mouse and hover events like any
//     other. Seen: the viewport answers a hover with a pick, and its first pick creates the picking pass's render
//     target and pipelines (11 RHI resources, made once and kept; bounded, not a leak), so the glass tests' resource
//     count after 200 resizes was 45 against 34 at the start whenever the pointer rested on the window.
//   - Another program's window taking the activation (other tests' windows open on the same screen all the time on
//     a busy machine). The test window is deactivated: Qt Quick takes the grab from the item that has it, so a drag
//     a test is making ends halfway (the section slider: the rest of its moves ignored), and the window's content
//     loses the active focus, so keys a test types go nowhere until it is active again.
// While the tests run, a native event filter drops, before Qt sees them, every real pointer message (mouse,
// non-client mouse, wheel, pointer, touch, gesture) sent to a window of this process, and the messages that tell one
// of its windows that a window of another process took the activation or the keyboard focus. Activation and focus
// moving between this process's own windows reach Qt as before, and so does every event a test injects.
// RAY_TEST_FOREIGN_INPUT=keep lets everything through (to try a test window by hand). Compiles to nothing off Windows.
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

// True when `hwnd` is not a window of this process (also when there is none: the activation left for a window this
// process is not told about).
bool foreignWindow(HWND hwnd)
{
    if (hwnd == nullptr)
        return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != GetCurrentProcessId();
}

// The messages that tell a window of this process that another program took the activation or the focus.
bool foreignDeactivation(const MSG& msg)
{
    switch (msg.message) {
    case WM_ACTIVATEAPP: // wParam FALSE: another program's thread is being activated
        return msg.wParam == FALSE;
    case WM_ACTIVATE: // deactivated, lParam the window being activated
        return LOWORD(msg.wParam) == WA_INACTIVE && foreignWindow(reinterpret_cast<HWND>(msg.lParam));
    case WM_KILLFOCUS: // wParam the window receiving the focus
        return foreignWindow(reinterpret_cast<HWND>(msg.wParam));
    default:
        return false;
    }
}

class ForeignInputFilter final : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
    {
        if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
            return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg == nullptr)
            return false;
        if (pointerMessage(msg->message)) {
            ++pointer;
        } else if (foreignDeactivation(*msg)) {
            ++deactivations;
        } else {
            return false;
        }
        if (result != nullptr)
            *result = 0;
        return true;
    }
    long long pointer = 0;
    long long deactivations = 0;
};

class ForeignInput : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        char keep[8] = {};
        if (GetEnvironmentVariableA("RAY_TEST_FOREIGN_INPUT", keep, sizeof(keep)) > 0 &&
            std::string_view(keep) == "keep")
            return;
        if (QCoreApplication::instance() == nullptr)
            return;
        filter_ = new ForeignInputFilter;
        QCoreApplication::instance()->installNativeEventFilter(filter_);
    }
    void testRunEnded(const Catch::TestRunStats&) override
    {
        if (filter_ == nullptr)
            return;
        if (QCoreApplication::instance() != nullptr)
            QCoreApplication::instance()->removeNativeEventFilter(filter_);
        if (filter_->pointer > 0 || filter_->deactivations > 0) {
            std::printf("foreign input ignored: %lld messages of the system pointer, %lld deactivations by other "
                        "programs\n",
                        filter_->pointer, filter_->deactivations);
            std::fflush(stdout);
        }
        delete filter_;
        filter_ = nullptr;
    }

private:
    ForeignInputFilter* filter_ = nullptr;
};

} // namespace

CATCH_REGISTER_LISTENER(ForeignInput)
#endif
