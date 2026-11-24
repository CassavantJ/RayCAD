# Fixture models

Workholding shipped with RayCAD's built-in library (task T-077). The CAM workspace's Fixtures panel lists every
`.step` / `.stp` file in the `fixtures` folder of each library (this one, and any shared or user library), and adds the
one chosen to the document as a fixture: a collision body the safety checks test the tool and holder against, never
machined. A shop can put its own vise or fixture plate into its shared library's `fixtures` folder the same way.

| File | Bodies | What it is |
|-|-|-|
| `generic-vise-6in.step` | `Generic 6 in vise`, `Generic 6 in vise moving jaw` | A plain milling vise of the 6 inch class: base, fixed jaw and screw as one body; the moving jaw as a body of its own, 2 in open |
| `generic-parallels-1x6in.step` | `Generic parallel 1/8 x 1 x 6 in A`, `... B` | A pair of parallels, 1/8 x 1 x 6 in |

Frame of the vise (mm): X along the jaws, Y along the screw (the moving jaw opens towards +Y), Z up. The floor between
the jaws, where parallels stand, is z = 0; the fixed jaw's clamping face is the plane y = 0, looking +Y, centred on
x = 0; the jaws are 152.4 wide and 44.45 tall above the floor. The jaw opening is set in RayCAD as the gap of a mate
between the moving jaw's clamping face and the fixed jaw's.

## Descriptions (placing a vise on the stock)

A model may have a description beside it, the same name with `.json` (`generic-vise-6in.json`), which lets the Fixtures
panel's "Place on stock" stand a setup's stock in the vise: the stock's bottom on the floor (or on the parallels' top
when the setup has parallels), centred along the jaws, the fixed jaw against one side, the moving jaw closed on the
other. It names the bodies the file imports as and gives the sizes that needs, in the frame above (mm):

| File | Description |
|-|-|
| `generic-vise-6in.json` | `"kind": "vise"`, the body and the moving jaw by name, `jawLength` 152.4, `jawHeight` 44.45, `maxOpening` 270 |
| `generic-parallels-1x6in.json` | `"kind": "parallels"`, the two bodies by name, `height` 25.4, `thickness` 3.175 |

The generic vise's `maxOpening` is where its moving jaw (60 thick) reaches the front end of its base (y = 330): a
property of this generic model, not of any product. A shop's own vise can have a description of its own when its model
is drawn in the same frame (`src/api/include/ray_api/cam_fixtures.h` gives the format).

## How they were made

They are original, simple, generic shapes drawn for RayCAD from round numbers of the size class (jaws 6 in wide and
1.75 in tall; parallels 1/8 x 1 x 6 in): boxes and one cylinder built with RayCAD's own kernel (`kernel::makeBox`,
`kernel::makeCylinder`, `kernel::boolean`) and written with `kernel::exportStep` (AP214, millimetres). No vendor's model,
drawing, catalogue, dimension table, name or trade dress was used or consulted. The source is
`tests/unit/cam/fixture_models.h`; the test `ray_cam_fixture_tests` ("the shipped fixture models are the shapes built in
code") checks that these files are exactly those shapes, and writes them when asked:

```
set RAY_T077_WRITE_FIXTURES=<repository>\resources\library\fixtures
build\windows-msvc\bin\Release\ray_cam_fixture_tests.exe "[library]"
```

The writer replaces the header line OpenCASCADE fills with the clock and its own placeholders by a fixed one (a fixed
date, no author, no organisation) and names the products after the bodies, so the files carry no date, person or
place. They are under the project's own licence, LGPL-2.1-or-later (THIRD_PARTY.md, "Original assets of the project").
