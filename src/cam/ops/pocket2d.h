// The "pocket2d" operation type (task T-054), private to src/cam and its tests: offset pocketing of the empty area
// above picked floor faces, with islands, open edges, helix, ramp or pre-drilled entry, floor and wall finishing and
// stock to leave. Parameters: schemas/ops/pocket2d.schema.json. docs/operation_types.md describes the pattern of an
// operation type; drill.h is its first instance.
//
// THE POCKET. Each picked face is the floor of one pocket: a planar face looking up the setup's Z
// (geom2d::faceBoundary).
//   Closed pocket (geom2d::pocketAboveFace finds walls all round): the footprint is the wall loop at the floor.
//   Open pocket (no wall all round): the footprint is the floor's outer loop united with a strip 2 r + stockToLeaveWall
//   + wallAllowance wide outside every run of open edges, square at its ends (the run and its geom2d::offsetOpen
//   joined; geom2d::sweptArea where the offset comes in pieces), so the tool runs out past the open side without
//   wrapping round the faces beside it. An edge
//   is open where the point kOpenProbeNm outside it (at the middle of each piece of at most kOpenPieceNm) is not
//   material just above the floor (geom2d::sliceRegions at the floor).
// At each depth z the pocket region is the footprint minus the material at z - stockToLeaveFloor and above (the union
// of the part's slices at every depth of the pocket, each taken stockToLeaveFloor lower, from the top down to z, so an
// island lower than the top is cut over until the depth reaches it, and the stock to leave on its top is kept),
// keeping the pieces that meet the floor face.
//
// HEIGHTS (Operation::heights), all relative to the top of the stock box (ResolvedGeometry::stockBounds.max.z):
//   clearance, retract, feedPlane as linking uses them (link.h Options), top = where the material starts (the first
//   depth is one step below it). Ordered clearance >= retract >= feedPlane >= top, retract >= 0 (not into the stock
//   box). heights.bottom is not read: the bottom of each pocket is its floor + stockToLeaveFloor.
// DEPTHS of one pocket, absolute WCS Z: bottom = floor + stockToLeaveFloor; roughBottom = bottom + floorAllowance when
//   finishFloor, else bottom. n = ceil((top - roughBottom) / stepdown) roughing depths z_i = top - round((top -
//   roughBottom) * i / n) (integer nm, half away from zero), i = 1..n, so z_n = roughBottom exactly and no step exceeds
//   the stepdown. Every up-facing flat face of the part inside the footprint whose height + stockToLeaveFloor lies
//   strictly between roughBottom and the top (the top of an island lower than the top, a step) adds that depth, so it
//   is cleared rather than left under the next depth up. finishFloor adds the floor depth `bottom` after them.
//
// RINGS. At each depth: the roughing room R = the region shrunk by r + stockToLeaveWall + wallAllowance (r the tool
// radius, wallAllowance only when finishWalls); rings are R shrunk by k * s for k = 0, 1, ... while not empty (s the
// stepover, toNm(D * stepover / 100), D the tool diameter); every loop of every ring is a closed, rotatable pass. A
// part of a room or ring narrower than 2 kSliverNm (a sliver where an offset lands within micrometres of half the
// width) is dropped: the coverage check below finds what it would have cut. Rings are cut from the innermost
// outwards, each depth after the one above (link levels: cluster = the pocket, level = the depth's order then the
// ring's). Direction: a loop is cut with the region on its left (outer loops
// counter-clockwise, island loops clockwise), which with a clockwise (M3) spindle is climb; conventional reverses it.
// The step from one ring to the next is a stay-down step-in (link.h Options::stepIn, with stepInSlot: a stepover
// above the tool radius steps sideways into what the next ring cuts, at most a full-width slot). maxStayDown is
// max(2 D, 3 s).
// COVERAGE (per distinct room, shared by every depth that has it). The flat of the tool has radius rf = r (flat end
// mill) or r - cornerRadius (bull nose). The area the flat can reach is R grown by rf; the area cut is the sweep of
// every ring at rf; what is left, shrunk by kScallopLimitNm / 2, must be empty: no uncut area wider than the scallop
// limit (10 um). It can be non-empty only near the medial axis when s > rf; then for each piece left the ring at
// (k + 1/2) s of the band k it lies in (its parts that contain a piece) is added, which cuts it (with s < 2 rf every
// point of a band then lies within s / 2 < rf of a ring), and the check is run again; a piece still left is a refusal.
// What the tool cannot reach at all is reported as a warning when it reaches farther than r from the area the tool
// sweeps (a region narrower than the tool), never silently: corner remnants of radius r are not reported. Measured at
// the pocket's deepest depth, from the room the last pass round the walls uses, over the region shrunk by
// stockToLeaveWall (an open pocket: within the floor's outer loop, as the strip past the open edges is not pocket).
// FINISHING. finishFloor: the floor depth is cut with rings like a roughing depth. finishWalls: after the roughing and
// the floor, one pass round every loop of the region shrunk by r + stockToLeaveWall at every depth (top down), with a
// tangent arc lead-in and lead-out of radius max(r / 2, wallAllowance + 10 um) on the region side (made smaller by
// linking down to wallAllowance + 2 um if it does not fit the room), so it starts in cleared area.
// ENTRY (link.h): entry "helix": helix, ramp, pre-drilled point; "ramp": ramp, helix, pre-drilled point; "preDrill":
// pre-drilled points only. Never a plunge: flat and bull nose end mills are taken as not cutting at their centre. The
// helix diameter is toNm(D * helixDiameter / 100), tried smaller (link.h) down to round(D * kHelixMinFraction); ramps
// and helices descend at rampAngle, starting kEntryClearanceNm above the material. Pre-drilled points (WCS mm, the
// holes at least preDrillDiameter wide and drilled at least to the floor of the pocket they are in): the innermost
// ring of every part of the region that has a point inside it (or a point in the room whose straight line to it stays
// in the room) starts at that point with a straight cut to the ring's nearest point, so a pre-drilled entry can be made
// there (link.h: the pass must start inside the hole).
// CHECKS (refusals name the cause): the tool (a flat or bull nose end mill with a diameter, a corner radius under its
// radius, flutes at least the stepdown, a shank no wider than the cut unless the pocket is shallower than the
// shoulder or the end of a neck the tool states (after M3; a neck wider than the cut counts from the flutes' top), a
// stick-out that keeps the holder kHolderClearanceMm above the stock), the spindle (clockwise, within the
// machine), feeds, heights out of order, a top not above the floor, the stepover of a bull nose wider than twice its
// flat, a selection that is not an up-facing floor, a tool that does not fit a pocket at some depth (the room is
// empty), pre-drilled points that cannot be read or are narrower than the tool, an area left uncut, and (from
// linking) a pass no allowed entry reaches.
//
// VALIDATE runs the checks up to the regions and rooms of every depth (planPocket2d with PlanStage::Regions); check
// and generate run the whole plan and the linking (pocket2dToolpath), so a refusal from linking reaches the operation
// panel through check. Warnings (narrow regions, pre-drilled points in no pocket, leads made smaller) go into
// Toolpath::warnings in that order.
// DETERMINISM: integer nanometres from the start (geom2d), dmath for the one angle (the nearest point on an arc),
// stepover and helix diameter rounded half to even once (geom2d::roundNm), caches keyed by integer values in std::map.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr Nm kScallopLimitNm = 10'000;           // 10 um: an uncut area narrower than this is accepted
inline constexpr Nm kOpenProbeNm = 20'000;              // 20 um outside an edge is the probe of "is it open"
inline constexpr Nm kSliverNm = 5'000;                  // parts of a room or ring narrower than 10 um are dropped
inline constexpr Nm kOpenPieceNm = 1'000'000;           // edges are probed in pieces of at most 1 mm
inline constexpr double kHelixMinFraction = 0.3;        // the smallest helix is 30 % of the tool diameter
inline constexpr Nm kEntryClearanceNm = 500'000;        // a helix or ramp starts 0.5 mm above the material it enters
inline constexpr double kPocketHolderClearanceMm = 1.0; // the holder stays this far above the stock top
inline constexpr std::size_t kMaxDepths = 10'000;
inline constexpr std::size_t kMaxRings = 20'000; // rings per room

enum class PocketEntry { Helix, Ramp, PreDrill };

// What one distinct room gave (several depths share one when the pocket's walls are straight).
struct PocketCoverage {
    std::vector<Nm> depths;               // the depths that use this room
    std::size_t rings = 0, halfRings = 0; // full rings, and rings added at (k + 1/2) s
    geom2d::Regions room;                 // R: where the tool centre may go
    geom2d::Regions left;                 // what the flat does not cut of R grown by rf (before the scallop shrink)
    double reachableArea = 0;             // nm^2 of R grown by rf
};

struct PocketReport {
    std::size_t selection = 0; // index into ResolvedGeometry::selected
    bool open = false;
    Nm floor = 0, bottom = 0, top = 0;
    std::vector<Nm> depths; // roughing depths, then the floor finishing depth
    geom2d::Regions footprint;
    std::vector<PocketCoverage> coverage;
    geom2d::Regions narrow; // parts of the pocket the tool cannot get near (warned)
};

// Everything linking needs, worked out (and checked) once.
struct Pocket2dPlan {
    std::vector<link::Pass> passes;
    link::Options options;
    link::Stock stock;
    std::vector<PocketReport> pockets;
    std::vector<std::string> warnings;
};

enum class PlanStage { Regions, Full };

// The plan, or the first problem in words a machinist reads (InvalidArgument for what the user changes in the
// operation, ToolpathFailed for geometry that cannot be pocketed, KernelFailure and Cancelled from the kernel).
Result<Pocket2dPlan> planPocket2d(const GenerateInput& in, JobContext& ctx, PlanStage stage = PlanStage::Full);
// The toolpath of a plan: linkPasses, stats, warnings. No kernel calls.
Result<Toolpath> pocket2dToolpath(const Pocket2dPlan& plan, JobContext& ctx);
// "x y; x y; ..." in mm (spaces or a comma between x and y, ';' between points); InvalidArgument naming the problem.
Result<std::vector<Vec2>> parsePointList(std::string_view text);

std::unique_ptr<IOperationType> makePocket2dOperationType();

} // namespace ray::cam::ops
