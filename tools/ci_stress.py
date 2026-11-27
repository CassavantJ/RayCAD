#!/usr/bin/env python3
"""Runs one test executable many times at once and records every crash (the stress workflow,
.github/workflows/stress.yml): `rounds` batches of `parallel` processes started together, each under gdb where gdb is
on PATH (--debugger gdb: a backtrace of every thread when it stops on a signal), else plainly.

    ci_stress.py --exe build/linux-gcc/bin/Release/ray_naming_oracle_tests --filter "[random-holdout2]" \
        --env "RAYCAD_NAMING_NO_PLACEMENT=1" --parallel 4 --rounds 7 --budget-minutes 200 --out stress [--debugger gdb]

Writes stress/r<round>-p<slot>.log per process (the whole output, or under gdb the program's output and the
backtraces), stress/results.tsv (round, slot, outcome, exit code, signal, seconds, sanitizer report, last line) and
stress/summary.md (the counts, then per process the outcome and, for a crash, the first frames and the last 50 lines).
An outcome is "ok" (exit 0), "failed" (a test failed: an exit code), "signal" (it died by a signal, or an NTSTATUS
crash code on Windows), "sanitizer" (an AddressSanitizer or UndefinedBehaviorSanitizer report) or "timeout".

Rounds stop early when the next one would not end inside --budget-minutes (measured from the start, by the longest
round so far), so the job stays inside its time limit. Exit status 1 if any process died by a signal, reported a
sanitizer error or timed out; 0 otherwise, whatever the tests answered.
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

SANITIZER = re.compile(r"ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY: UndefinedBehaviorSanitizer")
GDB_SIGNAL = re.compile(r"(?:Program|Thread \S+ .*) received signal (SIG[A-Z]+)|Program terminated with signal (SIG[A-Z]+)")
GDB_EXIT = re.compile(r"\[Inferior \d+ \(process \d+\) exited (?:normally|with code (\d+))\]")
SIGNALS = {4: "SIGILL", 6: "SIGABRT", 7: "SIGBUS", 8: "SIGFPE", 9: "SIGKILL", 11: "SIGSEGV", 15: "SIGTERM"}


def parse_env(text):
    env = {}
    for word in shlex.split(text or ""):
        if "=" not in word:
            raise ValueError(f"'{word}' is not VAR=value")
        k, v = word.split("=", 1)
        env[k] = v
    return env


def command(exe, filt, debugger):
    if debugger == "gdb":
        return ["gdb", "-q", "-batch", "-nx", "-ex", "set pagination off", "-ex", "set print thread-events off",
                "-ex", "handle SIGPIPE nostop noprint", "-ex", "run", "-ex", "thread apply all bt", "--args", exe, filt]
    return [exe, filt]


def classify(code, text, debugger, timed_out):
    """(outcome, exit code, signal) of one finished process."""
    if timed_out:
        return "timeout", None, None
    if SANITIZER.search(text):
        return "sanitizer", code, None
    if debugger == "gdb":
        m = GDB_SIGNAL.search(text)
        if m:
            return "signal", None, m.group(1) or m.group(2)
        e = GDB_EXIT.search(text)
        if e:
            c = int(e.group(1) or "0", 8)  # gdb prints the exit code in octal
            return ("ok" if c == 0 else "failed"), c, None
        return "signal", code, "gdb gave no exit"  # gdb itself ended without the inferior's exit line
    if code is not None and code < 0:
        return "signal", code, SIGNALS.get(-code, f"signal {-code}")
    if code is not None and code >= 0xC0000000:  # an NTSTATUS crash code on Windows (0xC0000005 and others)
        return "signal", code, f"0x{code:08X}"
    return ("ok" if code == 0 else "failed"), code, None


def first_frames(text, n=12):
    frames = [l for l in text.splitlines() if re.match(r"\s*#\d+\s", l)]
    return frames[:n]


def run(a):
    env = dict(os.environ, **parse_env(a.env))
    os.makedirs(a.out, exist_ok=True)
    started = time.time()
    rows, longest = [], 0.0
    for rnd in range(1, a.rounds + 1):
        if longest and (time.time() - started) + longest > a.budget_minutes * 60:
            print(f"round {rnd}: not started, the next round would end past the {a.budget_minutes}-minute budget")
            break
        t0 = time.time()
        procs = []
        for slot in range(1, a.parallel + 1):
            log = open(os.path.join(a.out, f"r{rnd:02d}-p{slot}.log"), "w", encoding="utf-8", errors="replace")
            p = subprocess.Popen(command(a.exe, a.filter, a.debugger), env=env, stdout=log, stderr=subprocess.STDOUT)
            procs.append((slot, p, log, time.time()))
        for slot, p, log, t in procs:
            timed_out = False
            try:
                p.wait(timeout=max(1.0, a.timeout_minutes * 60 - (time.time() - t)))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                timed_out = True
            log.close()
            secs = time.time() - t
            text = open(log.name, encoding="utf-8", errors="replace").read()
            outcome, code, sig = classify(p.returncode, text, a.debugger, timed_out)
            lines = text.splitlines()
            rows.append({"round": rnd, "slot": slot, "outcome": outcome, "code": code, "signal": sig, "secs": secs,
                         "sanitizer": bool(SANITIZER.search(text)), "last": lines[-1] if lines else "",
                         "tail": lines[-50:], "frames": first_frames(text), "log": os.path.basename(log.name)})
        longest = max(longest, time.time() - t0)
        done = [r for r in rows if r["round"] == rnd]
        print(f"round {rnd}: {time.time() - t0:.0f} s; " + ", ".join(f"p{r['slot']} {r['outcome']}" for r in done),
              flush=True)
    return rows


def report(rows, a):
    bad = [r for r in rows if r["outcome"] in ("signal", "sanitizer", "timeout")]
    counts = {o: sum(1 for r in rows if r["outcome"] == o) for o in ("ok", "failed", "signal", "sanitizer", "timeout")}
    signals = {}
    for r in rows:
        if r["signal"]:
            signals[r["signal"]] = signals.get(r["signal"], 0) + 1
    with open(os.path.join(a.out, "results.tsv"), "w", encoding="utf-8") as f:
        f.write("round\tslot\toutcome\texit\tsignal\tseconds\tsanitizer\tlast line\n")
        for r in rows:
            f.write(f"{r['round']}\t{r['slot']}\t{r['outcome']}\t{r['code']}\t{r['signal'] or ''}\t{r['secs']:.0f}\t"
                    f"{int(r['sanitizer'])}\t{r['last'][:200]}\n")
    head = (f"{a.exe} {a.filter} {a.env or ''}: {len(rows)} runs ({a.parallel} at once), "
            + ", ".join(f"{k} {v}" for k, v in counts.items()) + "; signals: "
            + (", ".join(f"{k} {v}" for k, v in sorted(signals.items())) or "none"))
    with open(os.path.join(a.out, "summary.md"), "w", encoding="utf-8") as f:
        f.write(f"# Stress\n\n{head}\n\n| round | slot | outcome | exit | signal | s |\n|-|-|-|-|-|-|\n")
        for r in rows:
            f.write(f"| {r['round']} | {r['slot']} | {r['outcome']} | {r['code']} | {r['signal'] or ''} | "
                    f"{r['secs']:.0f} |\n")
        for r in bad:
            f.write(f"\n## round {r['round']} slot {r['slot']}: {r['outcome']} {r['signal'] or ''} ({r['log']})\n\n")
            f.write("First frames:\n\n```\n" + "\n".join(r["frames"]) + "\n```\n\nLast 50 lines:\n\n```\n"
                    + "\n".join(r["tail"]) + "\n```\n")
    print(head)
    for r in bad:
        print(f"  round {r['round']} slot {r['slot']}: {r['outcome']} {r['signal'] or ''}; "
              + (r["frames"][0].strip() if r["frames"] else r["last"][:160]))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(open(os.path.join(a.out, "summary.md"), encoding="utf-8").read().split("\n## ")[0] + "\n")
    return 1 if bad else 0


def self_test():
    assert parse_env("A=1 RAYCAD_X='a b'") == {"A": "1", "RAYCAD_X": "a b"}
    seg = "Thread 5 \"ray_naming\" received signal SIGSEGV, Segmentation fault.\n#0  0x1 in f ()\n#1  0x2 in g ()\n"
    assert classify(0, seg, "gdb", False) == ("signal", None, "SIGSEGV")
    assert classify(0, "[Inferior 1 (process 7) exited normally]\n", "gdb", False) == ("ok", 0, None)
    assert classify(0, "[Inferior 1 (process 7) exited with code 052]\n", "gdb", False) == ("failed", 42, None)
    assert classify(1, "==1==ERROR: AddressSanitizer: heap-use-after-free", "none", False)[0] == "sanitizer"
    assert classify(-11, "", "none", False) == ("signal", -11, "SIGSEGV")
    assert classify(0xC0000005, "", "none", False) == ("signal", 0xC0000005, "0xC0000005")
    assert classify(42, "test cases: 1 | 1 failed", "none", False) == ("failed", 42, None)
    assert classify(None, "", "none", True)[0] == "timeout"
    assert first_frames(seg) == ["#0  0x1 in f ()", "#1  0x2 in g ()"]
    print("ci_stress self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe")
    ap.add_argument("--filter", default="")
    ap.add_argument("--env", default="")
    ap.add_argument("--parallel", type=int, default=4)
    ap.add_argument("--rounds", type=int, default=15)
    ap.add_argument("--budget-minutes", type=float, default=200)
    ap.add_argument("--timeout-minutes", type=float, default=90, help="per process")
    ap.add_argument("--debugger", choices=["auto", "gdb", "none"], default="auto")
    ap.add_argument("--out", default="stress")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    if a.debugger == "auto":
        a.debugger = "gdb" if shutil.which("gdb") and sys.platform.startswith("linux") else "none"
    print(f"ci_stress: {a.exe} {a.filter} env [{a.env}] x{a.parallel} for {a.rounds} rounds, debugger {a.debugger}")
    return report(run(a), a)


if __name__ == "__main__":
    sys.exit(main())
