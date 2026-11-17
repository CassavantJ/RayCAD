# M0 spike results

Run on 2026-11-17 on one machine: Core i9-13900K (24 cores), 64 GB, RTX 3090 and Intel UHD 770, NVMe, Windows 11.
This is a High-tier machine. All ten spikes ran at the same time, so timings carry load noise (up to 2x on some).
Each spike's own `REPORT.md` has the method, the full numbers and what was not measured.

## Verdicts

| Spike | Question | Verdict | Headline number |
|-|-|-|-|
| S1 viewport | Own renderer on Qt RHI, or OCCT viewer in Qt Quick? | **Keep own renderer.** UHD 620 bar indicated, not confirmed. No Mac result. | UHD 770, 500k triangles, glass on: 4.7 ms p95 (211 FPS). OCCT viewer 1.9x slower on the same card and API. |
| S2 start-up | Time to first frame | **Warm measured; cold and HDD not measured.** | 0.7 s warm. Half is Qt window creation, half is building the QML UI. |
| S3 STEP import | Import and tessellation speed | **Section 9 budget not reachable as written.** | 47 MiB file: about 21 s to shaded view at High tolerance, 9.7 s at Low tolerance. Healing is the largest stage. |
| S4 naming | Does section 3.3 hold on real OCCT history? | **Fails as written (17 wrong of 33,785); passes with amended rules (0 wrong).** | Amended: 97.8 % Exact, 1.8 % Broken, 0 wrong. |
| S5 adaptive | Port, adapt or rewrite FreeCAD's Adaptive | **Adapt FreeCAD main's Clipper2 version.** Not usable as is. | 98.5 to 100 % of cut length within 1.1x target engagement; no path for a narrow slot; one hang and one crash at 32 concurrent runs. |
| S6 simulation | CPU tri-dexel fast and small enough? | **Pass for Low and Medium; High misses on 4 threads.** | 100k moves: 0.18 s at 0.5 mm, 0.88 s at 0.2 mm, 14.7 s at 0.05 mm (budget 10 s). |
| S7 sketch solver | PlaneGCS outside FreeCAD | **Vendoring passes; drag speed fails.** | 64-line shim. Drag step at 200 coupled curves: 0.4 to 0.8 s against a 16 ms target. Met only up to about 50 curves. |
| S8 post engine | QuickJS-NG as sandboxed post host | **Keep, with four design changes.** | 1 M moves to 15.6 MB of G-code in 0.68 s; byte-identical; script heap 0.19 MB. |
| S9 direct edit | How often do OCCT direct edits succeed? | **Holes, pockets, resize, push/pull: 95 % or better with booleans. Fillet removal is weak.** | Whole fillet run removed: 18 of 35 on real parts. Only 11 of 50 solids were real. |
| S10 file format | Save, lock file, crash recovery | **Design holds locally; one flaw found; real network not tested.** | 1,000 checked kills, 0 bad files. Plain snapshot copy during a save: 126 of 137 corrupt. |

## What the spikes change in the plan

Ordered by how much they move the plan. None is applied to `PLAN.md` yet.

### Changes to design

1. **Persistent naming (3.3, `naming.h`, `kernel.h`).** Adopt the amended rules from S4: sibling count in the hashed
   name; siblings told apart by surrounding face names, ordinal only as a last resort; family rule for Split and Merged;
   new `Merged` state; an element may carry several names; `Repaired` requires matching neighbour names and must not
   feed a feature rebuild or a posted operation until the user confirms it. `TopoRef` grows accordingly.
2. **Sketch drag (9, `sketch.h`).** The 16 ms target for 200 entities cannot be promised with unmodified PlaneGCS.
   Make drag a session (begin, move, end), state the 16 ms budget per coupled group of about 50 curves, and run drag
   solves on a worker that always takes the newest pointer position. Full solve and diagnosis never run on the UI thread.
   A follow-up spike on a reliable reduced drag system is worth doing before M7.
3. **Adaptive roughing (2, T-060, T-061, `cam.h`).** Fork FreeCAD main's `Adaptive.cpp` and rework it: remove clock-driven
   branches and globals, fix 32-bit `long`, add a narrow-slot entry (ramp or pre-drill), fit arcs, make it re-entrant.
   First job is to diagnose the hang at 32 concurrent runs. Replace T-060's acceptance numbers with the S5 table.
   Correct the stack table: only FreeCAD's unreleased main is on Clipper2.
4. **STEP import (9, `kernel.h`).** First view at 0.2 mm / 20 degrees on every tier, then refine in the background.
   Tessellate with OCCT's `InParallel`, one mesh call at a time under a process-wide lock; one job per body crashed.
   Do not use the Delabella mesher in parallel. Cap concurrent faces to bound memory (15.6 GB seen). The model-load
   phase of a STEP read cannot be cancelled (about 30 ms per MiB): read through our own stream and abandon on cancel.
   Report faces that got no triangles.
5. **File format (4.1 to 4.3, `store.h`).** Reader snapshot copies must be guarded by a save counter in the lock file.
   Lock file: create with `CREATE_NEW`, keep the handle open, measure heartbeat age on the server clock, break by rename.
   Take the SQLite write lock at open. The `-journal` file stays beside the document all session.
6. **Post engine (`post.h`).** Compile scripts in a second context without exposing `eval`; soft heap limit in the
   interrupt handler; output-size limit; vendor patch to poll the interrupt every 100 back-edges. A hostile script can
   still overrun the CPU limit inside one native call.
7. **Direct editing (M6, T-080 to T-083, `kernel.h`).** Build resize-hole and push/pull from booleans, not
   `BRepOffset_MakeOffset` (20 % and 4 % on real parts, with stack overflows). Every modifying kernel call validates its
   result and detects no-ops. "Delete fillet" is promised only for a simple fillet or a selected blend run, with a clear
   refusal otherwise. Kernel rewrites tool-face history as `Modified` so references survive a resize.
8. **Simulation (6.2, 9, `sim.h`, T-070).** Batch cut entry point (11x faster than one call per move); per-axis cell
   snapping; Low can move to 0.2 mm; High at 0.05 mm needs 8 or more threads or a 15 s budget until the GPU backend
   exists; High timeline keyframes cannot be full copies; `RapidIntoStock` needs a resolution-independent check.
9. **Rendering (2.3, 6, `render.h`).** Fallback (a) is a configuration switch (the same code ran on Qt's OpenGL backend).
   `detectHardware()` needs DXGI. Define the software-rasteriser case (WARP gives 22 FPS). `blurredBackdropTexture()`
   needs an ownership rule; `PickHit::point` needs a depth read-back.
10. **Start-up (9, build).** Delay-load all OCCT DLLs. Create the UI lazily. Split the budget into warm and cold rows.
    Drop `qmltc` (does not build with Qt Quick Controls in Qt 6.12.0). Stay on Direct3D 11 on Windows.

### Budgets in section 9 that need new numbers

| Row | Today | Evidence |
|-|-|-|
| Open 50 MB STEP to shaded view | 10 / 7 / 5 s | 9.7 s on High at the Low tolerance; 21 s at High tolerance. Synthetic file. |
| Sketch drag solve, 200 entities | 16 ms | 400 to 800 ms for one coupled group. |
| Material-removal sim, 100k moves, High | 10 s | 14.7 s on 4 threads, 8.8 s on 8. |
| Timeline seek, High | 100 ms | Not reachable with full-copy keyframes (57 ms per copy). |
| Adaptive, pocket with islands | none | 4.3 s on the High-tier machine today. |
| Cold start | 3.0 / 2.0 / 1.5 s | Still a guess. Warm is 0.7 s on High. |
| Import peak memory | 4 GB (High total) | 15.6 GB seen with 32 mesh threads. |

## Not done, and what it needs

| Gap | Needs |
|-|-|
| Anything on a Low-tier machine (S1, S2, S3, S6 give estimates only) | A UHD 620-class laptop |
| S1 on macOS (Metal) | A Mac |
| S1 option B on the integrated GPU | A per-app GPU preference change in Windows; steps in the S1 report |
| S2 cold start | Reboot, then `powershell -ExecutionPolicy Bypass -File M:\RayCAD\spikes\s2-startup\bench.ps1 -Cold` |
| S10 on a real network share, power loss, cable pull | A writable folder on the shop file server or a NAS, about 1 GB |
| S3 and S9 on real customer-size files | Real 20 to 100 MB STEP files and 20 or more real job-shop parts |
| Cross-OS determinism (S5, S7, S8) and Linux/macOS builds (S7) | CI on the three operating systems (task T-002 onward) |
| S5 cause of the hang and crash at 32 concurrent runs | First step of T-060 |
| S4 beyond one part: imports, direct edits, revolve, patterns | Follow-up within the naming tasks |

## Housekeeping

- Agents could not write their own `REPORT.md` (the harness returns subagent findings as text). Six reports
  (S2, S5, S6, S7, S8, S10) were saved by the lead from the returned text. S1, S3 and S9 produced the file themselves
  by writing it under another name and renaming or generating it.
- Dependencies live outside the repo: OpenCASCADE 8.0.1 in `M:\dev\vcpkg`, Qt 6.12.0 in `M:\Qt\6.12.0\msvc2022_64`.
- `third_party/`, `build/` and `data/` folders inside spikes are not committed; each report records tags and commits.
