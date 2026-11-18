#!/usr/bin/env python3
"""Compares produced outputs with the golden files under tests/golden/ and updates them on request
(PLAN.md section 11: toolpath, G-code and geometry goldens are byte-identical; a change needs a reviewed update).

A test run writes its outputs into a folder laid out like tests/golden/<suite>/. This tool compares the two trees
file by file, byte for byte, and prints a summary: files added, changed and removed, with their sizes, and for a
changed file the first differing line (text) or byte offset (binary). It writes nothing unless --write is given:
then added and changed files are copied into tests/golden/<suite>/. Goldens with no produced counterpart are
deleted only with --write --prune.

Exit code: 0 when the trees are identical or were made identical by --write; 1 when they differ and nothing was
written (so the plain comparison works as a CI check); 2 for a usage error.

Usage:
  update_golden.py --produced DIR --suite NAME            compare DIR with tests/golden/NAME
  update_golden.py --produced DIR --suite NAME --write    and copy added and changed files into the goldens
  update_golden.py --produced DIR --suite NAME --write --prune   and delete goldens that were not produced
  update_golden.py --self-test
"""
import argparse
import filecmp
import os
import shutil
import sys
import tempfile

import ray_modules as rm

GOLDEN_ROOT = os.path.join(rm.REPO_ROOT, "tests", "golden")
MAX_SHOWN = 120


def tree(root):
    """Relative paths (with /) of every file below root."""
    found = {}
    if not os.path.isdir(root):
        return found
    for folder, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(folder, name)
            found[rm.rel(path, root)] = path
    return found


CONTROL = bytes(c for c in range(32) if c not in (9, 10, 12, 13))


def _is_text(data):
    if any(c in CONTROL for c in data[:8192]):
        return False
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def _clip(text):
    text = text.rstrip("\r\n")
    return text if len(text) <= MAX_SHOWN else text[:MAX_SHOWN] + "..."


def first_difference(old, new):
    """A short description of where two byte strings first differ."""
    if _is_text(old) and _is_text(new):
        if old.replace(b"\r\n", b"\n") == new.replace(b"\r\n", b"\n"):
            return "only line endings differ (CRLF against LF)"
        old_lines = old.decode("utf-8").splitlines(True)
        new_lines = new.decode("utf-8").splitlines(True)
        for number, (a, b) in enumerate(zip(old_lines, new_lines), 1):
            if a != b:
                return "line %d:\n      golden:   %s\n      produced: %s" % (number, _clip(a), _clip(b))
        number = min(len(old_lines), len(new_lines)) + 1
        if len(old_lines) > len(new_lines):
            return "line %d: produced output ends here; golden goes on with: %s" % (number, _clip(old_lines[number - 1]))
        return "line %d: golden ends here; produced output goes on with: %s" % (number, _clip(new_lines[number - 1]))
    for offset, (a, b) in enumerate(zip(old, new)):
        if a != b:
            return "byte %d (0x%x): golden %02x, produced %02x" % (offset, offset, a, b)
    offset = min(len(old), len(new))
    return "byte %d (0x%x): %s ends here" % (offset, offset, "produced output" if len(old) > len(new) else "golden")


def compare(produced_dir, golden_dir):
    """Returns (added, changed, removed, same): lists of relative paths; changed holds (path, description)."""
    produced, golden = tree(produced_dir), tree(golden_dir)
    added = sorted(p for p in produced if p not in golden)
    removed = sorted(p for p in golden if p not in produced)
    changed, same = [], []
    for path in sorted(p for p in produced if p in golden):
        if filecmp.cmp(produced[path], golden[path], shallow=False):
            same.append(path)
        else:
            with open(golden[path], "rb") as handle:
                old = handle.read()
            with open(produced[path], "rb") as handle:
                new = handle.read()
            changed.append((path, first_difference(old, new)))
    return added, changed, removed, same


def summary(produced_dir, golden_dir, added, changed, removed, same, out):
    size = lambda root, path: os.path.getsize(os.path.join(root, *path.split("/")))  # noqa: E731
    out("golden:   %s" % golden_dir)
    out("produced: %s" % produced_dir)
    for path in added:
        out("  added    %s (%d bytes)" % (path, size(produced_dir, path)))
    for path, where in changed:
        out("  changed  %s (%d -> %d bytes), first difference: %s" % (
            path, size(golden_dir, path), size(produced_dir, path), where))
    for path in removed:
        out("  removed  %s (%d bytes; golden has it, produced output does not)" % (path, size(golden_dir, path)))
    out("summary: %d added, %d changed, %d removed, %d unchanged" % (len(added), len(changed), len(removed), len(same)))


def write(produced_dir, golden_dir, added, changed, removed, prune, out):
    for path in added + [p for p, _ in changed]:
        target = os.path.join(golden_dir, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(os.path.join(produced_dir, *path.split("/")), target)
    if prune:
        for path in removed:
            os.remove(os.path.join(golden_dir, *path.split("/")))
    out("wrote %d golden file(s)%s" % (
        len(added) + len(changed), ", deleted %d" % len(removed) if prune else
        (", kept %d golden(s) with no produced output (use --prune to delete them)" % len(removed) if removed else "")))


def run(produced_dir, golden_dir, do_write, prune, out=print):
    if not os.path.isdir(produced_dir):
        out("update_golden: produced folder %s does not exist" % produced_dir)
        return 2
    added, changed, removed, same = compare(produced_dir, golden_dir)
    summary(produced_dir, golden_dir, added, changed, removed, same, out)
    differs = bool(added or changed or removed)
    if not differs:
        return 0
    if not do_write:
        out("goldens NOT updated; review the differences, then rerun with --write to accept them")
        return 1
    write(produced_dir, golden_dir, added, changed, removed, prune, out)
    return 1 if removed and not prune else 0


# ---------- self-test ----------

def self_test():
    failures = []
    expect = rm.expect
    with tempfile.TemporaryDirectory() as root:
        produced = os.path.join(root, "produced")
        golden = os.path.join(root, "golden", "toolpaths")
        rm.write_text(os.path.join(golden, "same.nc"), "G0 X0\nG1 X1\n")
        rm.write_text(os.path.join(golden, "text.nc"), "G0 X0\nG1 X1 F100\nM30\n")
        rm.write_text(os.path.join(golden, "old_only.nc"), "%\n")
        rm.write_text(os.path.join(golden, "sub", "eol.nc"), "A\nB\n")
        with open(os.path.join(golden, "path.bin"), "wb") as handle:
            handle.write(bytes([1, 2, 3, 4, 5]))
        rm.write_text(os.path.join(produced, "same.nc"), "G0 X0\nG1 X1\n")
        rm.write_text(os.path.join(produced, "text.nc"), "G0 X0\nG1 X1 F120\nM30\n")
        rm.write_text(os.path.join(produced, "new.nc"), "G0 X5\n")
        os.makedirs(os.path.join(produced, "sub"))
        with open(os.path.join(produced, "sub", "eol.nc"), "wb") as handle:
            handle.write(b"A\r\nB\r\n")
        with open(os.path.join(produced, "path.bin"), "wb") as handle:
            handle.write(bytes([1, 2, 9, 4, 5, 6]))

        def snapshot():
            return {p: open(f, "rb").read() for p, f in tree(golden).items()}

        before = snapshot()
        lines = []
        code = run(produced, golden, False, False, lines.append)
        text = "\n".join(lines)
        expect(failures, code == 1, "differences without --write must exit 1, got %d" % code)
        expect(failures, snapshot() == before, "goldens changed without --write")
        expect(failures, "added    new.nc (6 bytes)" in text, "added file not reported:\n" + text)
        expect(failures, "changed  text.nc (21 -> 21 bytes), first difference: line 2:" in text and "G1 X1 F120" in text,
               "changed text line not reported:\n" + text)
        expect(failures, "changed  path.bin (5 -> 6 bytes), first difference: byte 2 (0x2): golden 03, produced 09" in text,
               "binary difference not reported:\n" + text)
        expect(failures, "sub/eol.nc" in text and "only line endings differ" in text, "CRLF difference not named:\n" + text)
        expect(failures, "removed  old_only.nc (2 bytes" in text, "removed file not reported:\n" + text)
        expect(failures, "summary: 1 added, 3 changed, 1 removed, 1 unchanged" in text, "summary line wrong:\n" + text)

        lines = []
        code = run(produced, golden, True, False, lines.append)
        after = snapshot()
        expect(failures, after.get("new.nc") == b"G0 X5\n" and after.get("path.bin") == bytes([1, 2, 9, 4, 5, 6]),
               "--write did not copy added and changed files")
        expect(failures, "old_only.nc" in after and code == 1, "--write without --prune must keep removed goldens")
        lines = []
        code = run(produced, golden, True, True, lines.append)
        expect(failures, code == 0 and "old_only.nc" not in snapshot(), "--write --prune did not delete the stale golden")
        lines = []
        code = run(produced, golden, False, False, lines.append)
        expect(failures, code == 0 and "summary: 0 added, 0 changed, 0 removed, 5 unchanged" in "\n".join(lines),
               "rerun after --write is not clean: %r" % lines)

        expect(failures, "line 3: produced output ends here" in first_difference(b"a\nb\nc\n", b"a\nb\n"),
               "shorter text not described")
        expect(failures, run(os.path.join(root, "missing"), golden, False, False, lambda *_: None) == 2,
               "missing produced folder is not a usage error")
    return rm.self_test_result("update_golden", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--produced", help="folder with the outputs of a test run, laid out like the golden suite")
    parser.add_argument("--suite", help="golden suite under tests/golden/, e.g. toolpaths, gcode, geometry")
    parser.add_argument("--golden-root", default=GOLDEN_ROOT)
    parser.add_argument("--write", action="store_true", help="copy added and changed files into the goldens")
    parser.add_argument("--prune", action="store_true", help="with --write: delete goldens that were not produced")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.produced or not args.suite:
        parser.error("--produced and --suite are required")
    if args.prune and not args.write:
        parser.error("--prune only works together with --write")
    if os.path.isabs(args.suite) or ".." in args.suite.replace("\\", "/").split("/"):
        parser.error("--suite is a folder name under tests/golden/")
    return run(args.produced, os.path.join(args.golden_root, args.suite), args.write, args.prune)


if __name__ == "__main__":
    sys.exit(main())
