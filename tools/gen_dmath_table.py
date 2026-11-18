#!/usr/bin/env python3
"""Regenerates tests/unit/core/dmath_table.bin, the dmath reference table (task T-002).

Builds tools/dmath_table/gen_dmath_table.c together with the vendored musl sources in third_party/dmath, with
the floating-point flags of third_party/dmath/CMakeLists.txt (no fused multiply-add contraction, no fast
math), runs it, and writes the table. The table is committed; ray_core_tests checks ray::dmath against it bit for bit on every
platform. Regenerate it only when the vendored code changes on purpose, and say so in the commit.

Usage:
  gen_dmath_table.py [--cc COMPILER] [--out FILE]
  --cc   the C compiler: cl (MSVC, run from a developer prompt or after tools\\win-env.cmd), gcc, clang or cc.
         Default: cl on Windows, otherwise cc.
"""
import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DMATH = os.path.join(ROOT, "third_party", "dmath")
GENERATOR = os.path.join(ROOT, "tools", "dmath_table", "gen_dmath_table.c")
DEFAULT_OUT = os.path.join(ROOT, "tests", "unit", "core", "dmath_table.bin")


def compile_command(cc, sources, exe):
    includes = [os.path.join(DMATH, "src"), os.path.join(DMATH, "include")]
    if os.path.basename(cc).lower() in ("cl", "cl.exe"):
        return [cc, "/nologo", "/std:c11", "/O2", "/fp:precise", "/W0"] + ["/I" + d for d in includes] + sources + ["/Fe" + exe]
    return [cc, "-std=c11", "-O2", "-ffp-contract=off", "-fno-fast-math", "-w"] + ["-I" + d for d in includes] + sources + ["-o", exe, "-lm"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default="cl" if os.name == "nt" else "cc")
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args()

    sources = sorted(glob.glob(os.path.join(DMATH, "src", "*.c"))) + [GENERATOR]
    with tempfile.TemporaryDirectory() as build:
        exe = os.path.join(build, "gen_dmath_table" + (".exe" if os.name == "nt" else ""))
        command = compile_command(args.cc, sources, exe)
        result = subprocess.run(command, cwd=build, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            print("gen_dmath_table: compiling failed", file=sys.stderr)
            return 1
        if subprocess.call([exe, args.out]) != 0:
            return 1
    with open(args.out, "rb") as handle:
        data = handle.read()
    print("gen_dmath_table: wrote %s (%d bytes, sha256 %s)" % (os.path.relpath(args.out, ROOT), len(data), hashlib.sha256(data).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
