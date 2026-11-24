// The "adaptive2d" operation type (task T-061), private to src/cam and its tests: 2D adaptive roughing of the empty
// area above picked floor faces, with islands, open edges, helix, ramp or pre-drilled entry, several depths and stock
// to leave. Parameters: schemas/ops/adaptive2d.schema.json. docs/operation_types.md describes the pattern of an
// operation type; pocket2d.h is its nearest sibling (the pockets and their depths are found by pocket2d's rules).
//
// THE AREAS. Each picked face is the floor of one area, found as pocket2d.h THE POCKET says: a closed pocket's
// footprint is its wall loop at the floor; an open pocket's is the floor's outer loop united with a strip 2 r +
// stockToLeaveWall + kAdaptiveFitToleranceNm wide outside every run of open edges (r the tool radius), so the tool can
// come in from outside the stock. At each depth z the area is the footprint minus the material at z - stockToLeaveFloor
// and above (islands lower than the top are cut over until the depth reaches them), keeping the pieces that meet the
// floor face. The depths and the area at each are pocket2d's rule, in its one implementation: area_depths.h.
//
// HEIGHTS (Operation::heights) from the top of the stock box, as pocket2d.h HEIGHTS: clearance, retract and feedPlane
// for linking, top where the material starts; ordered clearance >= retract >= feedPlane >= top, retract >= 0.
// heights.bottom is not read.
// DEPTHS (pocket2d.h DEPTHS, without its floor finishing): bottom = floor + stockToLeaveFloor; n = ceil((top -
// bottom) / stepdown) equal steps z_i = top - round((top - bottom) * i / n), and an up-facing flat face whose height +
// stockToLeaveFloor lies strictly between them adds that depth only where the area the tool reaches there (the area at
// that depth shrunk by r + stockToLeaveWall + kAdaptiveFitToleranceNm, grown back by r) overlaps it (an island's top,
// a lower step of the part open to the area); the floor of a pocket closed off inside the part, which the area does
// not reach, adds none (pocket2d.h DEPTHS says it in full).
//
// AT EACH DEPTH (adaptive2d/entry.h planAdaptive, one call per depth; a depth whose engine input is the same as an
// earlier depth's, as with straight walls, reuses that result: the input has no Z, so the passes are the same):
//   regions        the area at z (nanometres) shrunk by stockToLeaveWall + kAdaptiveFitToleranceNm, the engine's stock
//                  to leave 0 (one rule for walls and the stock outline below). The arc fitting below may move the
//                  tool by up to its tolerance towards a wall: the room for it is left the same way;
//   stock          the stock outline at z: the stock box's rectangle, or with ResolvedGeometry::stockSlices the
//                  material that is left there (STOCK SLICES), united with the part's material at z grown by
//                  stockToLeaveWall + kAdaptiveFitToleranceNm; the tool may run outside it (an open pocket is entered
//                  from there). The engine keeps only the tool radius from the stock outline where it lets the tool
//                  run outside it: without the grown part, a wall that meets the stock edge (an open step) lost its
//                  stock to leave at that corner (the property test: 0.27 of 0.293 mm);
//   cleared        empty, or with stockSlices the stock box less that material;
//   tool           diameter D = 2 r; load = stepover / 100 of D; tolerance adaptive2d::kDefaultTolerance (0.1 mm);
//                  finishing pass on (every depth ends with one light pass along the walls that takes the last tenth
//                  of the load);
//   helices        helixDiameter and helixMinDiameter (percent of D, rounded half to even to the nanometre);
//   entry order    entry "helix": helix, ramp, pre-drilled hole; "ramp": ramp, helix, pre-drilled hole; "preDrill":
//                  pre-drilled holes only (planAdaptive's order and fall-back; a region with area already cleared,
//                  an open pocket's outside, is entered from it whatever the order). Never a plunge: flat and bull
//                  nose end mills are taken as not cutting at their centre.
// The minimum cutting radius of PLAN.md's example is not a setting: the engine has no control of the radius its passes
// turn at (engine_adaptive.cpp), and the operation does not pretend to one.
//
// STOCK SLICES (rest roughing; ResolvedGeometry::stockSlices, when present: loops in the WCS's XY of the material left
// at each listed Z, mm). The material at depth z is the union of the slices at the highest listed Z at or below z and
// every listed Z above it (material higher up is counted too, so nothing that is there is missed); with no listed Z
// at or below z nothing is known there and the whole stock box is material. The part's own material at the depth
// (THE AREAS) is added to it, so slices that leave out part of the part cannot let the tool into it. The engine gets
// that material as its stock and the box less it as cleared area, so it clears only what is left and enters from the
// cleared area; linking gets, for every listed Z, the box less the union of the slices at that Z and above as
// Stock::cleared. Absent: the stock box everywhere, as before.
//
// PASSES (link.h; keepOrder: the engine's order is the order of cutting, which its engagement depends on). Areas in
// selection order, each depth top down, each region in the engine's order. A region's paths become one link::Pass
// at z, cut where a path is LinkRetract (a link that must leave the material): linking then retracts, or stays down
// when its own test finds the straight move free (maxStayDown 2 D, lifted by kAdaptiveLinkLiftNm). Inside a pass:
//   Cut paths     arc-fitted (geom2d::fitArcs, tolerance kAdaptiveFitToleranceNm) in runs of equal feed: the cutting
//                 feed, or the reduced feed (reducedFeed percent of it, rounded half to even, at least 1 um/min) on
//                 the segments planAdaptive flags (entry.h REDUCED FEED); role Cut. The tolerance: the engine's points
//                 lie on its grid of about 2 um and follow its step search, so arcs cover few of them below about 6 um;
//                 measured on S5's circle and islands pockets at 10 and 20 % load, 5 um gave 0.95 to 1.05 moves per
//                 mm, 8 um 0.37 to 0.73 (the acceptance asks at most 1). The walls are moved in by it (AT EACH DEPTH),
//                 so a fitted path never comes nearer a wall than the engine's own path does;
//   LinkClear     the engine's tool-down links through cleared area (entry.h LINKS: checked to cut nothing): lifted,
//                 straight up by lift = min(kAdaptiveLinkLiftNm, half the step from the depth above), along the
//                 fitted path, straight down, at the cutting feed, role Link; one shorter than the tool diameter (and
//                 than kAdaptiveMinLiftedLinkNm) stays at depth (role Link): the engine's short stretches of leads
//                 through cleared area are many, and two more moves each for a few millimetres is not worth it. The
//                 lift stays inside the depth's own cut.
//   entry         the region's first pass carries the entry: Helix (Pass::helixCentre = the engine's helix centre, so
//                 linking's helix is the circle the engine planned and counted as cleared), Ramp (the ramp's line is a
//                 pass of its own, entered by linking's Ramp along it: a zigzag down to depth, then the line at depth
//                 at the reduced feed), PreDrill (linking's PreDrill at the hole centre; the move from the centre to
//                 the hole's side is a Link at depth), FromCleared (linking's descent through free area; Stock::cleared
//                 and the stock outline make an open pocket's outside free).
// Cut areas (link.h Pass::cutArea; linking's own sweep of a pass of a few metres of fitted arcs takes seconds to
// minutes: measured 4 to 106 s on S5's islands pocket, against 1 ms for this): the last pass of each depth gives the
// engine's cleared area of that depth shrunk by kAdaptiveCutAreaMarginNm (the fitted arcs may lie that much inside the
// engine's polylines); a pass followed in its region by another (after a link that must retract) gives the tool's
// disc where that one starts, since the engine starts each lead-in in its cleared area (not at a depth where it warned
// of a lead that did not reach cleared area: then nothing, and linking enters by an entry of the order), so the next
// pass is entered by a descent through free area; every other pass gives nothing (linking then retracts from it and
// enters the next region by its own entry).
// Linking options: heights from the stock top; ramp angle; entryClearance kAdaptiveEntryClearanceNm; the feeds:
// plunge (descents, pre-drilled entries), ramp (helices, ramps); stayDownEntry (the next depth is entered from the cut
// above when linking finds the move free); tipHeight a bull nose's corner radius; pre-drilled holes with the lowest
// floor of the areas they lie in as their bottom.
//
// STATS (Toolpath::Stats): cut and rapid length, min and max Z from the moves; from the engine's estimates of every
// Cut segment of every depth (adaptive2d::Path::engagementDeg, the ramp's line included at 180 degrees): the largest,
// the 99th percentile by length, and the air-cut length (segments estimated under kAdaptiveAirDeg).
// WARNINGS (Toolpath::warnings), in this order: pre-drilled points in no area; then each kind of engine and entry
// warning in words for the machinist, with where (X Y) and the depths it happened at (one line for the same warning
// at several depths).
// CHECKS (refusals name the cause): the tool (a flat or bull nose end mill with a diameter, flutes at least the
// stepdown, a shank and holder that clear the stock as in pocket2d.h CHECKS), the spindle (clockwise, within the
// machine), feeds, heights out of order, a top not above the floor, a selection that is not an up-facing floor, the
// smallest helix above the helix, a tool that does not fit an area at some depth, pre-drilled points that cannot be
// read or are narrower than the tool; from planAdaptive: a region no allowed entry reaches (naming what each kind
// needs), the engine's limits; from linking: an entry it cannot make.
// VALIDATE and CHECK run the checks up to the areas of every depth (Adaptive2dStage::Regions): the engine is not run
// (seconds on a large area, and check cannot be cancelled), so a refusal from planAdaptive or linking, and the
// engine's warnings, come with generate.
// DETERMINISM: integer nanometres (geom2d, the engine's output), parameters rounded half to even once, std::map and
// sorted vectors only; planAdaptive and linking are re-entrant, so any number of these operations generate at once.
#pragma once

#include "../../geom2d.h"
#include "../../link.h"
#include "entry.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr Nm kAdaptiveFitToleranceNm = 8'000;      // 8 um: arc fitting of the engine's polylines
inline constexpr Nm kAdaptiveLinkLiftNm = 250'000;        // 0.25 mm: tool-down links run this far above the floor
inline constexpr Nm kAdaptiveMinLiftedLinkNm = 1'000'000; // 1 mm: a shorter tool-down link is never lifted
inline constexpr Nm kAdaptiveEntryClearanceNm = 500'000;  // 0.5 mm: a helix or ramp starts this far above material
inline constexpr Nm kAdaptiveCutAreaMarginNm = kAdaptiveFitToleranceNm + 1'000; // the engine's cleared area shrunk
inline constexpr double kAdaptiveAirDeg = 2.0;            // a cut estimated under this engagement cuts air
inline constexpr double kAdaptiveHolderClearanceMm = 1.0; // the holder stays this far above the stock top
inline constexpr std::size_t kAdaptiveMaxDepths = 10'000;

enum class Adaptive2dStage { Regions, Full };

// One depth of one area, as the plan made it (for the tests and the report).
struct Adaptive2dDepth {
    std::size_t area = 0; // index into Adaptive2dPlan::areas
    Nm z = 0;
    bool reused = false;                        // the engine's result of an earlier depth with the same input
    std::size_t output = 0;                     // index into Adaptive2dPlan::outputs
    std::vector<adaptive2d::EntryMode> entries; // per region, in the engine's order
    adaptive2d::Stats engine;
};
struct Adaptive2dArea {
    std::size_t selection = 0; // index into ResolvedGeometry::selected
    bool open = false;
    Nm floor = 0, bottom = 0, top = 0;
    std::vector<Nm> depths;
    geom2d::Regions footprint;
    std::vector<geom2d::Regions> region; // per depth: the area at that depth
};

struct Adaptive2dPlan {
    std::vector<link::Pass> passes;
    link::Options options;
    link::Stock stock;
    std::vector<Adaptive2dArea> areas;
    std::vector<Adaptive2dDepth> depths;
    std::vector<std::string> warnings;
    std::size_t engineCalls = 0;             // planAdaptive calls (a reused depth makes none)
    std::vector<adaptive2d::Output> outputs; // what each call returned, in call order
    double maxEngagementDeg = 0, p99EngagementDeg = 0, airCutLengthMm = 0;
    double engagedLengthMm = 0; // the length the engagement statistics are taken over
};

// The plan, or the first problem in words a machinist reads (InvalidArgument for what the user changes in the
// operation, ToolpathFailed for geometry that cannot be roughed, KernelFailure and Cancelled from the kernel and the
// engine).
Result<Adaptive2dPlan> planAdaptive2d(const GenerateInput& in, JobContext& ctx,
                                      Adaptive2dStage stage = Adaptive2dStage::Full);
// The toolpath of a plan: linkPasses, stats, warnings. No kernel or engine calls.
Result<Toolpath> adaptive2dToolpath(const Adaptive2dPlan& plan, JobContext& ctx);

// The 99th percentile by length of (degrees, length) samples: the smallest d with at least 99 % of the length at or
// below it; 0 for none. Exposed for the tests.
double percentileByLength(std::vector<std::pair<double, double>> samples, double fraction);

std::unique_ptr<IOperationType> makeAdaptive2dOperationType();

} // namespace ray::cam::ops
