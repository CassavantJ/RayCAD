#!/usr/bin/env python3
"""Colour-literal rule (PLAN.md section 0.4 rule 6 and section 6.5).

No hard-coded colours in UI or viewport code: colours come from render::Token and the theme files in
resources/themes/. Reported in src/, apps/ and resources/ (except resources/themes/):

  - a quoted hex colour: "#rgb", "#rgba", "#rrggbb", "#aarrggbb" (QML, JavaScript, C++, JSON)
  - a hex colour passed to QColor, QRgb or QColor::fromString: QColor(0xff8800), QColor("#f80")
  - Qt.rgba(...), Qt.hsla(...), Qt.hsva(...), Qt.color(...) and qRgb(...)/qRgba(...) with literal numbers
  - a QML colour property set to a named colour: color: "red"

A line can be excused with a trailing comment  // colour-lint: allow <reason>.

Usage: lint_colours.py [--root DIR] [--self-test]
"""
import argparse
import re
import sys
import tempfile

import ray_modules as rm

SCAN_DIRS = ["src", "apps", "resources"]
EXEMPT_PREFIXES = ("resources/themes/",)
EXTENSIONS = rm.CXX_EXTENSIONS + (".qml", ".js", ".mjs", ".json", ".frag", ".vert", ".comp", ".glsl", ".svg")
ALLOW = "colour-lint: allow"

QUOTED_HEX = re.compile(r"""(["'])#(?:[0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})\1""")
QCOLOR_HEX = re.compile(r"\b(?:QColor|QRgb|QRgba64)\b[^;\n]*?\b0[xX][0-9a-fA-F]{3,8}\b")
QT_FUNCTION = re.compile(r"\b(?:Qt\s*\.\s*(?:rgba|hsla|hsva|color|tint|lighter|darker)|qRgba?)\s*\(\s*[\"'0-9.]")
QML_NAMED = re.compile(r"""\b[A-Za-z.]*[cC]olor\s*:\s*(["'])[a-zA-Z]+\1""")
# "transparent" carries no theme decision.
QML_NAMED_OK = re.compile(r"""(["'])transparent\1""")


def check_text(relpath, raw):
    problems = []
    text = rm.strip_cxx(raw, keep_strings=True)
    raw_lines = raw.split("\n")
    for number, line in enumerate(text.split("\n"), 1):
        if ALLOW in raw_lines[number - 1]:
            continue
        found = None
        for pattern, what in (
            (QUOTED_HEX, "hex colour literal"),
            (QCOLOR_HEX, "hex colour literal"),
            (QT_FUNCTION, "colour built from literal numbers"),
        ):
            match = pattern.search(line)
            if match:
                found = (what, match.group(0))
                break
        if found is None and relpath.endswith(".qml"):
            match = QML_NAMED.search(line)
            if match and not QML_NAMED_OK.search(match.group(0)):
                found = ("named colour literal", match.group(0))
        if found:
            problems.append(
                "%s:%d: %s %s; use a theme token (PLAN.md 0.4 rule 6), colours live in resources/themes/"
                % (relpath, number, found[0], found[1].strip())
            )
    return problems


def check(root):
    problems = []
    count = 0
    for path in rm.walk_files(root, SCAN_DIRS, EXTENSIONS):
        relpath = rm.rel(path, root)
        if relpath.startswith(EXEMPT_PREFIXES):
            continue
        count += 1
        problems += check_text(relpath, rm.read_text(path))
    return problems, count


def self_test():
    failures = []
    clean_qml = """import QtQuick
Rectangle {
    // color: "#ff0000" in a comment
    color: theme.panelBg
    border.color: "transparent"
    property string anchorId: "#section"   // not a colour: wrong length
    Text { text: qsTr("Issue #12345") }
}
"""
    rm.expect(failures, check_text("src/ui/qml/Ok.qml", clean_qml) == [], "clean QML was reported: %r" % check_text("src/ui/qml/Ok.qml", clean_qml))
    clean_cpp = 'std::uint32_t mask = 0xff00ff00u; const char* tag = "#include"; int hash = h("#1");\n'
    rm.expect(failures, check_text("src/render/ok.cpp", clean_cpp) == [], "clean C++ was reported: %r" % check_text("src/render/ok.cpp", clean_cpp))

    planted = [
        ("src/ui/qml/Bad.qml", 'Rectangle { color: "#ff8800" }\n', "#ff8800"),
        ("src/ui/qml/Bad.qml", "Rectangle { color: '#f80' }\n", "#f80"),
        ("src/ui/qml/Bad.qml", 'Rectangle { border.color: "#80ff8800" }\n', "#80ff8800"),
        ("src/ui/qml/Bad.qml", 'Rectangle { color: "red" }\n', "named colour"),
        ("src/ui/qml/Bad.qml", "Rectangle { color: Qt.rgba(1, 0.5, 0, 1) }\n", "literal numbers"),
        ("src/render/bad.cpp", 'QColor c("#112233");\n', "#112233"),
        ("src/render/bad.cpp", "QColor c(0xff8800);\n", "0xff8800"),
        ("src/render/bad.cpp", "auto c = qRgb(255, 128, 0);\n", "literal numbers"),
        ("apps/raycad/bad.cpp", 'const char* accent = "#0A84FF";\n', "#0A84FF"),
    ]
    for relpath, content, expected in planted:
        problems = check_text(relpath, content)
        rm.expect(failures, len(problems) == 1 and expected in problems[0], "planted %r not caught as %s: %r" % (content, expected, problems))

    # Theme files are the one place colours may be written.
    with tempfile.TemporaryDirectory() as root:
        rm.write_text(root + "/resources/themes/dark.json", '{ "Accent": "#0A84FF" }\n')
        rm.write_text(root + "/resources/library/machines/m.json", '{ "name": "Mill" }\n')
        problems, count = check(root)
        rm.expect(failures, problems == [] and count == 1, "theme folder not exempt: %r" % problems)
        rm.write_text(root + "/resources/icons/bad.svg", '<path fill="#123456"/>\n')
        rm.write_text(root + "/src/ui/qml/panels/Bad.qml", 'Item { property color c: "#123456" }\n')
        problems, _ = check(root)
        rm.expect(failures, len(problems) == 2, "expected 2 problems outside resources/themes, got %r" % problems)
    return rm.self_test_result("lint_colours", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems, count = check(args.root)
    return rm.report("lint_colours", problems, "%d files" % count)


if __name__ == "__main__":
    sys.exit(main())
