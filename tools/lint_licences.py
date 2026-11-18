#!/usr/bin/env python3
"""Licence ledger check (PLAN.md section 2.4).

THIRD_PARTY.md holds one table row per dependency:

    | Component | vcpkg port | Version | Licence (SPDX) | Source | How it is used |

The "vcpkg port" cell lists port names or glob patterns in backticks (`boost-*`), or "-" for a dependency
that does not come from vcpkg (Qt, vendored code, tools).

Checks:
  1. Every dependency named in vcpkg.json is covered by a ledger row.
  2. With --installed DIR (a vcpkg_installed folder): every port vcpkg installed there, including
     transitive and host ports, is covered by a ledger row, and the licence vcpkg recorded for it
     (share/<port>/vcpkg.spdx.json) is not GPL-only.
  3. No ledger row has a GPL-only licence, and every row has a component, version, licence and source.

"GPL-only" means the SPDX expression cannot be satisfied without GPL or AGPL terms: LGPL is fine,
"GPL-2.0-only OR MIT" is fine, "GPL-3.0-or-later" and "MIT AND GPL-2.0-only" are not.

One exception, and it must be written down: a row whose "How it is used" cell starts with the words
"Build tool only" names a program that runs during the build and is neither linked into RayCAD nor
shipped with it (gperf, Qt's qsb). Such a row may be GPL-only.

Usage: lint_licences.py [--root DIR] [--installed DIR] [--self-test]
"""
import argparse
import fnmatch
import json
import os
import re
import sys
import tempfile

import ray_modules as rm

LEDGER = "THIRD_PARTY.md"
MANIFEST = "vcpkg.json"
BUILD_TOOL_ONLY = "build tool only"
GPL_ID = re.compile(r"^A?GPL-", re.I)
# GPL "WITH" exceptions that permit linking from differently licensed code.
LINKING_EXCEPTIONS = re.compile(r"^(Classpath-exception|GCC-exception|Bison-exception|Autoconf-exception|Libtool-exception|LLVM-exception|OCCT-exception)", re.I)


# ---------- SPDX expressions ----------
def _tokens(expression):
    return re.findall(r"\(|\)|[^\s()]+", expression)


def gpl_only(expression):
    """True if every way to satisfy the SPDX expression involves GPL or AGPL terms."""
    tokens = _tokens(expression)
    position = [0]

    def peek():
        return tokens[position[0]] if position[0] < len(tokens) else None

    def take():
        token = peek()
        position[0] += 1
        return token

    def factor():
        token = take()
        if token is None:
            return False
        if token == "(":
            value = expr()
            if peek() == ")":
                take()
            return value
        value = bool(GPL_ID.match(token))
        if peek() is not None and peek().upper() == "WITH":
            take()
            exception = take() or ""
            if LINKING_EXCEPTIONS.match(exception):
                value = False
        return value

    def term():
        value = factor()
        while peek() is not None and peek().upper() == "AND":
            take()
            value = factor() or value
        return value

    def expr():
        value = term()
        while peek() is not None and peek().upper() == "OR":
            take()
            value = term() and value
        return value

    return expr()


# ---------- Ledger ----------
def parse_ledger(text):
    """Returns (rows, problems). A row is a dict with component, ports, version, licence, source, line."""
    rows = []
    problems = []
    columns = None
    for number, line in enumerate(text.split("\n"), 1):
        stripped = line.strip()
        if not stripped.startswith("|"):
            columns = None
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if columns is None:
            lowered = [cell.lower() for cell in cells]
            if "vcpkg port" in lowered:
                columns = {
                    "component": lowered.index("component") if "component" in lowered else 0,
                    "ports": lowered.index("vcpkg port"),
                    "version": next((i for i, c in enumerate(lowered) if c.startswith("version")), None),
                    "licence": next((i for i, c in enumerate(lowered) if c.startswith("licence")), None),
                    "source": next((i for i, c in enumerate(lowered) if c.startswith("source")), None),
                    "use": next((i for i, c in enumerate(lowered) if c.startswith("how")), None),
                }
                if None in columns.values():
                    problems.append("%s:%d: the ledger table needs Version, Licence, Source and 'How it is used' columns" % (LEDGER, number))
                    columns = None
            continue
        if all(set(cell) <= set("-: ") for cell in cells):
            continue
        if len(cells) <= max(columns.values()):
            problems.append("%s:%d: ledger row has too few cells" % (LEDGER, number))
            continue
        ports_cell = cells[columns["ports"]]
        ports = re.findall(r"`([^`]+)`", ports_cell)
        row = {
            "component": cells[columns["component"]],
            "ports": ports,
            "version": cells[columns["version"]],
            "licence": cells[columns["licence"]].strip("`"),
            "source": cells[columns["source"]],
            "build_tool_only": cells[columns["use"]].lower().startswith(BUILD_TOOL_ONLY),
            "line": number,
        }
        if not ports and ports_cell not in ("-", ""):
            problems.append("%s:%d: write vcpkg ports in backticks, or '-' for none" % (LEDGER, number))
        rows.append(row)
    return rows, problems


def covering_row(rows, port):
    for row in rows:
        if any(fnmatch.fnmatchcase(port, pattern) for pattern in row["ports"]):
            return row
    return None


def manifest_ports(manifest):
    ports = []

    def add(entries):
        for entry in entries or []:
            ports.append(entry if isinstance(entry, str) else entry.get("name", ""))

    add(manifest.get("dependencies"))
    for feature in (manifest.get("features") or {}).values():
        if isinstance(feature, dict):
            add(feature.get("dependencies"))
    return sorted(set(p for p in ports if p))


def installed_ports(installed_dir):
    """Reads vcpkg_installed/vcpkg/status. Returns {port: set(triplets)} for installed packages."""
    status_path = os.path.join(installed_dir, "vcpkg", "status")
    ports = {}
    if not os.path.isfile(status_path):
        return None
    for paragraph in re.split(r"\r?\n\r?\n", rm.read_text(status_path)):
        fields = dict(re.findall(r"^([A-Za-z-]+):[ \t]*(.*?)\r?$", paragraph, re.M))
        if "Package" not in fields or "install ok installed" not in fields.get("Status", ""):
            continue
        ports.setdefault(fields["Package"], set()).add(fields.get("Architecture", ""))
    return ports


def recorded_licence(installed_dir, port, triplets):
    """The licence vcpkg wrote into share/<port>/vcpkg.spdx.json, or None."""
    for triplet in sorted(triplets):
        path = os.path.join(installed_dir, triplet, "share", port, "vcpkg.spdx.json")
        if not os.path.isfile(path):
            continue
        try:
            document = json.loads(rm.read_text(path))
        except ValueError:
            return None
        for package in document.get("packages", []):
            for key in ("licenseConcluded", "licenseDeclared"):
                value = package.get(key)
                if value and value not in ("NOASSERTION", "NONE"):
                    return value
    return None


def check(root, installed_dir=None):
    problems = []
    ledger_path = os.path.join(root, LEDGER)
    manifest_path = os.path.join(root, MANIFEST)
    if not os.path.isfile(ledger_path):
        return ["%s is missing" % LEDGER], "nothing"
    if not os.path.isfile(manifest_path):
        return ["%s is missing" % MANIFEST], "nothing"
    rows, problems = parse_ledger(rm.read_text(ledger_path))
    if not rows:
        problems.append("%s: no ledger table found (a table with a 'vcpkg port' column)" % LEDGER)

    for row in rows:
        where = "%s:%d" % (LEDGER, row["line"])
        for field in ("component", "version", "licence", "source"):
            if not row[field] or row[field] == "?":
                problems.append("%s: '%s' has no %s" % (where, row["component"] or "row", field))
        if row["licence"] and gpl_only(row["licence"]) and not row["build_tool_only"]:
            problems.append("%s: '%s' is GPL-only (%s); RayCAD is LGPL-2.1-or-later and cannot use it" % (where, row["component"], row["licence"]))

    direct = manifest_ports(json.loads(rm.read_text(manifest_path)))
    for port in direct:
        if covering_row(rows, port) is None:
            problems.append("%s: dependency '%s' is not listed in %s" % (MANIFEST, port, LEDGER))
    checked = "%d ledger rows, %d direct vcpkg dependencies" % (len(rows), len(direct))

    if installed_dir is not None:
        ports = installed_ports(installed_dir)
        if ports is None:
            problems.append("%s: no vcpkg/status file; configure the build first" % installed_dir)
        else:
            for port in sorted(ports):
                if covering_row(rows, port) is None:
                    problems.append("installed vcpkg port '%s' (%s) is not listed in %s" % (port, ", ".join(sorted(ports[port])), LEDGER))
                licence = recorded_licence(installed_dir, port, ports[port])
                row = covering_row(rows, port)
                if licence and gpl_only(licence) and not (row and row["build_tool_only"]):
                    problems.append("installed vcpkg port '%s' is GPL-only according to vcpkg (%s)" % (port, licence))
            checked += ", %d installed ports" % len(ports)
    return problems, checked


def _write_installed(root, ports):
    status = ""
    for port, licence in ports.items():
        status += "Package: %s\nVersion: 1.0\nArchitecture: x64-test\nMulti-Arch: same\nAbi: 0\nStatus: install ok installed\n\n" % port
        document = {"packages": [{"name": port, "licenseConcluded": licence or "NOASSERTION"}]}
        rm.write_text("%s/inst/x64-test/share/%s/vcpkg.spdx.json" % (root, port), json.dumps(document))
    rm.write_text(root + "/inst/vcpkg/status", status)


def self_test():
    failures = []
    for expression, expected in [
        ("MIT", False), ("LGPL-2.1-only", False), ("LGPL-2.1-or-later", False), ("LGPL-3.0-only", False),
        ("GPL-3.0-only", True), ("GPL-2.0-or-later", True), ("AGPL-3.0-only", True),
        ("GPL-2.0-only OR MIT", False), ("MIT AND GPL-2.0-only", True), ("GPL-2.0-only AND (MIT OR BSD-3-Clause)", True),
        ("(GPL-2.0-only AND MIT) OR (GPL-3.0-only)", True), ("(GPL-3.0-only OR BSL-1.0) AND MIT", False),
        ("GPL-2.0-or-later WITH Classpath-exception-2.0", False), ("GPL-3.0-only WITH Font-exception-2.0", True),
        ("LGPL-2.1-only WITH OCCT-exception-1.0", False), ("Apache-2.0 OR GPL-2.0-only", False),
    ]:
        rm.expect(failures, gpl_only(expression) == expected, "gpl_only(%r) should be %r" % (expression, expected))

    header = "| Component | vcpkg port | Version | Licence (SPDX) | Source | How it is used |\n|-|-|-|-|-|-|\n"
    good_rows = (
        "| OCCT | `opencascade` | 8.0.1 | `LGPL-2.1-only WITH OCCT-exception-1.0` | https://example.org | dynamic |\n"
        "| Boost | `boost-*` | 1.92 | `BSL-1.0` | https://example.org | headers |\n"
        "| Qt | - | 6.12 | `LGPL-3.0-only` | https://example.org | dynamic |\n"
    )
    manifest = '{"dependencies": ["opencascade", {"name": "boost-graph"}]}'

    def run(ledger_rows, manifest_text=manifest, installed=None):
        with tempfile.TemporaryDirectory() as root:
            rm.write_text(root + "/THIRD_PARTY.md", "# Ledger\n\n" + header + ledger_rows + "\nText after.\n")
            rm.write_text(root + "/vcpkg.json", manifest_text)
            installed_dir = None
            if installed is not None:
                _write_installed(root, installed)
                installed_dir = root + "/inst"
            return check(root, installed_dir)[0]

    rm.expect(failures, run(good_rows) == [], "a clean ledger was reported: %r" % run(good_rows))
    clean_installed = {"opencascade": "LGPL-2.1-only", "boost-graph": "BSL-1.0", "boost-config": None}
    rm.expect(failures, run(good_rows, installed=clean_installed) == [], "a clean installed tree was reported: %r" % run(good_rows, installed=clean_installed))

    problems = run(good_rows, '{"dependencies": ["opencascade", "boost-graph", "newlib"]}')
    rm.expect(failures, len(problems) == 1 and "'newlib' is not listed" in problems[0], "unlisted direct dependency not caught: %r" % problems)
    problems = run(good_rows + "| Solver | `slvs` | 3.1 | `GPL-3.0-or-later` | https://example.org | static |\n")
    rm.expect(failures, len(problems) == 1 and "GPL-only" in problems[0], "GPL-only ledger row not caught: %r" % problems)
    problems = run(good_rows, installed={"opencascade": "LGPL-2.1-only", "zlib": "Zlib"})
    rm.expect(failures, len(problems) == 1 and "'zlib'" in problems[0], "unlisted transitive port not caught: %r" % problems)
    problems = run(good_rows, installed={"opencascade": "LGPL-2.1-only", "boost-evil": "GPL-2.0-only"})
    rm.expect(failures, len(problems) == 1 and "GPL-only according to vcpkg" in problems[0], "GPL-only installed port not caught: %r" % problems)
    tool_row = "| gperf | `gperf` | 3.3 | `GPL-3.0-or-later` | https://example.org | Build tool only: generates a hash table |\n"
    problems = run(good_rows + tool_row, installed={"opencascade": "LGPL-2.1-only", "gperf": "GPL-3.0-or-later"})
    rm.expect(failures, problems == [], "a GPL build tool marked 'Build tool only' was reported: %r" % problems)
    problems = run(good_rows + tool_row.replace("Build tool only: generates", "Linked; generates"))
    rm.expect(failures, len(problems) == 1 and "GPL-only" in problems[0], "GPL row without the build-tool mark not caught: %r" % problems)
    problems = run(good_rows + "| Mystery | `mystery` | 1.0 |  | https://example.org | static |\n")
    rm.expect(failures, len(problems) == 1 and "has no licence" in problems[0], "missing licence not caught: %r" % problems)
    problems = run("")
    rm.expect(failures, any("not listed" in p for p in problems), "empty ledger not caught: %r" % problems)
    return rm.self_test_result("lint_licences", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=rm.REPO_ROOT)
    parser.add_argument("--installed", help="a vcpkg_installed folder to check transitive ports against the ledger")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems, checked = check(args.root, args.installed)
    return rm.report("lint_licences", problems, checked)


if __name__ == "__main__":
    sys.exit(main())
