#!/usr/bin/env python3
"""Replays single naming sequences on the platform that runs it and keeps everything a review needs (the probe
workflow, .github/workflows/probe.yml; it can run anywhere the tests are built).

A case is one line (cases are separated by newlines or ";"):

    <test executable> <Catch2 filter> <part:seq> [VAR=value ...]

such as "ray_naming_oracle_tests [random] 2:2 RAYCAD_ORACLE_MUTATE=copy-position,structural-pieces" or
"ray_naming_oracle_tests [random-directedit-review1] 17:24". The filter is one word (a tag or tags); "#" starts a
comment. Each case runs its executable with RAYCAD_NAMING_ONLY=<part:seq>, RAYCAD_NAMING_VERBOSE=1 and the case's
variables, into probe/<n>-<label>/:
- stdout.txt: the whole output (the scripts, the verdicts and, for one sequence, every element before and after);
- sequence/: the shapes before and after (before/after.rbrep and .step), written by a T-081a block's replay
  (random_test.cpp, RAYCAD_EVIDENCE_DIR);
- evidence/<block>/<part>-<seq>_<kind><n>/: for each answer of a direct-edit block in the output that is wrong, not
  judged or a conflict, the oracle's [.evidence] folder (shapes, elements, scripts, claims; tools/ci_naming_evidence.py);
- case.txt: the command, the variables and the exit status.

    ci_naming_probe.py --cases "<text>" --bindir build/<preset>/bin/Release --out probe
    ci_naming_probe.py --cases "<text>" --targets       # the executables to build, one per line

Exit status 0 whatever the tests answer (a probe gathers, it judges nothing); 1 only for a case that cannot be read.
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ci_naming_evidence  # noqa: E402

MAX_EVIDENCE = 30  # [.evidence] folders per case


def parse(text):
    """The cases of `text`: (exe, filter, part_seq, {var: value}). Raises ValueError for a malformed line."""
    cases = []
    for raw in re.split(r"[\n;]", text):
        # Only a "#" that starts a word starts a comment.
        line = re.sub(r"(^|\s)#.*$", "", raw).strip()
        if not line:
            continue
        words = line.split()
        if len(words) < 3 or not re.fullmatch(r"\d+:\d+", words[2]):
            raise ValueError(f"case '{line}': expected <executable> <filter> <part:seq> [VAR=value ...]")
        env = {}
        for w in words[3:]:
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*=\S*", w):
                raise ValueError(f"case '{line}': '{w}' is not VAR=value")
            k, v = w.split("=", 1)
            env[k] = v
        cases.append((words[0], words[1], words[2], env))
    return cases


def label(exe, filt, part_seq, env):
    tag = re.sub(r"[^A-Za-z0-9]+", "-", filt).strip("-") or "all"
    extra = "".join("-" + re.sub(r"[^A-Za-z0-9]+", "", k.replace("RAYCAD_", "").lower()) for k in env)
    return f"{tag}-{part_seq.replace(':', '-')}{extra}"[:80]


def run(cases, bindir, out):
    os.makedirs(out, exist_ok=True)
    summary = []
    for n, (exe, filt, part_seq, env) in enumerate(cases, 1):
        dest = os.path.join(out, f"{n:02d}-{label(exe, filt, part_seq, env)}")
        os.makedirs(dest, exist_ok=True)
        path = os.path.join(bindir, exe + (".exe" if os.name == "nt" else ""))
        case_env = dict(env, RAYCAD_NAMING_ONLY=part_seq, RAYCAD_NAMING_VERBOSE="1",
                        RAYCAD_EVIDENCE_DIR=os.path.abspath(os.path.join(dest, "sequence")))
        try:
            r = subprocess.run([path, filt], env=dict(os.environ, **case_env), capture_output=True, text=True,
                               timeout=3600)
            text, status = r.stdout + r.stderr, f"exit {r.returncode}"
        except (subprocess.TimeoutExpired, OSError) as e:
            text, status = str(e), "did not run"
        with open(os.path.join(dest, "stdout.txt"), "w", encoding="utf-8") as f:
            f.write(text)
        found = ci_naming_evidence.findings(text.splitlines(), unjudged=True)
        lines = ci_naming_evidence.gather(found, path, os.path.join(dest, "evidence"), MAX_EVIDENCE, env)
        verdicts = [l.strip() for l in text.splitlines() if l.strip().startswith("verdicts:")]
        with open(os.path.join(dest, "case.txt"), "w", encoding="utf-8") as f:
            f.write(f"{path} {filt}\n")
            f.write("".join(f"{k}={v}\n" for k, v in case_env.items()))
            f.write(f"{status}\n" + "\n".join(verdicts) + "\n" + "\n".join(lines) + "\n")
        summary.append(f"{os.path.basename(dest)}: {status}; " + (verdicts[0] if verdicts else "no verdict line") +
                       f"; {len(found)} direct-edit answer(s) gathered")
        summary.extend(lines)
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary))


def self_test():
    text = ("ray_naming_oracle_tests [random] 2:2\n"
            "  # a comment line\n"
            "ray_naming_oracle_tests [random] 2:2 RAYCAD_ORACLE_MUTATE=copy-position,structural-pieces ; "
            "ray_naming_oracle_tests [random-directedit-review1] 17:24 RAYCAD_NAMING_NO_PLACEMENT=1 # trailing\n\n")
    got = parse(text)
    assert got == [("ray_naming_oracle_tests", "[random]", "2:2", {}),
                   ("ray_naming_oracle_tests", "[random]", "2:2",
                    {"RAYCAD_ORACLE_MUTATE": "copy-position,structural-pieces"}),
                   ("ray_naming_oracle_tests", "[random-directedit-review1]", "17:24",
                    {"RAYCAD_NAMING_NO_PLACEMENT": "1"})], got
    assert label(*got[1]) == "random-2-2-oraclemutate"
    for bad in ("ray_naming_oracle_tests [random]", "x [y] 2-2", "x [y] 2:2 lower=1"):
        try:
            parse(bad)
        except ValueError:
            continue
        raise AssertionError(bad)
    print("ci_naming_probe self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cases", default="")
    ap.add_argument("--bindir")
    ap.add_argument("--out", default="probe")
    ap.add_argument("--targets", action="store_true", help="print the executables to build and stop")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    try:
        cases = parse(a.cases)
    except ValueError as e:
        print(f"ci_naming_probe: {e}", file=sys.stderr)
        return 1
    if not cases:
        print("ci_naming_probe: no cases", file=sys.stderr)
        return 1
    if a.targets:
        print("\n".join(sorted({c[0] for c in cases})))
        return 0
    run(cases, a.bindir, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
