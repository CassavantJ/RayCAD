#!/usr/bin/env python3
"""dmath rule (PLAN.md section 0.4 rule 4 and section 8).

Deterministic modules (src/cam, src/post, src/naming, src/sketch, src/sim) must not call the platform
math library for trigonometric, exponential, logarithmic or power functions: results differ between
operating systems. They call ray::dmath instead (core.h).

Reported: a call to one of the functions below that is not qualified with dmath::, whether written
std::sin(x), ::sin(x) or sin(x), and the C header <math.h>/<tgmath.h>. Including <cmath> for floor, fabs,
isfinite and the like is allowed. sqrt is IEEE-exact and is not reported.

A line can be excused with a trailing comment  // dmath-lint: allow <reason>  (for display-only code).

Usage: lint_dmath.py [--root DIR] [--self-test]
"""
import argparse
import re
import sys
import tempfile

import ray_modules as rm

FUNCTIONS = [
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
    "exp", "exp2", "expm1", "log", "log2", "log10", "log1p", "pow", "cbrt", "hypot",
    "erf", "erfc", "tgamma", "lgamma",
]
_SUFFIXED = "|".join(FUNCTIONS)
# An identifier that is one of the functions (optionally with the C f/l suffix), followed by "(".
CALL = re.compile(r"(?<![A-Za-z0-9_])((?:::\s*)?(?:[A-Za-z_][A-Za-z0-9_]*\s*::\s*)*)(%s)[fl]?\s*\(" % _SUFFIXED)
MEMBER_BEFORE = re.compile(r"(\.|->)\s*$")
C_MATH_HEADER = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"](math\.h|tgmath\.h)[>"]', re.M)
ALLOW = "dmath-lint: allow"
# A declaration such as "double sin(double);" names the function without calling the platform one.
DECLARATION_BEFORE = re.compile(r"\b(double|float|auto|constexpr|inline|static)\s+$")


def check_text(relpath, raw):
    problems = []
    text = rm.strip_cxx(raw)
    raw_lines = raw.split("\n")
    for match in C_MATH_HEADER.finditer(rm.strip_cxx(raw, keep_strings=True)):
        line = text.count("\n", 0, match.start()) + 1
        if ALLOW not in raw_lines[line - 1]:
            problems.append("%s:%d: <%s> in a deterministic module; use ray::dmath (PLAN.md section 8)" % (relpath, line, match.group(1)))
    for match in CALL.finditer(text):
        qualifier = re.sub(r"\s+", "", match.group(1))
        # "return ::atan2(y, x)" reads as the qualifier "return::"; the keyword is not a namespace.
        qualifier = re.sub(r"^(return|throw|else|case|co_return|co_yield)(?=::)", "", qualifier)
        if qualifier.endswith("dmath::"):
            continue
        before = text[: match.start()]
        if MEMBER_BEFORE.search(before[-8:]):
            continue  # a member function of some object, e.g. angle.sin()
        if qualifier == "" and DECLARATION_BEFORE.search(before[-24:]):
            continue
        if qualifier not in ("", "::", "std::", "::std::"):
            continue  # some other namespace's function, not the platform library
        line = text.count("\n", 0, match.start()) + 1
        if ALLOW in raw_lines[line - 1]:
            continue
        problems.append(
            "%s:%d: %s%s() in a deterministic module; use ray::dmath::%s (PLAN.md section 8)"
            % (relpath, line, qualifier, match.group(2), match.group(2))
        )
    return problems


def check(root):
    problems = []
    count = 0
    for path in rm.walk_files(root, rm.DETERMINISTIC_DIRS, rm.CXX_EXTENSIONS):
        count += 1
        problems += check_text(rm.rel(path, root), rm.read_text(path))
    return problems, count


def self_test():
    failures = []
    clean = """#include <cmath>
#include <ray/core.h>
// std::sin(x) in a comment
const char* text = "cos(x)";
double f(double x) { return ray::dmath::sin(x) + dmath::cos(x) + std::floor(x) + std::sqrt(x) + std::fabs(x); }
double g(Angle a) { return a.sin() + p->cos(); }
double display(double x) { return std::sin(x); } // dmath-lint: allow backplot display only
int explode(int); int n = explode(3); int cosine(int); int m = cosine(1);
"""
    rm.expect(failures, check_text("src/cam/a.cpp", clean) == [], "clean text was reported: %r" % check_text("src/cam/a.cpp", clean))

    planted = {
        "double f(double x) { return std::sin(x); }\n": "std::sin()",
        "double f(double x) { return cos (x); }\n": "cos()",
        "double f(double x, double y) { return ::atan2(y, x); }\n": "::atan2()",
        "float f(float x) { return sinf(x); }\n": "sin()",
        "double f(double x) { return std::pow(x, 2.5); }\n": "std::pow()",
        "double f(double x) { return std :: exp(x); }\n": "std::exp()",
        "#include <math.h>\n": "<math.h>",
        "double f(double x) { return 1.0 / std::hypot(x, 2.0); }\n": "std::hypot()",
    }
    for content, expected in planted.items():
        problems = check_text("src/cam/bad.cpp", content)
        rm.expect(failures, len(problems) == 1 and expected in problems[0], "planted %r not caught as %s: %r" % (content, expected, problems))

    # The scan covers the deterministic folders and nothing else.
    with tempfile.TemporaryDirectory() as root:
        bad = "double f(double x) { return std::tan(x); }\n"
        for folder in rm.DETERMINISTIC_DIRS:
            rm.write_text("%s/%s/ops/bad.cpp" % (root, folder), bad)
        rm.write_text(root + "/src/render/fine.cpp", bad)
        rm.write_text(root + "/src/ui/fine.cpp", bad)
        problems, count = check(root)
        rm.expect(failures, len(problems) == len(rm.DETERMINISTIC_DIRS) and count == len(rm.DETERMINISTIC_DIRS),
                  "expected one problem per deterministic folder, got %r" % problems)
    return rm.self_test_result("lint_dmath", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems, count = check(args.root)
    return rm.report("lint_dmath", problems, "%d files in %s" % (count, ", ".join(rm.DETERMINISTIC_DIRS)))


if __name__ == "__main__":
    sys.exit(main())
