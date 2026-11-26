#!/usr/bin/env python3
"""Evidence for the wrong answers of the direct-edit naming blocks, gathered in the CI job that found them.

A sequence authored on one platform cannot be replayed on another (its kernel builds other geometry), so the
evidence comes from the same build on the same runner: for each WRONG line of a direct-edit block in a ctest log, the
oracle's hidden [.evidence] test replays that sequence and writes the shapes before and after (.rbrep, .step), the
elements, both scripts and the claims to a folder of its own (tests/naming/oracle/random_directedit_test.cpp).

    ci_naming_evidence.py --log ctest.log --bin build/.../ray_naming_oracle_tests --out evidence [--unjudged]

Writes evidence/<block>/<part>-<seq>_<kind><n>/ per finding and prints one line per finding. --unjudged also takes the
answers not judged (UNJUDGED) and the conflicts (CONFLICT) of those blocks, for a review (tools/ci_naming_probe.py).
The T-081a blocks (random_test.cpp) have no [.evidence] test and are left out here; with RAYCAD_EVIDENCE_DIR and
RAYCAD_NAMING_ONLY their replay writes its shapes itself (the probe does that). Exit status 0 whatever the findings:
the tests have already failed the job when there are any.
"""

import argparse
import os
import re
import subprocess
import sys

_ID = r"(#\d+\|(?:pick \S+ )?(?:face|edge|vertex) \d+)"
# "  [4:31 suppress R5; ...] directedit raycad-synthetic/fillet_block_1.stp #31|face 23: WRONG different element; ..."
WRONG = re.compile(r"^\s+\[(\d+):(\d+) [^\]]*\] (\S+) \S+ " + _ID + r": WRONG")
# "  [7:11 ...] UNJUDGED directedit raycad-synthetic/keyway_tslot_1.stp #11|vertex 23: resolver ...", and CONFLICT
OTHER = re.compile(r"^\s+\[(\d+):(\d+) [^\]]*\] (?:UNJUDGED|CONFLICT) (\S+) \S+ " + _ID + r": ")

# The environment of [.evidence] for the sequences of each direct-edit block, by the block's key
# (random_directedit_test.cpp: the blocks' seeds and generators).
BLOCKS = {
    "directedit": {},
    "directedit-sorted": {"RAYCAD_LOOK_SORTED": "1"},
    "directedit-holdout1": {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "A81B7000"},
    "directedit-holdout2": {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "B2081B00"},
    "directedit-review1": {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "6B1D3F5A2C800000"},
}

MAX_FINDINGS = 12  # each replays one sequence: seconds to a minute


def block_env(key):
    """The [.evidence] environment of a direct-edit block's sequences, or None for another block. A block of any seed
    ([.random-directedit-seed], keys "directedit-seed<hex>") is sorted, with its seed."""
    if key in BLOCKS:
        return BLOCKS[key]
    m = re.fullmatch(r"directedit-seed([0-9A-Fa-f]+)", key)
    if m:
        return {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": m.group(1)}
    return None


def findings(lines, unjudged=False):
    """The distinct direct-edit WRONG answers of a log (with unjudged, also the answers not judged and the conflicts):
    (block, part, seq, id), in order of appearance."""
    seen = []
    for line in lines:
        m = WRONG.match(line) or (OTHER.match(line) if unjudged else None)
        if m and block_env(m.group(3)) is not None:
            f = (m.group(3), int(m.group(1)), int(m.group(2)), m.group(4))
            if f not in seen:
                seen.append(f)
    return seen


def folder(out, block, part, seq, ref):
    kind, n = re.search(r"(face|edge|vertex) (\d+)$", ref).groups()
    return os.path.join(out, block, f"{part}-{seq}_{kind}{n}")


def gather(found, exe, out, limit=MAX_FINDINGS, extra_env=None):
    """Runs [.evidence] for each finding into its folder; returns one line per finding."""
    lines = []
    for block, part, seq, ref in found[:limit]:
        dest = folder(out, block, part, seq, ref)
        os.makedirs(dest, exist_ok=True)
        env = dict(os.environ, **(extra_env or {}), RAYCAD_LOOK=f"{part}:{seq}", RAYCAD_LOOK_ID=ref,
                   RAYCAD_EVIDENCE_DIR=dest, **block_env(block))
        try:
            r = subprocess.run([exe, "[.evidence]"], env=env, capture_output=True, text=True, timeout=900)
            text, status = r.stdout + r.stderr, f"exit {r.returncode}"
        except (subprocess.TimeoutExpired, OSError) as e:
            text, status = str(e), "did not run"
        with open(os.path.join(dest, "run.log"), "w", encoding="utf-8") as log:
            log.write(text)
        lines.append(f"  {block} {part}:{seq} {ref}: {status}, {len(os.listdir(dest))} file(s) in {dest}")
    if len(found) > limit:
        lines.append(f"  ({len(found) - limit} more not gathered)")
    return lines


def self_test():
    log = [
        "  [4:31 suppress R5; insert cut X1 at 3; resize R1] directedit raycad-synthetic/fillet_block_1.stp #31|face 23:"
        " WRONG different element; resolver Split {21 28} (), oracle expects {28}, congruent -1",
        "  [7:29 move R1 by its width] directedit-holdout2 raycad-synthetic/keyway_tslot_1.stp #29|pick a1 edge 16: WRONG x",
        "  [8:18 change size of R4] raycad-synthetic/l_bracket_1.stp #18|edge 104: WRONG different element",
        "  [3:1 move R2] holdout6 raycad-synthetic/closed_pocket_3.stp #1|face 3: WRONG unflagged merge",
        "  [4:31 suppress R5; insert cut X1 at 3; resize R1] directedit raycad-synthetic/fillet_block_1.stp #31|face 23:"
        " WRONG different element; again",
        "  [16:11 move R2 by its width] UNJUDGED directedit-review1 raycad-synthetic/thin_wall_box_1.stp #11|face 22:"
        " resolver Merged {5}, no part of the logical element overlaps [NOT REVIEWED]",
        "  [7:20 apply delete_hole D1] CONFLICT directedit-sorted raycad-synthetic/keyway_tslot_1.stp #20|face 0:"
        " congruent in place 0, oracle expects {0 24}, resolver Split {0 24} [NOT REVIEWED]",
        "  [2:3 move R1] directedit-seed1A2B raycad-synthetic/x.stp #3|vertex 4: WRONG different element",
        "  [10:2 change depth of R4] UNJUDGED raycad-synthetic/open_pocket_step_1.stp #2|face 16: resolver Exact {17}",
    ]
    got = findings(log)
    assert got == [("directedit", 4, 31, "#31|face 23"), ("directedit-holdout2", 7, 29, "#29|pick a1 edge 16"),
                   ("directedit-seed1A2B", 2, 3, "#3|vertex 4")], got
    assert folder("e", *got[1]) == os.path.join("e", "directedit-holdout2", "7-29_edge16")
    every = findings(log, unjudged=True)
    assert ("directedit-review1", 16, 11, "#11|face 22") in every and ("directedit-sorted", 7, 20, "#20|face 0") in every
    assert len(every) == 5, every
    assert block_env("directedit-review1")["RAYCAD_LOOK_SEED"] == "6B1D3F5A2C800000"
    assert block_env("directedit-seed1A2B") == {"RAYCAD_LOOK_SORTED": "1", "RAYCAD_LOOK_SEED": "1A2B"}
    assert block_env("holdout6") is None
    print("ci_naming_evidence self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log")
    ap.add_argument("--bin")
    ap.add_argument("--out", default="evidence")
    ap.add_argument("--unjudged", action="store_true", help="also the answers not judged and the conflicts")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    with open(a.log, encoding="utf-8-sig", errors="replace") as f:
        found = findings(f.read().splitlines(), a.unjudged)
    print(f"ci_naming_evidence: {len(found)} direct-edit answer(s) to gather in {a.log}")
    print("\n".join(gather(found, a.bin, a.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
