#!/usr/bin/env python3
"""Evidence for the wrong answers of the direct-edit naming blocks, gathered in the CI job that found them.

A sequence authored on one platform cannot be replayed on another (its kernel builds other geometry), so the
evidence comes from the same build on the same runner: for each WRONG line of a direct-edit block in a ctest log, the
oracle's hidden [.evidence] test replays that sequence and writes the shapes before and after (.rbrep, .step), the
elements, both scripts and the claims to a folder of its own (tests/naming/oracle/random_directedit_test.cpp).

    ci_naming_evidence.py --log ctest.log --bin build/.../ray_naming_oracle_tests --out evidence

Writes evidence/<block>/<part>-<seq>_<kind><n>/ per finding and prints one line per finding. The T-081a blocks
(random_test.cpp) have no [.evidence] test and are left out. Exit status 0 whatever the findings: the tests have
already failed the job when there are any.
"""

import argparse
import os
import re
import subprocess
import sys

# "  [4:31 suppress R5; ...] directedit raycad-synthetic/fillet_block_1.stp #31|face 23: WRONG different element; ..."
WRONG = re.compile(r"^\s+\[(\d+):(\d+) [^\]]*\] (\S+) \S+ (#\d+\|(?:pick \S+ )?(?:face|edge|vertex) \d+): WRONG")

# The environment of [.evidence] for the sequences of each direct-edit block, by the block's key.
BLOCKS = {
    "directedit": {},
    "directedit-sorted": {"RAYCAD_LOOK_SORTED": "1"},
    "directedit-holdout1": {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "A81B7000"},
    "directedit-holdout2": {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "B2081B00"},
}

MAX_FINDINGS = 12  # each replays one sequence: seconds to a minute


def findings(lines):
    """The distinct direct-edit WRONG answers of a log: (block, part, seq, id), in order of appearance."""
    seen = []
    for line in lines:
        m = WRONG.match(line)
        if m and m.group(3) in BLOCKS:
            f = (m.group(3), int(m.group(1)), int(m.group(2)), m.group(4))
            if f not in seen:
                seen.append(f)
    return seen


def folder(out, block, part, seq, ref):
    kind, n = re.search(r"(face|edge|vertex) (\d+)$", ref).groups()
    return os.path.join(out, block, f"{part}-{seq}_{kind}{n}")


def self_test():
    log = [
        "  [4:31 suppress R5; insert cut X1 at 3; resize R1] directedit raycad-synthetic/fillet_block_1.stp #31|face 23:"
        " WRONG different element; resolver Split {21 28} (), oracle expects {28}, congruent -1",
        "  [7:29 move R1 by its width] directedit-holdout2 raycad-synthetic/keyway_tslot_1.stp #29|pick a1 edge 16: WRONG x",
        "  [8:18 change size of R4] raycad-synthetic/l_bracket_1.stp #18|edge 104: WRONG different element",
        "  [3:1 move R2] holdout6 raycad-synthetic/closed_pocket_3.stp #1|face 3: WRONG unflagged merge",
        "  [4:31 suppress R5; insert cut X1 at 3; resize R1] directedit raycad-synthetic/fillet_block_1.stp #31|face 23:"
        " WRONG different element; again",
    ]
    got = findings(log)
    assert got == [("directedit", 4, 31, "#31|face 23"), ("directedit-holdout2", 7, 29, "#29|pick a1 edge 16")], got
    assert folder("e", *got[1]) == os.path.join("e", "directedit-holdout2", "7-29_edge16")
    print("ci_naming_evidence self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log")
    ap.add_argument("--bin")
    ap.add_argument("--out", default="evidence")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    with open(a.log, encoding="utf-8-sig", errors="replace") as f:
        found = findings(f.read().splitlines())
    print(f"ci_naming_evidence: {len(found)} direct-edit wrong answer(s) in {a.log}")
    for block, part, seq, ref in found[:MAX_FINDINGS]:
        dest = folder(a.out, block, part, seq, ref)
        os.makedirs(dest, exist_ok=True)
        env = dict(os.environ, RAYCAD_LOOK=f"{part}:{seq}", RAYCAD_LOOK_ID=ref, RAYCAD_EVIDENCE_DIR=dest, **BLOCKS[block])
        try:
            r = subprocess.run([a.bin, "[.evidence]"], env=env, capture_output=True, text=True, timeout=900)
            text, status = r.stdout + r.stderr, f"exit {r.returncode}"
        except subprocess.TimeoutExpired as e:
            text, status = str(e), "timed out"
        with open(os.path.join(dest, "run.log"), "w", encoding="utf-8") as log:
            log.write(text)
        print(f"  {block} {part}:{seq} {ref}: {status}, {len(os.listdir(dest))} file(s) in {dest}")
    if len(found) > MAX_FINDINGS:
        print(f"  ({len(found) - MAX_FINDINGS} more not gathered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
