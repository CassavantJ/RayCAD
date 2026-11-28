# Naming: what a reference follows

The architect's policy (T-081b round 12, after review round 9 on the frozen block 5, seed 0x7E2B49C1D58A0000).

**For an element with identity, a reference follows the thing that made it.**

- The wall made by sketch line *k* is the same wall wherever line *k* goes, including when the profile is mirrored and
  moved.
- A whole end cap of a moved rib or slot is the same cap, even where its visible patch now lies elsewhere.
- An edge is the stretch between the same end elements: a slid end vertex does not make it another edge.

Geometry in place (where the picked ground went) is the test only for answers **without** identity.

## Examples (review round 9, `VERDICTS.md`)

| case | answer | by this policy |
|---|---|---|
| block 5 13:12 face 21 (slot R2 moved) | Exact {9} | R2's end cap, one face before and after, clipped otherwise by the block's edge: right |
| block 5 14:23 face 47 (rib R3 moved by its width) | Exact {46} | R3's start cap, its visible patch now below the plate: right |
| block 5, no placements, 6:13 face 4 and 11:14 face 16 | Exact | the moved wall's visible part (R2's arc wall, R1's wall s4): right |
| mirror and move of a profile (harness i2b, document S5) | Exact on the far wall | the wall of the same sketch line: right |
| block 5 11:18 edges 18 and 19 (chamfer R2 resized, deleted by R4) | Exact {18}, Exact {19} | the stretches between the same ends; the vertex the deleted chamfer left slid along the curve: right |

Known miss: a pure mirror of a profile (no move) answers Broken although identity would hold (`KNOWN_MISSES.md`).

## How the oracle applies it

The oracle (`tests/naming/oracle/oracle.cpp`, `expect`) keeps its own model of identity, from the script alone:

- **WHOLE GENERATOR.** The pick was the whole face of one generator (a sweep's side wall from one profile entity, or a
  cap of one loop): its description names that one owner with no separators, and it was the only face on it at pick
  time. The edited script still has the generator, edited (moved, mirrored, resized), with exactly one face on it, not
  merged. Then that face is the expected answer, wherever it lies; the structural check does not apply to it.
- **SLID END.** The pick was one of two edges on the same two faces meeting at a vertex on only those faces, each with
  its other end on a third element, the two told apart by their owners. After the edit the curve is where it was, the
  element is again two such edges, and the one that ends on the pick's end element is the expected answer, as long as
  it holds part of the pick and neither edge lies on another pick-time edge's ground.

Every other element is judged as before. Each rule has a mutation switch (`RAYCAD_ORACLE_MUTATE=whole-generator`,
`slid-end`, and the negatives' `whole-generator-once`, `slid-end-ends`, `slid-end-ground`) and pins in
`random_directedit_test.cpp` and `acceptance_test.cpp`.

Rulings this supersedes for whole generator faces of an edited generator (their pins now say so): round 4,
fillet_block_1 #29 face 17 (a moved boss's start-cap window, the class of 14:23); round 10 ruling A, review block 4
13:25 face 5 (a moved cut's end cap whose image left the part, the class of 13:12); round 11 ruling K, T-081a random
11:43 face 8 and random-directedit 8:3 face 15. The resolver refuses identity there (Broken), now a miss.
