# S9 Direct editing success rate

Spike for `PLAN.md` section 13, M0. Code in this folder is throwaway.

## 1. Question

How often can OpenCASCADE 8.0.1 perform the five v1 direct edits on imported, history-free solids:
delete fillet, delete hole, delete pocket, resize hole, push/pull a planar face? Which OCCT approach works
best for each, how do they fail, how long do they take, and do they return history that persistent naming
can use?

## 2. Short answer

| Edit | Best approach found | Success, all 50 solids | Success, 11 real parts | Success, 39 synthetic |
|-|-|-|-|-|
| Delete hole, plain bore | `BRepAlgoAPI_Defeaturing` on the bore faces plus blind bottom | 119 of 123 (97%) | 27 of 28 (96%) | 92 of 95 (97%) |
| Delete hole with counterbore, countersink or chamfer | Defeaturing on the whole coaxial stack | 63 of 63 (100%) | 33 of 33 | 30 of 30 |
| Delete pocket (closed, non-round) | Defeaturing on walls, floor, islands | 36 of 36 (100%) | 22 of 22 | 14 of 14 |
| Delete fillet, face not touching another blend | Defeaturing on the one face | 97 of 104 (93%) | 19 of 25 (76%) | 78 of 79 (99%) |
| Delete fillet, one face out of a run of blends | Defeaturing on the one face | 11 of 62 (18%) | 8 of 28 (29%) | 3 of 34 (9%) |
| Delete fillet, whole connected run | Defeaturing on all faces of the run | 29 of 46 (63%) | 18 of 35 (51%) | 11 of 11 |
| Resize hole, larger (+25% radius) | Boolean cut with a cylinder | 175 of 176 (99%) | 50 of 51 (98%) | 125 of 125 |
| Resize hole, smaller (-20% radius) | Boolean fuse of a sleeve; if the bore ends are not plain circles, Defeaturing then cut | 173 of 176 (98%) | 51 of 51 | 122 of 125 (98%) |
| Push/pull planar face, outward | Prism of the face, boolean fuse | 245 of 248 (99%) | 52 of 55 (95%) | 193 of 193 |
| Push/pull planar face, inward | Prism of the face, boolean cut | 244 of 248 (98%) | 51 of 55 (93%) | 193 of 193 |

`BRepOffset_MakeOffset` with `SetOffsetOnFace` is not usable for either resize or push/pull: 67% and 39%
overall, 20% and 4% on real parts, with access violations and stack overflows in one of its modes (section 5.4).

Only 11 of the 50 solids are real parts. The 39 synthetic solids are simple, were built by the same kernel
that then edits them, and flatter the result. Where the real and synthetic columns differ, believe the real one.

## 3. Method

### 3.1 Environment

- Core i9-13900K, 64 GB RAM, Windows 11 (the High-tier machine from `spikes/README.md`). No Low-tier numbers.
- OpenCASCADE 8.0.1 from vcpkg (port-version 1, triplet `x64-windows`, Release DLLs), MSVC 14.51, C++20, `/EHsc`.
- OCCT parallel mode off (`SetRunParallel(false)`) for Defeaturing and booleans.
- Other spike agents were building and running on the same machine during the measurements, so times are noisy.

### 3.2 Corpus: 50 solids, 11 real and 39 synthetic

**Real (11).** NIST MBE PMI Validation and Conformance Testing models, CTC 01-05 and FTC 06-11.
Source: `https://www.nist.gov/document/nist-pmi-step-files` (redirects to
`.../noindex/2024/06/19/NIST-PMI-STEP-Files.zip`, 13,976,599 bytes,
SHA-256 `8fa78429e6d8d9b0d7681d223b6aa9ec98c3772185c55b1a0e3679b21c181911`), downloaded 2026-11-17.
Licence: works of the US Government (NIST), public domain in the United States under 17 U.S.C. 105. The
README inside the archive carries no licence text; I did not re-read NIST's licence page in this spike.
Files used: the ten `AP203 geometry only/nist_{ctc_01..05,ftc_06..10}_*.stp` files, and
`nist_ftc_11_asme1_ap242-e2.stp` for FTC 11, because the geometry-only file for FTC 11 in this archive holds
a 6-face solid, not the part. The five STC models in the archive are the FTC geometry again and were left out.
Each file holds one solid; all eleven pass `BRepCheck_Analyzer` after import.

| Real solid | Faces | Spline faces | Fillet faces found | Blend runs | Holes | Hole stacks | Pockets | Planar faces |
|-|-|-|-|-|-|-|-|-|
| nist_ctc_01 | 139 | 0 | 39 | 5 | 10 | 0 | 2 | 80 |
| nist_ctc_02 | 663 | 34 | 116 | 8 | 94 | 78 | 2 | 128 |
| nist_ctc_03 | 139 | 0 | 28 | 0 | 15 | 0 | 2 | 82 |
| nist_ctc_04 | 517 | 0 | 136 | 10 | 60 | 52 | 6 | 112 |
| nist_ctc_05 | 208 | 9 | 26 | 4 | 31 | 21 | 2 | 62 |
| nist_ftc_06 | 144 | 0 | 28 | 1 | 22 | 10 | 2 | 71 |
| nist_ftc_07 | 269 | 20 | 103 | 4 | 27 | 4 | 0 | 70 |
| nist_ftc_08 | 270 | 0 | 112 | 14 | 20 | 0 | 3 | 79 |
| nist_ftc_09 | 158 | 0 | 12 | 0 | 29 | 4 | 2 | 62 |
| nist_ftc_10 | 214 | 1 | 98 | 4 | 42 | 8 | 0 | 53 |
| nist_ftc_11 | 42 | 0 | 8 | 2 | 1 | 0 | 2 | 26 |

These are test models for dimensioning, not job-shop customer parts, and they come from few CAD exporters.
No other real source was added: a search for further public-domain machined-part STEP files did not turn up
direct file links within the time spent, and files with unclear licences were not used.

**Synthetic (39).** `gen.cpp` builds 13 families in 3 variants each with OCCT (box, cylinder, cone, prism,
boolean, `BRepFilletAPI_MakeFillet`, `MakeChamfer`), writes each to STEP with `STEPControl_Writer` and reads
it back, so no construction history survives. Variant 1 of each family is passed through
`ShapeUpgrade_ShapeDivideClosed` first, so its cylinders arrive as two half faces, as many exporters write them.
All 39 are valid single solids with 10 to 60 faces (the real parts have 42 to 663).

| Family (faces in the 3 variants) | Content |
|-|-|
| plate_holes (16/26/28) | plate, rounded corners, hole grid, top chamfer in one |
| cbore_plate (22/36/26) | counterbored holes, blind flat holes, countersunk holes, top chamfer |
| pocket_block (15/23/24) | pocket with corner radii; floor fillets; two pockets of different depth |
| l_bracket (13/18/17) | extruded L, inner and outer fillet, holes in both legs, a slot |
| filleted_block (18/26/11) | vertical fillets plus top fillet loop; all 12 edges filleted; three radii meeting at one vertex |
| manifold (10/17/22) | cross-drilled bores that intersect, blind holes with 118 degree points, side port |
| slot_plate (20/22/23) | obround slot, pocket, hole through the pocket floor, hole cutting the pocket wall |
| step_block (11/15/20) | open step with root fillet, chamfers, holes on both levels |
| fixture_plate (44/56/60) | BP1-like: two pockets with 0.25 in corner radii, tapped-hole bores, counterbores, chamfer, floor fillets in one |
| boss_bracket (14/23/24) | round boss with root and top fillets, bore through the boss, rectangular boss |
| u_channel (17/24/23) | extruded U, inner and outer fillets, cross holes through both walls |
| island_pocket (20/18/34) | pocket with a rectangular or round island, hole in the island, floor fillets |
| flange (14/43/22) | disc with hub, bolt circle, counterbores, flats, keyway cutting the bore |

### 3.3 Automatic target choice (`main.cpp`, `analyse`)

- **Hole:** concave cylindrical faces on one cylinder (same axis and radius, connected) whose angular spans
  add up to 360 degrees. The delete target also takes "cap" faces whose every edge borders the target
  (flat or conical blind bottom).
- **Hole stack:** a hole plus the coaxial concave cylinders and cones and the annular planes joined to it
  (counterbore, countersink, mouth chamfer, drill point).
- **Fillet:** a cylindrical face under 175 degrees of arc, tangent (normals within 2 degrees) across two
  straight edges to two different neighbours; or a toroidal face tangent to two different neighbours.
  Pocket corner radii qualify. Spline blend surfaces are not targets.
- **Blend run:** fillet faces connected through shared edges, plus corner patches (sphere, torus or spline
  faces whose every edge is tangent-continuous and that touch at least two fillet faces). Only runs of two
  or more faces.
- **Pocket:** for a planar face with an inner loop, the faces reached from that loop without crossing the
  planar face, provided the set never reaches the outer boundary of that face, lies on or below its plane,
  and contains at least one planar wall. Pockets with a hole through the floor and round pockets are
  therefore not pocket targets (round ones count as holes).
- **Planar face:** all planar faces sorted by area; the picks are spread from largest to smallest.
- Up to 5 targets per kind per solid, spread evenly over the detected list.

Targets available in the corpus: 876 fillet faces, 63 blend runs, 533 holes, 209 hole stacks, 37 pockets,
1,261 planar faces (real parts: 706, 52, 351, 177, 23, 825).

### 3.4 Edits attempted

| Name in the data | What it does |
|-|-|
| `del_fillet`, `del_fillet_chain`, `del_hole`, `del_hole_stack`, `del_pocket` | `BRepAlgoAPI_Defeaturing` on the target faces |
| `rs_cut_grow` | radius x 1.25: boolean cut with a cylinder over the bore's axial extent, extended by one radius past open ends |
| `rs_plug_shrink` | radius x 0.8: boolean fuse of a sleeve (old radius outside, new inside); only when the bore ends are plain circles |
| `rs_defeat_grow`, `rs_defeat_shrink` | Defeaturing of the hole, then boolean cut at the new radius |
| `rs_off_{ip,ic,ap}_{grow,shrink}` | `BRepOffset_MakeOffset`, global offset 0, `SetOffsetOnFace` on the bore faces; join Intersection (i) or Arc (a), `Intersection` flag off (p) or on (c) |
| `pp_prism_{out,in}` | `BRepPrimAPI_MakePrism` of the face along its normal, boolean fuse (out) or cut (in) |
| `pp_prismu_{out,in}` | the same followed by `ShapeUpgrade_UnifySameDomain` on the result |
| `pp_off_{ip,ic,ap}_{out,in}` | `BRepOffset_MakeOffset` with `SetOffsetOnFace` on the one planar face |

Push/pull distance is 1% of the bounding-box diagonal, clamped to 0.25..2.0 mm (0.75 to 2.0 mm in this corpus).

### 3.5 What counts as success

One process per attempt (`run.py`). An attempt is `ok` only if all of these hold:

1. The API reports done without errors, and the result contains exactly one solid.
2. `BRepCheck_Analyzer` is valid, `ShapeAnalysis_Shell` finds no free and no badly oriented edges, volume > 0.
3. Volume changed (more than 1e-9 relative), in the expected direction, by a plausible amount:
   hole delete 0.3..1.05 x pi r^2 h (stack 0.1..1.05 with the widest radius); pocket 0.15..1.05 x the
   bounding box of its faces; fillet up to 1.3 x sum(area x radius), sign from convexity; resize
   0.3..1.7 x pi |r'^2 - r^2| h; push/pull 0.5..1.6 x area x distance.
4. Target points behave as intended, tested against the result: a point on each removed face must now be
   strictly inside (material added) or outside (material removed); for resize a point on the new wall, and
   for push/pull a point on the moved plane, must lie on the boundary (distance under 1e-5 mm).

Failure categories: `api_error` (clean error status), `exception` (C++ exception caught), `invalid` (rule 1
or 2), `no_op` (volume unchanged), `wrong` (rule 3 or 4), `timeout` (operation alone over 10 s; a watchdog
thread ends the process), `crash` (structured exception caught by `__try/__except` around the operation),
`hard_crash` (process died without a result line).

History check on every `ok` attempt: for each input face that was not a target, is it still present, or does
`Modified()` name faces that exist in the result, or only `Generated()`, or `IsRemoved()`, or nothing; does
the image lie on the same analytic surface; and does every result face have a source in the inputs.

### 3.6 Runs

The full matrix ran five times during development, 8 processes at a time, as approaches were added and
checker defects fixed. The last three runs share 4,553 attempts; their outcome counts are identical except
for one attempt that sits at the 10 s limit. The numbers below are from the last run (`data/run5`,
4,729 attempts). Times are from a separate one-process-at-a-time pass over the recommended approaches
(`data/run5_seq`, 2,185 attempts). About 28,600 processes were launched in total.

Checker defects found and fixed during development, stated because they changed numbers: the first run
used `BRepClass3d_SolidClassifier` alone for the "on boundary" test, and it answered IN for points lying
exactly on a re-cut cylinder wall (at least 9 false failures); pocket islands level with the top face and
countersink radii were not allowed for (7 false failures); the cut tool's length was taken from vertices only.

## 4. Numbers

### 4.1 All approaches, all 50 solids

| Edit | Attempts | Solids | ok | Rate | api error | exception | invalid | no-op | wrong | timeout | crash (SEH) |
|-|-|-|-|-|-|-|-|-|-|-|-|
| del_fillet | 168 | 44 | 108 | 64% | 0 | 0 | 2 | 51 | 5 | 2 | 0 |
| del_fillet_chain | 46 | 17 | 29 | 63% | 0 | 0 | 5 | 1 | 9 | 2 | 0 |
| del_hole | 176 | 42 | 158 | 90% | 0 | 0 | 0 | 17 | 1 | 0 | 0 |
| del_hole_stack | 63 | 14 | 63 | 100% | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| del_pocket | 36 | 19 | 36 | 100% | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| rs_cut_grow | 176 | 42 | 175 | 99% | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| rs_plug_shrink | 176 | 42 | 160 | 91% | 16 (not applicable) | 0 | 0 | 0 | 0 | 0 | 0 |
| rs_defeat_grow | 176 | 42 | 159 | 90% | 17 | 0 | 0 | 0 | 0 | 0 | 0 |
| rs_defeat_shrink | 176 | 42 | 158 | 90% | 17 | 0 | 0 | 0 | 1 | 0 | 0 |
| rs_off_ip_grow | 176 | 42 | 118 | 67% | 21 | 18 | 19 | 0 | 0 | 0 | 0 |
| rs_off_ip_shrink | 176 | 42 | 118 | 67% | 21 | 17 | 19 | 0 | 1 | 0 | 0 |
| rs_off_ic_grow | 176 | 42 | 118 | 67% | 16 | 18 | 10 | 0 | 0 | 14 | 0 |
| rs_off_ic_shrink | 176 | 42 | 118 | 67% | 17 | 17 | 9 | 0 | 1 | 14 | 0 |
| rs_off_ap_grow | 176 | 42 | 0 | 0% | 5 | 141 | 0 | 0 | 20 | 0 | 10 |
| rs_off_ap_shrink | 176 | 42 | 0 | 0% | 0 | 141 | 0 | 0 | 25 | 0 | 10 |
| pp_prism_out | 248 | 50 | 245 | 99% | 0 | 0 | 0 | 0 | 3 | 0 | 0 |
| pp_prism_in | 248 | 50 | 244 | 98% | 0 | 0 | 2 | 0 | 2 | 0 | 0 |
| pp_prismu_out | 248 | 50 | 238 | 96% | 0 | 0 | 7 | 0 | 3 | 0 | 0 |
| pp_prismu_in | 248 | 50 | 241 | 97% | 0 | 0 | 5 | 0 | 2 | 0 | 0 |
| pp_off_ip_out | 248 | 50 | 96 | 39% | 42 | 16 | 27 | 0 | 67 | 0 | 0 |
| pp_off_ip_in | 248 | 50 | 94 | 38% | 44 | 15 | 31 | 0 | 64 | 0 | 0 |
| pp_off_ic_out | 248 | 50 | 96 | 39% | 33 | 15 | 27 | 0 | 63 | 14 | 0 |
| pp_off_ic_in | 248 | 50 | 94 | 38% | 34 | 15 | 27 | 0 | 62 | 15 | 1 |
| pp_off_ap_out | 248 | 50 | 0 | 0% | 0 | 203 | 0 | 0 | 33 | 0 | 12 |
| pp_off_ap_in | 248 | 50 | 0 | 0% | 5 | 203 | 0 | 0 | 28 | 0 | 12 |

No `hard_crash` in this run; see 5.5 for the one seen in an earlier run.

### 4.2 Real parts only (11 solids)

| Edit | Attempts | ok | Rate | api error | exception | invalid | no-op | wrong | timeout | crash |
|-|-|-|-|-|-|-|-|-|-|-|
| del_fillet | 55 | 27 | 49% | 0 | 0 | 1 | 25 | 0 | 2 | 0 |
| del_fillet_chain | 35 | 18 | 51% | 0 | 0 | 5 | 1 | 9 | 2 | 0 |
| del_hole | 51 | 38 | 75% | 0 | 0 | 0 | 12 | 1 | 0 | 0 |
| del_hole_stack | 33 | 33 | 100% | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| del_pocket | 22 | 22 | 100% | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| rs_cut_grow | 51 | 50 | 98% | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| rs_plug_shrink | 51 | 47 | 92% | 4 | 0 | 0 | 0 | 0 | 0 | 0 |
| rs_defeat_shrink | 51 | 38 | 75% | 12 | 0 | 0 | 0 | 1 | 0 | 0 |
| rs_off_ip_grow / shrink | 51 / 51 | 10 / 10 | 20% | 11 / 10 | 15 / 15 | 15 / 15 | 0 | 0 / 1 | 0 | 0 |
| pp_prism_out | 55 | 52 | 95% | 0 | 0 | 0 | 0 | 3 | 0 | 0 |
| pp_prism_in | 55 | 51 | 93% | 0 | 0 | 2 | 0 | 2 | 0 | 0 |
| pp_prismu_out / in | 55 / 55 | 51 / 51 | 93% | 0 | 0 | 1 / 2 | 0 | 3 / 2 | 0 | 0 |
| pp_off_ip_out / in | 55 / 55 | 2 / 2 | 4% | 10 / 12 | 16 / 15 | 16 / 19 | 0 | 11 / 7 | 0 | 0 |

Synthetic-only rates: del_fillet 72%, del_fillet_chain 100%, del_hole 96%, del_hole_stack 100%, del_pocket
100%, rs_cut_grow 100%, rs_plug_shrink 90% (the rest not applicable), rs_defeat_shrink 96%, pp_prism 100%,
pp_prismu 97-98%, rs_off_ip 86%, pp_off_ip 48-49%.

### 4.3 Breakdowns that explain the delete rates

Delete fillet, single face (168 attempts, 2 of which timed out and are not in this table):

| Target | Attempts | ok | no-op | wrong | invalid |
|-|-|-|-|-|-|
| Face touches no other blend face | 104 | 97 (93%) | 6 | 0 | 1 |
| Face is one piece of a run | 62 | 11 (18%) | 45 | 5 | 1 |

Real parts: isolated 19 of 25, run member 8 of 28. Synthetic: isolated 78 of 79, run member 3 of 34.
The 5 `wrong` results are valid solids in which material was added or removed on the wrong side
(for example one corner piece of a pocket-floor fillet removed alone: volume went up, not down).

Delete hole, bore faces only (176 attempts):

| Target | Attempts | ok | no-op | wrong |
|-|-|-|-|-|
| Plain bore (not part of a stack) | 123 | 119 (97%) | 3 | 1 |
| Bore that has a counterbore, countersink or chamfer, removed without them | 53 | 39 (74%) | 14 | 0 |

Real: plain 27 of 28, in-stack 11 of 23. The plain-bore failures are bores crossed by other bores (3,
synthetic manifold blocks) and one groove-like bore in FTC 11 where the result lost volume.
Removing the whole stack instead succeeded 63 of 63.

Whole blend runs (46 attempts, 35 on real parts): 29 ok; 9 wrong (some faces of the run left in place or
volume far outside range), 5 invalid, 2 timeouts, 1 no-op. All 17 failures are on real parts.

### 4.4 Resize hole: which approach

| Strategy on the same 176 holes | ok | All | Real | Synthetic |
|-|-|-|-|-|
| Larger: cut with a cylinder | 175 | 99% | 98% | 100% |
| Larger: Defeaturing then cut | 159 | 90% | 76% | 96% |
| Smaller: sleeve fuse only | 160 | 91% | 92% | 90% |
| Smaller: Defeaturing then cut only | 158 | 90% | 75% | 96% |
| Smaller: sleeve fuse, else Defeaturing then cut | 173 | 98% | 100% | 98% |
| Either: `BRepOffset_MakeOffset` (Intersection join) | 118 | 67% | 20% | 86% |

The one failed enlargement is real: 25% more radius on a CTC 05 bore split the part into two solids.
Defeaturing-then-cut fails exactly where the bore cannot be deleted on its own (17 holes). The sleeve was
"not applicable" for 16 bores whose ends are not plain circles (cross-drilled or cutting a wall); 13 of
those were then handled by Defeaturing then cut, 3 were not (crossed bores in the synthetic manifolds).

### 4.5 Push/pull: which approach

| | Prism + boolean | Prism + boolean + unify | Offset, Intersection join |
|-|-|-|-|
| All neighbours square to the face (125 faces): ok out / in | 124 / 124 | 118 / 121 | 88 / 88 |
| A neighbour is tangent or at another angle (123 faces): ok out / in | 121 / 120 | 120 / 120 | 8 / 6 |

- Prism + boolean produced a valid solid with the face moved in 489 of 496 attempts. Where every neighbour
  is square to the face the result equals a true face offset. Where a neighbour is a fillet, chamfer or
  drafted wall (half of the sampled faces), the result is a slab added or a notch cut, with a step next to
  that neighbour; the checks in 3.5 pass such results, so the 98% in that row means "valid and moved",
  not "neighbours extended".
- `BRepOffset_MakeOffset` moves every face tangent-connected to the target, not just the target (67 and 64
  `wrong` results: same volume change whichever face of a rounded outline was picked), rebuilds the whole
  solid, and on real parts failed for 53 of 55 faces in each direction.
- `ShapeUpgrade_UnifySameDomain` turned 10 results that were valid after the boolean into invalid solids
  (`BRepCheck_NotClosed`, `UnorientableShape`, `InvalidCurveOnClosedSurface`); 9 of the 10 are on synthetic
  parts, mostly where the prism's side faces continue a cylindrical corner. It also merges faces anywhere
  in the body (face count changed by up to -170 on CTC 02), not only around the edit.
- Without unify the pushed face's neighbours are continued by separate coplanar or co-cylindrical faces:
  median +4 faces (out), +2 (in).
- The 7 prism failures are all on real parts: 5 on FTC 11 (tiny faces, the moved cap ended up buried in or
  clear of other material), 1 inward cut that split CTC 03 into 4 solids, 1 with badly oriented edges on CTC 02.

### 4.6 Time (one process at a time, operation only, without STEP load or checks)

| Edit | Median ms all / real / synthetic | 95th percentile ms all / real | Worst completed ms | Over 10 s |
|-|-|-|-|-|
| del_fillet | 26 / 103 / 20 | 424 / 518 | 2,262 | 2 of 168 |
| del_fillet_chain | 140 / 237 / 18 | 821 / 846 | 5,006 | 2 of 46 |
| del_hole | 18 / 70 / 12 | 229 / 300 | 1,210 | 0 |
| del_hole_stack | 39 / 65 / 20 | 244 / 260 | 320 | 0 |
| del_pocket | 34 / 83 / 7 | 151 / 151 | 271 | 0 |
| rs_cut_grow | 6 / 27 / 5 | 56 / 102 | 174 | 0 |
| rs_plug_shrink | 8 / 30 / 7 | 57 / 86 | 103 | 0 |
| rs_defeat_shrink | 25 / 110 / 18 | 270 / 326 | 1,256 | 0 |
| pp_prism_out | 5 / 27 / 4 | 56 / 103 | 141 | 0 |
| pp_prism_in | 7 / 29 / 6 | 53 / 157 | 351 | 0 |
| pp_prismu_out | 8 / 44 / 6 | 80 / 167 | 426 | 0 |
| pp_prismu_in | 8 / 46 / 7 | 70 / 183 | 444 | 0 |

The four Defeaturing attempts over 10 s (all on CTC 02 and CTC 05) were re-run with a 150 s limit; none
finished within 150 s. With a `Message_ProgressIndicator` whose `UserBreak()` turns true 2.0 s after the
start, Defeaturing returned `BOPAlgo_AlertUserBreak` after 2.45, 2.49, 5.2 and 11.4 s, so cancellation works
but took up to 9.4 s to be honoured. In the offset runs, the `Intersection=true` mode hit the 10 s limit 57
times (not followed up).

### 4.7 History

On successful attempts:

| Approach | History object returned | Untouched faces still present or reported `Modified` | Untouched faces whose image is on a different surface | Result faces with no source | Edited face |
|-|-|-|-|-|-|
| Defeaturing (394 ok attempts over the five delete kinds) | 100% | 100% | 0 of 40,993 | 1 face in 1 attempt (a blend run on a real part) | `IsRemoved` for 100% of removed faces |
| Boolean cut / sleeve fuse for resize (335) | 100% | 100% | 0 of 31,884 | 0 | old wall `IsRemoved`; the new wall is the image of the tool's cylinder face, found in 335 of 335 |
| Defeaturing then cut (317) | 100% (two histories joined with `BRepTools_History::Merge`) | 100% | 0 of 20,363 | 0 | as above, 317 of 317 |
| Prism + boolean, with or without unify (968) | 100% | 100% | 0 of 69,958 | 0 | old face `IsRemoved`; the moved face is the image of the prism's end cap, found in 968 of 968 |
| `BRepOffset_MakeOffset` (852) | 100% | 0%: every face, moved or not, is reported only through `Generated()` | 48 of 27,624 | 0 | neither removed nor modified |

"Still present" means the result reuses the same face object, which booleans and Defeaturing do for faces
they do not touch; `Modified()` is then empty and `IsDeleted()` false, and the caller has to treat that as
"unchanged". The history check here verifies that a record exists and that the image lies on the same
plane, cylinder, cone, sphere or torus. It does not prove that the image is the right piece when a face is split.

## 5. Failures, stated plainly

### 5.1 Defeaturing says "done" when it did nothing

69 delete attempts returned `IsDone()` true, no error, and the input shape unchanged. 63 carried the warning
`BOPAlgo_AlertUnableToRemoveTheFeature`; 6 carried no warning at all (toroidal boss fillets that had been
split into two halves). 7 more delete attempts returned an invalid solid as "done", and 15 returned a valid
solid that was not the requested edit. The caller must compare volume and face count and must validate.

### 5.2 Fillets are the weak edit

One fillet face out of a run almost never comes out alone (18%). Whole runs come out 63% of the time
overall and 51% on real parts, with invalid and wrong results among the failures, and 4 of 214 fillet
attempts ran for more than 150 s.

### 5.3 `ShapeUpgrade_UnifySameDomain` can break a valid solid

See 4.5: 10 of 496, deterministic (same result in every run).

### 5.4 `BRepOffset_MakeOffset` with per-face offsets

- Arc join: 0 successes in 848 attempts; 638 threw `StdFail_NotDone` and 44 raised structured exceptions,
  26 stack overflows (0xC00000FD) and 18 access violations (0xC0000005), on CTC 02, CTC 04 and three
  synthetic parts.
- Intersection join: 1 access violation (CTC 03, push/pull inward), C++ exceptions of several kinds
  (`Standard_ConstructionError`, `Standard_OutOfRange` from `BSplCLib::Interpolate`, `Standard_NoSuchObject`),
  `BRepOffset_Error` 8, null results, and invalid solids reported as done.
- The exceptions repeat for every target on the same part: the algorithm rebuilds all faces, so one
  difficult face anywhere in the part fails every edit on that part.

### 5.5 Crashes and containment

- In the Defeaturing, boolean and unify paths: 0 C++ exceptions and 0 structured exceptions in 2,185
  attempts per run, with the same outcome in the last three full runs and both sequential passes.
- All 45 structured exceptions in the last run came from `BRepOffset_MakeOffset`. Every one was caught by a
  `__try/__except` block around the operation, including the stack overflows, and the process then wrote
  its result line and exited. The spike does not go on using OCCT in that process afterwards, so this
  shows that the fault can be caught, not that the process is healthy after catching it.
- One process in about 28,600 died with exit code 0xC0000005 and no result line (run 2, FTC 07,
  `rs_off_ip_shrink`, target 1, after 0.5 s; the same attempt normally ends in a caught
  `Standard_ConstructionError`). It did not recur in 400 repeats of that attempt or in three later full
  runs. Cause unknown; an unhandled-exception reporter was added afterwards and never fired.
- OCC exceptions (`Standard_Failure`) were always catchable with `catch (const Standard_Failure&)`.

## 6. What was not tested

- Customer-grade real parts: imported files with healing problems, sewn surfaces, tolerant edges, spline
  fillets, drafted walls, thin walls, inch-unit job-shop parts. 11 real solids from one source is a small sample.
- Assemblies, multi-body files, sheet bodies, IGES.
- Resizing by other factors than x1.25 and x0.8; resizing blind holes so that the bottom follows (the
  drill-point cone or flat bottom keeps its old size in the approaches tested); tapped-hole semantics.
- Push/pull by large distances, through neighbouring features, or of non-planar faces. A push/pull that
  extends non-square neighbours (true face offset) has no working implementation here.
- Deleting chamfers, bosses, slots, open pockets, pockets with a hole through the floor, round pockets as pockets.
- Whether the history is correct for split faces, and anything about edges and vertices.
- Sequences of edits on the same body; replay determinism across machines and operating systems.
- Low-tier hardware, memory use, OCCT parallel mode.
- Process state after a caught structured exception.
- The timings include no tessellation or naming work.

## 7. Recommendation

### 7.1 What v1 can promise

| Edit | Promise | How |
|-|-|-|
| Delete hole | Yes. Select any face of the hole; the kernel removes the bore, its blind bottom, and its counterbore / countersink / chamfer together. | Defeaturing on the coaxial stack (100% here), on the bore alone when there is no stack (97%) |
| Delete pocket | Yes, for closed pockets. | Defeaturing on the connected depression (36 of 36 here; small sample) |
| Resize hole | Yes, for through and blind cylindrical bores; the bottom shape is not rescaled. | Larger: boolean cut. Smaller: sleeve fuse, falling back to Defeaturing then cut |
| Push/pull planar face | Yes, when every neighbouring face is square to it. For other faces either refuse or offer it as "add / remove a slab", because the neighbours are not extended. | Prism + boolean; unify afterwards only if the unified result validates |
| Delete fillet | Only as "works on simple fillets, may refuse". Offer run selection; do not promise it in release notes or in the M6 demo script without a fallback part. | Defeaturing on the selected face, or on the whole run when the face belongs to one |

Do not build on `BRepOffset_MakeOffset` for direct editing.

Every direct edit needs the same guard in the kernel: run it, then reject the result unless it is one valid
closed solid whose volume moved in the expected direction and whose target faces are gone or moved. Without
that guard about 4% of delete attempts would hand back a wrong or invalid body as success (22 of 489), and
another 14% would silently do nothing (69 of 489).

### 7.2 Wording for failures

| Situation | Message |
|-|-|
| Defeaturing left the shape unchanged | "Can't remove these faces. The faces around them can't be extended to close the gap. Nothing was changed." |
| Single fillet face that belongs to a run | "This fillet is part of a longer blend. Select the whole blend and try again." (with a button that selects the run) |
| Result invalid or not the requested change | "The edit produced a body that failed its check, so it was discarded. Nothing was changed." |
| Result is more than one solid | "This change would cut the part in two. Nothing was changed." |
| Over the time limit or cancelled | "Stopped after N seconds. Nothing was changed." |
| Push/pull on a face with a fillet, chamfer or angled neighbour | "This face meets a fillet, chamfer or angled wall. Push/pull can only add or remove a slab here and will leave a step." |
| Hole enlargement that breaks out | "A hole this large would break through the part. Nothing was changed." |

### 7.3 Changes suggested to `PLAN.md` and `kernel.h`

1. **`kernel.h` `offsetFaces`**: document as planar faces only, implemented as prism + boolean, with a
   warning in `OpResult.warnings` when a neighbour is not square. Per-face surface offset is not available.
2. **`kernel.h` `resizeHole(Shape, SubId cylinderFace, ...)`**: a hole is often two or more faces. Document
   that the kernel expands the given face to all faces on that cylinder, or take a span of faces.
3. **`kernel.h` history for direct edits**: the tool body is internal, so `HistoryEntry.inputIndex` cannot
   refer to it. For resize and push/pull OCCT reports the edited face as deleted and the new face as coming
   from the tool. The kernel should rewrite that as `Modified` (old face to new face); the mapping was
   recoverable in 1,620 of 1,620 successful attempts. Without this, T-081's scenario "resize a hole
   referenced by a drill op (`Exact`)" cannot pass. State also that "face object unchanged, no history
   entry" means unchanged.
4. **`kernel.h` `unifySameDomain`**: must validate its result and return the input with a warning when the
   result is invalid (10 of 496 here). Decide whether it may merge faces away from the edit; if a direct
   edit calls it on the whole body, naming sees merges of unrelated faces.
5. **`kernel.h` contract line "Every function catches Standard_Failure"**: add that every modifying
   function validates its result (`BRepCheck_Analyzer`, one closed solid) and detects no-ops, because OCCT
   reports both as success. Structured exceptions were only seen in `BRepOffset_MakeOffset`; if that class
   is ever used, it needs an SEH guard or a separate process.
6. **`PLAN.md` T-080**: replace "success rate at least the rate S9 measured" with the per-edit rates of
   section 2 on this corpus, and define "cancel works" with a latency (Defeaturing took up to 9.4 s to honour
   a cancel request and can otherwise run for minutes). Add the fall-back chains (stack then bore; sleeve
   then Defeaturing-and-cut; unify only if valid) to the task text.
7. **`PLAN.md` T-081**: "offset a face that splits a neighbour (`Split` with both pieces)" does not
   describe what prism + boolean does. Neighbours are continued by new faces (no unify) or merged (unify).
   Add a merge case to the naming scenarios.
8. **`PLAN.md` M6 demo and T-083**: "delete a fillet" should read "delete a simple fillet or a selected
   blend run, with a clear refusal otherwise"; add run selection to T-083. Push/pull arrow only on faces
   with square neighbours, or with the slab warning.
9. **Corpus**: before M6, add at least 20 real job-shop STEP files with clear licences (Jake's own customer
   parts would do) and re-run `run.py`; the fillet and push/pull numbers are the ones most likely to move.

## 8. Reproduce

From the repository root, with the OpenCASCADE `bin` folder (`installed/x64-windows/bin` under the vcpkg
root) on `PATH`:

```
cmd /c "spikes/build.cmd s9-directedit"
cd spikes/s9-directedit
build/s9.exe gen data/corpus                 (39 synthetic STEP files)
(copy the 11 NIST files named in 3.2 into data/corpus)
python run.py data/corpus data/run 8         (about 4 minutes)
python summarize.py data/run > data/run/summary.md
build/s9.exe crashtest data/corpus/syn_01_plate_holes_0.stp    (shows an access violation being contained)
```

Files: `main.cpp` (target detection, edits, checks), `gen.cpp` (synthetic corpus), `run.py` (driver),
`summarize.py` (tables). `data/` is git-ignored (92 MB): corpus, the NIST archive, `run5/results.jsonl`
with one JSON line per attempt, `run5/summary.md` with per-solid tables and every failure message.
Environment switches used for the side experiments: `S9_ONLY`, `S9_TIMEOUT_MS`, `S9_CANCEL_MS`, `S9_DEBUG`,
`S9_UNIFY`, `S9_HEAP`.
