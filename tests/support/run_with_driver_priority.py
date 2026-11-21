"""Runs a program and, on Windows, keeps the graphics driver's idle-priority threads in it from starving.

    python -I run_with_driver_priority.py <program> [arguments...]

The process-side counterpart of driver_threads_listener.cpp (which says why), for tests that run an application
rather than a Catch2 executable (raycad_smoke). While the program runs, every 20 ms, each of its threads at
THREAD_PRIORITY_IDLE is raised to normal priority. The program's output goes straight to this process's own, and its
exit code is returned. RAY_TEST_DRIVER_PRIORITY=keep leaves the threads alone. Elsewhere the program just runs.
"""
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


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: run_with_driver_priority.py <program> [arguments...]")
    child = subprocess.Popen(sys.argv[1:])
    watch = os.name == "nt" and os.environ.get("RAY_TEST_DRIVER_PRIORITY") != "keep"
    while child.poll() is None:
        if watch:
            raise_idle_threads(child.pid)
        time.sleep(0.02)
    return child.returncode


if __name__ == "__main__":
    sys.exit(main())
