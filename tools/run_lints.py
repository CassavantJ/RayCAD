#!/usr/bin/env python3
"""Runs every RayCAD lint on the tree (including the corpus manifest check), then every self-test, including those of
the corpus fetcher and the golden-file updater. Exit code 1 if anything fails.

Usage: run_lints.py [--installed DIR] [--format]
  --installed DIR   also check the licence ledger against a vcpkg_installed folder (transitive ports)
  --format          also run tools/check_format.py (needs clang-format)
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LINTS = ["lint_includes", "lint_cmake_deps", "lint_dmath", "lint_colours", "lint_licences", "corpus_check"]
# Tools that have nothing to check on the tree by themselves, only a self-test.
SELF_TESTS = ["corpus_fetch", "update_golden", "ci_naming_evidence", "ci_naming_probe", "ci_stress"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--installed")
    parser.add_argument("--format", action="store_true")
    args = parser.parse_args()

    runs = [[name + ".py"] for name in LINTS]
    runs += [[name + ".py", "--self-test"] for name in LINTS + SELF_TESTS]
    if args.installed:
        runs.append(["lint_licences.py", "--installed", args.installed])
    if args.format:
        runs.append(["check_format.py"])

    failed = []
    for run in runs:
        sys.stdout.flush()
        code = subprocess.call([sys.executable, os.path.join(HERE, run[0])] + run[1:])
        if code != 0:
            failed.append(" ".join(run))
    if failed:
        print("run_lints: FAILED: " + "; ".join(failed), file=sys.stderr)
        return 1
    print("run_lints: all %d runs ok" % len(runs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
