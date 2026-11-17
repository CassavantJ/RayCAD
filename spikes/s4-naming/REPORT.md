# S4 Persistent naming prototype: report

Spike S4 of `PLAN.md` section 13. Throwaway code in `spikes/s4-naming/`. Raw output of the final run is in `results.md` (per rule set: every table quoted here plus the first 60 wrong cases) and `probes.txt`.

Machine: Core i9-13900K, 64 GB, Windows 11. OpenCASCADE 8.0.1 (vcpkg, x64-windows, release). MSVC 14.51, Release.
Build: `spikes\build.cmd s4-naming`. Run: `build\s4.exe` (about 8 minutes, writes `results.md`), `build\s4.exe -h` (history probes).

## 1. Question

Does the persistent-naming design of `PLAN.md` 3.3 and `docs/interfaces/naming.h` hold on real OCCT 8.0.1 history: after an upstream edit and a rebuild, does every stored reference resolve to the same logical element (`Exact`), report `Split`, get `Repaired`, or go `Broken`, and never silently resolve to a different element?

## 2. Short answer

- The design **as written fails the "0 wrong" requirement**: 17 wrong resolutions in 33,785 (16 of 31,819 recorded references, 1 of 1,966 feature-internal edge picks). 8 are silent (`Exact` on the wrong or an incomplete element); 9 are `Repaired` on a different element, one of which made a chamfer feature build on the wrong edge.
- On the narrower acceptance set that 3.3 itself defines (edit each of the first 10 features: dimension change, added sketch line, reorder) the design as written **passes**: 99.5 % `Exact`, 0.5 % `Broken`, 0 wrong in 12,186 resolutions. All 17 wrong cases come from the wider edit list of this spike (hole break-out, sketch moved so topology changes, a cut added early next to a split face, a degenerate tangency).
- Three causes, each with a rule change that removed it in the prototype (section 6). With the changes: **0 wrong in 33,785**, 97.8 % `Exact`, 0.1 % `Split`, 0.2 % `Merged` (new state), 0.1 % `Repaired`, 1.8 % `Broken` (of the 572 `Broken`, 483 are references whose element really no longer exists).
- OCCT 8.0.1 history was better than feared: no operation returned wrong history, booleans and `SimplifyResult` were complete, fillet and chamfer leave 4 edges and 4 vertices per blend without any history. Two kernel failures unrelated to naming were hit (section 7).
- Recommendation: the 3.3 design is sound in structure and should go ahead, but `naming.h` and rule 4 of 3.3 must change before features are built on it (section 10). Residual risk is real and listed there.

## 3. Method

### 3.1 The part

`script.cpp` builds one prismatic block (160 x 100 x 30 mm plate with bosses, pockets, a step, slots, a lug) from 40 features, every dimension a named parameter:

| kind | count | features |
|-|-|-|
| extrude, new/join | 5 | plate, rectangular boss, round boss, island in a pocket, lug |
| extrude, cut | 8 | step, rounded pocket, sharp pocket, obround through slot, through notch, pentagon pocket, channel between two pockets, side groove |
| hole (circle profile cut) | 10 features, 18 holes | 4-hole through pattern, counterbores, bore plus counterbore, blind holes, cross hole into the bore, dowels, hole through island and lug |
| fillet | 9 | plate corners, pocket corners, boss corners, boss base (torus), pocket floor loop by tangent propagation, notch corners, step root |
| chamfer | 8 | bottom perimeter and slot rim by tangent propagation, boss top loop (planes and cones), countersinks, bore mouth |

Sketch profiles are lines, arcs and circles with stable entity tags and loop ids. Extrudes are `BRepPrimAPI_MakePrism` plus `BRepAlgoAPI_Fuse`/`Cut` followed by `SimplifyResult()` (same-domain unification is on for every boolean). Fillets and chamfers are `BRepFilletAPI_MakeFillet`/`MakeChamfer`; their edge inputs are `TopoRef`s recorded when the baseline was built and **resolved through the prototype resolver on every rebuild**, so a naming error changes the geometry of the rebuilt part. Baseline: 141 faces, 347 edges, 219 vertices, volume 448,221 mm3, `BRepCheck` valid after every feature. The baseline contains four faces split in two by a later feature (8 pieces), 19 edge pieces from splits, one face merged from two features by unification (channel floor and pocket floor), and a bspline intersection (cross hole into bore).

### 3.2 References

Every face, every non-seam edge and every vertex of the baseline that the oracle can vouch for is a reference: **677** (141 faces, 317 edges, 219 vertices). Faces by role: 30 cut walls, 5 cut floors, 18 hole walls, 8 hole floors, 29 fillet faces, 29 chamfer faces, 15 boss and plate sides, 5 top faces, 2 bottom faces. 30 seam edges are excluded (one adjacent face). 65 of the 677 (10 edges, 55 vertices) are "weak-oracle" references (section 3.5). In addition the 42 edge picks held by the 17 fillet and chamfer features are judged at the point in the tree where each feature resolves them.

### 3.3 Edit scenarios

47 scenarios, each a full rebuild from the edited script (per-scenario numbers in 4.2): no-op; a dimension change on each of the first ten features (F01 base twice, F02, F35, F03 to F09); F10 hole diameter and position; move a hole; two fillet radius changes; add a feature early (two variants) and at the end; delete (5 variants); reorder (5 variants); add a sketch line (4 sketches); sketch changed so a face splits (2); faces merge (3) and un-merge (1); suppress (3); a hole that breaks out of the part; a degenerate tangent hole; two scenarios built to attack ordinals; one combined edit.

### 3.4 The prototype (`naming.cpp`, `s4.h`)

`propagate(NamingInput) -> ElementMap` and `resolve(TopoRef) -> Resolved` follow the header shapes. History comes only from `Modified()`, `Generated()`, `IsDeleted()` of the makers, normalised to `HistoryEntry` with `SubId` indices (`kernelx.cpp`). No BRepGraph.

- Rule 1 (sweeps): side face = (feature, `*.side`, [sketch entity]); caps = (feature, `*.startcap/endcap`, [loop]). Edges and vertices of the prism from the faces that meet there.
- Rule 2 (history): `Modified` 1:1 keeps the name; 1:n gives (feature, `split`, [input name]) plus a `Split` lineage event; n:1 gives the result all input names; `Generated` gives (feature, role, [source names]).
- Rule 3 (no history): edge and vertex from the names of their adjacent faces; face from its named edges.
- Rule 4: ordinal among siblings by quantised fingerprint (centroid, area, axis).
- Resolver: name lookup, then lineage walk (`Split`), then `Repaired` by fingerprint, else `Broken`.

Three rule sets were run on identical scenarios:

| set | content |
|-|-|
| **v0** | 3.3 as written. Two things 3.3 does not specify had to be decided even here: an element produced by a merge carries all input names, and a reference to such a name resolves as a new state `Merged`. |
| **v1** | v0 + A1 (sibling count is part of the hashed name) + A2 (a name that has siblings is trusted only while the stored fingerprint still matches, else `Broken`) + A8 (`Repaired` also needs the stored neighbour names to resolve to exactly the faces around the candidate). |
| **v2** | v1 + A5 (family fallback: a lost name whose family now has more members resolves `Split` to all of them; a lost piece name whose family is whole again resolves `Merged`) + A4 (same, through the parent of a `split` name) + A7 (siblings are told apart by the names of the faces around them, ordinal only if that fails) + A3 (infer edge history from face history; never fired, see 7). |

Deviations from `naming.h` in the prototype: 64-bit FNV hash with collision abort instead of `Hash256`; string feature ids; `TopoRef` carries all names of the element, their `TopoName` records and the neighbour names; `ElementMap` allows several names per element; extra `ResolveState::Merged`; `serialize`, `nameImported`, `rematchImported` not written.

### 3.5 Ground truth (`truth.cpp`), independent of the naming code

The oracle never sees a name, a history entry or an index from another build. It knows:

1. the script and its current parameters. From them it derives, for every logical face of every feature (each sketch entity's side face, each cap, each blend), an **analytic carrier** (plane, cylinder, torus or cone with its parameters and the side the material is on) and an **analytic extent** (segment range x sweep height, arc angle range, inside of the profile loop, neighbourhood of the blended edge). Blend carriers are computed in closed form from the two parent carriers and the radius or distance.
2. the geometry of a built shape: mesh nodes and area-weighted interior samples of faces, 16 points per edge, vertex points.

Identity of a baseline element = the owner(s) whose carrier all its nodes lie on (1e-4 mm, surface type and material side must agree) and whose extent covers it; if a logical face is in several pieces, also on which side of which other bounding carrier the piece lies. An edge is described by the descriptions of its two faces, a vertex by all its faces. After an edit the carriers are recomputed from the new parameters and the elements of the new shape that satisfy the description are the expected answer `E` (possibly empty, several, or flagged "merged" when a matching face extends beyond the description). Verdicts:

| resolver | expected | verdict |
|-|-|-|
| `Broken` | `E` empty | broken and really gone (correct) |
| `Broken` | `E` not empty | missed (safe loss) |
| elements X | X = E | correct (but `Exact` on an element the oracle calls merged = wrong: unflagged merge) |
| elements X | X strict subset of E | **wrong: incomplete set** |
| elements X | `E` empty | **wrong: ghost** |
| elements X | otherwise | **wrong: different element** |

Checks on the oracle itself:

- **Self-check at baseline**: a reference is only recorded if its description selects exactly that element on the baseline. All 141 faces passed; 74 references needed piece separators.
- **Mutation test**: in every scenario every correct single answer was replaced by the nearest other element of the same kind and geometry type and re-judged. 28,235 of 28,235 mutants were flagged wrong. Every correct `Split` answer with one piece removed was flagged (33 of 33). For the weak-oracle references 2,994 of 3,013 (99.4 %).
- **Congruence cross-check**: if exactly one element of the new shape has the reference's pick-time fingerprint in place, the oracle must name it. It disagreed 8 times (v0; 3 in v1/v2 where the model differs), all in scenario P23; in all of them the congruent element is a different logical element (section 6, cause C).
- **No-op rebuild** N00 resolves 677 of 677 `Exact` in all rule sets.

"Weak-oracle" references (65): three bspline pieces of the cross-hole/bore intersection and their vertices, which no analytic carrier separates, and vertices that lie on only two faces (seam vertices of hole rims). For these the oracle knows only the set of pieces of the logical element; it checks that the answer is inside that set and, when a unique congruent element exists, that an `Exact` answer is that element.

## 4. Numbers

### 4.1 Overall (47 scenarios x 677 references = 31,819 resolutions, plus 1,966 feature-internal picks)

| rule set | Exact | Split | Merged | Repaired | Broken | of Broken: really gone / missed | **WRONG (refs + picks)** |
|-|-|-|-|-|-|-|-|
| v0, 3.3 as written | 98.0 % (31,183) | 0.1 % (34) | 0.0 % (8) | 0.2 % (73) | 1.6 % (521) | 460 / 61 | **16 + 1 = 17** |
| v1, safety only | 97.5 % (31,025) | 0.1 % (34) | 0.0 % (8) | 0.1 % (18) | 2.3 % (734) | 483 / 251 | **0** |
| v2, amended | 97.8 % (31,127) | 0.1 % (40) | 0.2 % (62) | 0.1 % (18) | 1.8 % (572) | 483 / 89 | **0** |

Counts in brackets are reconstructed from the per-scenario percentages; they sum to 31,819 in each row.
Wrong by type in v0: different element 2, incomplete set 6, ghost 8 + 1 pick, unflagged merge 0.
Feature-internal picks: 99.1 % `Exact`, 0.1 % `Split`, 0.8 % `Broken` (all really gone) in every rule set; v0 has the one wrong `Repaired` pick.

PLAN 3.3 acceptance set (18 scenarios: D01, D01h, D02, D35, D03 to D09, K31 to K34, O19, O20, O22; 12,186 resolutions):

| rule set | Exact | Split | Merged | Repaired | Broken | really gone / missed | WRONG |
|-|-|-|-|-|-|-|-|
| v0 | 99.5 % | 0.0 % | 0.0 % | 0.0 % | 0.5 % | 58 / 3 | 0 |
| v1 | 98.8 % | 0.0 % | 0.0 % | 0.0 % | 1.1 % | 58 / 82 | 0 |
| v2 | 99.4 % | 0.0 % | 0.1 % | 0.0 % | 0.6 % | 58 / 10 | 0 |

Among references whose element still exists, the lowest per-scenario rate of `Exact`+`Split`+`Merged` is 97.0 % (v0, P23), 96.3 % (v1, D02), 96.9 % (v2, P23). The 95 % bar of 3.3 is met in every scenario by every rule set.

By kind (v2): faces 98.4 % Exact, 6 missed, 0 wrong of 6,627; edges 97.7 %, 47 missed, 0 wrong of 14,899; vertices 97.7 %, 36 missed, 0 wrong of 10,293.

### 4.2 Per scenario

E/S/M/R/B = percent Exact / Split / Merged / Repaired / Broken of 677. "gone/missed" splits the Broken count.

| id | edit | v0 E/S/M/R/B | v0 wrong | v1 missed | v2 E/S/M/R/B | v2 gone/missed | v2 wrong |
|-|-|-|-|-|-|-|-|
| N00 | no-op rebuild | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D01 | F01 base: L 160->172 | 99.1/0/0/0/0.9 | 0 | 11 | 98.8/0/0/0/1.2 | 6/2 | 0 |
| D01h | F01 base: H 30->34 | 100/0/0/0/0 | 0 | 15 | 100/0/0/0/0 | 0/0 | 0 |
| D02 | F02 corner fillet r 6->9 | 99.0/0/0/0.3/0.7 | 0 | 22 | 98.1/0/1.0/0.3/0.6 | 2/2 | 0 |
| D35 | F35 bottom chamfer d 1->1.5 | 100/0/0/0/0 | 0 | 22 | 99.4/0/0/0/0.6 | 0/4 | 0 |
| D03 | F03 boss: x1 62->66, h 14->17 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D04 | F04 round boss: r 20->21, h 10->12 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D05 | F05 step: wall y 88->86.5, depth 8->10 | 100/0/0/0/0 | 0 | 2 | 100/0/0/0/0 | 0/0 | 0 |
| D06 | F06 pocket A: x0 72->69, depth 14->17 | 100/0/0/0/0 | 0 | 1 | 100/0/0/0/0 | 0/0 | 0 |
| D07 | F07 pocket B: three dimensions | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D08 | F08 pocket corner fillet r 4->6 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D09 | F09 slot: r 5->6, x1 132->136 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| D10 | F10 holes: dia 9->9.6, one hole moved | 100/0/0/0/0 | 0 | 11 | 99.4/0/0/0/0.6 | 0/4 | 0 |
| D10t | F10 holes dia 9->10: two holes tangent to the side (degenerate; result fails BRepCheck) | 99.1/0.9/0/0/0 | **4** | 15 | 96.9/1.5/0/0/1.6 | 0/11 | 0 |
| B36 | F10 holes 3, 4 moved so they break out of the side | 97.5/0.6/0/0/1.9 | **2** | 17 | 96.6/0.9/1.6/0/0.9 | 2/4 | 0 |
| M12 | move a hole (two holes) | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| R13 | F20 pocket floor fillet r 2->3 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| R13b | F15 boss corner fillet r 5->7 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| A14 | add early: through notch that splits the x=L side | 96.9/0.3/0/2.4/0.4 | 0 | 9 | 96.9/0.3/0/1.5/1.3 | 0/9 | 0 |
| A15 | add early: blind pocket | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| A16 | add at end: groove across the boss top (splits faces, edges) | 98.7/1.3/0/0/0 | 0 | 0 | 98.7/1.3/0/0/0 | 0/0 | 0 |
| X16 | delete F09 slot | 94.1/0/0/0/5.9 | 0 | 0 | 94.1/0/0/0/5.9 | 40/0 | 0 |
| X17 | delete F24 island | 98.4/0/0/0/1.6 | 0 | 0 | 98.4/0/0/0/1.6 | 11/0 | 0 |
| X18 | delete F21 notch (face pieces re-join) | 93.1/0/0/0/6.9 | 0 | 10 | 93.1/0/1.5/0/5.5 | 37/0 | 0 |
| X19 | delete F38 channel (edge pieces re-join, merged floor un-merges) | 95.9/0.1/0/0/4.0 | 0 | 8 | 95.9/0.1/1.2/0/2.8 | 19/0 | 0 |
| X20 | delete F40 groove (face pieces re-join) | 96.0/0/0/0/4.0 | 0 | 6 | 96.0/0/0.9/0/3.1 | 21/0 | 0 |
| O19 | reorder: swap F06 and F07 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| O20 | reorder: swap F09 and F10 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| O21 | reorder: F31 to just after F01 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| O22 | reorder: swap F03 and F04 | 100/0/0/0/0 | 0 | 0 | 100/0/0/0/0 | 0/0 | 0 |
| O23 | reorder: F21 notch (a splitting cut) to just after F02 | 97.3/0/0/2.7/0 | 0 | 16 | 97.3/0/0/0.3/2.4 | 0/16 | 0 |
| P23 | sketch moved: slot breaks through the y=0 side | 94.8/0/0/3.4/1.8 | **9** | 20 | 92.6/0/0/0/7.4 | 30/20 | 0 |
| P24 | sketch changed: pocket C extended through the step wall | 96.6/0.4/0/1.2/1.8 | 0 | 15 | 96.6/0.4/0.6/0/2.4 | 5/11 | 0 |
| U25 | un-merge: channel depth 6->5 | 99.0/0.1/0/0/0.9 | 0 | 6 | 99.0/0.1/0.6/0/0.3 | 0/2 | 0 |
| G25 | merge: pocket A floor coplanar with channel and pocket C floors | 98.7/0.6/0.3/0/0.4 | 0 | 0 | 98.7/0.6/0.3/0/0.4 | 3/0 | 0 |
| G26 | sketch changed: two pocket sides become collinear and merge | 98.2/0/0.9/0.4/0.4 | 0 | 0 | 98.2/0/0.9/0.4/0.4 | 3/0 | 0 |
| G27 | lug lowered flush with the bottom | 99.6/0.4/0/0/0 | 0 | 0 | 99.6/0.4/0/0/0 | 0/0 | 0 |
| Z28 | suppress F15 | 91.1/0/0/0/8.9 | 0 | 0 | 91.1/0/0/0/8.9 | 60/0 | 0 |
| Z29 | suppress F02 | 86.4/0/0/0/13.6 | 0 | 16 | 86.1/0/0.6/0/13.3 | 90/0 | 0 |
| Z30 | suppress F06 pocket A | 85.5/0/0/0/14.5 | 0 | 0 | 85.5/0/0/0/14.5 | 98/0 | 0 |
| K31 | sketch line added: F03 corner cut | 97.8/0/0/0.3/1.9 | 0 | 2 | 97.8/0/0/0/2.2 | 13/2 | 0 |
| K32 | sketch line added: F01 corner cut | 96.6/0/0/0/3.4 | 0 | 7 | 96.6/0/0/0/3.4 | 23/0 | 0 |
| K33 | sketch line added: F07 corner cut | 98.7/0/0/0/1.3 | 0 | 0 | 98.7/0/0/0/1.3 | 9/0 | 0 |
| K34 | sketch lines added: F05 step wall jog | 99.3/0/0/0/0.7 | 0 | 0 | 99.3/0/0/0/0.7 | 5/0 | 0 |
| V34 | ordinal attack: cut added early beside a split face | 99.7/0.1/0/0.1/0 | **2** | 3 | 99.7/0.1/0/0.1/0 | 0/0 | 0 |
| V35 | ordinal attack 2: groove moved up | 100/0/0/0/0 | 0 | 6 | 100/0/0/0/0 | 0/0 | 0 |
| C35 | combined edit of six parameters | 99.1/0/0/0/0.9 | 0 | 11 | 98.8/0/0/0/1.2 | 6/2 | 0 |

Features that failed during rebuilds: X16 F39, X17 F26, X18 F22, Z30 F20 and F26 have no resolvable edge because their edges are gone (correct failure). P23 F39 fails in v1/v2 because its picked edge is gone; in v0 it was `Repaired` to the wrong edge and built. D10t: `BRepCheck` invalid from F37 on.

### 4.3 By the rule that produced the reference's name (v2, all scenarios)

| name came from | resolutions | Exact | Split | Merged | Repaired | Broken: gone / missed | wrong (v0) | wrong (v2) |
|-|-|-|-|-|-|-|-|-|
| rule 1: sweep side face | 2,679 | 98.8 % | 0.3 % | 0.1 % | 0 | 22 / 0 | 0 | 0 |
| rule 1: sweep cap face | 893 | 99.3 % | 0.1 % | 0.1 % | 0 | 4 / 0 | 0 | 0 |
| rule 2: `Generated()` blend face | 2,632 | 98.1 % | 0.2 % | 0 | 0 | 43 / 2 | 0 | 0 |
| rule 2: `Generated()` section edge | 2,350 | 96.1 % | 0.3 % | 0.3 % | 0.2 % | 47 / 24 | 7 | 0 |
| rule 2: `Generated()` section vertex | 2,491 | 96.2 % | 0 | 0.3 % | 0.1 % | 61 / 22 | 2 | 0 |
| rule 2: split piece, edge | 893 | 94.4 % | 0 | 2.9 % | 0 | 11 / 13 | 0 | 0 |
| rule 2: split piece, face | 376 | 95.2 % | 0 | 3.2 % | 0.5 % | 0 / 4 | 2 | 0 |
| rule 3: neighbour-named edge, sweep tool | 3,807 | 98.7 % | 0.2 % | 0.1 % | 0 | 37 / 0 | 2 | 0 |
| rule 3: neighbour-named vertex, sweep tool | 1,504 | 99.1 % | 0 | 0 | 0 | 14 / 0 | 0 | 0 |
| rule 3: neighbour-named edge, after fillet/chamfer | 7,849 | 98.0 % | 0.1 % | 0 | 0.1 % | 134 / 10 | 1 | 0 |
| rule 3: neighbour-named vertex, after fillet/chamfer | 6,298 | 98.0 % | 0 | 0 | 0.1 % | 110 / 14 | 2 | 0 |
| merged element (several names) | 47 | 91.5 % | 6.4 % | 2.1 % | 0 | 0 / 0 | 0 | 0 |

The v0 wrongs in the rule 3 rows are all `Repaired`-by-fingerprint errors (cause C), not errors of neighbour naming.

## 5. How the neighbour-naming fallback (3.3 rule 3) performed

It carries more than half of all names: on the baseline 207 prism edges and 138 prism vertices (by design), and 191 edges and 154 vertices after fillets and chamfers (no history exists for them, section 7). 19,458 resolutions of such names: 98.2 % `Exact`, 0 wrong by name in any rule set, 24 missed in v2 (4 in v0). Faces never needed it: every result face of every boolean, fillet and chamfer had `Modified` or `Generated` history. Its weakness is fragility, not wrong answers: a neighbour-named element's name contains the names of its faces, so when a neighbouring face is split or renamed the element's name changes although the element did not (A14, O23, P23: 9 to 20 missed references each). `Repaired` recovers part of that.

## 6. The wrong cases in v0, and what fixes each

All 17 are listed in `results.md`. Three causes.

**A. Ordinal by fingerprint sort swaps siblings (2 wrong, silent, V34).** F40 splits the x=0 side face into an upper and a lower piece: `(F40, split, [side], 0/2)` and `1/2`, ordered by centroid. Adding an unrelated shallow cut early in the tree removes 120 mm2 from the upper piece; its centroid y moves from 48.58 to 51.34, past the lower piece's 50.00. The ordinals swap. Both references resolve `Exact` to the other piece. Nothing in 3.3 detects this. Fix that worked: A7, tell siblings apart by the names of the faces that bound them (here the two groove walls) and use an ordinal only among siblings that are still indistinguishable; plus A1+A2, a name that depends on an ordinal is trusted only while the stored fingerprint still matches. V34 then resolves 99.7 % `Exact`, 0 wrong. In v2 only 7 of 677 references still depend on an ordinal (among them the bspline intersection pieces).

**B. One element becomes several siblings; the old name is the name of one of them (6 wrong, silent, B36 and D10t).** A hole rim edge is `(F10, hole.gFE, [face, wall], 0)`. When the hole is moved so it breaks out of the part, the rim is in two arcs: OCCT reports both as `Generated` from the same two faces, they become ordinals 0 and 1 of the same key, and because 3.3 does not put the sibling count into the name, ordinal 0 hashes to the old name. The reference resolves `Exact` to one arc; the other is silently dropped (no `Split` lineage exists, because the edge was never "modified", it was generated differently). Same in the tangent case D10t. Fix that worked: A1 (sibling count in the hash, so the old name disappears) and A5 (family rule: a lost name whose family now has several members is `Split` and returns all of them). B36 then gives `Split` with both arcs, 0 wrong.

**C. `Repaired` by fingerprint picks a different element that sits where the old one was (8 wrong + 1 feature pick, flagged Repaired, P23).** The slot was moved by exactly its own width, so its other wall now lies in the plane where the picked wall was. Edges and vertices of the new wall are congruent in place with the old ones (same length, centroid, type, neighbour count) and are the unique fingerprint match. The resolver returned them as `Repaired`; the F39 chamfer then chamfered the wrong rim. Faces were not affected only because the face fingerprint contains the outward normal. 8 of the 73 `Repaired` reference answers in v0 were wrong (11 %), plus the pick. Fix that worked: A8, a repair candidate must also be surrounded by exactly the faces that the neighbour names stored at pick time resolve to. That left 18 `Repaired` answers, all correct. A further change is recommended but is not a prototype result: a `Repaired` reference must never be consumed by a feature rebuild without confirmation; 3.3 currently says it is "used".

No rule change is known that keeps v0's lower miss count and also removes these cases; the safe variants pay with `Broken` (v0 61 missed, v2 89, v1 251).

## 7. OCCT 8.0.1 history: what was missing, incomplete or wrong

Minimal probes (`s4.exe -h`, one operation on a 40 x 30 x 20 box; full table in `probes.txt`) and the audit of all 39 modifying operations of the baseline build:

| operation | finding | repro |
|-|-|-|
| `BRepFilletAPI_MakeFillet` | Faces complete: neighbours `Modified` 1:1, blend `Generated` from the edge, corner patch `Generated` from the vertex. Trimmed neighbour edges ARE reported `Modified` (the inference rule A3 never fired on 17 blend features). **Missing:** the boundary edges of the blend and their vertices have no ancestor at all: 4 edges + 4 vertices per blend (13 edges + 9 vertices for a three-edge corner). The end vertices of the blended edge are `IsDeleted` with nothing generated. | fillet one vertical edge of the box, r=5: result 15 edges, 4 without history; 10 vertices, 4 without history |
| `BRepFilletAPI_MakeChamfer` | Identical pattern to fillet. | chamfer the same edge, d=3 |
| fillet/chamfer tangent propagation | Edges OCCT adds to the contour by tangent continuity get `Generated` blend faces like picked edges. History complete. Consequence for the feature model: the blended set is larger than the picked set. | F20, F35, F39 of the part |
| `BRepAlgoAPI_Cut`/`Fuse` | Complete on everything tested: `Modified` 1:1 and 1:n (splits), section edges `Generated` from the two faces, new vertices `Generated` from edges/faces, deletions reported. 0 input elements vanished without `IsDeleted`; 0 `Modified` results on a different surface or curve type in 22 booleans. **Incomplete in one sense:** when one logical intersection is produced in several pieces, each piece is `Generated` from the same parents and nothing says they are pieces of one thing (cause B). | through hole; through notch splitting a side face; two boxes side by side |
| `SimplifyResult()` after a boolean | History is merged correctly: two coplanar faces both report `Modified` to the one unified face, removed edges and vertices `IsDeleted`. | fuse of two boxes with 4 coplanar face pairs: 4 result faces with 2 `Modified` ancestors each |
| `ShapeUpgrade_UnifySameDomain` standalone | Same, via `History()` (`BRepTools_History`). | same shape |
| `BRepPrimAPI_MakePrism` | `Generated(edge)` -> side face, `Generated(vertex)` -> side edge, `FirstShape`/`LastShape` -> caps: complete. Edges and vertices of the end cap are reachable only via `LastShape` of the profile sub-shape; the prototype names them from faces instead. | prism of a rectangle |
| `MakeChamfer` robustness (not history) | The bottom-perimeter chamfer (tangent chain of 4 lines and 4 arcs) returned "not done" for d = 0.8 and 1.0 and a `BRepCheck`-invalid solid for d = 0.5 once the through holes and slot existed in the bottom face; with those features absent it built and was valid at d = 1.0. Cause not investigated. The feature was moved to third in the tree to get a valid part. | `s4.exe -x` |
| `BRepAlgoAPI_Cut` with a tangent tool (not history) | A hole exactly tangent to a side face (D10t) gives a result that `BRepCheck_Analyzer` rejects four features later. History for it splits the rim into two arcs at the tangent point. | scenario D10t |
| seams | A cylinder that meets another cylinder (cross hole into bore) gives 3 bspline section edges for one logical closed curve, cut where the seams are. All three are `Generated` from the same two faces. Which piece is which is an artefact of seam position; only an ordinal distinguishes them. | F23 of the part |

Nothing was found that is plainly wrong (a `Modified` or `Generated` pointing at an unrelated element).

## 8. Cost

| | measured |
|-|-|
| `propagate`, mean per feature (prism tool + boolean, or blend) | 0.22 ms (v0 228 us, v2 215 us) |
| `propagate`, max per feature | 0.46 ms (v0), 0.55 ms (v2) |
| `propagate`, whole 40-feature build | 9 ms, against 620 to 770 ms spent inside the OCCT operations (1.2 to 1.4 %) |
| by operation (mean per call, v2) | prism 0.03 ms, fuse 0.12 ms, cut 0.20 ms, fillet 0.22 ms, chamfer 0.23 ms |
| final `ElementMap` | 708 names for 707 elements; 25.6 kB if stored as `Hash256` -> `SubId` |
| dictionary of every `TopoName` record issued during the build | 1,014 records, 133 kB with `Hash256` parents (v0 132 kB) |
| lineage events for one build | 308 |

Sizes are computed, not serialised. Not measured: `resolve` on its own, the fingerprint scan that `Repaired` needs (one `BRepGProp` per element of that kind, only when a name is lost), and anything on a part with thousands of faces. Extracting the history (`Modified`/`Generated`/`IsDeleted` for every input sub-shape) is inside the OCCT figure, not the propagate figure.

## 9. How this result could be wrong

The 0 is only as good as the oracle. Known ways it can be fooled, and what was done:

1. **It shares the script's idea of identity.** "The same logical element" means the same feature and sketch entity (or cap, or blend target). Naming rule 1 uses the same idea, so an identity error both sides share is invisible. Example not tested: a sketch edit that inverts a rectangle so that two tagged lines swap places; both would follow the tags. Whether the user agrees is a UX question the spike cannot answer.
2. **Coincident carriers.** Found twice during development, both times as false "wrong" reports, both fixed in the oracle and stated here because the opposite error is equally possible: (a) the plane chosen to tell two pieces apart was an unrelated hole floor at the same height as the groove wall; separators must now touch the piece inside the owner's extent. (b) the hidden side of the lug, back to back with the plate side on the same plane, was counted as a piece of the plate side; face carriers are now compared with the material side. Edges and vertices still have no orientation check: two different edges on the same two carriers inside both extents are "pieces of one logical edge" to the oracle. P23 shows the oracle does separate the two walls of a moved slot, because they belong to different sketch entities.
3. **A third oracle bug fixed during development:** deleting the feature that separated two pieces made the oracle call every element next to a piece "merged". Separators of deleted features now keep their baseline geometry.
4. **Seam vertices and seam-induced pieces (65 references) are only weakly checked.** A wrong piece of the right logical edge would pass unless a congruent element contradicts it (19 of 3,013 mutants passed). Seam vertices were moved into this class after they produced 2 "wrong" verdicts in the degenerate tangent scenario; the reasoning is that a vertex on two faces is only "some point of that curve", but it is a judgment call that removed 2 counted errors.
5. **Tolerances and sampling.** Faces count as part of a region from 2 % area overlap and as fully inside from 98 %. A sliver below 2 % of a merged face is ignored. Interior samples are triangle centroids (2.5 mm grid), tolerance 0.06 mm off the carrier for curved faces; nodes are checked at 1e-4 mm.
6. **Only analytic carriers.** Plane, cylinder, torus, cone. The part was designed to stay inside that set; fillets on non-perpendicular cylinder/plane pairs, variable fillets and sculpted faces are not covered by the oracle at all.
7. **Selection bias in scenarios.** One author wrote the part, the naming code, the oracle and the scenarios. V34, V35 and B36 were written to break the design; P23 broke it by accident. Scenarios nobody thought of are not covered. 100 % `Exact` on most dimension changes is what a structural name should give, and the mutation test shows the oracle is not vacuous, but it is one part.
8. The oracle never checks the 30 seam edges, and it cannot judge faces it could not label; on this part that was 0 faces.

## 10. Recommendation

**The section 3.3 design is sound in structure** (names from origin, role and parents; history first, neighbours second; four-state resolution; loud failure) and OCCT 8.0.1 history is good enough to carry it for extrude, cut, fuse with unification, hole, fillet and chamfer. **It is not safe as written.** Change the following before any feature is built on it.

### Changes to 3.3

1. **Rule 4 (ordinal by fingerprint) must not be the primary tie-break.** Replace with: siblings (same origin, role, parents) are first told apart by a topological discriminator, the sorted names of the faces around the element that are not among its parents (for a face: adjacent faces; for an edge: faces at its end vertices), preferring faces made by the current feature. The fingerprint ordinal is the last resort among siblings with identical discriminators, and such a name is marked ambiguous.
2. **The sibling count is part of the hashed name.** A name issued alone and a name issued as one of n must differ.
3. **Family rule in resolution.** Keep, per build, family -> member names, where family = hash(kind, origin, role, parents). A lost name whose family now has more members resolves `Split` to all members; a lost piece name whose family has one member, or whose `split` parent exists again, resolves `Merged`.
4. **An ambiguous name is trusted only while the stored fingerprint still matches**, otherwise `Broken`.
5. **Merges need a state.** State what a merged element is called (prototype: it carries every input name) and add `Merged` to the resolution states: the element contains what was picked plus more. Treat like `Split` for consumers (fillet may proceed, CAM asks).
6. **`Repaired` needs adjacency by name, not by count**, and must not be consumed by a feature rebuild or a posted CAM operation until confirmed. 3.3 says a repaired reference is "used, but flagged"; for a feature input that builds geometry on a guess (it happened in P23).
7. Say explicitly that cap faces are named from a **stable region (loop) id** and hole walls from a **stable position id**, and make that a requirement on the sketch and hole features. The spike assumed both.
8. Lineage "kept so a reference valid three edits ago can be walked forward" was not needed: names are rebuilt deterministically on every rebuild and the lineage of the current build was enough. Either drop the cross-rebuild claim or give it its own test.

### Changes to `naming.h`

```cpp
struct TopoName {
    Hash256 id;                         // hash of everything below except family
    kernel::SubKind kind;
    FeatureId origin;
    std::string role;
    std::vector<Hash256> parents;
    std::vector<Hash256> discriminator; // NEW: names of surrounding faces that tell siblings apart; may be empty
    std::uint32_t ordinal;              // last resort, among siblings with equal discriminator
    std::uint32_t siblingCount;         // NEW: hashed; > 1 means "ambiguous, verify the hint"
    Hash256 family;                     // NEW: hash(kind, origin, role, parents)
};
class ElementMap {
    // CHANGED: an element has one or more names (several after a merge)
    std::span<const Hash256> namesOf(kernel::SubId) const;
    std::span<const Hash256> family(const Hash256& familyId) const;   // NEW: members issued in this build
    const TopoName* record(const Hash256& nameId) const;              // NEW
};
struct TopoRef {
    BodyId body;
    kernel::SubKind kind;
    std::vector<Hash256> names;          // CHANGED: all names of the picked element
    std::vector<Hash256> families;       // NEW, parallel to names; plus siblingCount / discriminator-present flag
    std::vector<Hash256> splitParents;   // NEW: parent of each name whose role is "split"
    std::vector<Hash256> neighbourNames; // NEW: names of the faces around the element at pick time (for Repaired)
    Fingerprint hint;
};
enum class ResolveState { Exact, Split, Merged /* NEW */, Repaired, Broken };
```

- `Fingerprint::neighbourCount` is not enough adjacency for `Repaired`; keep it only as a filter.
- Quantised fingerprints must be compared with a tolerance (the prototype used 1e-3 mm on centroid, 1e-7 relative on area); exact equality on values rounded to 1e-4 mm will flip at rounding boundaries. Not measured; stated as a design caution.
- `LineageEvent::Merged` was recorded but never needed by the resolver once elements carry all names; `Deleted` was used only to word the reason of a `Broken`.

### Changes to `kernel.h`

- `ShapeHistory` must be normalised by the kernel: an unchanged sub-shape appears as `Modified` to itself; results that are not in the final shape are dropped; `SimplifyResult` history is already folded in by OCCT.
- `SweepFaceOrigin` needs a loop/region id for caps (it only has `curveTag`).
- `fillet`/`chamfer` should return which input edges OCCT actually blended (tangent propagation), so the feature can show it; naming does not need it.

### Residual risk

- One part, one author, 47 scenarios. Structural identity held; the three failure causes were found only because scenarios were written to look for them. Others of the same family probably exist (for example siblings that swap discriminators after a sketch inversion; point 1 of section 9).
- The amended rules buy safety with `Broken`: 89 references (0.28 %) were lost although their element exists, mostly generated or neighbour-named edges and vertices next to a face that got split or renamed. CAM users will see "reference broken" on some edits that left their element untouched. `Repaired` with the neighbour check recovers little (18 cases).
- Seam-dependent pieces (bspline intersections of two cylinders) can only be told apart by ordinal. With the amendments they go `Broken` whenever their geometry changes. Acceptable, but any CAM reference to such an edge is fragile.
- OCCT version changes can change which edges are split at seams. The design does not depend on list order, but it does depend on the number of pieces.
- Blend faces are named from the edge that was blended. If that edge's name is itself fragile (neighbour-named after an earlier fillet), the blend face name is too. Blends on blends were tested only two deep (F16 on F15).

## 11. What was not tested

- Imports and rule 5 (`nameImported`, `rematchImported`), and every direct-edit operation (`delete_face`, `offset_face`, `resize_hole`, `move_face`). No history-free solid was named.
- Revolve, draft, shell, patterns, mirror, split body, transforms, multi-body documents, datum planes on `TopoRef`s, sketches placed on a referenced face.
- Booleans without `SimplifyResult`. Non-analytic geometry beyond one cylinder/cylinder intersection. Variable or setback fillets, asymmetric chamfers.
- Serialisation of `ElementMap`, references stored across sessions, `Hash256`, determinism across threads, machines, compilers or OCCT versions. Determinism was checked only as a second build in the same process.
- Cross-rebuild lineage. Stale references older than the baseline.
- Sketch-level identity: that entity tags and loop ids survive trims, splits and constraint solves was assumed.
- Performance beyond 141 faces.
- User judgment of whether `Split`, `Merged` and `Repaired` answers are acceptable in practice.

## 12. Acceptance items

| # | item | result |
|-|-|-|
| 1 | Prototype `propagate` and resolver per `naming.h`, deviations noted | done (3.4, 10) |
| 2 | 40-feature scripted part, at least 200 references | done: 40 features, 677 references + 42 feature picks |
| 3 | Every 3.3 edit scenario plus the listed ones, rebuild, resolve all | done: 47 scenarios, 33,785 resolutions per rule set |
| 4 | Ground truth independent of naming | done, with stated limits (3.5, 9); 28,235 of 28,235 mutants caught |
| 5 | Percent per scenario and overall, wrong count 0 | **FAIL for 3.3 as written: 17 wrong** (0 on 3.3's own acceptance set). 0 wrong with the amended rules |
| 6 | OCCT history gaps with repro, fallback performance | done (5, 7) |
| 7 | Cost | done (8) |
