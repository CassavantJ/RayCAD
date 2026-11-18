#!/usr/bin/env python3
"""Checks the test corpus manifest (tests/corpus/manifest.json) against the files (PLAN.md section 11).

For every committed entry (path under files/): the file exists, and its size and SHA-256 match the manifest.
Every file under tests/corpus/files/ must be listed. The manifest itself is validated: unique ids and paths,
required fields, known formats, licence and source recorded, and only redistributable files committed.
Fetch-only entries (path under cache/) are checked only with --cache, after tools/corpus_fetch.py has run.
Needs no network.

Usage:
  corpus_check.py                       check the manifest and the committed files
  corpus_check.py --cache               also check the fetched files in tests/corpus/cache/
  corpus_check.py --summary             also print counts by format, source and licence
  corpus_check.py --describe FILE...    print a manifest row (format, size, hash, exporter, units) for new files
  corpus_check.py --self-test
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys
import tempfile

import ray_modules as rm

CORPUS_DIR = os.path.join(rm.REPO_ROOT, "tests", "corpus")
FORMATS = ("STEP", "IGES", "DXF")
EXPECT = ("pass", "fail")
REQUIRED = ("id", "path", "format", "size", "sha256", "source", "url", "licence", "exporter", "units",
            "redistributable", "expect")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._/-]*$")
PATH_RE = re.compile(r"^(files|cache)/[A-Za-z0-9._/+-]+$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


# ---------- file inspection (also used by --describe) ----------

def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def format_of(name, head):
    """Format from the extension; the magic of the first bytes wins where it is unambiguous."""
    lower = name.lower()
    if head.lstrip().startswith(b"ISO-10303-21"):
        return "STEP"
    if head.startswith(b"AutoCAD Binary DXF"):
        return "DXF"
    if lower.endswith((".stp", ".step", ".p21", ".stpz")):
        return "STEP"
    if lower.endswith((".igs", ".iges")):
        return "IGES"
    if lower.endswith(".dxf"):
        return "DXF"
    return None


def _step_params(text):
    """Splits a STEP parameter list at its top-level commas, respecting strings and nested lists."""
    params, depth, quoted, current = [], 0, False, ""
    for ch in text:
        if quoted:
            current += ch
            if ch == "'":
                quoted = False  # an escaped quote '' simply re-enters quoted mode on the next character
            continue
        if ch == "'":
            quoted = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            params.append(current.strip())
            current = ""
            continue
        current += ch
    params.append(current.strip())
    return params


def _step_strings(text):
    """The string literals in a STEP parameter, in order ('' is an escaped quote)."""
    return [m.group(1).replace("''", "'") for m in re.finditer(r"'((?:[^']|'')*)'", text)]


def _step_string(param):
    values = _step_strings(param)
    return values[0].strip() if values else ""


def _step_header_entry(header, name):
    match = re.search(r"\b" + name + r"\s*\((.*?)\)\s*;", header, re.S)
    return match.group(1) if match else None


STEP_PREFIX_UNITS = {"MILLI": "mm", "CENTI": "cm", "DECI": "dm", "MICRO": "um", "KILO": "km", "": "m"}
STEP_NAMED_UNITS = {"INCH": "inch", "FOOT": "foot", "MIL": "mil", "THOU": "mil", "YARD": "yard", "MILE": "mile",
                    "MILLIMETRE": "mm", "MILLIMETER": "mm", "CENTIMETRE": "cm", "CENTIMETER": "cm", "METRE": "m",
                    "METER": "m", "MICROMETRE": "um", "MICRON": "um"}


def inspect_step(data):
    text = data.decode("latin-1", "replace")
    info = {"exporter": None, "units": None, "schema": None}
    end = text.find("ENDSEC")
    header = text[: end if end > 0 else 20000]
    file_name = _step_header_entry(header, "FILE_NAME")
    if file_name:
        # FILE_NAME(name, time_stamp, (author), (organization), preprocessor_version, originating_system,
        # authorization). The exporter is the originating system, then the preprocessor (translator) version.
        fields = _step_params(file_name)
        preprocessor = _step_string(fields[4]) if len(fields) > 4 else ""
        originating = _step_string(fields[5]) if len(fields) > 5 else ""
        parts = [p for p in (originating, preprocessor) if p and p.upper() not in ("UNKNOWN", "NONE", "$")]
        if len(parts) == 2 and parts[0] == parts[1]:
            parts = parts[:1]
        info["exporter"] = " / ".join(parts) if parts else None
    file_schema = _step_header_entry(header, "FILE_SCHEMA")
    if file_schema:
        names = _step_strings(file_schema)
        info["schema"] = ", ".join(n.strip() for n in names if n.strip()) or None
    # The length unit is the one a GLOBAL_UNIT_ASSIGNED_CONTEXT refers to; a conversion-based unit (inch) also
    # brings the SI unit it is defined from, which must not be counted.
    entities = []
    for context in re.finditer(r"GLOBAL_UNIT_ASSIGNED_CONTEXT\s*\(\s*\(([^)]*)\)", text):
        for ref in re.findall(r"#(\d+)", context.group(1)):
            match = re.search(r"#" + ref + r"\s*=\s*(.*?);", text, re.S)
            if match:
                entities.append(match.group(1))
    if not entities:
        entities = [m.group(1) for m in re.finditer(r"(\([^;]*?LENGTH_UNIT\s*\(\s*\)[^;]*?\))\s*;", text)]
    units = []
    for entity in entities:
        if not re.search(r"LENGTH_UNIT\s*\(", entity):
            continue
        si = re.search(r"SI_UNIT\s*\(\s*(?:\.(\w+)\.|\$)\s*,\s*\.METRE\.", entity)
        named = re.search(r"CONVERSION_BASED_UNIT\s*\(\s*'([^']*)'", entity)
        if named:
            unit = STEP_NAMED_UNITS.get(named.group(1).strip().upper(), named.group(1).strip().lower())
        elif si:
            unit = STEP_PREFIX_UNITS.get(si.group(1) or "", (si.group(1) or "").lower() + "m")
        else:
            continue
        if unit not in units:
            units.append(unit)
    info["units"] = ", ".join(units) if units else None
    return info


IGES_UNITS = {1: "inch", 2: "mm", 3: None, 4: "foot", 5: "mile", 6: "m", 7: "km", 8: "mil", 9: "um", 10: "cm",
              11: "microinch"}


def _iges_global_params(text):
    """Splits the IGES global section into parameters, honouring Hollerith strings (nH...)."""
    lines = text.splitlines()
    glob = "".join(line[:72] for line in lines if len(line) >= 73 and line[72] == "G")
    if not glob:
        return []
    # Parameters 1 and 2 name the delimiters (default "," and ";"), themselves as Hollerith strings.
    pdelim, rdelim = ",", ";"
    if glob.startswith("1H"):
        pdelim = glob[2]
    params, i, current = [], 0, ""
    while i < len(glob):
        match = re.match(r"\s*(\d+)H", glob[i:])
        if match and current.strip() == "":
            count = int(match.group(1))
            start = i + match.end()
            current = glob[start : start + count]
            i = start + count
            continue
        ch = glob[i]
        if ch == pdelim or ch == rdelim:
            params.append(current.strip())
            current = ""
            if len(params) == 2 and len(params[1]) == 1:
                rdelim = params[1]
            elif ch == rdelim:
                break
        else:
            current += ch
        i += 1
    return params


def inspect_iges(data):
    text = data.decode("latin-1", "replace")
    info = {"exporter": None, "units": None, "schema": None}
    params = _iges_global_params(text)
    if len(params) >= 15:
        system, preprocessor = params[4], params[5]
        parts = [p for p in (system, preprocessor) if p]
        if len(parts) == 2 and parts[0] == parts[1]:
            parts = parts[:1]
        info["exporter"] = " / ".join(parts) or None
        try:
            flag = int(float(params[13] or "0"))
        except ValueError:
            flag = 0
        unit = IGES_UNITS.get(flag)
        if unit is None and params[14]:
            unit = params[14].strip().lower()
        info["units"] = unit
    if len(params) >= 23 and params[22]:
        version = {"1": "1.0", "3": "2.0", "4": "3.0", "5": "4.0", "6": "5.0", "7": "5.1", "8": "5.2",
                   "9": "5.3", "10": "5.3", "11": "5.3"}.get(params[22].split(".")[0])
        if version:
            info["schema"] = "IGES " + version
    return info


DXF_VERSIONS = {"AC1006": "R10", "AC1009": "R11/R12", "AC1012": "R13", "AC1014": "R14", "AC1015": "R2000",
                "AC1018": "R2004", "AC1021": "R2007", "AC1024": "R2010", "AC1027": "R2013", "AC1032": "R2018"}
DXF_INSUNITS = {0: None, 1: "inch", 2: "foot", 3: "mile", 4: "mm", 5: "cm", 6: "m", 7: "km", 8: "microinch",
                9: "mil", 10: "yard", 13: "um", 14: "dm"}


def inspect_dxf(data):
    info = {"exporter": None, "units": None, "schema": None}
    if data.startswith(b"AutoCAD Binary DXF"):
        info["schema"] = "binary DXF"
        match = re.search(rb"\$ACADVER\x00\x01(AC\d{4})", data)
        if match:
            version = match.group(1).decode()
            info["schema"] = "binary DXF %s (%s)" % (version, DXF_VERSIONS.get(version, "?"))
        return info
    text = data.decode("latin-1", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.split("\n")]
    pairs = list(zip(lines[0::2], lines[1::2]))
    values, comments, appids = {}, [], []
    for index, (code, value) in enumerate(pairs[:20000]):
        if code == "999" and len(comments) < 3:
            comments.append(value)
        if code == "9" and index + 1 < len(pairs):
            values.setdefault(value, pairs[index + 1][1])
        if code == "0" and value == "APPID":
            for code2, value2 in pairs[index + 1 : index + 12]:
                if code2 == "2":
                    # AutoCAD's own application ids appear in files from many writers and name no exporter.
                    internal = value2.upper().startswith("ACAD") or value2.upper() == "HATCHBACKGROUNDCOLOR"
                    if not internal and value2 not in appids:
                        appids.append(value2)
                    break
    version = values.get("$ACADVER")
    if version:
        info["schema"] = "DXF %s (%s)" % (version, DXF_VERSIONS.get(version, "?"))
    try:
        info["units"] = DXF_INSUNITS.get(int(values.get("$INSUNITS", "0")))
    except ValueError:
        info["units"] = None
    if info["units"] is None and values.get("$MEASUREMENT") in ("0", "1"):
        info["units"] = "unitless (MEASUREMENT=%s)" % ("imperial" if values["$MEASUREMENT"] == "0" else "metric")
    hints = []
    if comments:
        hints.append(comments[0])
    if appids:
        hints.append("APPID " + ", ".join(appids[:3]))
    info["exporter"] = "; ".join(hints) if hints else None
    return info


def describe(path, name=None):
    """A manifest row for a local file, with the fields that can be read from the file itself."""
    with open(path, "rb") as handle:
        data = handle.read()
    name = name or os.path.basename(path)
    fmt = format_of(name, data[:200])
    info = {"exporter": None, "units": None, "schema": None}
    try:
        if fmt == "STEP":
            info = inspect_step(data)
        elif fmt == "IGES":
            info = inspect_iges(data)
        elif fmt == "DXF":
            info = inspect_dxf(data)
    except Exception as error:  # a broken file must not stop the description of the others
        info["exporter"] = "unreadable header (%s)" % type(error).__name__
    return {
        "format": fmt,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "exporter": info["exporter"],
        "units": info["units"],
        "schema": info["schema"],
    }


# ---------- manifest checks ----------

def load_manifest(corpus_dir):
    with open(os.path.join(corpus_dir, "manifest.json"), "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_manifest(manifest):
    problems = []
    sources = manifest.get("sources", {})
    archives = manifest.get("archives", {})
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        return ["manifest: no 'files' list"]
    ids, paths = set(), set()
    for index, entry in enumerate(files):
        where = "manifest file %d (%s)" % (index, entry.get("id", "?"))
        for field in REQUIRED:
            if field not in entry:
                problems.append("%s: missing field '%s'" % (where, field))
        if any(field not in entry for field in REQUIRED):
            continue
        if not ID_RE.match(entry["id"]):
            problems.append("%s: bad id" % where)
        if entry["id"] in ids:
            problems.append("%s: duplicate id" % where)
        ids.add(entry["id"])
        path = entry["path"]
        if not PATH_RE.match(path) or ".." in path.split("/"):
            problems.append("%s: bad path %r (relative to tests/corpus, under files/ or cache/)" % (where, path))
        if path.lower() in paths:
            problems.append("%s: duplicate path %s" % (where, path))
        paths.add(path.lower())
        if entry["format"] not in FORMATS:
            problems.append("%s: unknown format %r" % (where, entry["format"]))
        if not isinstance(entry["size"], int) or entry["size"] < 0:
            problems.append("%s: bad size" % where)
        if not SHA_RE.match(str(entry["sha256"])):
            problems.append("%s: bad sha256" % where)
        if entry["source"] not in sources:
            problems.append("%s: source %r not in 'sources'" % (where, entry["source"]))
        if not str(entry["licence"]).strip():
            problems.append("%s: no licence" % where)
        if not str(entry["url"]).startswith("https://") and not str(entry["url"]).startswith("tools/"):
            problems.append("%s: url must be https:// or the tools/ script that generated the file" % where)
        if entry["expect"] not in EXPECT:
            problems.append("%s: expect must be one of %s" % (where, "/".join(EXPECT)))
        if path.startswith("files/") and entry["redistributable"] is not True:
            problems.append("%s: committed but not redistributable" % where)
        if path.startswith("cache/"):
            if "archive" in entry:
                if entry["archive"] not in archives:
                    problems.append("%s: archive %r not in 'archives'" % (where, entry["archive"]))
                if not entry.get("member"):
                    problems.append("%s: archive entry without 'member'" % where)
            elif not str(entry["url"]).startswith("https://"):
                problems.append("%s: fetch-only entry needs an https url" % where)
    for name, archive in archives.items():
        for field in ("url", "size", "sha256"):
            if field not in archive:
                problems.append("manifest archive %s: missing %s" % (name, field))
    return problems


def check_files(manifest, corpus_dir, with_cache):
    problems, notes = [], []
    listed = set()
    for entry in manifest["files"]:
        path = entry.get("path", "")
        listed.add(path.lower())
        committed = path.startswith("files/")
        if not committed and not with_cache:
            continue
        full = os.path.join(corpus_dir, *path.split("/"))
        if not os.path.isfile(full):
            if committed:
                problems.append("%s: missing %s" % (entry["id"], path))
            else:
                notes.append("%s: not fetched yet" % entry["id"])
            continue
        size = os.path.getsize(full)
        if size != entry["size"]:
            problems.append("%s: size %d, manifest says %d (%s)" % (entry["id"], size, entry["size"], path))
            continue
        if sha256_of(full) != entry["sha256"]:
            problems.append("%s: SHA-256 differs from the manifest (%s)" % (entry["id"], path))
    files_dir = os.path.join(corpus_dir, "files")
    for folder, dirs, names in os.walk(files_dir):
        dirs.sort()
        for name in sorted(names):
            rel = rm.rel(os.path.join(folder, name), corpus_dir)
            if rel.lower() not in listed:
                problems.append("unlisted file %s: add it to manifest.json or remove it" % rel)
    return problems, notes


def summary(manifest):
    files = manifest["files"]
    lines = []

    def table(title, key):
        counter = collections.Counter(key(entry) for entry in files)
        lines.append(title + ":")
        for name, count in sorted(counter.items(), key=lambda item: (-item[1], item[0])):
            lines.append("  %5d  %s" % (count, name))

    table("by format", lambda e: e["format"])
    table("by source", lambda e: e["source"])
    table("by licence", lambda e: e["licence"])
    committed = [e for e in files if e["path"].startswith("files/")]
    lines.append("committed: %d files, %.2f MB; fetch-only: %d files, %.2f MB" % (
        len(committed), sum(e["size"] for e in committed) / 1e6, len(files) - len(committed),
        sum(e["size"] for e in files if not e["path"].startswith("files/")) / 1e6))
    lines.append("expect fail: %d; generated: %d" % (
        sum(e["expect"] == "fail" for e in files), sum(e.get("generated", False) for e in files)))
    return "\n".join(lines)


def run_check(corpus_dir, with_cache, show_summary):
    try:
        manifest = load_manifest(corpus_dir)
    except (OSError, ValueError) as error:
        print("corpus_check: cannot read manifest: %s" % error, file=sys.stderr)
        return 1
    problems = check_manifest(manifest)
    notes = []
    if not problems:
        file_problems, notes = check_files(manifest, corpus_dir, with_cache)
        problems += file_problems
    if show_summary and not problems:
        print(summary(manifest))
    if notes:
        print("corpus_check: %d fetch-only file(s) not in the cache; run tools/corpus_fetch.py" % len(notes))
    committed = sum(e["path"].startswith("files/") for e in manifest.get("files", []))
    return rm.report("corpus_check", problems, "%d entries, %d committed%s" % (
        len(manifest.get("files", [])), committed, ", cache checked" if with_cache else ""))


# ---------- self-test ----------

STEP_SAMPLE = b"""ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('a part'),'2;1');
FILE_NAME('part.stp','2020-01-01T00:00:00',('someone'),('some org'),
  'ST-DEVELOPER v16','SolidWorks 2019',' ');
FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));
ENDSEC;
DATA;
#1=(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.));
#2=(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.));
ENDSEC;
END-ISO-10303-21;
"""
STEP_INCH = b"""ISO-10303-21;
HEADER;
FILE_DESCRIPTION((''),'2;1');
FILE_NAME('x','t',(''),(''),'pre','',$);
FILE_SCHEMA(('CONFIG_CONTROL_DESIGN'));
ENDSEC;
DATA;
#5=( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNIT_ASSIGNED_CONTEXT((#7,#11)) REPRESENTATION_CONTEXT('','') );
#7=( CONVERSION_BASED_UNIT('INCH',#8) LENGTH_UNIT() NAMED_UNIT(#9) );
#8=LENGTH_MEASURE_WITH_UNIT(LENGTH_MEASURE(25.4),#10);
#10=( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) );
#11=( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) );
ENDSEC;
"""


def iges_sample():
    glob = ("1H,,1H;,4Hpart,8Hpart.igs,9HSomeCAD 7,11HIGES 5.3 pp,32,38,6,308,15,4Hpart,1.,1,4HINCH,1,0.1,"
            "15H20200101.000000,1E-05,100.,4Hname,3Horg,11,0,15H20200101.000000;")
    lines = ["%-72sS%7d" % ("sample", 1)]
    for i in range(0, len(glob), 72):
        lines.append("%-72sG%7d" % (glob[i : i + 72], i // 72 + 1))
    return ("\n".join(lines) + "\n").encode()


DXF_SAMPLE = b"999\nSomeCAD DXF writer\n0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1015\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n" \
             b"0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nAPPID\n0\nAPPID\n2\nACAD\n70\n0\n0\nAPPID\n2\nEZDXF\n70\n0\n" \
             b"0\nENDTAB\n0\nENDSEC\n0\nEOF\n"


def self_test():
    failures = []
    expect = rm.expect

    info = inspect_step(STEP_SAMPLE)
    expect(failures, info["exporter"] == "SolidWorks 2019 / ST-DEVELOPER v16", "STEP exporter: %r" % info)
    expect(failures, info["units"] == "mm", "STEP units: %r" % info)
    expect(failures, info["schema"].startswith("AUTOMOTIVE_DESIGN"), "STEP schema: %r" % info)
    info = inspect_step(STEP_INCH)
    expect(failures, info["units"] == "inch" and info["exporter"] == "pre", "STEP inch: %r" % info)
    info = inspect_iges(iges_sample())
    expect(failures, info["exporter"] == "SomeCAD 7 / IGES 5.3 pp", "IGES exporter: %r" % info)
    expect(failures, info["units"] == "inch" and info["schema"] == "IGES 5.3", "IGES units/version: %r" % info)
    info = inspect_dxf(DXF_SAMPLE)
    expect(failures, info["units"] == "mm" and info["schema"] == "DXF AC1015 (R2000)", "DXF: %r" % info)
    expect(failures, info["exporter"] == "SomeCAD DXF writer; APPID EZDXF", "DXF exporter: %r" % info)
    expect(failures, format_of("x.bin", b"ISO-10303-21;") == "STEP", "STEP magic not recognised")
    expect(failures, format_of("x.igs", b"garbage") == "IGES", "IGES extension not recognised")

    with tempfile.TemporaryDirectory() as corpus:
        good = os.path.join(corpus, "files", "src", "a.stp")
        rm.write_text(good, STEP_SAMPLE.decode())
        row = describe(good)
        entry = {"id": "src/a", "path": "files/src/a.stp", "format": "STEP", "size": row["size"],
                 "sha256": row["sha256"], "source": "src", "url": "https://example.org/a.stp", "licence": "CC0-1.0",
                 "exporter": row["exporter"], "units": row["units"], "redistributable": True, "expect": "pass"}
        fetch = dict(entry, id="src/b", path="cache/src/b.stp", url="https://example.org/b.stp")
        manifest = {"sources": {"src": {"name": "test"}}, "archives": {}, "files": [entry, fetch]}

        def write(m):
            with open(os.path.join(corpus, "manifest.json"), "w", encoding="utf-8") as handle:
                json.dump(m, handle)

        def problems_of(m, cache=False):
            write(m)
            found = check_manifest(m)
            if not found:
                found += check_files(m, corpus, cache)[0]
            return found

        expect(failures, problems_of(manifest) == [], "clean corpus reported: %r" % problems_of(manifest))
        _, notes = check_files(manifest, corpus, True)
        expect(failures, len(notes) == 1, "missing cache file not noted: %r" % notes)

        planted = [
            ("size", dict(entry, size=entry["size"] + 1), "size"),
            ("hash", dict(entry, sha256="0" * 64), "SHA-256"),
            ("missing", dict(entry, path="files/src/gone.stp"), "missing"),
            ("not redistributable", dict(entry, redistributable=False), "not redistributable"),
            ("no licence", dict(entry, licence=" "), "no licence"),
            ("bad format", dict(entry, format="STL"), "unknown format"),
            ("escape", dict(entry, path="files/../x.stp"), "bad path"),
            ("unknown source", dict(entry, source="nope"), "not in 'sources'"),
            ("bad expect", dict(entry, expect="maybe"), "expect"),
        ]
        for what, bad, needle in planted:
            found = problems_of(dict(manifest, files=[bad, fetch]))
            expect(failures, any(needle in p for p in found), "planted %s not caught: %r" % (what, found))
        found = problems_of(dict(manifest, files=[entry, dict(fetch, id="src/a")]))
        expect(failures, any("duplicate id" in p for p in found), "duplicate id not caught: %r" % found)
        found = problems_of(dict(manifest, files=[entry, {"id": "x"}]))
        expect(failures, any("missing field" in p for p in found), "missing field not caught: %r" % found)

        rm.write_text(os.path.join(corpus, "files", "src", "stray.dxf"), "0\nEOF\n")
        found = problems_of(manifest)
        expect(failures, any("unlisted file files/src/stray.dxf" in p for p in found), "unlisted file: %r" % found)
        os.remove(os.path.join(corpus, "files", "src", "stray.dxf"))

        with open(good, "ab") as handle:
            handle.write(b"x")
        found = problems_of(manifest)
        expect(failures, any("size" in p for p in found), "changed file not caught: %r" % found)
    return rm.self_test_result("corpus_check", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", default=CORPUS_DIR)
    parser.add_argument("--cache", action="store_true")
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--describe", nargs="+", metavar="FILE")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.describe:
        rows = [dict({"file": path}, **describe(path)) for path in args.describe]
        print(json.dumps(rows, indent=2))
        return 0
    return run_check(args.corpus, args.cache, args.summary)


if __name__ == "__main__":
    sys.exit(main())
