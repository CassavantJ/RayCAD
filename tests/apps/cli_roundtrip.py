#!/usr/bin/env python3
"""raycad-cli end to end (task T-017): import a STEP file and save it, reopen the .raycad file with the STEP file
gone and compare the state hash and every body's content hash, export, list and invoke actions, and check the exit
codes and messages of failures.

Usage: cli_roundtrip.py --cli PATH --step PATH --work DIR [--errors]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

FAILURES = []


def expect(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAILED: " + message, file=sys.stderr)


def run(cli, *args, env=None):
    proc = subprocess.run([cli] + list(args), capture_output=True, text=True, encoding="utf-8", env=env)
    return proc.returncode, proc.stdout, proc.stderr


def parse_info(stdout):
    lines = [line for line in stdout.splitlines() if line.strip()]
    state = None
    description = None
    for index, line in enumerate(lines):
        if line.startswith("state "):
            state = line.split()[1]
            if index + 1 < len(lines):
                description = json.loads(lines[index + 1])
    return state, description


def body_hashes(description):
    return {body["id"]: body.get("contentHash") for body in description["bodies"]}


def roundtrip(cli, step, work, env):
    source = os.path.join(work, "input" + os.path.splitext(step)[1])
    shutil.copyfile(step, source)
    saved = os.path.join(work, "part.raycad")
    code, out, err = run(cli, "import", source, "-o", saved, "--overwrite", env=env)
    expect(code == 0, "import exited %d: %s" % (code, err))
    state, description = parse_info(out)
    expect(state is not None and description is not None, "import printed no state and description: %r" % out)
    if description is None:
        return
    hashes = body_hashes(description)
    expect(len(hashes) > 0 and all(hashes.values()), "import built no bodies: %r" % description["bodies"])
    expect(description["stateHash"] == state, "the description names another state hash")

    os.remove(source)
    code, out, err = run(cli, "info", saved, env=env)
    expect(code == 0, "info exited %d: %s" % (code, err))
    reopened_state, reopened = parse_info(out)
    expect(reopened_state == state, "reopened state %s differs from saved %s" % (reopened_state, state))
    if reopened is not None:
        expect(body_hashes(reopened) == hashes, "body content hashes differ after reopening")
        expect(reopened == description, "the description differs after reopening")

    code, out, err = run(cli, "hash", saved, env=env)
    expect(code == 0 and out.strip() == "state " + state, "hash printed %r (%d, %s)" % (out, code, err))

    for name in ("out.step", "out.igs", "out.stl"):
        target = os.path.join(work, name)
        code, out, err = run(cli, "export", saved, target, env=env)
        expect(code == 0, "export %s exited %d: %s" % (name, code, err))
        expect(os.path.isfile(target) and os.path.getsize(target) > 0, "export wrote no %s" % name)

    # A command through invoke, saved in place: the hash changes, and edit.undo then has something to undo.
    args = json.dumps({"id": "00000000-0000-4000-8000-0000000000aa", "name": "w", "expression": "1 in",
                       "kind": "length"})
    code, out, err = run(cli, "invoke", "param.set", args, "--open", saved, "--save", env=env)
    expect(code == 0, "invoke param.set exited %d: %s" % (code, err))
    code, out, err = run(cli, "hash", saved, env=env)
    expect(code == 0 and out.strip() != "state " + state, "the saved command did not change the state hash")


def errors(cli, work, env):
    code, out, err = run(cli, "--version", env=env)
    expect(code == 0 and out.startswith("raycad-cli "), "--version failed: %r" % out)

    code, out, err = run(cli, "actions", env=env)
    expect(code == 0, "actions exited %d" % code)
    actions = json.loads(out)
    ids = [a["id"] for a in actions]
    for wanted in ("file.save", "file.open", "file.import", "edit.undo", "feature.add", "app.about"):
        expect(wanted in ids, "actions does not list %s" % wanted)
    expect(ids == sorted(ids), "actions are not sorted by id")
    expect(all(isinstance(a["argsSchema"], dict) for a in actions), "an action schema is not an object")

    first = None
    for attempt in range(2):
        code, out, err = run(cli, "actions", "--search", "sav", "--limit", "1", env=env)
        found = json.loads(out)
        expect(code == 0 and len(found) == 1 and found[0]["id"] == "file.save", "search sav gave %r" % out)
        if first is None:
            first = out
        expect(out == first, "search results differ between runs")

    code, out, err = run(cli, "invoke", "app.version", env=env)
    expect(code == 0 and "version" in json.loads(out), "invoke app.version gave %r" % out)

    code, out, err = run(cli, "invoke", "file.saveAs", '{"path":5}', env=env)
    expect(code == 3 and "/path" in err, "bad args gave %d %r" % (code, err))

    code, out, err = run(cli, "invoke", "no.such.action", env=env)
    expect(code == 4, "an unknown action gave %d" % code)

    code, out, err = run(cli, "invoke", "edit.undo", env=env)
    expect(code == 4 and "No document" in err, "undo without a document gave %d %r" % (code, err))

    missing = os.path.join(work, "missing.raycad")
    code, out, err = run(cli, "info", missing, env=env)
    expect(code == 4 and "does not exist" in err, "a missing file gave %d %r" % (code, err))

    corrupt = os.path.join(work, "corrupt.raycad")
    with open(corrupt, "wb") as f:
        f.write(b"x" * 4096)
    code, out, err = run(cli, "info", corrupt, env=env)
    expect(code in (1, 7) and "error" in err, "a corrupt file gave %d %r" % (code, err))

    code, out, err = run(cli, "frobnicate", env=env)
    expect(code == 2, "an unknown command gave %d" % code)
    code, out, err = run(cli, env=env)
    expect(code == 2 and "usage" in err, "no command gave %d" % code)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--step")
    parser.add_argument("--work", required=True)
    parser.add_argument("--errors", action="store_true")
    args = parser.parse_args()

    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    env = dict(os.environ)
    env["RAYCAD_USER_DATA_DIR"] = os.path.join(args.work, "user")
    if args.step:
        roundtrip(args.cli, args.step, args.work, env)
    if args.errors:
        errors(args.cli, args.work, env)
    if FAILURES:
        print("cli_roundtrip: %d failure(s)" % len(FAILURES), file=sys.stderr)
        return 1
    print("cli_roundtrip: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
