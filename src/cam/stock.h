// In-process stock (task T-075), private to src/cam and its tests: the stock an earlier setup leaves (cam.h
// CarriedStock), put into a later setup's frame, and its slices (ResolvedGeometry::stockSlices).
//
// WHO DOES WHAT. src/cam cannot run the simulator (src/sim depends on src/cam, PLAN.md 1.1). The engine (engine.cpp)
// generates the earlier setup's toolpaths, asks CamEngineOptions::simulateSetup (set by the app, src/api/cam_sim.cpp)
// for the stock at its end, keeps the result (cache below), and hands it to setup resolution (setup.cpp), which puts it
// into the later setup's frame (orientCarried) and slices it (carriedSlices). Everything here is a pure function of its
// arguments: no tier, no thread count, no clock reaches it.
//
// RE-ORIENTATION. A vertex p of the carried soup (in the earlier setup's WCS) goes to the model frame by that setup's
// modelFromWcs M, then into the later setup's orientation O by the rows R of its axes: q = R (M p), each step as
// written (IEEE arithmetic, no contraction), so the same stock gives the same bits. A flip (180 degrees about X: the
// bottom face as the Z face) negates Y and Z, exactly. Translation to the later WCS (the origin) comes after its box.
//
// THE BOX (stockBounds) is the box of the oriented soup, each side then:
//   - on the earlier setup's stock box (the box its simulation spanned), when it lies within kCarriedSnapMm of it: the
//     float vertices of the soup (sim.h toSolid) are within a few micrometres of that box, and a side the earlier setup
//     never cut is that box exactly, so a stock-corner origin on an uncut face is exact;
//   - else, when that side does NOT lie across the earlier setup's Z (its axis is not the earlier Z, either way),
//     moved out by one cell (never past the earlier box): the stock model's columns are one cell wide and hold
//     material when the ray through their centre does, so material can stand up to half a cell outside the soup there,
//     and a rapid planned against the box must not meet it (more material, never less);
//   - else as measured (a side across the earlier Z is a column top or bottom, exact to the float depth of a dexel).
// Each side is then rounded outward to a whole nanometre.
//
// SLICES (carriedSlices; cam.h resolveSetupGeometry). The grid: the stock box's XY in n = ceil(extent / cell) equal
// cells per axis (the stock model's own snapping rule, so a flip lands on the same columns). Per grid cell, `top` = the
// highest Z of any soup triangle whose XY box overlaps the cell by more than kCarriedEdgeCells of a cell (a triangle on
// a cell's edge belongs to neither side; a column's own top face covers the cell exactly), raised by `topPad` (one cell
// when the later Z lies across the earlier setup's X or Y, whose columns are one cell coarse; else 0). Then every cell
// takes the highest top of its 3 x 3 neighbourhood (one cell of growth, for the half cell a column can miss, and for a
// grid that does not line up with the earlier one). A slice at height z holds the cells whose top is ABOVE z: the
// shadow of the material above z, as kernel::sliceAt's "material just above z". The levels: the stock's bottom and each
// distinct top, when there are at most kCarriedExactLevels of them (2.5D work: exact); else the bottom and every top
// rounded DOWN to a whole cell above the bottom (each level then says what lies above it, so a depth between two levels
// reads the lower one's material: more, never less). Between levels a consumer reads the level at or below its depth
// (adaptive2d's rule, op.h STOCK SLICES), which with these rules is all the material above that depth.
// Loops: the cells of a slice as rectilinear loops along the cell edges, outer loops counter-clockwise and holes
// clockwise, every corner kept and nothing else; where two filled cells touch only at a corner one of the empty cells
// beside them is filled first (more material), so no two loops touch. Loops start at their lowest corner (y, then x)
// and come in that order.
//
// WHERE IT IS COARSER THAN THE STOCK. Material thinner than one cell of the simulation between two cuts (a wall, a fin)
// can be missing from the soup altogether (the stock model's rays both fall in the cuts); no growth brings it back.
// At kCarriedStockCellMm that is a wall under 0.25 mm (0.010 in). Everything else errs toward more material.
#pragma once

#include <ray/cam.h>

#include <array>
#include <utility>
#include <vector>

namespace ray::cam::detail {

// A side of the oriented stock within this of the earlier setup's stock box lies on it (see THE BOX).
inline constexpr double kCarriedSnapMm = 1e-3;
// A triangle's XY box must overlap a grid cell by more than this fraction of the cell to count there (see SLICES).
inline constexpr double kCarriedEdgeCells = 1e-6;
// At most this many distinct tops are listed as levels exactly; more are rounded down to whole cells (see SLICES).
inline constexpr std::size_t kCarriedExactLevels = 64;
// The rules of carrying and slicing; part of the input hash of every operation of a FromSetupStock setup, so a change
// of these rules (or of kCarriedStockCellMm) makes them stale. Raise it when either changes what a generator receives.
inline constexpr std::int64_t kCarriedRulesVersion = 1;

using StockSlices = std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>;

// The carried stock in the orientation O of a later setup (rotation: R as an Xform, translation 0): the soup moved
// into O (see RE-ORIENTATION) and its box in O, in nanometres (see THE BOX). InvalidArgument for an empty soup or one
// with indices out of range; Cancelled.
struct OrientedStock {
    kernel::TriSoup soup;
    std::array<Nm, 3> lo{}, hi{};
};
Result<OrientedStock> orientCarried(const CarriedStock& carried, const Xform& rotation, JobContext& ctx);

// How much the slices raise each top (see SLICES): 0 when the later setup's Z (row 2 of wcsFromModel) is the earlier
// setup's Z or its reverse exactly, else carried.cellMm.
double carriedTopPad(const CarriedStock& carried, const Xform& wcsFromModel);

// Moves every vertex by -origin (O to the WCS).
void shiftSoup(kernel::TriSoup& soup, const Vec3& origin);

// The slices of `soup` (closed, in the WCS) over `box` (its stockBounds) at `cellMm` (see SLICES). Cancelled.
Result<StockSlices> carriedSlices(const kernel::TriSoup& soup, const Box3& box, double cellMm, double topPad,
                                  JobContext& ctx);

} // namespace ray::cam::detail
