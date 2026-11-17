# S6 Tri-dexel simulation: report

## Question

Is a CPU tri-dexel stock model (three orthogonal grids of depth-interval lists) fast and small enough for
material-removal simulation on a Low-tier machine (4 cores, integrated graphics, 8 GB RAM), and do the
simulation budgets in `PLAN.md` section 9 look achievable at the tier cell sizes 0.5, 0.2 and 0.05 mm?

## Short answer

- Yes for Low and Medium, with a large margin. On 4 threads the prototype does 555 000 moves/s at 0.5 mm and
  113 000 moves/s at 0.2 mm on this machine. The Low-tier estimate at 0.5 mm is about 0.6 s per 100k moves
  against a 20 s budget.
- 0.05 mm on 4 threads misses the High budget: 14.5 to 14.9 s per 100k moves against 10 s. On 8 P-cores it was
  8.8 s per 100k moves (one run, little margin).
- Memory is not the constraint at any of the three sizes: 2 MB, 13 MB and 214 MB for the stock.
- Stock state hash is identical for 1 and 4 threads, repeated runs, and batch sizes 1, 64 and 512.

## Method

Prototype: `main.cpp` (one file, standard library plus Win32 for affinity and memory counters, no third-party
code). Build: `spikes\build.cmd s6-dexel`, MSVC 19.51, `/O2 /fp:precise`, default x64 code generation (no AVX2).
Run: `build\s6dexel.exe <cellMm> [full|quick|t8] [repeats]`. Raw output is in `results-0.5.txt`,
`results-0.2.txt`, `results-0.05.txt` and `results-0.05-t8.txt`.

Stock model:

- Stock 152.4 x 101.6 x 25.4 mm. Three grids: Z rays indexed by (y, x), X rays by (y, z), Y rays by (x, z).
  Rays pass through cell centres.
- A dexel is 24 bytes: a count, a capacity and two inline intervals (float lo, hi). More than two intervals go
  to a heap block.
- The cell is snapped per axis to `extent / ceil(extent / cell)`, so the stock is spanned exactly. Without
  this, 0.5 mm on a 25.4 mm stock gave a systematic +2 % volume error on the lateral grids.
- Tools: flat end mill 12.7 mm, ball end mill 6.35 mm, axis along Z. The tool is treated as unbounded upwards
  (no flute-length limit).
- Swept volume of a linear move is exact per ray, not sampled along the move:
  flat tool = stadium cross-section of the sub-segment that reaches the ray's height; ball tool = capsule
  (swept sphere) plus the swept cylinder above it. Horizontal, sloped and vertical moves are all handled.
  One interval is subtracted per dexel per move (the cross-section is convex).
- Threading: moves are applied in batches (512 by default). Each grid is split into fixed strips of rows about
  2 mm wide. A strip applies the batch's moves in program order to its own dexels only. Worker threads take
  strips from a shared counter. The result therefore cannot depend on thread count or batch size.
- Removed volume per move comes from the Z grid, summed in fixed-point integers so it is also independent of
  thread count.
- "Exactly 4 threads": a pool of 4 workers (the calling thread waits), process pinned to logical processors
  0, 2, 4, 6 = four separate P-cores, no hyper-thread siblings, no E-cores. The 1-thread runs use the calling
  thread under the same mask.

Workload (112 092 linear moves, 140.9 m of path, mean move 1.26 mm, 31 250 sloped moves):

| Part | Tool | Moves | Description |
|-|-|-|-|
| Facing | flat 12.7 | 28 | Zigzag, 1 mm deep, 60 % stepover, moves 168 mm long |
| Adaptive-like pocket | flat 12.7 | 80 860 | 90 x 80 mm pocket, six 3 mm levels to 18 mm deep, trochoidal loops of radius 3 mm, a two-turn helical ramp per level, move length 0.5 to 2 mm |
| Ball scallop | ball 6.35 | 31 204 | 0.25 mm stepover over two wavy surfaces (one cut from the faced top, one on the pocket floor), points 0.5 to 2 mm apart, every move changes Z |

Link and retract moves are not in the workload. A move above the stock is rejected by a bounding test.

## Machine

Core i9-13900K (24 cores), 64 GB RAM, Windows 11. This is a High-tier machine.
Other agents and desktop programs were running during the measurements: total CPU load read 97 % during the
first runs and 51 % during the last. The same configuration varied by up to 1.5x between runs. The tables give
the best observed run; slower runs are listed where they matter.

## Numbers

### Throughput and memory (measured, this machine)

| Cell | Grid cells | Dexels | 1 thread | 4 threads | Time for 100k moves, 4 threads | Stock memory | Process peak working set |
|-|-|-|-|-|-|-|-|
| 0.5 mm | 305 x 204 x 51 | 88 179 | 153 000 moves/s | 555 000 moves/s | 0.18 s | 2.1 MB | 15.5 MB |
| 0.2 mm | 762 x 508 x 127 | 548 386 | 29 800 moves/s | 113 000 moves/s | 0.88 s | 13.3 MB | 38.5 MB |
| 0.05 mm | 3048 x 2032 x 508 | 8 774 176 | 1 990 moves/s | 6 700 to 6 900 moves/s | 14.5 to 14.9 s | 213.6 MB | 268.4 MB |

- Slower runs seen under load: 0.5 mm 105 000 / 290 000 moves/s (1 / 4 threads); 0.2 mm 19 500 / 58 500;
  0.05 mm 1 280 / 4 900.
- Stock memory = dexel arrays plus overflow lists (0.1, 0.7 and 12.7 MB). Peak working set is for one stock
  plus the move list, before meshing.
- Most intervals in one dexel after the workload: Z 1, X 37, Y 25 (at 0.05 mm).
- Extra point, 0.05 mm on 8 threads pinned to 8 P-cores: 11 400 moves/s, 8.8 s per 100k moves (one run).
- 4-thread speed-up over 1 thread: 3.6x, 3.8x and 3.4x to 3.5x.

Where the time goes (Z grid alone, same workload):

| Cell | Z grid only, 1 thread | Z grid only, 4 threads | Share of 1-thread tri-dexel time spent in the X and Y grids |
|-|-|-|-|
| 0.5 mm | 572 000 moves/s | 1 780 000 moves/s | about 73 % |
| 0.2 mm | 122 000 moves/s | 405 000 moves/s | about 76 % |
| 0.05 mm | 8 730 moves/s | 29 900 moves/s | about 77 % |

Batch size (4 threads, moves/s):

| Cell | Batch 512 | Batch 64 | Batch 1 (one `cut()` call per move, threaded inside) | Batch 1, 1 thread |
|-|-|-|-|-|
| 0.5 mm | 555 000 | 464 000 | 50 300 | 133 000 |
| 0.2 mm | 113 000 | 106 000 | 32 900 | 28 100 |
| 0.05 mm | 6 920 | 6 890 | 5 470 | 1 720 |

Waking four threads per move costs more than the move itself at 0.5 mm and gains almost nothing at 0.2 mm.
The 0.05 mm row comes from the earlier, more heavily loaded run.

### Low-tier estimate (not measured)

Estimate for a 4-core 2017 laptop CPU (Core i5-8250U class, 15 W). Scaling factors are assumptions, not
measurements: single thread 2.5x slower than a 13900K P-core; 4 threads 3.5x slower, because a 15 W part
lowers its clock under all-core load and has slower memory.

| Cell | 1 thread [estimate] | 4 threads [estimate] | 100k moves, 4 threads [estimate] | Budget it would be held to |
|-|-|-|-|-|
| 0.5 mm | 61 000 moves/s | 159 000 moves/s | 0.6 s | Low: 20 s |
| 0.2 mm | 11 900 moves/s | 32 000 moves/s | 3.1 s | Low: 20 s |
| 0.05 mm | 800 moves/s | 1 900 to 2 000 moves/s | 50 to 52 s | Low: 20 s (fails) |

Even if the 4-thread factor were 10x instead of 3.5x, 0.2 mm would take about 9 s per 100k moves on Low.

### What resolution fits in 2 GB

0.05 mm already fits: 214 MB for the stock, 268 MB process peak, 517 MB peak if a whole-stock display mesh is
built at full resolution. Memory scales with 1 / cell squared, so by extrapolation (not measured) 0.025 mm
needs about 0.85 GB and the 2 GB limit is reached near 0.016 mm for this stock. The rule is
`bytes = 24 x (XY + YZ + XZ areas) / cell^2` plus about 6 % for overflow lists. A 24 x 16 x 4 in stock at
0.2 mm would need about 215 MB by the same rule. Time, not memory, limits the cell size.

### Accuracy: removed volume against the analytic value (percent error)

Positions were chosen off the grid. Columns are the three grids measured independently.

| Case | Exact mm^3 | 0.5 mm: Z / X / Y | 0.2 mm: Z / X / Y | 0.05 mm: Z / X / Y |
|-|-|-|-|-|
| (a) Slot, flat, full depth, along X | 32 249.79 | -1.86 / -1.79 / -0.03 | +0.72 / +0.71 / -0.01 | -0.002 / -0.001 / -0.001 |
| (a') Same slot at 15 degrees | 32 249.79 | -0.001 / +0.011 / -0.016 | +0.016 / +0.006 / +0.003 | -0.001 / -0.000 / -0.001 |
| (b) Groove, ball, 5 mm deep, along X | 2 592.93 | +0.37 / +0.36 / +0.31 | +0.42 / +0.62 / +0.09 | +0.012 / -0.002 / +0.012 |
| (b') Same groove at 15 degrees | 2 592.93 | +0.06 / +0.32 / +0.33 | +0.005 / +0.098 / +0.102 | -0.000 / +0.013 / +0.013 |
| (c) Plunge, flat, 10 mm deep | 1 266.77 | -0.20 / -0.36 / -0.55 | +0.07 / +0.06 / -0.06 | -0.023 / -0.007 / -0.007 |
| (c') Plunge, ball, 10 mm deep | 283.18 | +1.73 / +0.87 / +0.58 | -0.27 / +0.18 / -0.11 | +0.039 / +0.016 / +0.016 |

- The worst cases are axis-aligned walls, where a whole row of rays falls just inside or just outside the cut.
  The slot is 12.7 mm wide, so one row is 3.9 % at 0.5 mm; the measured 1.9 % is within one cell layer, which
  is the tolerance in the `PLAN.md` test table.
- The value returned by the prototype's `cut()` equals the Z-grid figure.
- Whole workload, agreement of the three grids on total removed volume (155 800 mm^3): X and Y differ from Z by
  -0.06 % and -0.10 % at 0.5 mm, -0.02 % at 0.2 mm, under 0.01 % at 0.05 mm. This is the only check on sloped
  moves; there is no analytic case for them.

### Display mesh extraction

Method: heightfield from the Z dexels (top of the highest interval), one vertex per cell with a normal, two
triangles per cell. Vertical walls come out as steep triangles one cell wide. The X and Y grids are not used.

| Cell | Dirty region 20 x 20 mm | Triangles | Whole stock, 1 thread | Whole stock, 4 threads | Triangles | Mesh size |
|-|-|-|-|-|-|-|
| 0.5 mm | 0.02 ms | 3 042 | 1.3 ms | 0.9 ms | 123 424 | 2.8 MB |
| 0.2 mm | 0.25 ms | 19 602 | 7.1 ms | 4.6 ms | 771 654 | 17.7 MB |
| 0.05 mm | 3.2 ms (2.3 ms on 4 threads) | 318 402 | 116 ms | 69 ms | 12 376 914 | 283 MB |

At 0.05 mm the whole-stock mesh is 12.4 M triangles. That is far above the 500k-triangle viewport budget, so
the High tier needs decimation or a coarser display level; extraction time itself is not the problem.

### Determinism

Identical stock hash (FNV over every dexel's count and float bits, all three grids) for 1 thread, 4 threads,
repeated runs, batch sizes 1, 64 and 512, and 8 threads at 0.05 mm. Checked at all three cell sizes, with
several repeats per configuration at 0.5 and 0.2 mm and two 4-thread runs plus the 8-thread run at 0.05 mm.
A deep copy of the stock hashes the same. Same binary and same machine only.

### Rapid through stock as a query

Yes, it is cheap. The query walks the Z dexels under the swept tool, reads only, and stops at the first ray
that still has material above the tool's lowest point.

| Case | 0.5 mm | 0.2 mm | 0.05 mm |
|-|-|-|-|
| Rapid above the stock, 170 mm | under 0.01 us | under 0.01 us | 0.01 us |
| Rapid through remaining stock (three cases, all reported as hits) | 0.02 us | 0.02 us | 0.02 to 0.05 us |
| Rapid 60 mm inside the cleared pocket, no contact (full scan) | 2.8 us | 17 us | 270 us |

The miss case is the worst case and costs about the same as one Z-grid cut. The answer depends on the cell
size: material thinner than one cell between rays can be missed. It is therefore not a tier-independent
verdict.

### Clone (timeline keyframes)

Deep copy of the cut stock: 0.29 ms (2.1 MB), 3.6 ms (13.3 MB), 57.6 ms (213.6 MB).

## What was not measured

- A real Low-tier machine. All Low numbers are scaled estimates.
- Arcs (`cutArc`), general revolved tool profiles (bull nose, chamfer, drill, tapered), flute-length limit,
  shank and holder shapes.
- Stock that is not a box: `reset` from a triangle soup, `toSolid`, and a second setup where the stock has
  been re-oriented. The X and Y grids were cut and checked for volume, but nothing read them back as geometry.
- A display mesh that uses all three grids (sharp vertical walls). Only the heightfield method was timed.
- Incremental dirty-region tracking. The 20 x 20 mm region was a fixed window.
- Timeline seek end to end, keyframe spacing, and keyframe compression.
- Determinism across compilers, operating systems and CPUs with different instruction sets.
- oneTBB. The prototype uses its own four-thread pool.
- Stocks larger than 6 x 4 x 1 in. Memory is given by formula only.
- A quiet machine. Background load was 51 to 97 %.
- No optimisation was tried beyond the straightforward design (no SIMD, no skipping of the part of the swept
  volume already cleared by the previous move, no empty-layer skipping in the lateral grids).

## Recommendation

1. Keep CPU tri-dexel as the v1 backend. It meets the Low and Medium budgets with a wide margin.
2. Suggested cell sizes: Low 0.2 mm (estimated 3 s per 100k moves, 13 MB), Medium 0.1 mm (not measured;
   by interpolation about 4 s per 100k moves on this machine with 4 threads, about 55 MB), High 0.05 mm.
   If 0.5 / 0.2 / 0.05 is kept, Low and Medium still pass.
3. High at 0.05 mm on CPU is marginal: 14.5 s per 100k moves on 4 threads (fails 10 s), 8.8 s on 8 P-cores
   (passes, one run). Either require 8 or more threads for the High preset, relax the High budget to 15 s
   until the GPU backend exists, or spend optimisation effort on the lateral grids, which take about three
   quarters of the time.
4. Budgets in `PLAN.md` section 9:
   - Material-removal sim, 100k moves: Low 20 s achievable, Medium 15 s achievable, High 10 s not shown on
     4 cores.
   - Timeline seek: Low 250 ms and Medium 150 ms look achievable (clone under 4 ms, replay over 100k moves/s).
     High 100 ms at 0.05 mm does not look achievable with full-copy keyframes: each keyframe is 214 MB and
     57 ms to copy, and 100 ms of replay covers only about 700 moves. It needs delta or tiled keyframes.
   - Memory: the stock fits easily inside the Low 1.5 GB and High 4 GB totals.
5. Apply moves in batches. A per-move call that threads internally is 11x slower at 0.5 mm.
6. Snap the cell per axis so the stock is spanned exactly.
7. Display: cap or decimate the High-tier stock mesh; a full-resolution heightfield is 12.4 M triangles.

### Suggested changes to `docs/interfaces/sim.h`

- Add a batch entry point to `IStockModel`, for example
  `virtual void cutBatch(const ToolShape&, std::span<const CutMove>, std::span<double> removedMm3) = 0;`
  and have `ISimulator::run` use it. Keep `cut` for single steps.
- `StockResolution::cellMm`: document it as an upper bound that the model snaps per axis.
- `intersects`: document that the answer is resolution-dependent. `RapidIntoStock` is listed as
  tier-independent; against in-process stock that only holds if the query runs at a fixed resolution that
  does not come from the tier preset, or if the event is split into an analytic check against the initial
  stock and a resolution-labelled check against cut stock.
- `clone` for keyframes: note the size (214 MB at 0.05 mm for this stock) or add a cheaper snapshot type.
- `toMesh`: add a target triangle count or level-of-detail parameter for fine cell sizes.

### Suggested changes to `PLAN.md`

- Section 6.2: simulation cell size Low 0.2 mm, Medium 0.1 mm [interpolated], High 0.05 mm; mark the Low
  value as an estimate until a reference laptop runs it.
- Section 9: note that the High 10 s figure needs 8 or more threads on CPU, and that High timeline seek at
  0.05 mm depends on a keyframe scheme that is not a full copy.
- T-070: add batch cutting, per-axis cell snapping and fixed-strip scheduling (the source of thread-count
  independence) to the task description.
