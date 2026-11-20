#!/usr/bin/env python3
"""raycad-cli end to end (task T-017): import a STEP file and save it, reopen the .raycad file with the STEP file
gone and compare the state hash and every body's content hash, export, list and invoke actions, and check the exit
codes and messages of failures. With --errors, also create an empty document with new.

With --recovery (and --helper, the api tests' helper process that is killed like a crash): recover --list, recover
and recover --discard.

Usage: cli_roundtrip.py --cli PATH --step PATH --work DIR [--errors] [--recovery --helper PATH]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

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


def new_document(cli, work, env):
    """new -o: an empty document, saved, that reopens with the same state, plus its failures."""
    saved = os.path.join(work, "empty.raycad")
    code, out, err = run(cli, "new", "-o", saved, env=env)
    expect(code == 0, "new exited %d: %s" % (code, err))
    state, description = parse_info(out)
    expect(state is not None and description is not None, "new printed no state and description: %r" % out)
    expect(os.path.isfile(saved), "new wrote no file")
    if description is None:
        return
    expect(description["stateHash"] == state, "the description of the new document names another state hash")
    expect(description["bodies"] == [], "a new document has bodies: %r" % description["bodies"])
    code, out, err = run(cli, "info", saved, env=env)
    expect(code == 0, "info on the new document exited %d: %s" % (code, err))
    reopened_state, reopened = parse_info(out)
    expect(reopened_state == state, "reopened state %s differs from the new document's %s" % (reopened_state, state))
    expect(reopened == description, "the description of the new document differs after reopening")

    # Two new documents are the same document: a new document's state does not depend on time or ids.
    again = os.path.join(work, "empty2.raycad")
    code, out, err = run(cli, "new", "-o", again, "--template", "default", env=env)
    expect(code == 0 and parse_info(out)[0] == state, "a second new document hashes differently: %r" % out)

    code, out, err = run(cli, "new", "-o", saved, env=env)
    expect(code != 0 and "error" in err, "new over an existing file without --overwrite gave %d %r" % (code, err))
    code, out, err = run(cli, "new", "-o", saved, "--overwrite", env=env)
    expect(code == 0, "new --overwrite exited %d: %s" % (code, err))
    code, out, err = run(cli, "new", "-o", os.path.join(work, "t.raycad"), "--template", "nope", env=env)
    expect(code == 4 and "template" in err, "an unknown template gave %d %r" % (code, err))
    code, out, err = run(cli, "new", env=env)
    expect(code == 2, "new without -o gave %d" % code)


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


def run_and_kill(helper, args, out):
    """Runs the api test helper until it wrote what it reached, then kills it like a crash."""
    proc = subprocess.Popen([helper] + args)
    deadline = time.monotonic() + 180
    try:
        while not os.path.exists(out):
            if proc.poll() is not None:
                expect(False, "the helper exited with %r before it was killed" % proc.returncode)
                return None
            if time.monotonic() > deadline:
                expect(False, "the helper did not finish in time")
                return None
            time.sleep(0.05)
    finally:
        proc.kill()
        proc.wait()
    with open(out, encoding="utf-8") as f:
        return f.readline().strip()


def recovery(cli, helper, step, work, env):
    user = env["RAYCAD_USER_DATA_DIR"]
    saved = os.path.join(work, "doc.raycad")
    code, out, err = run(cli, "import", step, "-o", saved, env=env)
    expect(code == 0, "import exited %d: %s" % (code, err))

    reached = run_and_kill(helper, ["edit", user, saved, step, os.path.join(work, "reached.txt")],
                           os.path.join(work, "reached.txt"))
    code, out, err = run(cli, "recover", "--list", env=env)
    pending = json.loads(out) if code == 0 else []
    expect(len(pending) == 1 and not pending[0]["unsaved"] and pending[0]["steps"] > 0,
           "recover --list gave %r (%d, %s)" % (out, code, err))
    code, out, err = run(cli, "info", saved, env=env)
    expect(code == 6 and "recover" in err, "info on a file with pending changes gave %d %r" % (code, err))
    code, out, err = run(cli, "recover", saved, env=env)
    expect(code == 0, "recover exited %d: %s" % (code, err))
    state, _ = parse_info(out)
    expect(state == reached, "recovered state %s, the killed session reached %s" % (state, reached))
    code, out, err = run(cli, "recover", "--list", env=env)
    expect(code == 0 and json.loads(out) == [], "something is still pending after recovery: %r" % out)
    code, out, err = run(cli, "hash", saved, env=env)
    expect(code == 0 and out.strip() == "state %s" % reached, "the recovered file hashes to %r" % out)

    # A never-saved document: -o is required, and --discard deletes it.
    reached = run_and_kill(helper, ["new", user, step, os.path.join(work, "reached2.txt")],
                           os.path.join(work, "reached2.txt"))
    code, out, err = run(cli, "recover", "--list", env=env)
    pending = json.loads(out) if code == 0 else []
    expect(len(pending) == 1 and pending[0]["unsaved"], "recover --list gave %r" % out)
    if pending:
        code, out, err = run(cli, "recover", pending[0]["id"], env=env)
        expect(code == 2, "recovering an untitled document without -o gave %d" % code)
        code, out, err = run(cli, "recover", "--discard", pending[0]["id"], env=env)
        expect(code == 0, "recover --discard exited %d: %s" % (code, err))
    code, out, err = run(cli, "recover", "--list", env=env)
    expect(code == 0 and json.loads(out) == [], "something is still pending after discard: %r" % out)
    code, out, err = run(cli, "recover", "--discard", saved, env=env)
    expect(code == 4, "discarding nothing gave %d" % code)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--step")
    parser.add_argument("--work", required=True)
    parser.add_argument("--errors", action="store_true")
    parser.add_argument("--recovery", action="store_true")
    parser.add_argument("--helper", help="ray_api_helper, for --recovery")
    args = parser.parse_args()

    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    env = dict(os.environ)
    env["RAYCAD_USER_DATA_DIR"] = os.path.join(args.work, "user")
    if args.recovery:
        recovery(args.cli, args.helper, args.step, args.work, env)
    elif args.step:
        roundtrip(args.cli, args.step, args.work, env)
    if args.errors:
        errors(args.cli, args.work, env)
        new_document(args.cli, args.work, env)
    if FAILURES:
        print("cli_roundtrip: %d failure(s)" % len(FAILURES), file=sys.stderr)
        return 1
    print("cli_roundtrip: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
