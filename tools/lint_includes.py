#!/usr/bin/env python3
"""Include rules (PLAN.md section 0.4 rule 1 and section 1.1).

  1. Only src/kernel includes OCCT headers.
  2. Only src/ui and src/render include Qt headers (tests/ui may, for Qt Quick Test).
  3. A file in src/<module>/ includes <ray/X.h> only if X belongs to the module or to a module it may
     depend on (section 1.1). The same holds for the public headers in include/ray/ themselves.

Usage: lint_includes.py [--root DIR] [--self-test]
"""
import argparse
import re
import sys
import tempfile

import ray_modules as rm

SCAN_DIRS = ["src", "include", "apps", "tests"]
QT_ALLOWED_PREFIXES = ("src/ui/", "src/render/", "tests/ui/")
OCCT_ALLOWED_PREFIXES = ("src/kernel/",)

INCLUDE = re.compile(r'^[ \t]*#[ \t]*(?:include|import)[ \t]*([<"])([^>"\n]+)[>"]', re.M)
# OCCT headers are flat names ending in .hxx (BRepPrimAPI_MakeBox.hxx), or live under opencascade/.
OCCT_HEADER = re.compile(r"(^|/)(opencascade/.+|[A-Za-z0-9_]+\.(hxx|lxx|gxx))$")
# Qt headers: <QObject>, <QtCore/...>, <QtGui>, <qglobal.h>, <rhi/qrhi.h>, <private/q...>.
# quickjs.h (QuickJS-NG, used by src/post) starts with a q but is not Qt.
QT_HEADER = re.compile(
    r"^(Q[A-Z][A-Za-z0-9_]*|Qt[A-Z][A-Za-z0-9]*(/.+)?|(.*/)?(?!quickjs)q[a-z0-9_]+\.h|rhi/.+|private/q.+)$"
)
RAY_HEADER = re.compile(r"^(?:ray/)?([a-z_]+\.h)$")


def check(root):
    problems = []
    count = 0
    for path in rm.walk_files(root, SCAN_DIRS, rm.CXX_EXTENSIONS):
        relpath = rm.rel(path, root)
        count += 1
        text = rm.strip_cxx(rm.read_text(path), keep_strings=True)
        if relpath.startswith("include/ray/"):
            module = rm.HEADER_OWNER.get(relpath.split("/")[-1])
        else:
            module = rm.module_of(relpath)
        for match in INCLUDE.finditer(text):
            header = match.group(2).strip()
            line = text.count("\n", 0, match.start()) + 1
            where = "%s:%d" % (relpath, line)
            if OCCT_HEADER.search(header) and not relpath.startswith(OCCT_ALLOWED_PREFIXES):
                problems.append("%s: OCCT header <%s> outside src/kernel (rule 1)" % (where, header))
            if QT_HEADER.match(header) and not relpath.startswith(QT_ALLOWED_PREFIXES):
                problems.append("%s: Qt header <%s> outside src/ui and src/render (rule 1)" % (where, header))
            ray = RAY_HEADER.match(header)
            if ray and module is not None:
                is_ray_include = header.startswith("ray/") or relpath.startswith("include/ray/")
                owner = rm.HEADER_OWNER.get(ray.group(1))
                if is_ray_include and owner and owner != module and owner not in rm.MODULE_DEPS[module]:
                    problems.append(
                        "%s: module '%s' includes %s of module '%s', which PLAN.md 1.1 does not allow"
                        % (where, module, ray.group(1), owner)
                    )
    return problems, count


def self_test():
    failures = []
    with tempfile.TemporaryDirectory() as root:
        rm.write_text(root + "/src/kernel/a.cpp", "#include <BRepPrimAPI_MakeBox.hxx>\n#include <ray/core.h>\n")
        rm.write_text(root + "/src/ui/b.cpp", "#include <QtGui/QGuiApplication>\n#include <ray/api.h>\n")
        rm.write_text(root + "/src/render/c.cpp", "#include <rhi/qrhi.h>\n#include <ray/sim.h>\n")
        rm.write_text(root + "/src/cam/d.cpp", '// #include <QObject>\nconst char* s = "#include <gp_Pnt.hxx>";\n')
        rm.write_text(root + "/include/ray/sim.h", '#include "core.h"\n#include "cam.h"\n')
        problems, _ = check(root)
        rm.expect(failures, problems == [], "a clean tree was reported: %r" % problems)

        planted = {
            "src/cam/occt.cpp": ("#include <gp_Pnt.hxx>\n", "OCCT header"),
            "src/naming/occt2.h": ('#  include "TopoDS_Shape.hxx"\n', "OCCT header"),
            "apps/raycad/qt.cpp": ("#include <QGuiApplication>\n", "Qt header"),
            "src/doc/qt2.cpp": ("#include <QtCore/qglobal.h>\n", "Qt header"),
            "src/kernel/qt3.cpp": ("#include <qstring.h>\n", "Qt header"),
            "src/cam/up.cpp": ("#include <ray/render.h>\n", "does not allow"),
            "src/kernel/up2.cpp": ("#include <ray/naming.h>\n", "does not allow"),
            "include/ray/kernel.h": ('#include "document.h"\n', "does not allow"),
            "tests/unit/kernel/t.cpp": ("#include <BRep_Tool.hxx>\n", "OCCT header"),
        }
        for relpath, (content, expected) in planted.items():
            with tempfile.TemporaryDirectory() as bad:
                rm.write_text(bad + "/" + relpath, content)
                problems, _ = check(bad)
                rm.expect(
                    failures,
                    len(problems) == 1 and expected in problems[0] and relpath in problems[0],
                    "planted violation in %s not caught as '%s': %r" % (relpath, expected, problems),
                )
    return rm.self_test_result("lint_includes", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems, count = check(args.root)
    return rm.report("lint_includes", problems, "%d files" % count)


if __name__ == "__main__":
    sys.exit(main())
