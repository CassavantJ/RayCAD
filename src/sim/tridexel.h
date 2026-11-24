// CPU tri-dexel stock model (task T-070): makeTriDexelCpu (include/ray/sim.h states what every IStockModel does; this
// file says how this one does it, what a consumer can rely on beyond the contract, and gives test hooks).
//
// GRIDS. The stock box `bounds` is divided into nx x ny x nz cells, the cell snapped per axis to extent / n with
// n = ceil(extent / cellMm), so the box is spanned exactly (S6: +2 % volume error without). Three grids of rays pass
// through cell centres: Z rays indexed (y, x), X rays (z, y), Y rays (z, x). A ray (a dexel) holds the material on it
// as sorted, disjoint intervals of float depths measured from bounds.min along the ray; two intervals inline in a
// 20-byte dexel, more in its strip's overflow pool. Parts left shorter than kSliverMm are dropped.
//
// STRIPS AND THREADS. Each grid is split into strips of whole rows: kStripMm of y for the Z grid, kLayerStripMm of z
// (layers) for the X and Y grids, so that each layer's slice of a move is worked out once. A batch first lists, per
// strip, the moves whose swept tool reaches it, in program order;
// then the strips run in parallel (IJobSystem::parallelChunks, one chunk per strip), each applying its moves in order
// to its own dexels only. A dexel's contents therefore depend only on the moves in order: not on the thread count,
// the batch size, the strip width or how moves are split across calls. Per-move results are merged in integers.
//
// THE CUT. compileShape (toolshape.h) turns the cutting profile into convex pieces; every ray meets a piece swept along
// a straight move in one interval, computed in closed form, which is subtracted from the dexel. Arcs are split into
// chords as buildBackplot does (backplot.h), at a sagitta of kArcTolCells cells. Removed volume per move: the lengths
// removed from the Z rays, each rounded to 2^-20 mm, times the cell's area.
//
// NON-CUTTING PART. Shank and holder (ToolShape::shankProfile, holderProfile, the last continuing upward) never cut.
// cutBatch's nonCuttingHits and intersects() look along the Z rays under the swept non-cutting part for material
// overlapping it by more than kContactMm (so the flute top touching what the flutes just left is no contact).
//
// SNAPSHOTS AND CLONES. Strips are reference counted and copied on write: snapshot() and clone() copy the strip
// pointers only (microseconds); the first later cut into a shared strip copies that strip (two vector copies). A
// snapshot therefore holds on to the strips that change after it; in the worst case it is a full copy.
//
// DISPLAY MESH. Tiles are kTileCells x kTileCells Z rays. A tile is dirty when a Z ray in it lost material; restore()
// marks the tiles of every Z strip it replaces. toMesh meshes columns (one Z ray's cell, or a block of 2^k x 2^k of
// them inside one tile, their intervals united): a top and a bottom face per interval, side walls where neighbouring
// columns differ (and against nothing at the box edge), the vertical edges split at every height met there, so every
// edge pairs up exactly. Top faces get a normal tilted by the neighbours' heights (shading only; the geometry stays
// in steps of one column). Blocks are chosen as the finest that fits maxTriangles. toSolid is the same at one cell,
// welded. Vertices are floats in the stock's frame.
#pragma once

#include "toolshape.h"

#include <ray/sim.h>

#include <cstdint>

namespace ray::sim::detail {

inline constexpr double kStripMm = 2.0;           // Z grid strips: rows of about this many mm of y
inline constexpr double kLayerStripMm = 0.5;      // X and Y grid strips: layers of about this many mm of z
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
    int tilesX = 0, tilesY = 0;
};
TriDexelInfo triDexelInfo(const IStockModel&);
// FNV-1a over every dexel's count and float bits, all three grids in order. Equal stock, equal hash.
std::uint64_t triDexelHash(const IStockModel&);
// The bytes of strips held by `snap` that the model no longer shares (what keeping that keyframe costs now).
std::uint64_t snapshotPrivateBytes(const IStockModel&, const IStockModel::Snapshot& snap);
// Triangles of the whole stock's display mesh with columns of `step` x `step` cells (1: full resolution), counted
// without building it.
std::uint64_t triDexelMeshTriangles(const IStockModel&, int step);

} // namespace ray::sim::detail
