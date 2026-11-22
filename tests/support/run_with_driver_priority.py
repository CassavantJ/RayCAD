"""Runs a program and, on Windows, keeps the graphics driver's idle-priority threads in it from starving.

    python -I run_with_driver_priority.py <program> [arguments...]

The process-side counterpart of driver_threads_listener.cpp (which says why), for tests that run an application
rather than a Catch2 executable (raycad_smoke). While the program runs, every 20 ms, each of its threads at
THREAD_PRIORITY_IDLE is raised to normal priority. The program's output goes straight to this process's own, and its
exit code is returned. RAY_TEST_DRIVER_PRIORITY=keep leaves the threads alone. Elsewhere the program just runs.

RAY_TEST_SCREEN (tests/ui/screen/test_screen.h; README, "Which tests to run"), Windows only: the program is raycad,
and its window goes on the screen the variable names (a display device name such as DISPLAY3) instead of the primary
one. Its settings come from RAYCAD_USER_DATA_DIR/test-screen instead of RAYCAD_USER_DATA_DIR, with the window saved
there centred on that screen, at the size the application would give it (it restores a saved window; no option of
its own is needed). Every 20 ms its shown top-level windows are checked: one that touches the primary screen is
reported ("... was shown on the primary screen") and makes the exit code 1. Unset or empty, none of this happens.
"""
import json
import os
import subprocess
import sys
import time


def raise_idle_threads(pid):
    import ctypes
    import ctypes.wintypes as wt

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                    ("dwFlags", wt.DWORD)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    k32.OpenThread.restype = wt.HANDLE
    snapshot = k32.CreateToolhelp32Snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    if not snapshot or snapshot == wt.HANDLE(-1).value:
        return
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    more = k32.Thread32First(wt.HANDLE(snapshot), ctypes.byref(entry))
    while more:
        if entry.th32OwnerProcessID == pid:
            # THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION
            thread = k32.OpenThread(0x0800 | 0x0400, False, entry.th32ThreadID)
            if thread:
                if k32.GetThreadPriority(wt.HANDLE(thread)) == -15:  # THREAD_PRIORITY_IDLE
                    k32.SetThreadPriority(wt.HANDLE(thread), 0)  # THREAD_PRIORITY_NORMAL
                k32.CloseHandle(wt.HANDLE(thread))
        more = k32.Thread32Next(wt.HANDLE(snapshot), ctypes.byref(entry))
    k32.CloseHandle(wt.HANDLE(snapshot))


class TestScreen:
    """RAY_TEST_SCREEN for one raycad process: where its window goes, and what of it was seen where."""

    def __init__(self, value):
        import ctypes
        import ctypes.wintypes as wt

        self.ct = ctypes
        self.wt = wt
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.MonitorFromWindow.restype = wt.HMONITOR
        self.dwm = ctypes.WinDLL("dwmapi")
        shcore = ctypes.WinDLL("shcore")
        # Physical pixels for every monitor and window, as the application (per-monitor DPI aware) sees them. Affects
        # this process only.
        self.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2

        class MONITORINFOEXW(ctypes.Structure):
            _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD),
                        ("szDevice", wt.WCHAR * 32)]

        monitors = []

        def found(handle, _dc, _rect, _param):
            info = MONITORINFOEXW()
            info.cbSize = ctypes.sizeof(MONITORINFOEXW)
            self.user32.GetMonitorInfoW(wt.HMONITOR(handle), ctypes.byref(info))
            dpi_x, dpi_y = wt.UINT(), wt.UINT()
            shcore.GetDpiForMonitor(wt.HMONITOR(handle), 0, ctypes.byref(dpi_x), ctypes.byref(dpi_y))  # MDT_EFFECTIVE_DPI
            m, w = info.rcMonitor, info.rcWork
            monitors.append({"handle": handle, "name": info.szDevice, "primary": bool(info.dwFlags & 1),
                             "monitor": (m.left, m.top, m.right, m.bottom), "work": (w.left, w.top, w.right, w.bottom),
                             "scale": dpi_x.value / 96.0})
            return True

        callback = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)(found)
        self.user32.EnumDisplayMonitors(None, None, callback, 0)

        def normalised(name):
            name = name[4:] if name.startswith("\\\\.\\") else name
            return name.lower()

        listing = "".join("\n  %s%s: %s, %d%%" % (m["name"], " (primary)" if m["primary"] else "",
                                                   m["monitor"], round(m["scale"] * 100)) for m in monitors)
        matches = [m for m in monitors if normalised(m["name"]) == normalised(value)]
        if not matches:
            raise SystemExit("RAY_TEST_SCREEN=%s names no screen. Name one of the screens that are not primary, "
                             "or unset it. The screens:%s" % (value, listing))
        self.target = matches[0]
        if self.target["primary"]:
            raise SystemExit("RAY_TEST_SCREEN=%s names the primary screen, which the windows of these tests must "
                             "stay off. Name one of the others, or unset it. The screens:%s" % (value, listing))
        self.primary = next(m for m in monitors if m["primary"])
        self.value = value
        self.seen = set()
        self.on_target = set()
        self.violations = []

    def window_geometry(self):
        """The window rectangle the application would choose on the target screen, in Qt's coordinates there: the
        screen's top-left in physical pixels, sizes in device-independent ones (AppHost::load: 90 % of the available
        area, at most 1600 x 1000, centred)."""
        mx, my = self.target["monitor"][:2]
        left, top, right, bottom = self.target["work"]
        scale = self.target["scale"]
        ax, ay = mx + round((left - mx) / scale), my + round((top - my) / scale)
        aw, ah = round((right - left) / scale), round((bottom - top) / scale)
        w, h = min(1600, aw * 9 // 10), min(1000, ah * 9 // 10)
        return {"x": ax + (aw - w) // 2, "y": ay + (ah - h) // 2, "width": w, "height": h}

    def prepare(self, env):
        """Points the program at a settings folder of its own with the window saved on the target screen."""
        base = env.get("RAYCAD_USER_DATA_DIR")
        if not base:
            raise SystemExit("RAY_TEST_SCREEN: RAYCAD_USER_DATA_DIR must be set (the window is saved in the settings "
                             "there, never in the real user profile)")
        folder = os.path.join(base, "test-screen")
        os.makedirs(folder, exist_ok=True)
        settings = {"format": "raycad.settings", "version": 1, "window": self.window_geometry()}
        with open(os.path.join(folder, "settings.json"), "w", encoding="utf-8") as f:
            json.dump(settings, f)
        env["RAYCAD_USER_DATA_DIR"] = folder

    def check(self, pid):
        """Records the shown top-level windows of process `pid`, and those that touch the primary screen."""
        ct, wt = self.ct, self.wt
        windows = []

        def collect(hwnd, _param):
            owner = wt.DWORD()
            self.user32.GetWindowThreadProcessId(wt.HWND(hwnd), ct.byref(owner))
            if owner.value == pid and self.user32.IsWindowVisible(wt.HWND(hwnd)):
                windows.append(hwnd)
            return True

        callback = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(collect)
        self.user32.EnumWindows(callback, 0)
        for hwnd in windows:
            cloaked = wt.DWORD()
            if self.dwm.DwmGetWindowAttribute(wt.HWND(hwnd), 14, ct.byref(cloaked), 4) == 0 and cloaked.value:
                continue  # DWMWA_CLOAKED
            frame = wt.RECT()
            if self.dwm.DwmGetWindowAttribute(wt.HWND(hwnd), 9, ct.byref(frame), ct.sizeof(frame)) != 0:
                self.user32.GetWindowRect(wt.HWND(hwnd), ct.byref(frame))  # DWMWA_EXTENDED_FRAME_BOUNDS failed
            self.seen.add(hwnd)
            if self.user32.MonitorFromWindow(wt.HWND(hwnd), 0) == self.target["handle"]:  # MONITOR_DEFAULTTONULL
                self.on_target.add(hwnd)
            left, top, right, bottom = self.primary["monitor"]
            if frame.left < right and frame.right > left and frame.top < bottom and frame.bottom > top:
                if hwnd not in (v[0] for v in self.violations):
                    name = ct.create_unicode_buffer(128)
                    self.user32.GetClassNameW(wt.HWND(hwnd), name, 128)
                    message = ("RAY_TEST_SCREEN: a window of class %s was shown on the primary screen: frame %d,%d "
                               "%dx%d, primary screen %s" % (name.value, frame.left, frame.top,
                                                              frame.right - frame.left, frame.bottom - frame.top,
                                                              self.primary["monitor"]))
                    print(message, file=sys.stderr, flush=True)
                    self.violations.append((hwnd, message))

    def summary(self):
        return "RAY_TEST_SCREEN: windows on %s: %d top-level windows of the program shown (%d mostly on that " \
               "screen), %s" % (self.target["name"], len(self.seen), len(self.on_target),
                                "none on the primary screen" if not self.violations
                                else "%d on the primary screen" % len(self.violations))


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: run_with_driver_priority.py <program> [arguments...]")
    screen = None
    env = dict(os.environ)
    value = os.environ.get("RAY_TEST_SCREEN", "")
    if value:
        if os.name != "nt":
            sys.exit("RAY_TEST_SCREEN is set, but placing the program's window is done on Windows only; unset it")
        screen = TestScreen(value)
        screen.prepare(env)
    child = subprocess.Popen(sys.argv[1:], env=env if screen else None)
    watch = os.name == "nt" and os.environ.get("RAY_TEST_DRIVER_PRIORITY") != "keep"
    while child.poll() is None:
        if watch:
            raise_idle_threads(child.pid)
        if screen:
            screen.check(child.pid)
        time.sleep(0.02)
    if screen:
        print(screen.summary(), flush=True)
        if screen.violations:
            return 1
    return child.returncode


if __name__ == "__main__":
    sys.exit(main())
