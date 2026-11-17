# S5 Adaptive roughing: port, adapt or rewrite

## 1. Question

Should RayCAD's 2D adaptive clearing (`adaptive2d`) be a port of FreeCAD's `Adaptive.cpp`, an adaptation of it, or a rewrite, and what acceptance numbers should the product algorithm meet?

## 2. Sources and versions

| Item | Version | Notes |
|-|-|-|
| FreeCAD release | tag `1.1.4`, commit `4fd3bf320d9566a27e60069fc8387448aaa3a094` | `src/Mod/CAM/libarea/Adaptive.cpp`, `Adaptive.hpp`, `clipper.cpp`, `clipper.hpp`. Path is `Mod/CAM` (was `Mod/Path`). |
| FreeCAD main | commit `56062b61c7085af5af12ead4c02ce5765d0107c4` (2026-11-17) | `Adaptive.cpp`, `Adaptive.hpp` only |
| Clipper2 | tag `Clipper2_2.0.1`, commit `21ebba05db8894f0c7217ad35ea518080f324946`, from `PolyClipping/Clipper2` (`AngusJ/Clipper2` no longer exists) | BSL-1.0 |
| Compiler | MSVC 19.51 (toolset 14.51), `/O2 /fp:precise`, C++20, Ninja | via `spikes\build.cmd s5-adaptive` |

**Clipper version finding.** The plan's research said FreeCAD's Adaptive is on Clipper2. That is true only for `main`. Release 1.1.4 includes `clipper.hpp` version 6.4.2 (Clipper 1). Tag `26.3rc2` includes both headers. The Clipper2 port landed on main about 2026-11-03 (merge of branch `adaptive_clipper2`) and has had fixes since (latest touching commit 2026-11-03, "handle pancakes in the output of SimplifyPaths"). FreeCAD main links `Clipper2::Clipper2Z`, so Clipper2 must be built with `USINGZ`; the code stores per-vertex tags in Z.

Both versions were built and measured:

- **main**: `Adaptive.cpp` from main against Clipper2 2.0.1 with `USINGZ`. Zero source changes.
- **114**: `Adaptive.cpp` from 1.1.4 with its bundled Clipper 6.4.2. Zero source changes.

Licences: `Adaptive.cpp/hpp` are `LGPL-2.1-or-later`, copyright 2018 Kresimir Tusek. Clipper 6.4.2 and Clipper2 are BSL-1.0.

## 3. Method

**Machine.** Core i9-13900K (24 cores, 32 threads), 64 GB RAM, Windows 11. High tier. All timings are single-threaded wall time, median of 5 runs in one process.

**Driver** (`src/gen.cpp`). Calls `Adaptive2d::Execute` directly. Parameters: tool diameter 12.7 mm, `stepOverFactor` 0.2 and 0.1, `tolerance` 0.1 (FreeCAD default), `stockToLeave` 0, `finishingProfile` true, `forceInsideOut` false, `keepToolDownDistRatio` 3, clearing inside, helix diameter 9.525 mm (75% of tool; main: `helixRampTargetDiameter`, minimum left at its default of tool/8 = 1.59 mm; 1.1.4: `helixRampDiameter`).

**Pockets** (`src/cases.h`, mm, arcs as polylines with 1 um chord error):

| id | Pocket |
|-|-|
| p1 | BP1-style closed rectangle 63.5 x 38.1, corner radius 6.35 (equal to tool radius) |
| p2 | BP2-style closed circle, diameter 38.1, in 82.55 square stock |
| p3 | 90 x 60 R8 with two islands (circle D14, rectangle 16 x 10 R2) |
| p4 | Narrow slot 14.5 x 70, full-radius ends (1.8 mm wider than the tool) |
| p5 | Open pocket 50 x 30 R8, one side on the stock edge |
| p6 | Thin wall: two 40 x 30 R7 pockets separated by a 1 mm wall |
| p4b-p4f | Extra slots 13.2, 15.0, 16.0, 19.05, 23.0 mm wide |
| p7 | Extra, run time only: 152.4 x 101.6 R8 with two islands |

**Checker** (`src/check.cpp`), independent: it shares only the pocket definitions with the driver and uses no Adaptive or Clipper code.

- Raster stock model, 0.02 mm cells. Cells are air, pocket material, or keep-out material (walls, islands).
- The cutter disc is swept along every cutting move and every tool-down link move in steps of at most one cell. Retract links are not swept; their XY length is reported. The helix is swept as one circle of the tool centre around the reported helix centre and is not in the histogram.
- Where a path does not start at the end of the previous one (it happens on most passes, 0.1-0.25 mm, up to 2.3 mm on p3), the implied straight move is swept with that path's motion type.
- Engagement angle at each step: 1440 points on a circle of radius r minus 0.016 mm at the cutter position, counted when in material that the sweep has not removed up to 0.45 mm of path behind the cutter. Weighted by step length.
- Target angle for load fraction s is acos(1 - 2s): 53.13 deg at 20%, 36.87 deg at 10%.
- Gouge: analytic distance from each cutter centre to the closed pocket edges (edges on the stock outline excluded), reported as r minus the minimum distance; also the area of keep-out cells removed.
- Uncut: pocket cells left, restricted to the area the cutter can reach (morphological opening by the cutter, from two exact Euclidean distance transforms), and the largest thickness left.

**Checker self-test** (`s5_check selftest`):

| Case | Expected deg | Measured median |
|-|-|-|
| Full slot | 180.00 | 179.25 |
| Straight, ae 6.35 | 90.00 | 91.00 |
| Straight, ae 2.54 | 53.13 | 52.75 |
| Straight, ae 1.27 | 36.87 | 37.75 |
| Straight, ae 0.635 | 25.84 | 26.00 |
| Straight, ae 0.127 | 11.48 | 12.25 |
| Inside arc, ae 2.54 | 65.27 | 66.50 (range 64.75-68.00) |
| Inside arc, ae 1.27 | 46.78 | 48.00 (range 46.50-49.25) |

So the checker reads within about 1 deg on straight passes and about +1.2 deg (scatter 2 deg either way) on arcs. Differences of less than about 3 deg between target and measurement are inside checker error.

**Determinism tests.** (a) 20 fresh processes per pocket, SHA-256 of the written toolpath file. (b) 20 repeated runs in one process, FNV-1a hash of all output doubles. (c) 2, 8 and 32 threads each running its own `Adaptive2d` instance over all pockets, 3 repeats. The code has no threads of its own.

## 4. Results: the six pockets

Columns: time is `Execute` wall time. "> 1.1x" is percent of cut length with engagement above 1.1 times target. "air" is percent of cut-flagged length with engagement below 2 deg. "uncut" is reachable pocket area not removed. Gouge is the analytic value.

### 4.1 FreeCAD main + Clipper2 2.0.1, 20% load (target 53.13 deg)

| case | points | time ms | cut mm | link-down mm | retract mm | helix mm | median | p95 | p99 | max | % > target | % > 1.1x | % > 1.5x | % > t+10 deg | % air | uncut mm2 | gouge mm |
|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|
| p1 | 2352 | 741 | 1107.3 | 353.1 | 0 | 29.9 | 53.2 | 55.5 | 56.8 | 95.8 | 55.1 | 0.56 | 0.09 | 0.27 | 8.8 | 0.000 | 0.0000 |
| p2 | 943 | 263 | 439.1 | 1.3 | 0 | 29.9 | 53.5 | 55.5 | 56.2 | 61.5 | 54.9 | 0.11 | 0.00 | 0.00 | 1.6 | 0.000 | 0.0057 |
| p3 | 9127 | 4257 | 2870.5 | 1121.6 | 0 | 29.9 | 53.5 | 57.0 | 58.8 | 87.5 | 54.0 | 1.54 | 0.09 | 0.30 | 15.4 | 0.000 | 0.0072 |
| p4 | 0 | 5 | - | - | - | - | - | - | - | - | - | - | - | - | - | all (963.5) | - |
| p5 | 1663 | 362 | 799.6 | 602.2 | 0 | 0 | 52.8 | 55.0 | 56.2 | 58.0 | 41.5 | 0.00 | 0.00 | 0.00 | 10.9 | 0.000 | 0.0085 |
| p6 | 2479 | 755 | 962.3 | 240.7 | 29.9 | 59.8 | 53.5 | 55.8 | 57.2 | 63.2 | 53.4 | 0.63 | 0.00 | 0.00 | 11.1 | 0.000 | 0.0045 |

p4 printed "Start point not found!" and returned one region with no paths and the `StartPointNotFound` flag set.

Histogram, percent of cut length per 10 deg bin (0-10, 10-20, ..., 60-70, 70-80, 80-90; all higher bins 0.0):

- p1: 11.3 15.0 1.1 1.7 2.8 67.6 0.3 0.1 0.0
- p2: 2.8 27.1 3.2 2.1 2.1 62.6 0.1 0.0 0.0
- p3: 23.0 10.9 1.6 2.0 2.0 60.1 0.2 0.2 0.1
- p5: 13.5 10.6 1.3 1.6 2.8 70.2 0.0 0.0 0.0
- p6: 14.2 18.0 2.2 3.0 3.9 58.3 0.4 0.0 0.0

"Percent above target" sits near 50% because the algorithm regulates to the target, so the median is the target. The 1.1x and 1.5x columns are the useful ones.

The 10-20 deg bin is mostly the finishing profile pass, which cuts a skin of one tenth of the stepover (expected angle about 16 deg at 20% load).

Tool-down link moves: no engagement on p1, p2, p5, p6. On p3, 0.04% of link length was engaged, up to 81 deg, removing 1.28 mm2. That is a link move cutting material.

### 4.2 FreeCAD main + Clipper2 2.0.1, 10% load (target 36.87 deg)

| case | points | time ms | cut mm | link-down mm | median | p95 | p99 | max | % > 1.1x | % > 1.5x | % > t+10 deg | % air | uncut mm2 | gouge mm |
|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|
| p1 | 4599 | 1647 | 1902.8 | 720.9 | 37.5 | 39.2 | 41.0 | 95.0 | 1.29 | 0.10 | 0.28 | 5.9 | 0.000 | 0.0000 |
| p2 | 1430 | 206 | 703.2 | 0.0 | 37.5 | 39.2 | 39.8 | 45.0 | 0.10 | 0.00 | 0.00 | 0.4 | 0.000 | 0.0053 |
| p3 | 17086 | 7363 | 4687.8 | 2486.0 | 38.0 | 41.5 | 44.5 | 66.8 | 9.05 | 0.07 | 0.41 | 10.2 | 0.000 | 0.0074 |
| p4 | 12304 | 2721 | 1222.5 | 266.3 | 40.5 | 52.8 | 54.5 | 58.0 | 50.00 | 0.40 | 33.21 | 35.4 | 0.000 | 0.0043 |
| p5 | 2955 | 471 | 1378.1 | 1138.8 | 37.0 | 39.0 | 40.5 | 44.2 | 0.94 | 0.00 | 0.00 | 6.7 | 0.000 | 0.0069 |
| p6 | 4908 | 865 | 1585.0 | 472.2 | 37.8 | 40.0 | 42.8 | 50.0 | 2.97 | 0.00 | 0.12 | 8.1 | 0.000 | 0.0045 |

At 10% load 1.1x target is only 3.7 deg above target, which is close to checker error on arcs; the "t+10 deg" column is more reliable here. The slot p4 does run at 10% load because the finishing allowance is smaller and a 1.59 mm helix just fits.

### 4.3 FreeCAD 1.1.4 + Clipper 6.4.2, 20% load

| case | points | time ms | cut mm | link-down mm | retract mm | median | p95 | p99 | max | % > 1.1x | % > t+10 deg | % air | uncut mm2 (max thickness) | gouge mm |
|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|
| p1 | 1526 | 163 | 1044.8 | 396.0 | 0 | 53.0 | 57.8 | 65.0 | 71.5 | 4.35 | 1.59 | 5.1 | 4.85 (0.21 mm) | 0.0000 |
| p2 | 517 | 597 | 421.5 | 0.0 | 0 | 52.8 | 56.2 | 59.0 | 62.5 | 1.42 | 0.00 | 0.4 | 0.000 | 0.0118 |
| p3 | 5557 | 1095 | 2650.5 | 1353.1 | 41.0 | 53.0 | 61.5 | 70.0 | 83.5 | 9.20 | 3.51 | 10.7 | 0.000 | 0.0369 |
| p4 | 0 | 2 | - | - | - | - | - | - | - | - | - | - | all | - |
| p5 | 992 | 112 | 609.2 | 256.0 | 0 | 53.0 | 59.2 | 66.5 | 71.5 | 6.18 | 2.18 | 6.6 | 21.25 (1.72 mm) | 0.0062 |
| p6 | 1538 | 194 | 899.9 | 293.7 | 28.6 | 52.8 | 59.5 | 66.2 | 73.5 | 6.83 | 2.20 | 7.0 | 0.000 | 0.0000 |

These rows come from the fifth run in one process; 1.1.4 gives slightly different output on each in-process run (section 5), so point counts differ by a few from a first run. p4 returned an empty list with no flag. On p5, 1.1.4 kept the tool inside the pocket outline, used a helix, and left material at the open edge.

At 10% load 1.1.4 gave: p99 43.5-53.8 deg, max 47.2-74.2 deg, 3.8-14.1% above 1.1x, uncut 4.2 mm2 on p1 and 20.9 mm2 on p5, p4 no output.

### 4.4 Slot width sweep (extras), main, 20% load

| width mm | result | median | p99 | max | % > 1.1x | % air | time ms | points |
|-|-|-|-|-|-|-|-|-|
| 13.2 | no toolpath (start point not found) | - | - | - | - | - | 7 | 0 |
| 14.5 (p4) | no toolpath (start point not found) | - | - | - | - | - | 5 | 0 |
| 15.0 | runs, helix length 6.4 mm | 10.5 | 66.5 | 71.8 | 35.6 | 41.0 | 2753 | 5873 |
| 16.0 | runs | 19.2 | 63.5 | 66.5 | 26.9 | 33.3 | 1688 | 4580 |
| 19.05 | runs | 53.0 | 59.0 | 61.0 | 3.3 | 22.4 | 985 | 2943 |
| 23.0 | runs | 53.5 | 57.5 | 59.2 | 0.04 | 15.4 | 722 | 2356 |

Main needs slot width of at least tool diameter + minimum helix diameter (tool/8) + twice the finishing allowance (stepover/10): 14.54 mm at 20% load. Below that there is no fallback (no ramp, no plunge). In slots that do run, engagement never approached full slotting (max 72 deg), but a third of the cut length is above 1.1x target and a third to 40% is air.

1.1.4 produced no toolpath for 13.2 to 19.05 mm at either load; it ran at 23.0 mm.

### 4.5 Tolerance sweep, main, p2, 20% load (single runs)

| tolerance | grid units per mm | time ms | points | gouge mm | p99 | max |
|-|-|-|-|-|-|-|
| 0.1 | 480 | 156 | 943 | 0.0057 | 56.2 | 61.5 |
| 0.03 | 1600 | 892 | 2052 | 0.0017 | 55.8 | 59.0 |
| 0.01 | 4800 | 5311 | 4717 | 0.0007 | 55.8 | 59.2 |

Gouge scales with the grid; run time rises about 34x for a 10x finer grid.

### 4.6 Larger pocket (p7, 152.4 x 101.6 with two islands), 20% load, single run

main 4181 ms, 14877 points. 1.1.4 1133 ms, 9968 points. Not checked with the sweep checker.

## 5. Determinism

| Test | main | 1.1.4 |
|-|-|-|
| 20 fresh processes, file SHA-256, p1 p2 p3 p4d p5 p6 | identical 20/20 on all | identical 20/20 on all (including p4 empty output) |
| 20 fresh processes, p4 (failed start) | **20 different files** | identical (empty list) |
| 20 runs in one process | identical on all pockets with a path; p4b gave 3 distinct | **p1, p3, p5, p6, p4f: 20 distinct out of 20**; p2 identical |
| 2 concurrent instances x 3 | identical where a path exists; p4, p4b differ | p1, p3, p5, p6, p4f: 3 distinct (one per repeat) |
| 8 concurrent instances x 3 | identical where a path exists; p4, p4b differ | same as above |
| 32 concurrent instances x 3 | **did not terminate; rerun crashed** (below) | first attempt printed no result (exit status not captured); rerun completed, same 3-distinct pattern |
| Cross-OS | not measured | not measured |

Causes found by reading the code:

- **main, failed start:** `AdaptiveOutput` is returned with `HelixCenterPoint`, `StartPoint`, `ReturnMotionType` and `ClearedArea` uninitialised. The dump showed `ReturnMotionType = 4112`.
- **1.1.4, in-process:** `Interpolation` uses `rand()` (line 1176) and never reseeds, so the sequence carries on from the previous run. MSVC keeps `rand` state per thread, which is why concurrent threads agree with each other per repeat.
- **Both:** `clock()` decides behaviour. `ResolveLinkPath` gives up after `max(keepToolDownDistRatio, 3) / 6` seconds of wall time (0.5 s here) and then falls back to a different link. The progress callback is also clock-driven. None of the runs up to 8 threads hit the limit, but a slow or loaded machine can, and the output would then differ.

**32-instance failure on main (finding, not diagnosed).** `s5_gen_main mt 0.2 32 3` (32 threads, each with its own `Adaptive2d`, each running p1, p2, p3, p4, p5, p6, p4b, p4c, p4d, p4e, p4f at 20% load, tolerance 0.1, three times) did not terminate. It was killed after about 2 h 40 min with about 10,400 CPU-seconds on one core and 7 MB of memory, so one thread was spinning and the other 31 had finished. That build had no per-pocket logging, so the pocket is not known. A rerun with logging and a 5-minute limit (and p7 added) crashed with a segmentation fault during the second repeat, while threads were in p4f and p7. The same pockets run to completion single-threaded and at 2 and 8 threads. Candidates, not confirmed: the file-scope `PerfCounter` globals that every instance writes without locking, and the wall-clock limits firing under oversubscription and taking a path that does not converge. Either way the code as it stands is not safe to run as parallel jobs.

## 6. Cancel and progress

`Execute` takes `std::function<bool(TPaths)>`; returning true sets `stopProcessing`. The callback is called at most every 0.1 s of `clock()` time, plus forced calls. Measured with a callback that cancels on its first call (single runs):

- main: first callback after 2.3-10.1 ms; return after cancel 0.6-43.8 ms (worst p2 43.8 ms, p3 21.2 ms).
- 1.1.4: return after cancel 0.2-2.4 ms. On pockets that fail to start, the callback is never called.

The callback passes the progress paths by value (a copy each call). There is no way to cancel between callbacks; a cancel requested later in the run waits up to 0.1 s plus the time to reach the next check.

## 7. Code facts

| Fact | 1.1.4 | main |
|-|-|-|
| Lines, `Adaptive.cpp` / `.hpp` | 3375 / 224 | 4360 / 295 |
| Non-blank, non-comment lines in `.cpp` | 2866 | 3362 |
| Polygon library | Clipper 6.4.2 (bundled, 5673 lines) | Clipper2, `USINGZ` required |
| Geometry type | Integer polygons for clipping and offsets; doubles for directions, angles, areas, distances | Same, and the cut-area calculation is now done in doubles with circle geometry (comment at line 1197) |
| Integer grid | `16 / tolerance` units per mm | `48 / tolerance / min(1, stepover x diameter)` units per mm; 480 at the settings used (2.08 um) |
| `long` used for scaled coordinates | 93 lines mention `long` | 93 lines. `long` is 32-bit on Windows and 64-bit on Linux and macOS. Input conversion is `long(x * scaleFactor)`: truncation, not rounding, and a different overflow point per OS (at tolerance 0.01 with a small tool the grid reaches 16000 units/mm, which overflows at 134 mm on Windows) |
| Global state | 10 file-scope `PerfCounter` objects written by every instance; `rand()` | 9 file-scope `PerfCounter` objects; no `rand()` |
| Clock use | 11 `clock()` calls; link time limit and progress interval | Same |
| `<cmath>` trig calls (sin, cos, acos, asin, atan2) | 10 lines | 15 lines; none go through a fixed math library |
| Console output | 17 `cout` lines, always on | 12 `cout`/`cerr` lines, always on |
| Threads | none | none |
| Unordered or pointer-keyed containers | none found | none found (`std::map`, `std::set` with value keys) |
| Output | Polylines of `double` pairs per motion type; no arcs | Same; plus failure flags and cleared area per region |
| Rest machining input | none | `clearedPaths` argument (already-cleared area) |
| Open pocket from outside stock | not with these inputs | yes |
| Error reporting | prints text, returns empty list | prints text, sets flags, returns region with uninitialised fields |
| Tests upstream | not checked | not checked |

Ease of making it deterministic and cancellable (judgement, labelled as such): the changes are local. Replace the `clock()` limits with iteration counts, remove or make per-instance the `PerfCounter` globals, change `long` to `int64_t` and truncation to rounding, initialise the output struct, route trig through `ray::dmath`, replace `cout` with warnings, and poll a cancel token in the inner step loop. The step-search loop in `ProcessPolyNode` is one function of several hundred lines with many tuned constants; that part is hard to change safely without a checker like the one in this spike.

## 8. Recommendation: adapt

**Adapt FreeCAD main's `Adaptive.cpp` (the Clipper2 version, pinned at commit 56062b61 or later), as a modified fork inside `src/cam/ops/adaptive2d`, with the LGPL-2.1-or-later header and attribution kept. Do not vendor it unmodified. Do not use the 1.1.4 version.**

Reasons, from the measurements:

1. **The control quality of main is good and would be costly to reproduce.** On five pockets at 20% load the median is within 0.4 deg of target, p99 within 5.7 deg, 98.5-100% of cut length within 1.1x target, zero reachable area left uncut, and gouge under 9 um. That behaviour sits in about 3,400 lines of tuned heuristics. A rewrite inside the 4-week M4 window would start behind this.
2. **It cannot be used as is.** It fails RayCAD's determinism rules in five ways (clock-dependent branches, 32-bit `long` on Windows, unpinned trig, uninitialised outputs, shared globals), it hung and crashed with 32 concurrent instances, it emits only polylines, and it produces nothing for a narrow slot.
3. **1.1.4 is worse on every quality measure** (p99 6-17 deg above target, uncut material on two pockets, open pocket not handled, gouge up to 37 um, non-deterministic in-process) and is on the wrong Clipper. Its one advantage is speed (2-5x).
4. **Risk to state plainly.** The Clipper2 port is about two weeks old upstream, unreleased, and still receiving fixes. The 32-instance failure is undiagnosed. If that failure turns out to be in the algorithm's convergence and not in the globals or clock limits, the case for a rewrite gets stronger. Diagnosing it should be the first task of T-060.

Work list for the adaptation (each item is a measured or read fact above):

- Remove `clock()` from all decisions; use iteration and node-count limits.
- `long` to `std::int64_t`; round, do not truncate; check range and return an error.
- Remove file-scope counters; make the generator re-entrant; test 32 concurrent instances in CI with a time limit.
- Initialise all outputs; turn the seven flags into `Toolpath::warnings` or a failed result.
- Route trig through `ray::dmath`; build with `/fp:strict`; then run the cross-OS comparison.
- Cancel token polled per step; progress by fraction of area cleared.
- Narrow-slot entry: ramp along the slot axis or pre-drill, and a defined result when nothing fits.
- Fix link moves that cut material (p3) or classify them as cuts.
- Arc fitting on the output (already planned as RayCAD's own code).
- Convert from the adaptive grid to integer nanometres at the boundary; decide whether to run a finer grid near walls only, since a uniformly fine grid costs about 34x in time for 10x resolution.
- Reduce run time on pockets with islands (4.3 s today on the High-tier machine).

### Proposed acceptance numbers for the product algorithm

Measured with this spike's checker method (0.02 mm cells, lagged sweep), on the six S5 pockets, at 10% and 20% load, 12.7 mm tool. "Target" is acos(1 - 2 x load).

| Measure | Proposed limit | FreeCAD main today |
|-|-|-|
| Median engagement, pockets other than the slot | within 2 deg of target | 0.1 to 1.1 deg |
| p99 engagement, pockets other than the slot | at most target + 6 deg | +3.1 to +7.6 deg (p3 at 10% load is +7.6) |
| Cut length above target + 10 deg, pockets other than the slot | at most 0.5% | 0.00 to 0.41% |
| Cut length above 1.5x target | at most 0.1% | 0.00 to 0.10% |
| Maximum engagement outside entry moves | at most 100 deg, and the feed is reduced wherever engagement exceeds 1.25x target | 44 to 96 deg; no feed reduction exists |
| Narrow slot (14.5 mm, 12.7 tool) | must produce a toolpath; p99 at most 75 deg; never above 100 deg | no toolpath at 20% load; p99 54.5 at 10% |
| Tool-down link moves | 0 mm2 removed | 1.28 mm2 on p3 |
| Reachable area left uncut | 0.00 mm2 at checker resolution | 0.000 |
| Gouge beyond the input polygon | at most 2 um (guess at what a finer wall grid can reach; main gets 1.7 um at tolerance 0.03) | 0 to 8.5 um |
| Air cutting (cut-flagged moves under 2 deg) | at most 10% on BP1 and BP2 pockets, 15% on the others | 1.6 to 15.4% |
| Run time, High-tier machine, one thread | BP2 circle under 0.5 s; islands pocket (p3) under 3 s | 0.26 s; 4.3 s |
| Output size | arcs fitted; at most 1 move per mm of cut on average | polylines, 2.1 to 3.2 points per mm |
| Determinism | byte-identical over 20 processes, 20 in-process repeats, and 1, 2, 16 and 32 concurrent jobs; identical on Windows, Linux and macOS | passes the first two on successful outputs only; hang and crash at 32; cross-OS unknown |
| Cancel latency | under 50 ms at any point in the run | 0.6 to 44 ms when cancelled at the first callback; later cancels wait for the 0.1 s clock tick |
| Failure behaviour | defined error or warning, no uninitialised data, never a hang | uninitialised fields on failed start; one non-terminating run |

The existing T-060 line ("within +10% of target on 99.5% of cut length and never above +25%; no gouge beyond 1 um") is stricter than FreeCAD main achieves today on three counts: 98.5% not 99.5% on the islands pocket, short spikes up to +80%, and gouge up to 8.5 um. The +25% cap in particular is not met by any pocket except the open one.

## 9. What was not measured

- **Cross-OS determinism.** Only Windows was available. The code facts (32-bit `long`, `<cmath>` trig, `clock()`) make differences between Windows and Linux or macOS likely; this is a prediction from reading, not a measurement.
- **Which pocket caused the non-terminating run**, and the root cause of the hang and of the crash at 32 concurrent instances.
- **Low-tier and Medium-tier run time.** All times are from the High-tier machine.
- **Compilers other than MSVC 19.51**, and `/fp:strict` (built with `/fp:precise`).
- **Spike location.** Where in each path the maximum engagement occurs (entry, corner, link join) was not broken down; the 95.8 deg on p1 lasts under 0.1% of cut length.
- **1.1.4 with the open pocket outline extended past the stock edge**, which is how FreeCAD 1.1 users model open pockets.
- **Rest machining** through main's `clearedPaths` input, stock-to-leave, outside clearing and the profiling modes.
- **The larger pocket p7** was timed but not swept by the checker.
- **Feed, chip load, axial depth, helix ramp angle, Z moves and lifted links.** The checker is 2D only and treats tool-down links as moves at cutting depth.
- **Arc fitting** of the output and its effect on engagement.
- **BP3's 0.375 in slot** with a matching smaller tool.
- **Upstream test coverage** of `Adaptive.cpp` was not examined.
- Timings are medians of 5 in-process runs for the main tables; the tolerance sweep, p7 and cancel figures are single runs.

## 10. How to reproduce

```
cmd //c "spikes\\build.cmd s5-adaptive"
cd spikes/s5-adaptive
build/s5_check selftest
bash run_all.sh            # writes out/results_<variant>_<load>.txt and out/determinism.txt
python -I summarise.py     # markdown tables
```

`run_all.sh` includes the 32-thread test that hung; run that line under `timeout 300`. Third-party sources go in `third_party/freecad` (tag 1.1.4), `third_party/freecad-main` (commit in `COMMIT.txt`) and `third_party/clipper2` (tag `Clipper2_2.0.1`).
