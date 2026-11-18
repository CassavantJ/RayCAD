#!/usr/bin/env python3
"""clang-format check for RayCAD's own C++ sources (src/, apps/, tests/).

include/ray/ is left alone: those headers are kept line-for-line comparable with docs/interfaces/.
The style is .clang-format at the repository root. CI uses clang-format 22; other major versions may
format a few constructs differently, so use the same one locally.

Usage:
  check_format.py            report files that are not formatted (exit code 1 if any)
  check_format.py --fix      reformat them in place
  check_format.py --clang-format PATH   use this clang-format (default: $CLANG_FORMAT, then PATH)
"""
import argparse
import os
import shutil
import subprocess
import sys

import ray_modules as rm

SCAN_DIRS = ["src", "apps", "tests"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("--clang-format", dest="tool", default=os.environ.get("CLANG_FORMAT"))
    args = parser.parse_args()

    tool = args.tool or shutil.which("clang-format")
    if not tool:
        print("check_format: clang-format not found; set CLANG_FORMAT or pass --clang-format", file=sys.stderr)
        return 2
    version = subprocess.run([tool, "--version"], capture_output=True, text=True).stdout.strip()
    files = list(rm.walk_files(args.root, SCAN_DIRS, rm.CXX_EXTENSIONS))
    if not files:
        print("check_format: no files")
        return 0

    command = [tool, "--style=file"] + (["-i"] if args.fix else ["--dry-run", "--Werror"])
    failed = 0
    for start in range(0, len(files), 50):
        result = subprocess.run(command + files[start : start + 50], cwd=args.root, capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        failed += result.returncode != 0
    if failed:
        print("check_format: FAILED (%s); run tools/check_format.py --fix" % version, file=sys.stderr)
        return 1
    print("check_format: ok (%d files, %s)" % (len(files), version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
