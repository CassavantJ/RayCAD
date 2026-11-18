#!/usr/bin/env python3
"""Validates the golden document snapshot (tests/unit/doc/golden) against schemas/doc*.schema.json.

The C++ test ray_doc_tests pins those files byte for byte to what serializeState writes, so this checks that the
JSON RayCAD produces matches its published schema. Standard library only: it implements the subset of JSON Schema
(draft 2020-12) the doc schemas use: type, const, enum, pattern, minLength, minItems, maxItems, required,
properties, items, anyOf and local $ref. Unknown keywords fail, so the schemas cannot silently outgrow it.

Usage: check_doc_schema.py [--root DIR] [--self-test]
"""
import argparse
import copy
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ANNOTATIONS = {"$schema", "title", "description", "$defs"}
TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "integer": lambda v: (isinstance(v, int) and not isinstance(v, bool)) or (isinstance(v, float) and v.is_integer()),
}


def validate(value, schema, root, path, problems):
    for key in schema:
        if key not in ANNOTATIONS and key not in (
            "type", "const", "enum", "pattern", "minLength", "minItems", "maxItems", "required", "properties",
            "items", "anyOf", "$ref",
        ):
            problems.append("%s: schema keyword '%s' is not supported by this checker" % (path, key))
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/$defs/"):
            problems.append("%s: only local $defs references are supported" % path)
        else:
            validate(value, root["$defs"][ref[len("#/$defs/"):]], root, path, problems)
    if "type" in schema and not TYPES[schema["type"]](value):
        problems.append("%s: expected %s" % (path, schema["type"]))
        return
    if "const" in schema and value != schema["const"]:
        problems.append("%s: expected %r" % (path, schema["const"]))
    if "enum" in schema and value not in schema["enum"]:
        problems.append("%s: %r is not one of %r" % (path, value, schema["enum"]))
    if isinstance(value, str):
        if "pattern" in schema and not re.search(schema["pattern"], value):
            problems.append("%s: %r does not match %s" % (path, value, schema["pattern"]))
        if len(value) < schema.get("minLength", 0):
            problems.append("%s: shorter than %d" % (path, schema["minLength"]))
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            problems.append("%s: fewer than %d items" % (path, schema["minItems"]))
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            problems.append("%s: more than %d items" % (path, schema["maxItems"]))
        if "items" in schema:
            for index, item in enumerate(value):
                validate(item, schema["items"], root, "%s[%d]" % (path, index), problems)
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                problems.append("%s: missing required key '%s'" % (path, key))
        for key, sub in schema.get("properties", {}).items():
            if key in value:
                validate(value[key], sub, root, "%s.%s" % (path, key), problems)
    if "anyOf" in schema:
        if not any(_ok(value, sub, root) for sub in schema["anyOf"]):
            problems.append("%s: matches none of the anyOf alternatives" % path)


def _ok(value, schema, root):
    problems = []
    validate(value, schema, root, "", problems)
    return not problems


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def documents(root):
    """(file, schema file) pairs: the golden manifest and every golden feature object."""
    golden = os.path.join(root, "tests", "unit", "doc", "golden")
    pairs = [(os.path.join(golden, "manifest.json"), "doc.manifest.schema.json")]
    for name in sorted(os.listdir(golden)):
        if name.startswith("feature-") and name.endswith(".json"):
            pairs.append((os.path.join(golden, name), "doc.feature.schema.json"))
    return pairs


def check(root):
    problems = []
    schemas = {}
    for name in ("doc.manifest.schema.json", "doc.feature.schema.json"):
        schemas[name] = load(os.path.join(root, "schemas", name))
    pairs = documents(root)
    if len(pairs) < 2:
        problems.append("no golden feature objects found")
    for path, schema_name in pairs:
        found = []
        validate(load(path), schemas[schema_name], schemas[schema_name], os.path.basename(path), found)
        problems += found
    return problems, len(pairs)


def self_test(root):
    failures = []
    manifest_schema = load(os.path.join(root, "schemas", "doc.manifest.schema.json"))
    feature_schema = load(os.path.join(root, "schemas", "doc.feature.schema.json"))
    good_manifest = load(documents(root)[0][0])
    good_feature = load(documents(root)[1][0])
    if not _ok(good_manifest, manifest_schema, manifest_schema) or not _ok(good_feature, feature_schema, feature_schema):
        failures.append("the golden files do not validate")

    def planted(document, edit):
        bad = copy.deepcopy(document)
        edit(bad)
        return bad

    bad_manifests = [
        planted(good_manifest, lambda d: d.pop("settings")),
        planted(good_manifest, lambda d: d.update(schema=2)),
        planted(good_manifest, lambda d: d["settings"].update(units="furlong")),
        planted(good_manifest, lambda d: d["settings"].update(material="not-a-uuid")),
        planted(good_manifest, lambda d: d["parameters"][0].update(kind="volume")),
        planted(good_manifest, lambda d: d["parameters"][0].update(name="2x")),
        planted(good_manifest, lambda d: d["features"].append("ABC")),
        planted(good_manifest, lambda d: d["bodies"][0].update(color=[1, 1, 1])),
        planted(good_manifest, lambda d: d["bodies"][0].update(visible="yes")),
        planted(good_manifest, lambda d: d["settings"].update(decimals=2.5)),
    ]
    for index, bad in enumerate(bad_manifests):
        if _ok(bad, manifest_schema, manifest_schema):
            failures.append("planted manifest error %d was not caught" % index)
    bad_features = [
        planted(good_feature, lambda d: d.pop("props")),
        planted(good_feature, lambda d: d.update(type="")),
        planted(good_feature, lambda d: d.update(suppressed=0)),
        planted(good_feature, lambda d: d.update(props=[])),
    ]
    for index, bad in enumerate(bad_features):
        if _ok(bad, feature_schema, feature_schema):
            failures.append("planted feature error %d was not caught" % index)
    # Unknown keys are allowed (PLAN.md 4.4).
    if not _ok(planted(good_manifest, lambda d: d.update(futureKey={"x": 1})), manifest_schema, manifest_schema):
        failures.append("an unknown key was rejected")
    for failure in failures:
        print("check_doc_schema self-test: " + failure, file=sys.stderr)
    print("check_doc_schema self-test: %s" % ("FAILED" if failures else "ok"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=REPO_ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.root)
    problems, count = check(args.root)
    for problem in problems:
        print("check_doc_schema: " + problem, file=sys.stderr)
    print("check_doc_schema: %d files, %s" % (count, "%d problems" % len(problems) if problems else "ok"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
