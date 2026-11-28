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
