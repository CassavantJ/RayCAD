// The "slot" operation type (task T-055), private to src/cam and its tests: centre-line slotting, the tool exactly as
// wide as the slot, one open pass along the slot's centre line per depth, entered by linking's zigzag ramp along it.
// Parameters: schemas/ops/slot.schema.json. docs/operation_types.md describes the pattern of an operation type.
//
// GEOMETRY. Each selected element is the floor face of a slot: a planar face looking up the setup's Z (faceBoundary)
// with one boundary loop. A slot is a centre line (lines and arcs) swept by a disc of the slot's width w: its floor's
// boundary is two rails (the centre line offset by w / 2 each side) joined at each end by
//   a closed end: a half circle of radius w / 2 (a wall rises from it), or
//   an open end: a straight side of length w square to the rails with no wall above it (the slot runs out of the
//   part there: the material just above the floor, sliceRegions at the floor's height, does not touch it).
// Recognition (SlotShape): every span of the floor's boundary is classified as wall or open by testing the point
// kWallProbe beyond its middle against that slice; the ends are the open runs (each one straight line) and the
// convex half-circle arcs (sweep 180 degrees within kHalfTurnTol; when there are more than two, the two of smallest
// radius); there must be exactly two, of the same width within kWidthMatch, and everything between them wall. The
// centre line runs from end to end (a closed end's centre, an open end's midpoint): the left rail (the boundary runs
// counter-clockwise, so the floor is on its left) offset to its left by w / 2: exactly for a rail of one line or one
// arc, by geom2d::offsetOpen otherwise. Past an open end the centre line goes on straight, along its end tangent, for
// the tool radius plus kOpenEndRunOut, so the tool leaves the material whole.
// The candidate is then proved, not trusted:
//   (1) the floor lies inside the area the slot's width sweeps along the centre line (geom2d::sweptArea of w / 2 plus
//       kSweepTol): what is left of the floor outside it is under kContactMinArea;
//   (2) the tool clears the part: at the floor's height, at every pass depth, at the height of every planar face
//       perpendicular to Z between the floor and the slot top (faceHeights) and every kProbeStep in between, the part's
//       material just above that height (sliceRegions) does not reach into the area the tool's disc (radius - kWallTol)
//       sweeps along the extended centre line (mill::contactAt).
// A face that is not a slot is refused, naming what is wrong and pointing at the pocket or contour types.
//
// TOOL. A flat end mill (a ball or bull nose end mill would leave a rounded floor or floor edges the model does not
// have). Its diameter D against the slot's width w: D > w + kWiderTol: refused (it would cut both walls); D < w -
// widthTolerance (param): refused (a centre-line slot is as wide as its tool: use a tool of the slot's width, or a 2D
// pocket); otherwise accepted, with a warning when D < w - kWiderTol (the slot comes out that much narrower).
// The flutes must reach from the floor (plus floor stock) to the slot top; the stick-out must keep the holder
// kHolderClearanceMm above the stock top (mill::highestNm) at the deepest pass.
//
// LEVELS (absolute WCS Z, nm). Heights are relative to the stock top T (slot_common.h): clearance T + clearance,
// retract T + retract, feed plane T + feedPlane, and the slot top Z0 = T + top. heights.bottom is not read. The floor
// level F = floor Z + toNm(floorStock). Depths: n = ceil((Z0 - F) / stepDown) passes (at least 1), pass k (1..n) at
// Z0 - round_half_even(k (Z0 - F) / n), the last exactly at F. Z0 <= F is refused.
// PASSES (link.h, "T-055 slot"): per slot and depth one open, reversible pass along the extended centre line (cluster
// = the slot, level = the depth index), feed feedCut; no leads; room none. Linking: entry {Ramp} at rampAngle with
// minRampRun (0: the tool diameter), helixDiameter 0, entryClearance kRampClearance; maxStayDown = the tool diameter
// with stayDownEntry (link.h, added by this task), so from the end of one depth the tool stays in the slot, feeds up
// to the ramp's top (kRampClearance above that depth) and ramps down into the next (the next pass runs back); links
// between slots retract to the retract plane. Feeds: ramp feedRamp, descents feedPlunge, links and the feed up 0 =
// the cut feed.
//
// VALIDATE runs the same plan as generate (planSlot) and returns its first error; the plan's warnings go into
// Toolpath::warnings when generate runs.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr geom2d::Coord kWallProbe = 5'000;  // 5 um: a boundary span is a wall when material is this close
inline constexpr double kHalfTurnTol = 1e-4;        // radians: a closed end sweeps 180 degrees within this
inline constexpr geom2d::Coord kWidthMatch = 2'000; // 2 um: both ends of a slot have the same width within this
inline constexpr geom2d::Coord kEndMatch = 2'000;   // 2 um: an offset rail ends at the end's centre within this
inline constexpr geom2d::Coord kSweepTol = 1'000;   // 1 um: the floor lies within the width swept plus this
inline constexpr geom2d::Coord kWallTol = 1'000;    // 1 um: the tool may touch a wall this closely
inline constexpr geom2d::Coord kWiderTol = 1'000;   // 1 um: a tool counts as wider than the slot beyond this
inline constexpr geom2d::Coord kOpenEndRunOut = 1'000'000; // 1 mm past the tool radius beyond an open end
inline constexpr Nm kProbeStep = 1'000'000;                // 1 mm between the heights the clearance is proved at
inline constexpr Nm kRampClearance = 500'000;              // 0.5 mm: a ramp starts this far above the material

// A recognised slot: its centre line between its ends, its width and its ends.
struct SlotShape {
    geom2d::Path centre; // open, from end 0 to end 1 (not extended)
    geom2d::Coord width = 0;
    bool open[2] = {false, false}; // end 0 (centre.start), end 1 (the centre line's end)
    geom2d::Region floor;
    Nm floorZ = 0;
};
// The slot whose floor face is `face` of `shape` (frame: ResolvedGeometry::wcsFromModel); the material slice at the
// floor's height decides walls from open ends. An error naming what is not a slot.
Result<SlotShape> recognizeSlot(const kernel::Shape& shape, kernel::SubId face, const Xform& frame, JobContext& ctx);
// The centre line extended straight past each open end by `runOut`.
geom2d::Path extendOpenEnds(const SlotShape& slot, geom2d::Coord runOut);
// The pass depths from the slot top z0 down to the floor level f (f < z0), at most stepDown apart (LEVELS above).
std::vector<Nm> slotDepths(Nm z0, Nm f, Nm stepDown);

struct PlannedSlot {
    std::size_t part = 0;
    SlotShape shape;
    geom2d::Path path; // the tool's centre line (extended past open ends)
    std::vector<Nm> depths;
};

struct SlotPlan {
    std::vector<PlannedSlot> slots; // in selection order (linking orders the cut)
    std::vector<link::Pass> passes;
    link::Options options;
    link::Stock stock;
    std::vector<std::string> warnings;
};

// The plan of an operation, or the first problem in words a machinist reads (InvalidArgument for what the user must
// change, ToolpathFailed for geometry that cannot be slotted, KernelFailure / Cancelled from the kernel).
Result<SlotPlan> planSlot(const GenerateInput& in, JobContext& ctx);
// The toolpath of a plan (linking; no kernel calls).
Result<Toolpath> slotToolpath(const SlotPlan& plan, const lib::MachineDef& machine, JobContext& ctx);

std::unique_ptr<IOperationType> makeSlotOperationType();

} // namespace ray::cam::ops
