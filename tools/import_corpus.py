#!/usr/bin/env python3
"""Imports every STEP and IGES file under a folder and reports pass / fail / crash / hang per file (task T-005).

Each file is imported by ray_kernel_io_probe (tests/unit/kernel/io_probe.cpp) in a child process of its own, with
a time limit, so a crash or a hang inside OCCT is caught and blamed on one file instead of ending the run.

  PASS   the import returned at least one body
  FAIL   the import returned an error (FormatError, IoError, KernelFailure, ...): a clean, reported failure
  CRASH  the probe ended any other way (access violation, abort, unexpected exit code)
  HANG   the probe did not finish within the time limit and was killed

The summary also counts files that produced at least one valid solid (BRepCheck), the baseline that PLAN.md T-005
asks to record. Exit code: 0 if there was no crash and no hang, 1 otherwise, 2 for bad arguments.

Usage:
  import_corpus.py FOLDER [--probe EXE] [--timeout SECONDS] [--heal none|reader|full] [--no-sew]
                   [--cancel-stages] [--verbose]
  --probe          path to ray_kernel_io_probe; default: the newest one under build/*/bin/{Release,Debug}
  --timeout        per-file limit in seconds (default 120)
  --cancel-stages  also cancel each stage of each import half-way and print the cancel latency
  --verbose        print the probe's own lines (stages, warnings, cancel results) under each file

Files are matched by extension (.stp, .step, .p21, .igs, .iges, any case). The files are only read, never run.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTENSIONS = (".stp", ".step", ".p21", ".igs", ".iges")
PROBE_NAMES = ("ray_kernel_io_probe.exe", "ray_kernel_io_probe")


def find_probe():
    candidates = []
    for config in ("Release", "Debug", "RelWithDebInfo", ""):
        for name in PROBE_NAMES:
            candidates += glob.glob(os.path.join(ROOT, "build", "*", "bin", config, name))
    candidates = [c for c in candidates if os.path.isfile(c)]
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


def probe_environment(probe):
    """On Windows the delay-loaded OCCT DLLs are not next to the probe; put the vcpkg bin folder on PATH."""
    env = dict(os.environ)
    if os.name == "nt":
        build = os.path.dirname(os.path.dirname(os.path.dirname(probe)))  # build/<preset>
        debug = os.path.basename(os.path.dirname(probe)).lower() == "debug"
        for installed in (os.path.join(os.path.dirname(build), "vcpkg_installed"), os.path.join(build, "vcpkg_installed")):
            triplet = os.path.join(installed, "x64-windows")
            if os.path.isdir(triplet):
                dlls = os.path.join(triplet, "debug", "bin") if debug else os.path.join(triplet, "bin")
                env["PATH"] = dlls + os.pathsep + env.get("PATH", "")
                break
    return env


def corpus_files(folder):
    found = []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames.sort()
        for name in sorted(filenames):
            if name.lower().endswith(EXTENSIONS):
                found.append(os.path.join(dirpath, name))
    return found


def parse_fields(line):
    fields = {}
    for match in re.finditer(r'(\w+)=("([^"]*)"|\S+)', line):
        fields[match.group(1)] = match.group(3) if match.group(3) is not None else match.group(2)
    return fields


def run_one(probe, env, path, args):
    command = [probe, path, "--heal", args.heal]
    if args.no_sew:
        command.append("--no-sew")
    if args.cancel_stages:
        command.append("--cancel-stages")
    start = time.monotonic()
    timeout = args.timeout * (1 + 6 * args.cancel_stages)  # the cancel runs import the file again per stage
    try:
        done = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as expired:
        output = (expired.stdout or b"").decode("utf-8", "replace")
        return "HANG", time.monotonic() - start, {}, output
    elapsed = time.monotonic() - start
    output = done.stdout.decode("utf-8", "replace")
    summary = {}
    for line in output.splitlines():
        if line.startswith("result="):
            summary = parse_fields(line)
    if done.returncode == 0 and summary.get("result") == "ok":
        return "PASS", elapsed, summary, output
    if done.returncode == 1 and summary.get("result") == "error":
        return "FAIL", elapsed, summary, output
    summary["exit"] = "0x%08X" % (done.returncode & 0xFFFFFFFF) if done.returncode < 0 or done.returncode > 255 else str(done.returncode)
    return "CRASH", elapsed, summary, output


def describe(status, summary):
    if status == "PASS":
        return "bodies=%s solids=%s valid_solids=%s units=%s named=%s colored=%s warnings=%s import_ms=%s" % (
            summary.get("bodies"), summary.get("solids"), summary.get("valid_solids"), summary.get("units"),
            summary.get("named"), summary.get("colored"), summary.get("warnings"), summary.get("time_ms"))
    if status == "FAIL":
        return "%s: %s" % (summary.get("code"), summary.get("message"))
    if status == "CRASH":
        return "exit code %s" % summary.get("exit")
    return "killed after the time limit"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("--probe")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--heal", choices=("none", "reader", "full"), default="full")
    parser.add_argument("--no-sew", action="store_true")
    parser.add_argument("--cancel-stages", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    probe = args.probe or find_probe()
    if not probe or not os.path.isfile(probe):
        print("import_corpus: ray_kernel_io_probe not found; build the tests or pass --probe", file=sys.stderr)
        return 2
    if not os.path.isdir(args.folder):
        print("import_corpus: not a folder: %s" % args.folder, file=sys.stderr)
        return 2
    files = corpus_files(args.folder)
    if not files:
        print("import_corpus: no STEP or IGES files under %s" % args.folder, file=sys.stderr)
        return 2
    env = probe_environment(probe)
    print("import_corpus: %d files, probe %s, time limit %g s, heal %s%s" % (
        len(files), probe, args.timeout, args.heal, ", no sewing" if args.no_sew else ""))

    counts = {"PASS": 0, "FAIL": 0, "CRASH": 0, "HANG": 0}
    with_valid_solid = 0
    worst_cancel = 0.0
    for path in files:
        status, elapsed, summary, output = run_one(probe, env, path, args)
        counts[status] += 1
        if status == "PASS" and int(summary.get("valid_solids", "0")) > 0:
            with_valid_solid += 1
        print("%-5s %7.2f s  %s  %s" % (status, elapsed, os.path.relpath(path, args.folder), describe(status, summary)))
        for line in output.splitlines():
            if line.startswith("cancel stage="):
                fields = parse_fields(line)
                if fields.get("outcome") == "cancelled":
                    worst_cancel = max(worst_cancel, float(fields.get("latency_ms", "0")))
            if args.verbose and (line.startswith(("stage ", "warning ", "cancel ", "body ")) or status != "PASS"):
                print("        " + line)
        sys.stdout.flush()

    total = len(files)
    print("import_corpus: %d files: %d pass, %d fail, %d crash, %d hang; %d with at least one valid solid (%.1f%%)" % (
        total, counts["PASS"], counts["FAIL"], counts["CRASH"], counts["HANG"], with_valid_solid,
        100.0 * with_valid_solid / total))
    if args.cancel_stages:
        print("import_corpus: worst cancel latency %.1f ms" % worst_cancel)
    return 0 if counts["CRASH"] == 0 and counts["HANG"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
