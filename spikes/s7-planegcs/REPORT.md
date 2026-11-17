# S7: PlaneGCS standalone

Date: 2026-11-17. Status: complete. Windows only; Linux and macOS not built.

## Result in short

| Acceptance item | Result |
|---|---|
| 1. Builds outside FreeCAD, C++20, MSVC | **Pass** on Windows. Linux and macOS **unverified**. |
| 2. Drag solve under 16 ms for 200 entities | **Fail.** 414 to 808 ms median per step (worst 2,182 ms) when the 200 curves form one coupled group. Under 16 ms only at about 50 curves, or when the sketch splits into independent groups (0.63 ms). |
| 3. Bit-identical over 100 runs and across thread settings | **Pass with three conditions** the adapter must enforce (parameter storage order, pinned Eigen cache sizes, one set of compiler flags). Without them results change. |
| 4. Shim under 200 lines, headers listed | **Pass.** 64 lines, 4 headers. PlaneGCS sources unmodified. |
| 5. DOF, conflicting and redundant diagnosis | **Available** and exercised. Cost is 235 to 317 ms at 200 curves. |

## Question

Can FreeCAD's 2D constraint solver PlaneGCS be vendored outside FreeCAD with a small shim, is it fast enough for
interactive dragging at 200 entities (target in `docs/interfaces/sketch.h`: under 16 ms per drag step), and are its
results bit-identical from run to run?

## What was built

| Item | Source | Version | Size on disk |
|---|---|---|---|
| PlaneGCS, 11 files, 13,142 lines | github.com/FreeCAD/FreeCAD, `src/Mod/Sketcher/App/planegcs` | tag `1.1.4`, commit `4fd3bf320d9566a27e60069fc8387448aaa3a094` (2026-09-28) | 0.5 MB sources (2.9 MB with the partial clone's `.git`) |
| Eigen | gitlab.com/libeigen/eigen | tag `3.4.0`, commit `3147391d946bb4b6c68edd901f2add6ac1f31f8c` | 12 MB shallow clone; `Eigen/` itself is 6.7 MB |
| Boost headers (subset) | archives.boost.io `boost_1_92_0.tar.bz2` | 1.92.0 | 30 MB, 2,604 files |

- `1.1.4` is the newest FreeCAD release tag; `26.3rc2` exists but is a release candidate.
- FreeCAD 1.1.4's own `pixi.lock` pins Eigen 3.4.0 (and Boost 1.86.0), which is why Eigen 3.4.0 was used rather
  than 3.4.1 or 5.0.x. Eigen 5 was not tried.
- FreeCAD was fetched with `git clone --filter=blob:none --sparse --depth 1 --branch 1.1.4` and a non-cone sparse
  checkout of the one directory plus `LICENSE`.
- Boost: the release archive (199 MB) was downloaded to a temp folder, only `boost/` was extracted, and
  `tools/boost_subset.py` copied the include closure of the four headers PlaneGCS includes
  (`graph/adjacency_list.hpp`, `graph/connected_components.hpp`, `graph/graph_concepts.hpp`,
  `math/constants/constants.hpp`). The closure follows every `#include` regardless of `#if`, so 30 MB is a
  superset; it touches 75 top-level Boost libraries or headers. The archive and the extracted tree were deleted.
- Licences. PlaneGCS file headers say GNU Library GPL version 2 or later. Eigen is MPL-2.0; the build also
  succeeds with `EIGEN_MPL2_ONLY` defined (`build-mpl2`), so no non-MPL2 Eigen code is used. Boost is BSL-1.0.
- The PlaneGCS sources are not modified. `git status` in `third_party/freecad` is clean.

### Shim (acceptance item 4)

`shim/include/`, 64 lines in total including comments:

| FreeCAD header | How PlaneGCS includes it | Shim | Lines |
|---|---|---|---|
| `Base/Console.h` | `<Base/Console.h>` in `GCS.cpp` | `Base::Console().log(fmt, ...)` and `.warning(fmt, ...)`; dropped unless a sink function is set | 51 |
| `FCConfig.h` | `<FCConfig.h>` in `GCS.cpp` | empty; nothing from it is used | 2 |
| `SketcherGlobal.h` | `"../../SketcherGlobal.h"` in `GCS.h`, `Geo.h`, `Constraints.h` | defines `SketcherExport` as empty (static library) | 8 |
| `boost_graph_adjacency_list.hpp` | `<boost_graph_adjacency_list.hpp>` in `GCS.cpp` (a FreeCAD wrapper in `src/`) | includes `<boost/graph/adjacency_list.hpp>` | 3 |

The relative include `"../../SketcherGlobal.h"` is satisfied without touching the sources by adding
`shim/include/_up/_up` to the include path, so the path resolves to `shim/include/SketcherGlobal.h`. That
directory holds only a one-line placeholder file. `Base::TimeElapsed` is referenced only under
`PROFILE_DIAGNOSE`, which is not defined, so it is not stubbed.

### Build (acceptance item 1)

- Windows, MSVC 19.51 (toolset 14.51), `/std:c++20 /O2`, Ninja, via `spikes\build.cmd s7-planegcs`: builds and
  runs. Flags needed: `/bigobj`, `NOMINMAX`. Eight C4267 warnings (size_t to int) come from `GCS.cpp`.
- C++20 is required by the sources themselves (`<numbers>`, `std::ranges`).
- Linux and macOS: **not built, unverified.** WSL on this machine has no compiler and none was installed.
  Expectation, labelled as a guess: it builds, because the code is built by FreeCAD on GCC and Clang, the shim
  uses only standard C++, the include names match in case, and `CMakeLists.txt` already links
  `Threads::Threads` (needed for `std::async`). The `_up/_up` include trick relies on the directory existing on
  disk, which it does.

## Method

Benchmark source: `src/bench.cpp`. Raw output: `results/`. Scripts: `tools/run_timing.sh`,
`tools/run_determinism.sh`, `tools/build_variants.cmd`.

**Sketch.** A 5 x 4 grid of 20 units. Each unit is a rounded rectangle (4 lines, 4 arcs) plus 2 diagonal lines:
120 lines and 80 arcs, 200 curves, 1,200 solver parameters. Per unit: 8 endpoint tangents (coincident plus
angle-via-point, the recipe FreeCAD uses), 4 plain coincidents, horizontal, vertical, 2 parallel, 3 equal radius,
then radius or equal radius to the previous unit, distance or equal length to the previous unit for width and
height, and horizontal or vertical plus distance to a neighbour for position. 480 constraints plus 80 arc rules.
Every unit is linked to its neighbour, so the whole sketch is **one coupled group**. Variants:

- *Fully constrained*: 0 DOF.
- *Nearly constrained*: 5 dimensions left out, 5 DOF.
- *Loose*: no distance dimensions, 39 DOF, still one coupled group.
- *Decoupled*: every unit dimensioned and anchored on its own, so 20 independent groups; 1 DOF.

Sizes of 50, 100 and 150 curves use 1, 2 and 3 rows of the same grid.

**Full solve.** Starting geometry is the design plus pseudo-random offsets of up to 0.3 mm (0.02 rad on arc
angles). Timed: create the system and constraints, `declareUnknowns`, `initSolution` (which runs `diagnose`),
`solve` with DogLeg, `applySolution`. 100 runs.

**Drag.** Done the way FreeCAD's `Sketch::initMove` and `movePoint` do it: a temporary coincident constraint
with tag -1 between the dragged point and a target, then `solve()` and `applySolution()` per step. 100 steps of
0.05 to 0.11 mm each. Two modes: FreeCAD's default (every step solves from the drag-start state) and re-running
`initSolution` after every step. A third, "stateless" measurement rebuilds the system from coordinates for every
step, which is what the current `ISketchSolver::drag(const Sketch&, ...)` signature implies.

**Settings.** `debugMode = NoDebug`; everything else at PlaneGCS defaults (DogLeg, `maxIter` 100, convergence
1e-10, QR chosen automatically, which is sparse QR here because 1,200 parameters is above the threshold of 1,000).

**Machine.** Core i9-13900K (8 P-cores, 16 E-cores), 64 GB, Windows 11. Single-threaded code. Other spike agents
were building and running on the same machine during the measurements and the process was not pinned to a core, so
the numbers carry load noise; "worst" values especially. The 150-curve row being slower than the 200-curve row in
places is that noise or a property of the sparse QR; it was not investigated.

## Numbers

### Full solve, 200 curves (100 runs)

| Variant | Median | Worst | of which `initSolution` (diagnose), median | of which solve, median |
|---|---|---|---|---|
| Nearly constrained (5 DOF) | 644 ms | 982 ms | 317 ms | 327 ms |
| Fully constrained | 607 ms | 798 ms | 275 ms | 323 ms |
| Loose (39 DOF) | 590 ms | 740 ms | 297 ms | 286 ms |
| Decoupled (30 runs) | 499 ms | 1,421 ms | 492 ms | 4.2 ms |

All solves returned Success. Maximum constraint error after the solve is 2e-11 or less. The fully constrained
sketch came back to the intended design within 3.6e-15 mm.

### Drag step, 200 curves (100 steps each; decoupled 300)

| Case | Mode | Median | p95 | Worst |
|---|---|---|---|---|
| D1 slide the last unit (nearly constrained) | FreeCAD default | 808 ms | 1,260 ms | 2,182 ms |
| D1 | re-init every step | 413 ms | 487 ms | 742 ms |
| D2 resize unit 0, which resizes unit 1 (nearly constrained) | FreeCAD default | 414 ms | 465 ms | 501 ms |
| D2 | re-init every step | 435 ms | 531 ms | 552 ms |
| D3 pull a point that cannot move (fully constrained) | FreeCAD default | 92 ms | 107 ms | 131 ms |
| D4 slide in the loose sketch | FreeCAD default | 1,239 ms | 1,666 ms | 3,740 ms |
| D4 | re-init every step | 1,105 ms | 1,537 ms | 1,629 ms |
| D5 resize one unit in the decoupled sketch | FreeCAD default | 0.63 ms | 0.78 ms | 0.99 ms |
| D5 | re-init every step | 5.6 ms | 6.4 ms | 7.3 ms |

No step failed. Starting a drag (add the temporary constraint, `initSolution`) costs 3 to 6 ms because the
diagnosis is reused. A stateless drag step (rebuild, diagnose, solve) costs 329 to 1,383 ms median.

**The 16 ms target is missed by a factor of 25 to 80 for one coupled group of 200 curves.** It is met only when
the dragged geometry sits in a small independent group.

### Scaling with size (median / worst, FreeCAD default drag mode)

| Curves | Parameters | Full solve, nearly constrained | D1 slide step | D2 resize step | D3 blocked step |
|---|---|---|---|---|---|
| 50 | 300 | 18.7 / 24.7 ms | 8.0 / 13.3 ms | 16.1 / 28.1 ms | 3.1 / 9.0 ms |
| 100 | 600 | 147 / 202 ms | 81 / 119 ms | 54 / 75 ms | 14 / 22 ms |
| 150 | 900 | 717 / 831 ms | 269 / 349 ms | 310 / 334 ms | 46 / 106 ms |
| 200 | 1,200 | 644 / 982 ms | 808 / 2,182 ms | 414 / 501 ms | 92 / 131 ms |

Cost grows roughly with the cube of the coupled group's size (dense QR and LU inside PlaneGCS). The 16 ms line is
crossed at about 50 curves in one group. `sketch.h` counts points as entities; counted that way this sketch has
about 22 entities per unit, so "200 entities" is about 90 curves, which still gives 54 to 81 ms per step.

### Things tried to make it faster (200 curves)

| Change | Effect |
|---|---|
| AVX2 build (`/arch:AVX2`) | No gain outside noise: drag D1 502 ms, D2 320 ms median; full solve 808 ms. |
| DogLeg Gauss step `LeastNormLdlt` instead of `FullPivLU` | Solve part 265 ms instead of about 325 ms. |
| DogLeg Gauss step `LeastNormFullPivLU` | Slower: 619 ms. |
| Levenberg-Marquardt for the full solve | Slower: 732 ms. |
| BFGS for the full solve | **Failed** to converge (result Failed, error 0.84). |
| Dense QR for diagnosis instead of sparse | Slower: 965 ms instead of about 260 ms. |
| Eigen with OpenMP, 8 threads | No gain: 9.0 s wall against 8.1 s for the same workload, 41.7 s CPU against 7.4 s. |
| Drag with only the not-yet-determined parameters declared as unknowns (`getDependentParams`) | Fast when it works (D2: 0.78 ms, same end state within 7e-15; D4: 169 ms) but **the point did not move at all** in D1 at every size, in D2 at 50 curves and in D5. Not usable as is. Cause, from reading `qp_eq.cpp`: the drag solver gives up when the remaining constraints are not linearly independent, and `solve()` still returns Success. |

### Determinism (acceptance item 3)

Each run is 2 full solves plus 6 drags. Every solved coordinate along the way is compared as raw bytes.

| Test | 50 curves (6 x 100 drag steps, 18,600 doubles) | 200 curves (6 x 3 drag steps, 9,600 doubles) |
|---|---|---|
| 100 repeats in one process, same input | 0 of 100 differ | 0 of 100 differ |
| 100 repeats with the heap fragmented differently before constraints are allocated | 0 of 100 differ | 0 of 100 differ |
| 100 repeats with parameters at shuffled heap addresses (same values, same declaration order) | **100 of 100 differ**, up to 0.40 | **100 of 100 differ**, up to 0.51 |
| Separate processes, default build | identical | **two different results**, see below |
| Process pinned to 1 CPU (affects `std::async`) | identical | identical |
| `debugMode` Minimal against NoDebug | identical | identical |
| OpenMP build, `Eigen::setNbThreads` 1, 2, 8, 24 with cache sizes pinned | identical | identical |
| AVX2 build against default build | differs | differs |

Findings:

1. **Parameter addresses matter.** `System::solve(subsysA, subsysB)` sorts parameters by pointer value and several
   containers are keyed by `double*`. With the same values and the same declaration order but different memory
   addresses, every run gave a different result. With all parameters in one contiguous array, in a fixed order,
   results were identical. Constraint object addresses did not matter in this test.
2. **Eigen's detected cache sizes matter, and this CPU has two answers.** At 200 curves the same binary with the
   same input produced hash `b1762d1f6f09016d` when its thread ran on a P-core and `5283710287414692` on an
   E-core (checked with `start /affinity`, twice each). Eigen reads the cache sizes of the core it happens to be
   on at first use (L1 48 KB on P, 32 KB on E) and sizes its matrix-product blocks from them. Calling
   `Eigen::setCpuCacheSizes(...)` with fixed values at start-up made P-core, E-core and all thread counts give
   the same result. The difference does not show at 50 curves, where the matrices are too small for blocking.
   The same mechanism means different CPU models would give different bits unless the sizes are pinned.
3. **Thread count** made no difference once cache sizes were pinned. PlaneGCS calls `Eigen::setNbThreads(1)` in
   every `System` constructor; this is a process-wide Eigen setting. One earlier 24-thread run without pinned
   cache sizes gave the E-core hash; whether threads or core type caused it was not separated.
4. **PlaneGCS is not strictly single-threaded.** `diagnose()` runs one of its two QR decompositions through
   `std::async` with the default policy. Pinning the process to one CPU did not change results. It cannot be
   turned off without editing the source.
5. **Compiler flags are part of "one build".** AVX2 changes the bits.

### Diagnosis API (acceptance item 5)

Exercised in the benchmark (`== Diagnosis API ==` in `results/timing_200.txt`):

| Call | What it gave |
|---|---|
| `int System::diagnose(Algorithm)` / `dofsNumber()` | 0 for the fully constrained sketch, 5 for the nearly constrained one, 39 loose, -1 when constraints conflict |
| `getConflicting(VEC_I&)` | tags of both radius constraints after a second, different radius was put on the same arc; `solve()` then returns Failed |
| `getRedundant(VEC_I&)` | the tag of a horizontal constraint added to a line already parallel to a horizontal line; `solve()` still succeeds |
| `getPartiallyRedundant(VEC_I&)` | empty in these cases |
| `getDependentParams(VEC_pD&)`, `getDependentParamsGroups(...)` | the parameters that are still free, one group per DOF (5 groups, 216 parameters at 200 curves). The flat list contains duplicates at 50 curves (168 entries, 132 distinct) and is appended to, not reset, by `diagnose()`. |
| `hasConflicting()`, `hasRedundant()`, `hasPartiallyRedundant()`, `isEmptyDiagnoseMatrix()`, `calculateConstraintErrorByTag(int)` | present; the last one was used to check residuals |

Notes for the adapter:

- Tags are plain `int`, chosen by the caller per constraint. The adapter needs an int-to-`Uuid` table.
- Constraints with tag 0 are never reported as conflicting (FreeCAD uses tag 0 for arc rules). Negative tags are
  temporary constraints and are ignored by the diagnosis.
- `diagnose()` works on the whole sketch at once: 235 to 317 ms at 200 curves, and about 490 ms for the decoupled
  sketch, where the solve itself takes 4 ms. It reruns after any constraint is added or removed.
- Driven (reference) dimensions are supported through the `driving` argument and `declareDrivenParams`. Not
  exercised.
- `GCS.h` declares an add-function for every `ConstraintType` in `sketch.h` and has a `BSpline` curve type. Only
  coincident, horizontal, vertical, parallel, tangent (angle via point), equal length, equal radius, point
  distance, radius and fixed coordinate were exercised.

## What was not measured

- Linux and macOS builds; GCC and Clang.
- Low-tier hardware. All numbers are from a High-tier machine under load from other spikes.
- Cross-machine and cross-compiler bit identity. Only one machine and one compiler were used.
- Eigen 3.4.1 or 5.x; Boost versions other than 1.92.0.
- Splines, ellipses, symmetric, midpoint, angle, point-on-curve and driven constraints.
- Real user sketches. The test sketch is synthetic, and one fully coupled group of 200 curves is the hard case;
  how often real sketches look like that was not studied.
- Conflict detection quality on hard cases, and solver robustness for large drags or poor starting geometry.
- Memory use.
- Whether a row-independent subset of constraints would make the reduced-unknown drag reliable.
- Which part of the drag solve dominates (no profiler was run; the cubic growth is read from the table and the
  source).

## Recommendation

1. **Vendoring: go.** PlaneGCS builds unmodified with a 64-line shim, Eigen and a Boost header subset. Take the
   11 files at a pinned FreeCAD tag into `third_party/planegcs`.
2. **The 16 ms drag target for 200 entities cannot be promised with unmodified PlaneGCS.** What was measured: under
   16 ms up to roughly 50 coupled curves, 54 to 140 ms at 100, 0.4 to 1.2 s at 200. The plan needs one of:
   - a budget stated per coupled group (for example 16 ms up to 50 curves) plus drag solving on a worker thread
     that always takes the newest pointer position, so the UI stays responsive on large sketches while the
     geometry follows late;
   - a follow-up spike on making drag cheaper: a reliable reduced drag system (the naive version was 500 times
     faster when it worked and silently did nothing when it did not), or changes inside PlaneGCS, which means
     maintaining a fork;
   - a different solver. None was evaluated here.
3. **Full solve and diagnosis are not UI-thread work** at this size (0.6 s at 200 curves). `solve` already takes a
   `JobContext`; keep it on a worker.
4. **Determinism rules for the adapter**, all three needed:
   - keep all solver parameters in one contiguous array in canonical entity order, never individually allocated;
   - call `Eigen::setCpuCacheSizes` with fixed values once at start-up;
   - build the solver with one fixed set of floating-point and SIMD flags on every platform build.
   Also set `debugMode = NoDebug`, de-duplicate `getDependentParams`, and create a fresh `System` (or call
   `clear()`) rather than re-diagnosing an old one.
5. **Interface.** A drag should be a session (begin, move, end) that keeps the `System` alive; the stateless
   signature costs a rebuild and a diagnosis per step (22 to 42 ms even at 50 curves).
