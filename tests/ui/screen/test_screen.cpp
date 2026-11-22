// RAY_TEST_SCREEN (test_screen.h says what and why).
#include "test_screen.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/qevent.h>
#include <QtGui/qpa/qplatformscreen.h>
#include <QtGui/qpa/qwindowsysteminterface.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <dwmapi.h>
#endif

namespace ray::test::screen {

namespace {

std::string normalised(std::string name)
{
    if (name.rfind("\\\\.\\", 0) == 0)
        name.erase(0, 4);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

std::string listOf(const std::vector<ScreenId>& screens)
{
    std::string out;
    for (const ScreenId& s : screens) {
        out += "\n  " + s.name + (s.primary ? " (primary)" : "") + ": " + s.description;
    }
    return out;
}

} // namespace

Selection selectScreen(const std::optional<std::string>& value, const std::vector<ScreenId>& screens)
{
    Selection selection;
    if (!value || value->empty())
        return selection;
    selection.requested = true;
    const std::string wanted = normalised(*value);
    for (std::size_t i = 0; i < screens.size(); ++i) {
        if (normalised(screens[i].name) != wanted)
            continue;
        if (screens[i].primary) {
            selection.error = std::string(kVariable) + "=" + *value +
                              " names the primary screen, which the windows of these tests must stay off. Name one "
                              "of the others, or unset it. The screens:" +
                              listOf(screens);
            return selection;
        }
        selection.index = static_cast<int>(i);
        return selection;
    }
    selection.error =
        std::string(kVariable) + "=" + *value +
        " names no screen. Name one of the screens that are not primary, or unset it. The screens:" + listOf(screens);
    return selection;
}

QPoint placeFrame(const QRect& frame, const QRect& target, const QRect& primary)
{
    QPoint to = frame.topLeft();
    // Not on the screen at all (Qt laid it out on another one): centred on it first.
    if (!target.contains(frame.center()))
        to = QPoint(target.left() + (target.width() - frame.width()) / 2,
                    target.top() + (target.height() - frame.height()) / 2);
    const QRect f(to, frame.size());
    const QPoint centre = primary.center();
    if (centre.x() < target.left() && f.left() < target.left())
        to.rx() += target.left() - f.left();
    else if (centre.x() > target.right() && f.right() > target.right())
        to.rx() -= f.right() - target.right();
    if (centre.y() < target.top() && f.top() < target.top())
        to.ry() += target.top() - f.top();
    else if (centre.y() > target.bottom() && f.bottom() > target.bottom())
        to.ry() -= f.bottom() - target.bottom();
    return to;
}

void placeBeforeShow(QWindow& window)
{
    QScreen* screen = testScreen();
    if (screen == nullptr || window.handle() != nullptr)
        return;
    const QRect area = screen->availableGeometry();
    window.setScreen(screen);
    window.setPosition(area.left() + (area.width() - window.width()) / 2,
                       area.top() + (area.height() - window.height()) / 2);
}

namespace {

struct Violation {
    std::string what;
    std::string testCase;
    bool reported = false;
};

struct State {
    QScreen* target = nullptr;
    QScreen* realPrimary = nullptr;
    std::string targetDescription;
    QObject* filter = nullptr;

    std::mutex mutex; // guards everything below
    bool inTestCase = false;
    std::string testCase;
    std::set<quintptr> flagged;  // windows already reported in this test case
    std::set<quintptr> seen;     // top-level windows of this process seen shown
    std::set<quintptr> onTarget; // those of them seen mostly on the named screen
    std::set<std::pair<qint64, quintptr>> childSeen;
    std::set<std::pair<qint64, quintptr>> childOnTarget;
    std::vector<Violation> violations;
    int placed = 0; // windows moved inside the screen's edges by placeFrame

#ifdef _WIN32
    RECT primaryRect{};
    RECT targetRect{};
    HMONITOR targetMonitor = nullptr;
    std::thread poller;
    std::atomic<bool> stop{false};
#endif
};

State* g_state = nullptr;

QString describe(const QScreen* s)
{
    const QRect g = s->geometry();
    return QStringLiteral("%1, %2,%3 %4x%5, device pixel ratio %6")
        .arg(s->name())
        .arg(g.x())
        .arg(g.y())
        .arg(g.width())
        .arg(g.height())
        .arg(s->devicePixelRatio());
}

#ifdef _WIN32
HMONITOR monitorOf(QScreen* s)
{
    auto* native = s->nativeInterface<QNativeInterface::QWindowsScreen>();
    return native != nullptr ? native->handle() : nullptr;
}

std::string platformName(QScreen* s)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (HMONITOR monitor = monitorOf(s); monitor != nullptr && GetMonitorInfoW(monitor, &info))
        return QString::fromWCharArray(info.szDevice).toStdString();
    return s->name().toStdString();
}

bool monitorRect(QScreen* s, RECT& rect)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = monitorOf(s);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        return false;
    rect = info.rcMonitor;
    return true;
}

QRect toQRect(const RECT& r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

std::string describe(const RECT& r)
{
    std::ostringstream out;
    out << r.left << "," << r.top << " " << (r.right - r.left) << "x" << (r.bottom - r.top);
    return out.str();
}

// The visible frame of a shown top-level window (without the invisible resize borders); false for a hidden or
// cloaked one.
bool shownFrame(HWND hwnd, RECT& frame)
{
    if (!IsWindowVisible(hwnd))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
        return false;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        return true;
    return GetWindowRect(hwnd, &frame) != 0;
}

// Class names only: reading a window's title would send it a message, which a window of a busy thread answers late.
std::string className(HWND hwnd)
{
    wchar_t name[128] = {};
    GetClassNameW(hwnd, name, 128);
    return QString::fromWCharArray(name).toStdString();
}

std::vector<HWND> topLevelWindows(DWORD pid)
{
    struct Search {
        DWORD pid;
        std::vector<HWND> found;
    } search{pid, {}};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == s->pid)
                s->found.push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}
#endif

// Records a window that touches the primary screen, once per window and test case. Under the lock.
bool recordLocked(State& s, quintptr window, std::string what)
{
    if (!s.flagged.insert(window).second)
        return false;
    s.violations.push_back({std::move(what), s.inTestCase ? s.testCase : std::string("(between test cases)")});
    return true;
}

// Fails the running test case for every violation not reported yet. Main thread only.
void reportPending()
{
    State* s = g_state;
    if (s == nullptr)
        return;
    std::vector<std::string> now;
    {
        std::lock_guard lock(s->mutex);
        if (!s->inTestCase)
            return;
        for (Violation& v : s->violations) {
            if (!v.reported) {
                v.reported = true;
                now.push_back(v.what + " (seen during \"" + v.testCase + "\")");
            }
        }
    }
    for (const std::string& message : now)
        FAIL_CHECK(kVariable << ": " << message);
}

#ifdef _WIN32
// Checks the shown top-level windows of process `pid` (this one or a child). True if a new violation was recorded.
bool scanProcess(State& s, DWORD pid, bool child)
{
    bool found = false;
    for (HWND hwnd : topLevelWindows(pid)) {
        RECT frame{};
        if (!shownFrame(hwnd, frame))
            continue;
        const auto key = reinterpret_cast<quintptr>(hwnd);
        RECT overlap{};
        const bool onPrimary = IntersectRect(&overlap, &frame, &s.primaryRect) != 0;
        const bool onTarget = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == s.targetMonitor;
        std::lock_guard lock(s.mutex);
        if (child) {
            s.childSeen.insert({qint64(pid), key});
            if (onTarget)
                s.childOnTarget.insert({qint64(pid), key});
        } else {
            s.seen.insert(key);
            if (onTarget)
                s.onTarget.insert(key);
        }
        if (onPrimary)
            found |=
                recordLocked(s, key,
                             (child ? "a window of child process " + std::to_string(pid) : std::string("a window")) +
                                 " of class " + className(hwnd) + " was shown on the primary screen: frame " +
                                 describe(frame) + ", primary screen " + describe(s.primaryRect));
    }
    return found;
}
#endif

// Checks one of this process's Qt windows as it is shown, moved or resized. Main thread.
void checkWindow(State& s, QWindow* window)
{
#ifdef _WIN32
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT frame{};
    if (!shownFrame(hwnd, frame))
        return;
    RECT overlap{};
    const bool onPrimary = IntersectRect(&overlap, &frame, &s.primaryRect) != 0;
    const bool onTarget = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == s.targetMonitor;
    const std::string where = describe(frame);
    const auto key = reinterpret_cast<quintptr>(hwnd);
#else
    if (!window->isVisible())
        return;
    const QRect frame = window->frameGeometry();
    const bool onPrimary = frame.intersects(s.realPrimary->geometry());
    const bool onTarget = window->screen() == s.target;
    const std::string where = std::to_string(frame.x()) + "," + std::to_string(frame.y()) + " " +
                              std::to_string(frame.width()) + "x" + std::to_string(frame.height());
    const auto key = reinterpret_cast<quintptr>(window);
#endif
    bool found = false;
    {
        std::lock_guard lock(s.mutex);
        s.seen.insert(key);
        if (onTarget)
            s.onTarget.insert(key);
        if (onPrimary)
            found = recordLocked(s, key,
                                 "the window \"" + window->title().toStdString() + "\" (" +
                                     window->metaObject()->className() + ") was shown on the primary screen: frame " +
                                     where);
    }
    if (found)
        reportPending();
}

// A window larger than the screen, created (not yet shown) hanging over an edge that faces the primary screen, is
// moved back inside that edge. Native coordinates on Windows: Qt's own coordinates of a window over two screens
// depend on which screen it maps them through.
void placeWindow(State& s, QWindow* window)
{
#ifdef _WIN32
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT r{};
    if (!GetWindowRect(hwnd, &r))
        return;
    const QRect frame = toQRect(r);
    const QPoint to = placeFrame(frame, toQRect(s.targetRect), toQRect(s.primaryRect));
    if (to == frame.topLeft())
        return;
    SetWindowPos(hwnd, nullptr, to.x(), to.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    const QRect frame = window->frameGeometry();
    const QPoint to = placeFrame(frame, s.target->geometry(), s.realPrimary->geometry());
    if (to == frame.topLeft())
        return;
    window->setFramePosition(to);
#endif
    std::lock_guard lock(s.mutex);
    ++s.placed;
}

class WindowFilter : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched->isWindowType())
            return false;
        const QEvent::Type type = event->type();
        if (type != QEvent::PlatformSurface && type != QEvent::Expose && type != QEvent::Move && type != QEvent::Resize)
            return false;
        auto* window = static_cast<QWindow*>(watched);
        State* s = g_state;
        if (s == nullptr || !window->isTopLevel() || window->handle() == nullptr)
            return false;
        if (type == QEvent::PlatformSurface) {
            if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
                placeWindow(*s, window);
            return false;
        }
        checkWindow(*s, window);
        return false;
    }
};

} // namespace

QScreen* testScreen()
{
    return g_state != nullptr ? g_state->target : nullptr;
}

bool install(std::string& error)
{
    std::optional<std::string> value;
    if (qEnvironmentVariableIsSet(kVariable))
        value = qEnvironmentVariable(kVariable).toStdString();
    if (!value || value->empty())
        return true;
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        error = std::string(kVariable) +
                " is set, but this test process has no QGuiApplication when its tests start, so its windows "
                "cannot be kept off the primary screen";
        return false;
    }
    const QList<QScreen*> screens = QGuiApplication::screens();
    std::vector<ScreenId> ids;
    for (QScreen* s : screens) {
#ifdef _WIN32
        const std::string name = platformName(s);
#else
        const std::string name = s->name().toStdString();
#endif
        ids.push_back({name, describe(s).toStdString(), s == QGuiApplication::primaryScreen()});
    }
    const Selection selection = selectScreen(value, ids);
    if (!selection.error.empty()) {
        error = selection.error;
        return false;
    }
    auto state = std::make_unique<State>();
    state->target = screens.at(selection.index);
    state->realPrimary = QGuiApplication::primaryScreen();
    state->targetDescription =
        ids.at(std::size_t(selection.index)).name + " (" + describe(state->target).toStdString() + ")";
#ifdef _WIN32
    if (!monitorRect(state->realPrimary, state->primaryRect) || !monitorRect(state->target, state->targetRect)) {
        error = std::string(kVariable) + ": the monitors of the screens could not be read";
        return false;
    }
    state->targetMonitor = monitorOf(state->target);
#endif
    QWindowSystemInterface::handlePrimaryScreenChanged(state->target->handle());
    if (QGuiApplication::primaryScreen() != state->target) {
        error = std::string(kVariable) + ": Qt did not make " + state->targetDescription + " its primary screen";
        return false;
    }
    g_state = state.release();
    g_state->filter = new WindowFilter;
    QCoreApplication::instance()->installEventFilter(g_state->filter);
#ifdef _WIN32
    g_state->poller = std::thread([s = g_state] {
        const DWORD self = GetCurrentProcessId();
        while (!s->stop.load()) {
            if (scanProcess(*s, self, false))
                QMetaObject::invokeMethod(QCoreApplication::instance(), [] { reportPending(); }, Qt::QueuedConnection);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
#endif
    return true;
}

void testCaseStarting(const std::string& name)
{
    if (g_state == nullptr)
        return;
    {
        std::lock_guard lock(g_state->mutex);
        g_state->inTestCase = true;
        g_state->testCase = name;
        g_state->flagged.clear();
    }
    reportPending(); // anything seen between test cases
}

void testCaseEnded()
{
    if (g_state == nullptr)
        return;
#ifdef _WIN32
    scanProcess(*g_state, GetCurrentProcessId(), false);
#endif
    reportPending();
    std::lock_guard lock(g_state->mutex);
    g_state->inTestCase = false;
}

bool finish(std::string& summary)
{
    State* s = g_state;
    if (s == nullptr)
        return true;
#ifdef _WIN32
    s->stop.store(true);
    if (s->poller.joinable())
        s->poller.join();
    scanProcess(*s, GetCurrentProcessId(), false);
#endif
    QCoreApplication::instance()->removeEventFilter(s->filter);
    std::lock_guard lock(s->mutex);
    std::ostringstream out;
    out << kVariable << ": windows on " << s->targetDescription << ": " << s->seen.size()
        << " top-level windows of this process shown (" << s->onTarget.size() << " mostly on that screen)";
    if (!s->childSeen.empty())
        out << ", " << s->childSeen.size() << " of the raycad processes it started (" << s->childOnTarget.size()
            << " mostly on that screen)";
    out << ", " << s->placed << " moved onto it or inside its edges when created; ";
    bool unreported = false;
    if (s->violations.empty()) {
        out << "none on the primary screen";
    } else {
        out << s->violations.size() << " on the primary screen:";
        for (const Violation& v : s->violations) {
            out << "\n  " << v.what << " (during \"" << v.testCase << "\")" << (v.reported ? "" : ", not reported");
            unreported |= !v.reported;
        }
    }
    summary = out.str();
    return !unreported;
}

std::optional<QRect> childWindowGeometry(QSize size)
{
    if (g_state == nullptr)
        return std::nullopt;
    const QRect area = g_state->target->availableGeometry();
    const int w = std::min(size.width(), area.width());
    const int h = std::min(size.height(), area.height());
    return QRect(area.x() + (area.width() - w) / 2, area.y() + (area.height() - h) / 2, w, h);
}

bool waitForChild(QProcess& process, int timeoutMs)
{
    State* s = g_state;
    if (s == nullptr)
        return process.waitForFinished(timeoutMs);
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        if (process.waitForFinished(20))
            return true;
        if (process.state() == QProcess::NotRunning)
            return false;
#ifdef _WIN32
        if (scanProcess(*s, DWORD(process.processId()), true))
            reportPending();
#endif
        if (clock.elapsed() >= timeoutMs)
            return false;
    }
}

} // namespace ray::test::screen
