# Naming resolver: known misses

A miss is a reference the resolver answers Broken while the naming oracle says the picked element is still there (the
oracle's verdict "missed"). A miss is never a wrong answer: the user picks the element again. This list records the
misses whose mechanism is known and accepted for now, so that a later change can be checked against it. Each entry
names the block and sequence (`part:seq`), the references, and why the resolver gives up there.

## T-081b round 10 (accepted in round 11, ruling J)

Thirteen references of `[random-directedit]` (with placements) went from an answer the oracle judged correct to Broken,
a miss, in round 10 (the face moves of direct edits, derived identity, and the rule that no guess keeps Exact). Two
mechanisms were named by the architect; the other sequences were not diagnosed separately.

### A crossing of two surfaces that moved differently is not composed

The post-condition maps an edge's or a vertex's samples by ONE move: the move of the pick's own surfaces, known only
when every surface that moved moved alike (postcondition.cpp `Checker::moveOf`). Where the surfaces an edge or vertex
lies on were moved by different moves (each surface's own: a feature's placement, a body move, a direct edit's face
move), the move is not known, the samples stay where they were picked, and the answer, which lies where the moved
surfaces now cross, holds none of them: Broken. Composing the crossing of each surface under its own move is not
implemented.

- `[random-directedit]` 9:44 (manifold_block_1): vertices 17, 19, 21, 27 (were Repaired, correct). Edges 27 and 32 of
  the same sequence (each between a sweep's side and an imported face) were misses of this kind in round 10; since
  round 11 they are Exact, correct: identity, the imported face counted as the face it was by its one-to-one history
  (identity by history, postcondition.h), not by composing the moves.

### A face move matched by the face's names is lost when an inserted cut splits the target face

A direct edit's face move (NamingInput::faceMoves) is recorded with the names the face had in the edit's input, and a
pick's surface is carried by the moves recorded with its face's names (postcondition.cpp `faceMoveOf`, up the split
records' parents). When an edit of the sequence inserts a cut upstream that splits the face the push targets, the push
moves a piece whose names (a split piece's) are not the picked face's, and the picked face's move is no longer found by
its names: the samples stay where they were picked, and the moved face holds none of them: Broken.

- `[random-directedit]` 12:48 (plate_countersink_1): faces 14, 22 (were Merged, correct).

### Not diagnosed separately

- `[random-directedit]` 17:38 (nist_ftc_11_asme1_rb_geometry): edges 26, 37; vertex 22 (were Repaired, correct).
- `[random-directedit]` 4:29 (fillet_block_1): vertex 32 (was Exact, correct).
- `[random-directedit]` 6:37 (flange_bolt_circle_1): vertex 42 (was Exact, correct).

(The same comparison, round 9 to round 10, also moved eight references from "gone" to "missed" with the same Broken
answer: block_blind_holes_1 #36 edges 1, 10 and face 2; closed_pocket_3 #10 edges 3, 12, 13 and vertices 3, 9. Those
are changes of the oracle's verdict, not of the resolver's answer.)

## T-081b round 11 (ruling I)

A reference made with samples whose element had no point to give, neither sample nor territory point (some cones,
B-spline and other faces, some edges), can be checked against nothing: it is Exact only by identity (generator,
derived, or by history), a Split or Merged answer to it is Broken, and any other answer a suggestion (Repaired). Its
Split and Merged answers that the oracle judged correct are now misses:

- `[random]` 18:7 face 20 (827-9999-905), 17:24 face 14 (nist_ftc_11_asme1_rb_geometry): Split; 8:37 face 10
  (l_bracket_1): Merged.
- `[random-directedit]` 18:12 face 9 (827-9999-905): Split.

(Its Exact answers not by identity are Repaired, correct but not usable until confirmed: 29 in `[random]`, 27 in
`[random-directedit]`, with placements.)

## T-081b round 12 (the architect's policy, docs/naming/POLICY.md)

### A pure mirror of a profile answers Broken

A profile mirrored in its sketch without a move (review round 9, `VERDICTS.md`: harness case i2, document cases S4 and
S8, the holes' points swapped) answers Broken on the walls whose sketch lines changed sides, although identity would
hold (the wall made by line *k* is that line's wall wherever it goes). This is inconsistent with a mirror and a move
(i2b, S5), which answers Exact on the far wall, right by the policy. It is safe (Broken, never a wrong element) and not
fixed now.

### The refusal of identity on the pick's own surfaces, on a whole generator face

The resolver refuses an identity answer whose element lies on the pick's own surfaces and holds none of the pick
(postcondition.cpp `refusedIdentity`, OnPickSurfaces; round 10 ruling A, round 11 ruling K). Where that element is the
whole face of a generator the edit moved, one face before and after, the policy says it is the pick wherever it lies,
so the refusal's Broken is a miss (the oracle's WHOLE GENERATOR):

- review block 4 13:25 face 5 (plate_thru_holes_1; a moved cut's end cap whose image left the part).
- T-081a random 11:43 face 8 (plate_counterbore_1), random-directedit 8:3 face 15 (l_bracket_1).
- random-directedit-sorted 4:29 face 17 (fillet_block_1; a moved boss's start-cap window).

Refusal 3 (OnPickSurfaces) may be over-refusing whole generator faces; to revisit after M6, not now.

### A flagged suggestion left incomplete

Not a miss, recorded here as a known shortfall: block 5 (seed 0x7E2B49C1D58A0000, `[random-directedit-review5]`) 8:25
edge 94, R1 resized: the crossing the pick was is now two edges (83: 1.667 mm, 84: 2.178 mm), and the Repaired
suggestion names 84 only. It is flagged and not usable without confirmation; the oracle counts it WRONG (incomplete),
and that judgement is kept. Since the next round it is listed, by its answer, on the known flagged-wrong suggestions
(tests/naming/oracle/reviewed_flagged.txt, at most one per block, Repaired only): counted as "WRONG (flagged
suggestion, reviewed)", apart from the WRONG count the gates hold at 0.

## Review block 6 (oracle-freeze-6)

- 12:40 (move R2 by its width): R2, a 0.93 mm through-cut, no longer cuts after the move and fails on replay. The oracle
  described R4's start cap by R2's wall s3, coplanar with it; once R2 fails it cannot describe that cap, so the cap and
  its boundary (face 29; edges 54, 71, 74, 76; vertices 38, 47, 49) are not judged. All eight answers are right
  (reviewed_unjudged.txt).
- 12:40 edge 76: Merged {66} is flagged although the edge is identical (same end points, length 5.655066): the Merged flag
  is unnecessary (a flag, not a wrong answer).
