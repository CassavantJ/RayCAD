// Private to src/cam (task T-060): the constant-engagement engine of 2D adaptive clearing. It plans the passes of an
// adaptive (trochoidal-like) clearing of an area at one depth so the tool's radial engagement stays at a target, and
// returns them as tool-centre polylines in integer nanometres with what the operation needs to make moves of them.
// It is a modified fork of FreeCAD's Adaptive.cpp (engine_adaptive.cpp says what changed); this header is the whole
// interface: nothing else in src/cam may include engine_adaptive.h.
//
// WHO USES WHAT (PLAN.md M4):
//   T-063 entry      Region::entry (a helix the engine planned, or a start from cleared area), Region::toolBounds
//   (where
//                    the tool centre may go, for a ramp or a pre-drilled point), Input::allowUnentered (regions the
//                    engine cannot enter come back with entry None instead of a failed result), Input::cleared (after
//                    T-063 cuts an entry, it runs the engine again with the area that entry cleared: the engine then
//                    starts from it), Path::motion (LinkClear links are tool-down links through cleared area; T-063
//                    checks they cut nothing or reclassifies them) and Path::engagementDeg (feed reduction above
//                    1.25 x target).
//   T-061 operation  everything: Path::points to geom2d::fitArcs (the engine emits polylines only) then link.h
//                    (passFromPath, maxStayDown), Result::cleared for link::Stock::cleared, Path::engagementDeg for
//                    Toolpath::Stats (99th percentile engagement, air-cut length), warnings to Toolpath::warnings.
// The sweep checker in tests/tools/sweepcheck measures the output independently of this code.
//
// HOW. The input regions (nanometres, arcs flattened with sag kInputSag) are converted to the engine's own integer grid
// of g units per millimetre, g = round(48 / tolerance_mm / min(1, stepover * diameter_mm)) (480 at the default 0.1 mm
// tolerance: about 2.1 um; upstream's step constants are tied to that grid), every coordinate rounded half to even and
// range-checked. The engine then (upstream's algorithm) offsets the area by the tool radius to get where the tool
// centre may go, splits it into connected regions, and for each one: enters from cleared area if there is any to
// start from, else by a helix of the largest diameter between helixMinDiameter and helixDiameter that fits at the point
// farthest from the walls; then cuts passes, each step searching the direction whose new cut area per unit of travel
// matches the target (CalcCutArea, exact circle-polygon geometry), links passes through cleared area where it can
// (tool down) or marks the link as one that must retract, and ends with a finishing pass along the boundary that cuts
// the last stepover / 10. Output coordinates are converted back to nanometres, rounded half to even.
//
// UNITS AND LIMITS. Input and output coordinates are integer nanometres (geom2d), within geom2d::kMaxCoord. The extent
// of the input (regions, stock and cleared area together) on the grid must stay below kMaxGridExtent units (4.47 km at
// the default grid; 4.47 m at the finest, a 0.01 mm tolerance with stepover x diameter of 0.01 mm): beyond it the input
// is refused (InvalidArgument). Inside, every product of two coordinates fits 63 bits.
//
// DETERMINISM AND RE-ENTRANCY. No clock, no unordered containers, no shared mutable state: any number of runEngine
// calls may run at once on any threads, and the output depends on the input only (byte-identical on every platform:
// src/cam is built with strict floating point, and the engine's trigonometry, its offsets' included (engine_offset.h, a
// copy of Clipper2's offsetting: Clipper2 itself calls the platform's libm), is ray::dmath; Clipper2's booleans are
// built with RayCAD's patch for a stable intersection sort, ports/clipper2). Where upstream stopped
// on wall-clock time, the engine stops on counted work (Limits). The tests in tests/unit/cam/adaptive* check 20
// processes, 20 repeats, 1, 2, 16 and 32 concurrent runs and the goldens in tests/golden/adaptive2d.
//
// CANCEL. ctx.cancel is polled at every step of every loop (the step search, each cut-area evaluation of a lead or a
// link, the entry search, link and lead search, finishing); measured latency is in the cancel test. Progress
// (ctx.progress) is the fraction of the regions done, by area cleared, reported once per pass.
//
// RESULTS AND REFUSALS. Every field of every result is initialised. runEngine returns
//   InvalidArgument  bad input (each check below names what is wrong): no region, tool diameter <= 0, stepover outside
//                    [kMinStepover, kMaxStepover], tolerance outside [kMinTolerance, kMaxTolerance], negative stock to
//                    leave or helix diameters, keepToolDownRatio < 0, coordinates out of range, extent too large;
//   ToolpathFailed   a region the engine cannot enter (no cleared area to start from and the smallest helix does not
//                    fit; the message names where), unless allowUnentered; the engine gave up after
//                    Limits::failedEngagements passes that cut nothing; a hard limit was reached (named); an output
//                    point off the grid's range (an internal fault, never seen);
//   Cancelled        ctx.cancel was cancelled.
// Warnings (Result::warnings, in the order found) say what the engine did that a caller may refuse: a lead that did
// not reach cleared area, a finishing pass left out, area left uncut, area not cleared to within the finishing
// allowance (the finishing pass may then cut more than the target), a region not entered (allowUnentered).
#pragma once
#include "../../geom2d.h"

#include <ray/cam.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ray::cam::adaptive2d {

using geom2d::Coord;
using geom2d::Pt;
using geom2d::Regions;

// ---------- Ranges and defaults ----------
inline constexpr Coord kDefaultTolerance = 100'000; // 0.1 mm (FreeCAD's default): grid 480 units per mm
inline constexpr Coord kMinTolerance = 10'000;      // 0.01 mm (upstream clamps to [0.01, 1] mm)
inline constexpr Coord kMaxTolerance = 1'000'000;   // 1 mm
inline constexpr double kMinStepover = 0.01;        // of the tool diameter: about 11.5 degrees of engagement
inline constexpr double kMaxStepover = 1.0;         // full width: 180 degrees
inline constexpr Coord kInputSag = 1'000;           // 1 um: arcs of the input are flattened to this sag
inline constexpr std::int64_t kMaxGridExtent = std::int64_t(1) << 31;

// The target engagement angle (degrees) of a stepover fraction of the diameter for a straight cut, acos(1 - 2 s),
// and back. The engine regulates the stepover; these convert.
double angleForStepover(double stepover);
double stepoverForAngle(double degrees);

// ---------- Input ----------
// Work limits, replacing upstream's wall-clock ones (engine_adaptive.h fork::Limits says what each counts). The soft
// ones change what the engine does (a link search that stops retracts); the hard ones end the run with an error.
struct Limits {
    std::int64_t linkSearchSteps = 64; // soft: segments one tool-down link search examines
    std::int64_t linkSideSteps = 256;  // soft: sideways probes of one link search
    std::int64_t passes = 100'000;     // hard
    std::int64_t pointsPerPass = 1'000'000;
    std::int64_t steps = 20'000'000; // engagement steps of the whole run
    std::int64_t failedEngagements = 10'000;
    std::int64_t leadSteps = 10'000; // soft: steps of one lead-in or lead-out (then it fails: a warning)
};

struct Input {
    // The area to clear at this depth: where the tool may cut (a pocket's region, islands as holes). The tool stays
    // inside it (its edge within the tolerance), less stockToLeave.
    Regions regions;
    // Area already free of material (rest machining; an entry T-063 made). May be empty.
    Regions cleared;
    // The stock outline at this depth. When not empty and enterFromOutside, the tool may move outside it (an open
    // pocket, whose region reaches past the stock edge, is entered from outside); empty: the tool never leaves
    // `regions` (upstream's forceInsideOut).
    Regions stock;
    bool enterFromOutside = true;
    Coord toolDiameter = 0;
    double stepover = 0.2; // target radial engagement as a fraction of the diameter (angleForStepover)
    Coord tolerance = kDefaultTolerance;
    Coord stockToLeave = 0;
    bool finishingPass = true;      // the last stepover / 10 along the boundary, as one pass per boundary loop
    Coord helixDiameter = 0;        // the largest entry helix (of the tool centre); 0: the tool diameter
    Coord helixMinDiameter = 0;     // the smallest; 0: tool diameter / 8 (upstream's minimum)
    double keepToolDownRatio = 3.0; // a tool-down link may be this many times longer than the straight distance
    bool allowUnentered = false;    // see RESULTS (T-063)
    Limits limits;
};

// ---------- Output ----------
enum class Motion : std::uint8_t {
    Cut,        // cutting (passes, leads, finishing, and a link the engine found may cut through)
    LinkClear,  // a tool-down link through cleared area
    LinkRetract // a link that must leave the material (lift or retract); its points are only its two ends
};
enum class Role : std::uint8_t { Pass, LeadIn, LeadOut, Link, Finish };
struct Path {
    Role role = Role::Pass;
    Motion motion = Motion::Cut;
    std::vector<Pt> points; // the tool centre, nm; at least 1 point
    // engagementDeg[i] (i >= 1): the engine's estimate of the engagement angle of the segment points[i - 1] ->
    // points[i], from its cut area per unit of travel against its own cleared area (exact geometry, but the cleared
    // area is the engine's polygon model and a pass's own last steps count only as the disc behind the tool);
    // engagementDeg[0] = -1. -1 where there is no estimate. Same size as points.
    std::vector<double> engagementDeg;
};
enum class EntryMode : std::uint8_t {
    Helix,       // a helix of `radius` about `centre` down to the depth; the first pass starts at `start`, which is
                 // (centre.x, centre.y - radius) (on the helix)
    FromCleared, // from cleared area: the first path (a lead-in) starts at `start`; centre = start, radius 0
    None         // not entered (allowUnentered only): the region has no paths
};
struct Entry {
    EntryMode kind = EntryMode::None;
    Pt centre;
    Coord radius = 0;
    Pt start;
};
struct Region {
    Entry entry;
    std::vector<Path> paths;   // in cutting order: (link, lead-in,) pass, lead-out, ..., then finishing passes
    bool returnClear = false;  // the straight move from the last point back to entry.centre stays in cleared area
    double clearedAreaMm2 = 0; // the area this region cleared
    Regions toolBounds;        // where the tool centre may go in this region (nm)
};
struct Warning {
    enum class Code : std::uint8_t { LeadFailed, FinishingLeadFailed, FinishingSetup, UnclearedArea, NotEntered };
    Code code = Code::LeadFailed;
    std::string message;
};
struct Stats {
    std::int64_t gridUnitsPerMm = 0;
    std::int64_t steps = 0, cutAreaCalls = 0, passes = 0, linkSearches = 0, linkSearchSteps = 0,
                 linkSearchLimitHits = 0, linkSideProbes = 0, engageSearches = 0, failedPassesInARow = 0;
};
struct Output {
    std::vector<Region> regions; // in the engine's order (upstream's: the order of the tool-centre area's loops)
    Regions cleared;             // the area free of material afterwards (input cleared area included), nm
    std::vector<Warning> warnings;
    Stats stats;
};

Result<Output> runEngine(const Input&, JobContext&);

// Golden-file text of an output: integers only ('\n' line ends), engagement in millidegrees rounded half to even.
std::string toText(const Output&);

} // namespace ray::cam::adaptive2d
