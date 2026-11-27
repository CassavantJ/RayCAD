# Porting debt: macOS and Linux

Windows is the only platform that gates until the app is finished (PLAN.md section 12). The macOS and Linux CI jobs
still build and test on every push and in every nightly run, named "(not gating)": what fails there is recorded here
and fixed after the last milestone. Nothing for those platforms is removed meanwhile (code, presets, pinned replays,
reviewed-list entries, workflows).

Each entry: what fails, where it was seen, and what is known of the cause. Keys are those the naming tests print
(`[part:seq edits] <block> <part file> #<seq>|<element>`); "O" means the reviewer ruled the oracle wrong, "R" the
resolver. A macOS sequence cannot be replayed on Windows or Linux (its OpenCASCADE builds other geometry from the same
inputs, so the generators author other sequences there): to look at one, run `.github/workflows/probe.yml` on macOS
(`tools/ci_naming_probe.py`), which uploads the output, the shapes before and after and an `[.evidence]` folder per
answer.

## macOS: naming answers

| key | answer | oracle | known |
|-|-|-|-|
| main block (smoke) 2:2 `circular_pocket_1.stp #2\|face 14` (move R2 by its width) | Exact {20} | WRONG ghost; ORACLE SELF-CHECK FAILED on edge 58 | the oracle judges its own expected answer wrong: a judge error in this sequence (reviewer, round 4: leaning O); fails `ray_naming_random_smoke` on every macOS push |
| holdout3 4:7 `fillet_block_1.stp #7\|edge 26` (move R3 by its width) | Exact {22} | WRONG ghost | silent (no flag); not ruled (one log line, no shapes before the probe) |
| review1 17:24 `nist_ftc_11_asme1_rb_geometry.stp #24\|face 0` (swap R4 and R5; insert cut X2) | Merged {0} | WRONG ghost | faces 17, 1 and 2 of the same sequence are not judged (Merged {0}, {2}, {2}); not ruled |
| directedit 4:31 `fillet_block_1.stp #31\|face 23` (suppress R5; insert cut X1; resize R1) | Split {21 28} | WRONG, E {28} | R, flagged (reviewer, round 4, medium confidence): face 21 holds none of the pick mapped by R1's profile scaling; since 344f52f |
| directedit-sorted 4:29 `fillet_block_1.stp #29\|edge 50` (apply offset_face D1) | Exact {56} | WRONG incomplete, E {56 62} | O (reviewer, round 4, high confidence): 56 is the exact image of the pushed wall's top edge, 62 new ground past its mapped end; also fails `ray_naming_directedit_replays` there |
| directedit-holdout2 12:29 `plate_countersink_1.stp #29\|face 6` (move R3; insert cut X1; swap R1 and R2) | Exact {21} | not judged, not reviewed | Windows answers another element on other topology; not ruled |
| main block 10:2 `open_pocket_step_1.stp #2\|edge 53` (change depth of R4; change size of R3 and R5) | Exact {55} | not judged | reviewed by geometry (the arc where R5's corner patch meets R1's blend, at z 1.445), but its reviewed-list entry was dropped: it needs an end-point signature from a macOS run (face 16 of the same sequence is listed) |

The smoke failure (2:2) fails every macOS push (Release and Debug); the others fail the nightly naming and
direct-edit jobs.

## macOS: delete_hole leaves a seam sliver

directedit-sorted 7:20 `keyway_tslot_1.stp #20` (apply delete_hole D1), seen on macOS: the kernel's result has face
24, a 0.0062 mm2 sliver of the shaft at its seam (x 23.83 to 24.00), beside a new B-spline face 25. The body is valid
and the naming answer for face 0 is right (Split {0 24}, reviewed in tests/naming/oracle/reviewed_conflicts.txt,
review round 6 item 09), but the sliver is a healing artefact of the face deletion: for the kernel owner.

## Linux: intermittent segfault under load (cause not proven)

`[random-holdout2]` with `RAYCAD_NAMING_NO_PLACEMENT=1`, four blocks at once, GCC Release: about 1 crash in 18 block
runs on the development PC (WSL; review round 5: 1 in 19 under gdb), each time in another frame under
`kernel::minDistance`, called from naming's `territoryOf` from `recordReferences`. Since then a thread-pool race on
that path, found by ThreadSanitizer, was fixed (9694e9a); whether it was the cause is not proven. On GitHub's Linux
runners, with that fix, stress run 45934693932 (`.github/workflows/stress.yml`) ran 96 such block runs under gdb
(4 at once) and 72 under AddressSanitizer and UndefinedBehaviorSanitizer (4 at once): no signal, no sanitizer report.
Not closed until it is reproduced elsewhere, or ruled out on the development PC (whose CPU is known to misexecute
under load) by a clean stress run there.

## Linux: direct-edit answers without placements

With `RAYCAD_NAMING_NO_PLACEMENT=1` (references without placements: the product's path for holes, blends and direct
edits, where the document passes none; review round 5, A2) direct-edit blocks fail on Linux where Windows passes.
The failing sequences and answers are not written down yet: add them here (key, answer, oracle) when next seen.
Not to be confused with block 3's 10:21 edge 31 and 18:23 face 6 (review round 5), which are wrong on Windows and
Linux alike, with and without placements.

## Off Windows: keyway_tslot #0 vertex 0 is Broken

The pinned direct-edit replay `directedit keyway_tslot_1.stp #0|vertex 0` (block 7, "joined blend pieces",
tests/naming/oracle/random_directedit_test.cpp) is pinned Exact, judged correct, as Windows answers it. Off Windows it
answers Broken, so the pinned check fails in `ray_naming_directedit_replays` there.
