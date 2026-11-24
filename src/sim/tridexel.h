// CPU tri-dexel stock model (task T-070): makeTriDexelCpu (include/ray/sim.h states what every IStockModel does; this
// file says how this one does it, what a consumer can rely on beyond the contract, and gives test hooks).
//
// GRIDS. The stock box `bounds` is divided into nx x ny x nz cells, the cell snapped per axis to extent / n with
// n = ceil(extent / cellMm), so the box is spanned exactly (S6: +2 % volume error without). Three grids of rays pass
// through cell centres: Z rays indexed (y, x), X rays (z, y), Y rays (z, x). A ray (a dexel) holds the material on it
// as sorted, disjoint intervals of float depths measured from bounds.min along the ray; two intervals inline in a
// 20-byte dexel, more in its tile's overflow pool. Parts left shorter than kSliverMm are dropped.
//
// STRIPS, TILES AND THREADS. Each grid is split into strips of whole rows: kStripMm of y for the Z grid, kLayerStripMm
// of z (layers) for the X and Y grids, so that each layer's slice of a move is worked out once. A strip is stored as
// tiles of kTileCols columns (its rows by kTileCols rays; the unit of copy-on-write, below); every tile belongs to
// exactly one strip. A batch first lists, per strip, the moves whose swept tool reaches it, in program order; then the
// strips run in parallel (IJobSystem::parallelChunks, one chunk per strip), each applying its moves in order to the
// dexels of its own tiles only. A dexel's contents therefore depend only on the moves in order: not on the thread
// count, the batch size, the strip or tile width or how moves are split across calls. Per-move results are merged in
// integers.
//
// THE CUT. compileShape (toolshape.h) turns the cutting profile into convex pieces; every ray meets a piece swept along
// a straight move in one interval, computed in closed form, which is subtracted from the dexel. Arcs are split into
// chords as buildBackplot does (backplot.h), at a sagitta of kArcTolCells cells. Removed volume per move: the lengths
// removed from the Z rays, each rounded to 2^-20 mm, times the cell's area. Each Z tile also keeps, per run of
// kTopRun rays in a row, the highest material top: a cut that cannot reach below it skips the run (exactly: the same
// bits as without; most of a pocket's re-sweeps of its own floor). A ray from which a move's interval would take
// nothing is recognised inline and left alone (cutsInterval; the same bits, and on S6 about a sixth less time in the
// pocket's re-sweeps, where most lateral rays are such). No call polls a cancel token (the contract passes
// none): a caller cancels between batches, whose length it chooses.
//
// FLOATING POINT. The module is built with /fp:strict on MSVC (ray_deterministic). tridexel_cut.cpp and toolshape.cpp
// switch off only FENV_ACCESS and exception semantics (precise mode: no reassociation, no contraction): the same bits
// (checked: the benchmark's stock hash is equal both ways, and Linux GCC with -ffp-contract=off gives the golden hash
// of tridexel_determinism_test.cpp), about a third faster.
//
// NON-CUTTING PART. Shank and holder (ToolShape::shankProfile, holderProfile, the last continuing upward) never cut.
// cutBatch's nonCuttingHits and intersects() look along the Z rays under the swept non-cutting part for material
// overlapping it by more than kContactMm (so the flute top touching what the flutes just left is no contact).
//
// SNAPSHOTS AND CLONES. Tiles are reference counted and copied on write: snapshot() and clone() copy the tile
// pointers only (about 9,000 at 0.05 mm on the S6 stock: tens of microseconds). A cut copies a shared tile only when
// it is about to take material from one of its rays (a ray the move leaves as it was writes nothing, so a re-sweep of
// cleared floor copies nothing). A snapshot therefore holds on to the tiles whose material changed after it; in the
// worst case (facing every ray) it is a full copy. Measured on S6 (simulator_bench_test.cpp, hidden test "Simulator
// keyframe cost diagnosis"), keyframes 24 M rays of work apart at 0.05 mm: whole strips copied on first touch pinned
// 16.3 GB over the run, 17 times the 0.97 GB of dexels that actually changed; tiles of 64 columns copied on change pin
// 1.52 GB (Z 0.78, X 0.31, Y 0.44).
//   Why results cannot depend on the threads: during a batch only the task of a tile's strip reads or writes that
// tile's dexels, and its reference (the TileRef in the grid's tile array) is replaced by that task alone (a copy, when
// shared). Whether a tile is shared is read once when the task starts (StripTiles): no one can share it during a batch
// (snapshot and clone are not called while a batch runs), and a release elsewhere can only make it unshared, which at
// worst copies it needlessly. A copy holds the same bits, and a cut that takes nothing from a ray writes nothing, so
// whether, when and on which thread a tile was copied never changes a dexel. Reference counts are atomic; the
// acquire load that finds a tile unshared pairs with the release of every other owner's last reference, so their reads
// of it happen before our writes.
//
// DISPLAY MESH. Tiles are kTileCells x kTileCells Z rays, numbered row by row (tx + ty * tilesX). A tile is dirty when
// a Z ray in it lost material; restore() marks the display tiles over every Z tile it replaces. toMesh meshes columns
// (one Z ray's cell, or a block of 2^k x 2^k of them inside one tile, their intervals united): a top and a bottom face
// per interval, side walls where neighbouring columns differ, and walls against nothing on every tile edge, the
// vertical edges split at every height met there, so every edge of a tile pairs up exactly. Each tile's triangles are
// contiguous, with one Mesh::faces range (SubId{Face, tile number}). Top faces get a normal tilted by the neighbours'
// heights (shading only; the geometry stays in steps of one column). Blocks are chosen as the finest that fits
// maxTriangles. A column mesh has more triangles than S6's heightfield (top, bottom and walls: about 3.2 M for the
// benchmark stock at 0.2 mm against 0.77 M), which the budget absorbs. toSolid is the same at one cell without tile
// caps, welded. Vertices are floats in the stock's frame.
#pragma once

#include "toolshape.h"

#include <ray/sim.h>

#include <cstdint>

namespace ray::sim::detail {

inline constexpr double kStripMm = 1.0;           // Z grid strips: rows of about this many mm of y
inline constexpr double kLayerStripMm = 0.5;      // X and Y grid strips: layers of about this many mm of z
inline constexpr int kTileCols = 64;              // copy-on-write tiles: a strip's rows by this many columns
inline constexpr double kArcTolCells = 0.1;       // chord sagitta of an arc, in cells (of the smallest cell side)
inline constexpr double kSliverMm = 1e-4;         // a part of an interval shorter than this is dropped by a cut
inline constexpr double kContactMm = 1e-3;        // overlap with the non-cutting part that counts as contact
inline constexpr int kTileCells = 64;             // display tiles, in Z rays per side
inline constexpr double kVolumeScale = 1048576.0; // removed length per Z ray is rounded to 1 / kVolumeScale mm
// Above this many dexels in all (about 4 GB) reset() coarsens the cell (doubling it) and logs a warning.
inline constexpr std::uint64_t kMaxDexels = 200'000'000;

// What tests and benchmarks read from a CPU tri-dexel model (asserting it is one).
struct TriDexelInfo {
    int nx = 0, ny = 0, nz = 0;                // cells per axis
    double cx = 0, cy = 0, cz = 0;             // cell size per axis, mm
    Vec3 origin;                               // bounds.min
    std::uint64_t dexels = 0;                  // in all three grids
    std::uint64_t intervals[3] = {0, 0, 0};    // Z, X, Y grids
    std::uint32_t maxIntervals[3] = {0, 0, 0}; // most intervals in one dexel, per grid
    double volume[3] = {0, 0, 0};              // material volume measured by each grid, mm^3
    std::uint64_t bytes = 0; // the model's own memory: dexel arrays, pools, tables (shared strips too)
    std::uint64_t strips = 0;
    std::uint64_t tiles = 0;    // copy-on-write tiles in all three grids
    int tilesX = 0, tilesY = 0; // display tiles
};
TriDexelInfo triDexelInfo(const IStockModel&);
// FNV-1a over every dexel's count and float bits, all three grids in order. Equal stock, equal hash.
std::uint64_t triDexelHash(const IStockModel&);
// The bytes of strips held by `snap` that the model no longer shares (what keeping that keyframe costs now).
std::uint64_t snapshotPrivateBytes(const IStockModel&, const IStockModel::Snapshot& snap);
// Per grid (Z, X, Y), what keeping `snap` costs against the model now and what it would at least cost: the bytes held
// only by it (snapshotPrivateBytes), and the bytes of its dexels (20 each, and their pooled intervals) whose material
// differs from the model's (what a copy on write per dexel would pin; the diagnosis of the tile size).
struct SnapshotCost {
    std::uint64_t privateBytes[3] = {0, 0, 0};
    std::uint64_t changedBytes[3] = {0, 0, 0};
    std::uint64_t changedDexels[3] = {0, 0, 0};
};
SnapshotCost snapshotCost(const IStockModel&, const IStockModel::Snapshot& snap);
// Triangles of the whole stock's display mesh with columns of `step` x `step` cells (1: full resolution), counted
// without building it.
std::uint64_t triDexelMeshTriangles(const IStockModel&, int step);

} // namespace ray::sim::detail
