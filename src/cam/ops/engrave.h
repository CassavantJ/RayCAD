// The "engrave" operation type (task T-056), private to src/cam and its tests: the tool centre follows picked edges
// and a line of text on a plane, at a depth below it. Parameters: schemas/ops/engrave.schema.json; the pattern every
// type follows: docs/operation_types.md. The text and its font: engrave_font.h.
//
// WHAT IS ENGRAVED, in this order (each a geom2d path in the WCS XY, integer nanometres):
//   1. The picked edges (ResolvedGeometry::selected, each must be an edge of a part body), per part, chained by
//      geom2d::chainEdges along the setup's Z (edges along Z are skipped; ends within 1 um join; a point where more
//      than two picked edges meet ends the chains there). Each chain is projected onto the plane whatever its own
//      height; a chain more than 1 um above or below the plane gets a warning saying so. With fitArcs, every run of
//      three or more consecutive line spans of a chain (what the kernel makes of a spline or an ellipse, kernel.h
//      edgeCurves) is replaced by geom2d::fitArcs at its default tolerance (1 um); a closed chain of lines only is
//      fitted as a closed polyline. fitArcs itself leaves both ends of each arc on its circle as the IR wants them
//      (geom2d.h fitArcs), so the fitted spans are used as they come. Lines and arcs that came as lines and arcs are
//      kept as they are.
//   2. The text (param text, UTF-8, at most 200 characters), laid out by layoutText (engrave_font.h): height, extra
//      spacing, alignment, position and angle on the plane; after M3 in one or more lines (a line break starts one),
//      each aligned on its own, lineSpacing (percent of the height) from one baseline to the next.
//   Nothing at all (no edges, empty text or only spaces) is refused.
// LEVELS (absolute WCS Z; Operation::heights: top is the plane, clearance >= retract >= feedPlane >= top):
//   depth D = toNm(depth); with stepDown S > 0 the levels are n = ceil(D / toNm(S)) (computed in integers),
//   z_k = top - D * k / n rounded half to even for k = 1..n, the last exactly top - D; with S = 0 one level, top - D.
//   heights.bottom is not read.
// PASSES AND LINKING: one pass per path and level (link::passFromPath at z_k, the cut feed), cluster = the path,
//   level = k, so a path's levels are cut in order; every path is reversible, and every closed one rotatable (linking
//   starts it where the tool is, on a vertex or at least geom2d::kMinArcChord from the ends of an arc it splits); every
//   pass is entered by a straight plunge (entryOrder {Plunge}: the tool must cut at its centre), at the plunge feed
//   (feedPlunge, else the cut feed). link::linkPasses orders them (nearest neighbour and local improvement) and adds
//   the plunges, retracts (feed up to the feed plane, rapid to the retract plane) and rapids; stock = the setup's stock
//   box. The tool's tip height (link::Options::tipHeight: a pointed tool's point length, drill.h pointLengthNm; a ball
//   end mill's radius) tells linking that a groove is narrower than the tool below that height above it, so no rapid
//   or feed descent enters an earlier groove lower than where the tool's full width cleared it. Rapids over the stock
//   stay at or above its top: a feed plane below the stock's top is raised to it for linking (with a warning), and the
//   tool feeds from there. Linking alone would rapid lower, down to the feed plane over what an earlier pass cut, and
//   be right; the raise is kept because nothing independent can check that. The invariant checker's rapid floor
//   (tests/unit/cam/invariants/README.md, rapidLow, exact) is the stock top unless a plug-in claims a lower one, and it
//   cannot credit area already cut; a lower claim would hand every rapid between that floor and the stock top, over
//   uncut stock too, to rapidStock, which sees the stock only at its samples (0.1 mm apart). An engraver's point a
//   few hundredths of a millimetre into uncut stock is narrower than that and can pass between them
//   (checker_geometry_test.cpp, "a pointed tool's rapid just into uncut stock"), which is exactly the mistake a
//   lowered floor would have to catch. Decided after M3: no lower claim, the raise stays. It costs only feed time
//   from the stock top down to the plane, and only when the stock's top is above the plane.
// TOOL (refused otherwise, with the reason): an engraving cutter, a spot drill, a ball end mill, or a chamfer mill
//   with a pointed tip (tipDiameter 0): tools that cut at their centre (and not one whose library entry says it does
//   not: Tool::cutsAtCentre). A pointed tool (tip angle in (0, 180)) must
//   reach the depth within its point: D <= drill.h pointLengthNm(diameter, tip angle, tip diameter). D must fit the
//   flutes; with a stick-out, the holder must stay kHolderClearanceMm above the plane. The cut width at the depth
//   (pointed: tipDiameter + 2 D tan(a / 2); ball: 2 sqrt(D (diameter - D)) up to the diameter; else the diameter)
//   above a quarter of the text height gives a warning (the strokes run together).
// CUTTING: rpm > 0 within the machine's range, the spindle clockwise (M3: every library tool cuts right hand), a cut
//   feed > 0. Feeds: feedMicronPerMin = feed x 1000 rounded half to even.
// Every check runs in planEngrave, which validate and generate share: the linked moves are part of the plan, so a
// linking refusal reaches validate too.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

// The width of the groove a tool cuts at depth `depth` (mm), see TOOL above.
double engraveWidthMm(const lib::Tool& tool, double depth);
// A path with every run of three or more consecutive line spans fitted with arcs (geom2d::fitArcs, its defaults); a
// closed path of lines only is fitted as a closed polyline. Other spans are kept.
geom2d::Path fitLineRuns(const geom2d::Path& path);

struct EngravePlan {
    geom2d::Paths paths;       // what is engraved, edges first, then the text's strokes
    std::size_t edgePaths = 0; // how many of `paths` came from edges
    std::vector<Nm> levels;    // the Z of each level, top down
    Nm plane = 0;              // heights.top
    link::Linked linked;       // the whole toolpath's moves
    std::vector<std::string> warnings;
};

// The plan, or the first problem in words a machinist reads (InvalidArgument for what the user must change,
// ToolpathFailed for geometry or linking that cannot be done, KernelFailure / Cancelled from the kernel).
Result<EngravePlan> planEngrave(const GenerateInput& in, JobContext& ctx);
// The toolpath of a plan (deterministic; no kernel calls).
Toolpath engraveToolpath(const EngravePlan& plan);

std::unique_ptr<IOperationType> makeEngraveOperationType();

} // namespace ray::cam::ops
