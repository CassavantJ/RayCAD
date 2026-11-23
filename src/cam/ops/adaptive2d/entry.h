// Private to src/cam (task T-063): adaptive clearing at one depth with every region entered, ready for the operation.
// planAdaptive is the one function the adaptive2d operation (T-061) calls: it runs the constant-engagement engine
// (engine.h) and closes the gaps spike S5 found in what the engine alone can cut. A region the engine cannot helix
// into (a narrow slot) is entered by a ramp along its axis or at a pre-drilled hole; tool-down links are checked and a
// link that would cut becomes a cut; the engine's leads' stretches through cleared area come out as tool-down links,
// not cuts (air cutting; engine.h Motion::LinkClear); every cutting segment says whether its feed must be reduced.
//
// WHAT T-061 USES (all of it from one call, planAdaptive):
//   Region::entry       how to enter: Helix (a helix of `radius` about `centre`, link.h helix entry), Ramp (ramp down
//                       along the region's first path, Role::Ramp, from `centre` to `start`: link.h's Ramp entry on
//                       that path as a pass), PreDrill (plunge into the hole of `radius` at `centre`: link.h PreDrill;
//                       the first path is then a LinkClear from the centre to the hole's side, where the first pass
//                       starts), FromCleared (from cleared area: an open pocket or rest machining; a feed descent through
//                       cut area). None only with Input::allowUnentered.
//   Region::paths       in cutting order, each starting where the previous one ended: Path::points (nm) to
//                       geom2d::fitArcs then link::passFromPath; Path::motion: Cut (feed), LinkClear (a tool-down
//                       link through cleared area: link.h maxStayDown with stayDownLift), LinkRetract (retract and
//                       re-enter); Path::reducedFeed per segment (REDUCED FEED); Path::engagementDeg for
//                       Toolpath::Stats (99th percentile engagement; air-cut length: Cut segments estimated under 2
//                       deg).
//   Output::cleared     link::Stock::cleared after this depth. Output::warnings -> Toolpath::warnings.
//   The caller keeps everything about Z: depths, ramp angle, helix pitch, feeds. One call per depth; nanometres in and
//   out; the Input is runEngine's (its seeds, regionsAt and helixEntry left as they are).
//
// HOW (planAdaptive). Each region (connected area where the tool centre may go, the engine's) is entered by the first
// kind in EntryOptions::order that fits it:
//   Helix     the engine's helix at the point farthest from the walls. When anything follows Helix in the order, it is
//             first tried at full size (Input::helixDiameter) only; a smaller helix, down to Input::helixMinDiameter,
//             is the last resort after the other kinds. A helix smaller than asked for is a slot: the passes then cut
//             into a face as wide as the slot (S5's 14.5 mm slot at 10% load: a third of the cut above target + 10
//             degrees), which a ramp along the slot avoids.
//   Ramp      a straight run of the region's tool-centre area: the longest chord of that area over the directions of
//             its longer edges and the axes, the most central of the longest (so in a slot, its axis), its ends
//             kRampEndMargin grid units inside the area; it fits when at least minRampRun long (0: the tool
//             diameter). Its full length is used, up to maxRampRun (0: no limit; longer runs are cut to a centred
//             piece): in a slot the passes then only widen the ramp's cut on both sides.
//   PreDrill  the first hole of EntryOptions::preDrills at least as wide as the tool and the smallest helix (the room
//             the engine's smallest helix gives the passes to start: a 13 mm hole in S5's slot read 135 degrees at the
//             first cut, 14.3 mm at most 75) whose centre lies in the region's tool-centre area, kRampEndMargin grid
//             units clear of its edge. The first pass starts as after a helix, against the hole's side: the centre
//             moved by (hole radius - tool radius) less a grid unit, the first of -y, +x, +y, -x that stays in the
//             tool-centre area.
// The engine runs first with the first kind (a full helix; with Ramp or PreDrill first, no helix, to learn the
// regions), then once more for each further kind any region still needs, only on those regions (Input::regionsAt),
// with the ramps' and holes' cleared areas in Input::cleared and their pass starts as Input::seeds. An attempt fails
// when the engine finds no first cut from it (no Pass in the region); a run where some regions fail and others do not
// is left out and the others run again by themselves, so no failed attempt leaves cleared area or warnings behind. A
// region with area already cleared (Input::cleared: an open pocket's outside, rest machining) is entered from it,
// whatever the order.
// Then, per region:
//   - a ramp's line becomes the first path (Role::Ramp, Cut, 2 points, engagement estimate 180, reduced feed);
//   - a pre-drilled hole's first path runs from its centre to the first pass's start: LinkClear (exact: distance +
//     tool radius <= hole radius; else Cut, which does not happen with the start above);
//   - LINKS: a LinkClear path whose engagement estimate exceeds kLinkCutDeg on any segment becomes a Cut (the engine
//     plans tool-down links to stay a stepover clear of material, its smoothing can bring them closer; none did on the
//     S5 pockets, the sweep checker measures 0 mm2 removed by tool-down links there);
//   - REDUCED FEED (below).
//
// REDUCED FEED. reducedFeed[i] = 1 on a segment of a Cut path where the engine's engagement estimate
// (Path::engagementDeg: the cut area per unit of travel of that segment against the engine's cleared area, as the
// engagement of a straight cut of that radial depth; a lead's, link's or finishing pass's segment is measured in
// pieces of at most 8 x MIN_STEP_CLIPPER grid units, 0.8 mm at the default grid, and kept apart where they differ)
// exceeds kReducedFeedFactor (1.25) times the target (angleForStepover(stepover)); on the last segment of a lead-in
// whose estimate exceeds the target (it ends where the pass begins, before the engine's regulation: the sweep checker
// reads up to 1.4 x target there); and on the kReducedFeedReach (millimetres of path) before and after such a segment
// along the same path, as the cutter meets the high engagement a little before or after the estimate's segment says.
// Ramp paths are flagged whole (a full-width cut). LinkClear and LinkRetract paths are never flagged. What it means for
// T-061: cut those segments at a reduced feed (T-061 chooses how much; the flag says only where). The tests check the
// flag against the sweep checker's own engagement on every S5 pocket and the slot (adaptive_entry_test.cpp): no
// unflagged cut is read above 1.25 x target on the tool's forward half (the checker's reading of the material ahead of
// the line through the tool centre across its motion, the only part of the edge that can cut); the checker's full
// reading, which in turns of a few millimetres' radius also counts material its lagged removal has not caught up with
// behind that line, is reported beside it.
//
// UNITS, DETERMINISM, CANCEL. As engine.h: nanometres in and out, no clock, no shared state, byte-identical results;
// ctx.cancel is polled by every engine run and between steps; progress: the first run covers 90%, later runs the rest.
//
// RESULTS AND REFUSALS. planAdaptive returns runEngine's errors, and:
//   InvalidArgument  Input::seeds, regionsAt or helixEntry set (they are planAdaptive's own); an entry kind other
//                    than Helix, Ramp and PreDrill, or none; a pre-drilled hole of diameter <= 0 or off the coordinate
//                    range; minRampRun or maxRampRun negative;
//   ToolpathFailed   a region that no kind in the order can enter (unless Input::allowUnentered: then a region with
//                    entry None and a NotEntered warning with the same words): the message names what each kind
//                    needs, the helix as the narrowest width (tool diameter + smallest helix + 2 x finishing
//                    allowance), the ramp as the shortest straight run, the hole as the smallest diameter; and
//                    runEngine's "does not fit anywhere", which names the narrowest slot the tool can clear (tool
//                    diameter + 2 x (finishing allowance + stock to leave)): "a slot must be wider than W mm".
// Warnings: the engine's, and EntryFallback for a region entered by a kind other than the order's first (what did not
// fit), in region order.
#pragma once
#include "engine.h"

#include <ray/cam.h>

#include <vector>

namespace ray::cam::adaptive2d {

inline constexpr double kReducedFeedFactor = 1.25; // of the target engagement angle
inline constexpr double kReducedFeedReach = 0.5;   // mm of path before and after a segment above it
inline constexpr double kLinkCutDeg = 1.0;         // a tool-down link estimated above this is a cut
inline constexpr std::int64_t kRampEndMargin = 10; // grid units: ramp ends and hole centres inside the area

struct PreDrillHole {
    Pt centre;
    Coord diameter = 0;
};

struct EntryOptions {
    // Tried in this order for each region (HOW); Plunge is not an adaptive entry. link.h's default order.
    std::vector<EntryKind> order{EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill};
    std::vector<PreDrillHole> preDrills;
    Coord minRampRun = 0; // 0: the tool diameter
    Coord maxRampRun = 0; // 0: no limit
};

Result<Output> planAdaptive(const Input&, const EntryOptions&, JobContext&);

// ---------- planAdaptive's last steps (exposed for the tests) ----------
// LINKS: every LinkClear path with role Link whose engagement estimate exceeds kLinkCutDeg on a segment becomes a Cut;
// returns how many did.
std::int64_t cutLinksThatCut(std::vector<Path>&);
// REDUCED FEED: fills the path's reducedFeed for a target engagement angle (degrees).
void flagReducedFeed(Path&, double targetDeg);

} // namespace ray::cam::adaptive2d
