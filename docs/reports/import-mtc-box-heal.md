# STEP import: the NIST/MTC box comes in invalid with the default healing

A note for the owner of the STEP import (`src/kernel/io_import.cpp`, task T-005), from task T-080 round 2. Nothing in
the import was changed for it.

## What was seen

`tests/corpus/cache/nist-mtc/nist_mtc_crada_box_rev-d.stp` (NIST/MTC CRADA box, revision D; one solid of 408 faces,
left out of `tests/corpus/directedit/set_nist.tsv` for this reason) was imported with each `ImportOptions::heal`
mode and the result put through BRepCheck (`testing::validityFaults`) and `massProps`:

| `heal` | What runs | Faults BRepCheck finds | Volume (mm3) |
|-|-|-|-|
| `Heal::None` | the STEP reader with its built-in shape fix off | nearly every face `BRepCheck_UnorientableShape` | 426,521 |
| `Heal::Reader` | the STEP reader's built-in shape fix (XSAlgo) | none: the solid is valid | 329,390 |
| `Heal::Full` (the default) | reader fix off, then `ShapeFix_Shape` | faces 1, 71, 395, 396, 397, 400 and 404 (face SubIds) `BRepCheck_UnorientableShape` | 329,389 |

So the file's shell has faces oriented inconsistently with their neighbours. The reader's own fix repairs all of them;
the default path (reader fix off, `ShapeFix_Shape` after) leaves seven unrepaired, and the import then warns that the
body "is not valid geometry after import".

## What it suggests (not tried)

- With `Heal::Full`, a body that fails BRepCheck after `ShapeFix_Shape` could be read again with the reader's fix on
  (`Heal::Reader`) and the valid result kept, or `ShapeFix_Shell` (with `FixFaceOrientation`) could be run on the
  shells of such a solid before `ShapeFix_Shape`. Either changes the import's output for other files too, so it needs
  the import corpus (`tests/corpus`) rerun, which is why it was left to the import owner.
- How it was measured: a throwaway Catch2 case that called `importStep` with each `heal` value and printed
  `testing::validityFaults(body.shape, 40)` and `massProps(body.shape)->volume`; Windows, Release, OCCT 8.0.1 with the
  port patches of `ports/opencascade` at port-version 8.
