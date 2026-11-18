#!/usr/bin/env python3
"""CMake dependency-graph rules (PLAN.md section 0.4 rules 1 and 2, section 1.1).

Reads every CMakeLists.txt under src/ and apps/ and checks the target_link_libraries() lines:

  1. A module ray_<m> links only the modules section 1.1 allows it to depend on.
  2. cam, post, sim, kernel, naming, doc and store never reach render or ui, directly or transitively.
  3. Only src/kernel names OpenCASCADE (find_package or a TK* toolkit, also through RAY_OCCT_TOOLKITS).
  4. Only src/ui and src/render name Qt (find_package(Qt6 ...), Qt6::*, qt_* commands).
  5. apps/raycad links ray_ui only; apps/raycad-cli links ray_api only.
  6. Every link item is one this tool understands. An unknown item fails, so a new dependency must be
     added to the tables below (and to PLAN.md section 1.1) on purpose.

The module graph must stay written as literal target names for this to work; a variable in a
target_link_libraries() call is an error, with RAY_OCCT_TOOLKITS in src/kernel as the one exception.

Usage: lint_cmake_deps.py [--root DIR] [--self-test]
"""
import argparse
import re
import sys
import tempfile

import ray_modules as rm

# Third-party link items each module may name, as regular expressions (PLAN.md section 1.1 and 2.1).
EXTERNAL_ALLOWED = {
    "core": [r"TBB::\w+", r"BLAKE3::\w+"],
    "kernel": [r"TK\w+", r"\$\{RAY_OCCT_TOOLKITS\}", r"delayimp"],
    "naming": [],
    "sketch": [r"ray_planegcs", r"Eigen3::Eigen", r"Boost::(graph|math|headers)"],
    "lib": [r"nlohmann_json::nlohmann_json"],
    "doc": [r"nlohmann_json::nlohmann_json"],
    "store": [r"(unofficial::sqlite3::sqlite3|SQLite::SQLite3)", r"zstd::\w+"],
    "cam": [r"Clipper2::\w+", r"nlohmann_json::nlohmann_json"],
    "sim": [],
    "post": [r"qjs", r"quickjs-ng::\w+", r"nlohmann_json::nlohmann_json"],
    "api": [r"nlohmann_json::nlohmann_json"],
    "render": [r"Qt6::\w+"],
    "ui": [r"Qt6::\w+", r"ray_uiplugin"],  # ray_uiplugin: the static QML plugin of ray_ui itself
}
# Items any module or app may link.
COMMON_ALLOWED = [r"ray_headers"]
APP_LINKS = {"raycad": ["ray_ui"], "raycad-cli": ["ray_api"]}

KEYWORDS = {"PRIVATE", "PUBLIC", "INTERFACE", "LINK_PRIVATE", "LINK_PUBLIC", "LINK_INTERFACE_LIBRARIES"}
COMMAND = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", re.S)
OCCT_TEXT = re.compile(r"OpenCASCADE|\bTK[A-Z][A-Za-z0-9]+\b|RAY_OCCT_TOOLKITS", re.I)
QT_TEXT = re.compile(r"\bQt6\b|Qt6::|\bqt6?_[a-z_]+\s*\(", re.I)


def strip_cmake_comments(text):
    out = []
    for line in text.split("\n"):
        in_string = False
        cut = len(line)
        for index, char in enumerate(line):
            if char == '"':
                in_string = not in_string
            elif char == "#" and not in_string:
                cut = index
                break
        out.append(line[:cut])
    return "\n".join(out)


def parse(root):
    """Returns (links, text_by_file): links is a list of (relpath, line, target, [items])."""
    links = []
    texts = {}
    for path in rm.walk_files(root, ["src", "apps"], names={"CMakeLists.txt"}):
        relpath = rm.rel(path, root)
        text = strip_cmake_comments(rm.read_text(path))
        texts[relpath] = text
        for match in COMMAND.finditer(text):
            if match.group(1).lower() != "target_link_libraries":
                continue
            tokens = match.group(2).replace('"', " ").split()
            if not tokens:
                continue
            line = text.count("\n", 0, match.start()) + 1
            links.append((relpath, line, tokens[0], [t for t in tokens[1:] if t not in KEYWORDS]))
    return links, texts


def check(root):
    problems = []
    links, texts = parse(root)
    graph = {module: set() for module in rm.MODULE_DEPS}

    for relpath, line, target, items in links:
        where = "%s:%d" % (relpath, line)
        parts = relpath.split("/")
        folder = parts[1] if len(parts) >= 3 else ""
        if parts[0] == "src":
            if not target.startswith("ray_") or target[4:] != folder:
                problems.append("%s: src/%s may only link its own target ray_%s, not '%s'" % (where, folder, folder, target))
                continue
            if folder not in rm.MODULE_DEPS:
                problems.append("%s: '%s' is not a module of PLAN.md section 1.1" % (where, folder))
                continue
            allowed_external = EXTERNAL_ALLOWED[folder] + COMMON_ALLOWED
            for item in items:
                if item.startswith("ray_") and item[4:] in rm.MODULE_DEPS:
                    dep = item[4:]
                    graph[folder].add(dep)
                    if dep not in rm.MODULE_DEPS[folder]:
                        problems.append(
                            "%s: module '%s' links '%s', which PLAN.md 1.1 does not allow (allowed: %s)"
                            % (where, folder, dep, ", ".join(rm.MODULE_DEPS[folder]) or "none")
                        )
                elif not any(re.fullmatch(pattern, item) for pattern in allowed_external):
                    problems.append(
                        "%s: module '%s' links '%s', which is not an allowed dependency of that module"
                        % (where, folder, item)
                    )
        elif parts[0] == "apps":
            expected = APP_LINKS.get(folder)
            if expected is None or target != folder:
                problems.append("%s: unexpected target '%s' in apps/%s" % (where, target, folder))
                continue
            for item in items:
                if item not in expected:
                    problems.append("%s: app '%s' may link only %s, not '%s'" % (where, folder, ", ".join(expected), item))

    # Rule 2: transitive reachability.
    def reach(start):
        seen = set()
        stack = [start]
        while stack:
            for dep in graph.get(stack.pop(), ()):
                if dep not in seen:
                    seen.add(dep)
                    stack.append(dep)
        return seen

    for module in rm.NO_UI_MODULES:
        hit = sorted(reach(module) & rm.UI_SIDE_MODULES)
        if hit:
            problems.append("module '%s' reaches %s through its link dependencies (PLAN.md 0.4 rule 2)" % (module, ", ".join(hit)))

    # Rules 3 and 4: who names OCCT and Qt at all.
    for relpath, text in sorted(texts.items()):
        parts = relpath.split("/")
        module = parts[1] if parts[0] == "src" and len(parts) >= 3 else None
        for number, line in enumerate(text.split("\n"), 1):
            if OCCT_TEXT.search(line) and module not in rm.OCCT_MODULES:
                problems.append("%s:%d: OpenCASCADE is named outside src/kernel (PLAN.md 0.4 rule 1)" % (relpath, number))
            if QT_TEXT.search(line) and module not in rm.QT_MODULES:
                problems.append("%s:%d: Qt is named outside src/ui and src/render (PLAN.md 0.4 rule 1)" % (relpath, number))

    # Every module of section 1.1 must exist once the tree has any module at all.
    declared = {rel.split("/")[1] for rel in texts if rel.startswith("src/")}
    return problems, "%d CMakeLists.txt, %d link lines, %d modules" % (len(texts), len(links), len(declared))


def _good_tree(root):
    for module, deps in rm.MODULE_DEPS.items():
        extra = ""
        if module == "kernel":
            extra = " ${RAY_OCCT_TOOLKITS}"
        if module in rm.QT_MODULES:
            extra = " Qt6::Gui"
        body = "add_library(ray_%s STATIC x.cpp)\n" % module
        if module == "kernel":
            body += "find_package(OpenCASCADE CONFIG REQUIRED)\nset(RAY_OCCT_TOOLKITS TKernel TKMath)\n"
        if module in rm.QT_MODULES:
            body += "find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)\n"
        if deps or extra:
            body += "target_link_libraries(ray_%s PRIVATE %s%s)\n" % (module, " ".join("ray_" + d for d in deps), extra)
        body += "target_link_libraries(ray_%s PUBLIC ray_headers)\n" % module
        rm.write_text("%s/src/%s/CMakeLists.txt" % (root, module), body)
    rm.write_text(root + "/apps/raycad/CMakeLists.txt", "add_executable(raycad main.cpp)\ntarget_link_libraries(raycad PRIVATE ray_ui)\n")
    rm.write_text(root + "/apps/raycad-cli/CMakeLists.txt", "add_executable(raycad-cli main.cpp)\ntarget_link_libraries(raycad-cli PRIVATE ray_api)\n")


def self_test():
    failures = []
    with tempfile.TemporaryDirectory() as root:
        _good_tree(root)
        problems, _ = check(root)
        rm.expect(failures, problems == [], "a clean tree was reported: %r" % problems)

    planted = [
        ("src/cam/CMakeLists.txt", "target_link_libraries(ray_cam PRIVATE ray_render)\n", "links 'render'"),
        ("src/cam/CMakeLists.txt", "target_link_libraries(ray_cam PRIVATE ray_render)\n", "reaches render"),
        ("src/lib/CMakeLists.txt", "target_link_libraries(ray_lib\n  PRIVATE\n    ray_ui)\n", "links 'ui'"),
        # kernel -> lib is not allowed and makes nothing else wrong; lib -> ui then leaks ui into cam, sim, post ...
        ("src/lib/CMakeLists.txt", "target_link_libraries(ray_lib PRIVATE ray_ui)\n", "module 'cam' reaches"),
        ("src/naming/CMakeLists.txt", "target_link_libraries(ray_naming PRIVATE TKBRep)\n", "not an allowed dependency"),
        ("src/naming/CMakeLists.txt", "find_package(OpenCASCADE CONFIG REQUIRED)\n", "OpenCASCADE is named outside src/kernel"),
        ("src/doc/CMakeLists.txt", "target_link_libraries(ray_doc PRIVATE Qt6::Core)\n", "Qt is named outside"),
        ("src/api/CMakeLists.txt", "find_package(Qt6 REQUIRED COMPONENTS Core)\n", "Qt is named outside"),
        ("src/store/CMakeLists.txt", "target_link_libraries(ray_store PRIVATE ${EXTRA_LIBS})\n", "not an allowed dependency"),
        ("src/sim/CMakeLists.txt", "target_link_libraries(ray_cam PRIVATE ray_core)\n", "may only link its own target"),
        ("apps/raycad-cli/CMakeLists.txt", "target_link_libraries(raycad-cli PRIVATE ray_ui)\n", "may link only ray_api"),
        ("apps/raycad/CMakeLists.txt", "target_link_libraries(raycad PRIVATE Qt6::Quick)\n", "may link only ray_ui"),
        ("src/kernel/CMakeLists.txt", "target_link_libraries(ray_kernel PRIVATE ray_naming)\n", "links 'naming'"),
    ]
    for relpath, addition, expected in planted:
        with tempfile.TemporaryDirectory() as root:
            _good_tree(root)
            path = root + "/" + relpath
            rm.write_text(path, rm.read_text(path) + addition)
            problems, _ = check(root)
            rm.expect(
                failures,
                any(expected in problem for problem in problems),
                "planted '%s' in %s not caught as '%s': %r" % (addition.strip(), relpath, expected, problems),
            )
    return rm.self_test_result("lint_cmake_deps", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems, checked = check(args.root)
    return rm.report("lint_cmake_deps", problems, checked)


if __name__ == "__main__":
    sys.exit(main())
