"""Shared facts and helpers for the RayCAD lint tools (Python 3, standard library only).

The module table mirrors PLAN.md section 1.1. Change it only together with that section.
"""
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# module -> modules it may depend on directly (PLAN.md section 1.1, "May depend on").
MODULE_DEPS = {
    "core": [],
    "kernel": ["core"],
    "naming": ["core", "kernel"],
    "sketch": ["core", "kernel"],
    "lib": ["core"],
    "doc": ["core", "kernel", "naming", "sketch", "cam"],
    "store": ["core", "doc"],
    "cam": ["core", "kernel", "naming", "lib"],
    "sim": ["core", "kernel", "cam", "lib"],
    "post": ["core", "cam", "lib"],
    "api": ["core", "kernel", "naming", "sketch", "lib", "doc", "store", "cam", "sim", "post"],
    "render": ["core", "kernel", "sim"],
    "ui": ["api", "render"],
}

# public header in include/ray/ -> module that owns it.
HEADER_OWNER = {
    "core.h": "core",
    "kernel.h": "kernel",
    "naming.h": "naming",
    "sketch.h": "sketch",
    "libraries.h": "lib",
    "document.h": "doc",
    "store.h": "store",
    "cam.h": "cam",
    "sim.h": "sim",
    "post.h": "post",
    "api.h": "api",
    "render.h": "render",
}

# PLAN.md section 0.4 rule 1.
OCCT_MODULES = {"kernel"}
QT_MODULES = {"ui", "render"}
# PLAN.md section 0.4 rule 2: these never link render or ui, directly or through another module.
NO_UI_MODULES = ["cam", "post", "sim", "kernel", "naming", "doc", "store"]
UI_SIDE_MODULES = {"render", "ui"}
# PLAN.md section 0.4 rule 4 and section 8: code that must not call the platform math library.
DETERMINISTIC_DIRS = ["src/cam", "src/post", "src/naming", "src/sketch", "src/sim"]

CXX_EXTENSIONS = (".h", ".hpp", ".hxx", ".inl", ".c", ".cc", ".cpp", ".cxx", ".mm")


def rel(path, root):
    return os.path.relpath(path, root).replace("\\", "/")


def walk_files(root, subdirs, extensions=None, names=None):
    """Yields files below root/<subdir> for each subdir, sorted, skipping build output and VCS folders."""
    skip = {".git", "build", "out", "vcpkg_installed", "__pycache__", "third_party"}
    for sub in subdirs:
        top = os.path.join(root, sub)
        if not os.path.isdir(top):
            continue
        for folder, dirs, files in os.walk(top):
            dirs[:] = sorted(d for d in dirs if d not in skip)
            for name in sorted(files):
                if names is not None and name in names:
                    yield os.path.join(folder, name)
                elif extensions is not None and name.lower().endswith(extensions):
                    yield os.path.join(folder, name)


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


_CXX_TOKEN = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"([^()\s\\]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S,
)


def strip_cxx(text, keep_strings=False):
    """Blanks comments (and string and character literals unless keep_strings) but keeps every newline,
    so line numbers stay right. Works for C++, QML and JavaScript."""

    def blank(match):
        token = match.group(0)
        if keep_strings and not token.startswith("/"):
            return token
        return re.sub(r"[^\n]", " ", token)

    return _CXX_TOKEN.sub(blank, text)


def module_of(relpath):
    """'src/cam/ops/x.cpp' -> 'cam'; None for a path that is not inside a module folder."""
    parts = relpath.split("/")
    if len(parts) >= 3 and parts[0] == "src" and parts[1] in MODULE_DEPS:
        return parts[1]
    return None


def report(tool, problems, checked):
    """Prints the result of a lint run and returns the process exit code."""
    for problem in problems:
        print(problem)
    if problems:
        print("%s: FAILED, %d problem(s) in %s" % (tool, len(problems), checked), file=sys.stderr)
        return 1
    print("%s: ok (%s)" % (tool, checked))
    return 0


def self_test_result(tool, failures):
    for failure in failures:
        print("self-test: " + failure)
    if failures:
        print("%s self-test: FAILED" % tool, file=sys.stderr)
        return 1
    print("%s self-test: ok" % tool)
    return 0


def expect(failures, condition, what):
    if not condition:
        failures.append(what)
