// The "contour2d" operation type (task T-053), private to src/cam and its tests: the tool runs beside a chain of picked
// edges (or the loops of picked planar faces) at one or more depths. Parameters: schemas/ops/contour2d.schema.json.
// docs/operation_types.md describes the pattern of every operation type; drill.h is its first instance.
//
// GEOMETRY. Picked edges are chained by geom2d::chainEdges (per part body, with tangent extension when
// followTangent); picked faces give their loops by geom2d::faceBoundary (faceLoops: outer, inner or all). Each chain is
// one contour: closed or open, at its height zc (the lowest point of its edges, or the face's height). Refused: a
// selection that is not an edge or a face, a face that is not planar and perpendicular to setup Z, an open chain end
// that meets another chain end (picked edges that branch: chainEdges stops there and does not guess), a chain without
// length.
//
// SIDE. side "auto": the material side is found by probing the part's slices (geom2d::sliceRegions, every part body)
// kProbeOffset either side of the middle of the chain's longest spans, first just above zc, then just below its top
// (kProbeBelow); the first level where exactly one side is material decides, and the tool goes on the other side. No
// level decides: refused (pick a side). "outside"/"inside" (closed chains only), "left"/"right" (seen along the chain
// as chainEdges orients it) give the tool side directly. Then the chain is oriented with geom2d::toolSideFor(direction,
// spindle clockwise) and orientForTool, so the tool is on the left of travel for climb with M3 (G41), right for
// conventional (G42).
//
// OFFSETS (integer nanometres, geom2d's exact offsets: an arc of radius r offset by d has radius r +- d exactly, a line
// moves by d exactly). Radial passes j = n - 1 .. 0 (n = roughPasses) leave extra_j = stockToLeave + (finishPass ?
// finishAllowance : 0) + j * roughStepover beside the chain; the finish and spring passes leave stockToLeave. The tool
// centre path C of a pass is the chain offset by R + extra (R = toolRadius = tool diameter / 2): closed chains by
// geom2d::offset of the region they bound (grown when the tool is outside, shrunk when inside; every result loop is a
// pass, oriented like the chain), open chains by geom2d::offsetOpen to the tool side. With compensation in the control
// the programmed path P is the chain offset by `extra` only (the chain itself when extra = 0) and C = offset(P, R) is
// what the control will cut; the toolpath carries P. A closed pass starts at the middle of its longest span (of P in
// the control, of C otherwise); an open pass at the chain's start.
//
// TOOL TOO LARGE (refused, naming where): C empty (the tool does not fit inside the contour); C leaving the room the
// tool centre may use (below) at any depth (it would cut the part elsewhere, e.g. an adjacent wall at an open chain's
// end); with compensation in the control, a span of P longer than kSpanCheckMin that has no counterpart in C (its
// offset line or arc, same direction, within kSameCurveTol): an inside arc of radius at most R or a gap or corner
// narrower than the tool, where the control would gouge or alarm; and C made of more pieces than P. In the computer or
// wear such a span is a warning (material is left there, nothing is gouged).
//
// ROOM. Where the tool centre may go at depth z: everything outside the material grown by R + stockToLeave, the
// material being the union of the slices of every part and fixture body at every pass depth at or above z
// (kernel::sliceAt: the material just above each). C is checked against it in every mode (in the control too: linking
// works on C), and linking checks the leads against it.
//
// HEIGHTS (absolute WCS Z, nm): stock top T = stockBounds.max.z; clearance = T + heights.clearance, retract = T +
// heights.retract, feed plane = T + heights.feedPlane, top = T + heights.top; per chain bottom = zc + heights.bottom +
// stockToLeaveAxial. Refused unless clearance >= retract >= feed plane >= top > bottom and retract >= T.
// DEPTHS: with s = maxStepdown > 0, n = ceil((top - bottom) / s) depths z_k = top - round((top - bottom) k / n) (k
// = 1..n, half to even, exact integers; z_n = bottom); s = 0: one depth at the bottom. At most kMaxDepths. ORDER: per
// chain (chainEdges' order, then faces in selection order), per depth from the top, the radial passes from the
// outermost in; then the finish pass and the spring pass at the bottom. linkPasses keeps this order (keepOrder).
//
// LEADS (link.h Lead, side = the tool side): "arc" radius leadRadius (0: the tool diameter), sweep leadSweep, straight
// part leadLength; "line" a tangent line of leadLength (0: the tool diameter); "none". Linking shrinks a lead that does
// not fit the room down to minRadius (a quarter of it, at least 1 um) and refuses it below. Compensation (control and
// wear) needs a straight part on both leads (link.h): refused otherwise. (In the control the programmed lead arc is
// the tool's grown by R, below, so the control's own offset of it never vanishes.)
//
// COMPENSATION. Move::compensation on every pass and lead move: 0 in the computer; 1 (G41) or 2 (G42) for the tool
// side in the control and wear; linking switches it on on the lead-in's first (straight) move and off on the
// lead-out's last. In the control the linked toolpath (made on C, so linking's material model and its lead checks are
// the tool's) is turned into the programmed one: each lead move's points move by R towards the material, -R N(t)
// with N the left (G41) or right (G42) normal of the move's direction there (arc centres stay, so a lead arc's radius
// grows by R), the pass's moves become P's, and the moves at the lead-in's start and the lead-out's end (the approach,
// entry and retract, where compensation is off and the tool centre is the programmed point) move with them. Refused
// when such a point is closer to the material than R + stockToLeave (the tool would touch the part there).
//
// TABS (tabs on; closed and open chains): tabCount centres spread evenly along the chain, at (i + 1/2) L / n from its
// start (closed: from its point nearest the start of the innermost pass, so no tab crosses a pass's start), mapped onto
// each pass by its nearest point (and left off a pass farther from it than the pass's offset plus 10 um). On every pass
// with z below zt = bottom + tabHeight (refused when zt is not below the top), along the pass's path with arc length c
// of a tab, width w = tabWidth:
//   rectangular: straight up to zt at c - w/2 - R, along at zt to b = c + w/2 + R; down: a tool that cuts at its centre
//     straight down at b; any other ramps back along the path at rampAngle from b + Lr (Lr = (zt - z) / tan(rampAngle))
//     down to b and cuts forward to b + Lr again, so the tab keeps its square side and nothing plunges. Refused for
//     such a tool with compensation (G41/G42 cannot run backwards) or no ramp angle. The ramp is the slope-capped
//     link::appendSloped, so no piece of it is steeper than rampAngle after rounding: Lr is kRampSlack longer, and
//     where the path there is in many short pieces longer by a nanometre of descent per piece until it fits (added
//     after M3; a triangular tab's flanks with such a tool are capped the same way where they can be).
//   triangular: up along the path from c - w/2 - R to zt at c - R, along to c + R, down to z at c + w/2 + R: a flat
//     tool leaves a triangle w wide at its foot. Refused for a tool that does not cut at its centre when the flank at
//     the bottom (atan(2 tabHeight / w)) is steeper than rampAngle.
//   Tabs that overlap one another or run past the pass's end are refused.
// Straight up and down tab moves have the plunge feed; the rest of a tab the cutting feed (ramps: the ramp feed).
//
// ENTRY (linking): free from outside the stock or through area already cut, else a ramp along the lead-in or the
// contour at rampAngle (planar passes; passes with tabs only along their lead-in), else a plunge for a tool that cuts
// at its centre (ball end mills, unless the tool library says one does not: Tool::cutsAtCentre); compensation allows
// no ramp (link.h). Linking's refusal names why.
//
// CHECKS (InvalidArgument unless said): tool type (flat, bull, ball end mill, face mill) and diameter; flute length
// against the deepest cut of a pass (the stepdown, or the whole depth for the finish and spring passes); stick-out
// against the stock top - bottom plus kHolderClearanceMm; spindle speed set, clockwise and inside the machine's range;
// cutting feed set; heights; tabs. Errors and warnings: docs/operation_types.md.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops::contour2d {

inline constexpr geom2d::Coord kProbeOffset = 10'000;     // 10 um either side of the chain
inline constexpr geom2d::Coord kProbeBelow = 2'000;       // the second probe level, 2 um under the chain's top
inline constexpr geom2d::Coord kSpanCheckMin = 2'000;     // spans shorter than 2 um are not checked for a counterpart
inline constexpr geom2d::Coord kSameCurveTol = 2'000;     // a counterpart lies on the offset curve within 2 um
inline constexpr geom2d::Coord kRoomTol = 1'500;          // C may leave the room by 1.5 um (link.h's room test)
inline constexpr geom2d::Coord kEntryClearance = 500'000; // a ramp starts 0.5 mm above the material it enters
inline constexpr int kMaxDepths = 1000;
inline constexpr double kHolderClearanceMm = 1.0; // the holder stays this far above the stock top
inline constexpr double kRampSlack = 1e-4;        // tab ramps are this much longer than their angle needs

enum class Comp : std::uint8_t { Computer, Control, Wear };
enum class TabShape : std::uint8_t { Rectangular, Triangular };

// One tab on one pass: its centre's arc length along the pass's path.
struct Tab {
    double centre = 0;
};

// One cut pass as it is linked: `pass` carries C's moves (with tabs and compensation flags); in the control
// `programmed` carries P's moves the toolpath gets instead (same tabs), and `programmedStart` P's start point.
struct PlannedPass {
    link::Pass pass;
    geom2d::Path centre;     // C, at pass.start.z, started
    geom2d::Path programmed; // P (control only; empty otherwise)
    std::vector<Move> programmedMoves;
    std::size_t chain = 0;
    int depth = 0;  // 1-based depth index; finish and spring: the last
    int radial = 0; // j (0: the innermost); -1 finish, -2 spring
};

struct Contour2dPlan {
    Comp comp = Comp::Computer;
    geom2d::Coord toolRadius = 0;
    std::vector<PlannedPass> passes; // in cutting order
    link::Options link;
    link::Stock stock;
    link::Linked linked;     // linkPasses of the passes
    std::vector<Move> moves; // the toolpath's moves (the linked ones; programmed in the control)
    Nm startX = 0, startY = 0, startZ = 0;
    std::vector<std::string> warnings;
};

// The plan of an operation, linked, or the first problem in words a machinist reads (InvalidArgument for what the user
// must change, ToolpathFailed for geometry the tool cannot cut, KernelFailure / Cancelled from the kernel).
Result<Contour2dPlan> planContour2d(const GenerateInput& in, JobContext& ctx);
// The toolpath of a plan (deterministic; no kernel calls).
Toolpath contour2dToolpath(const Contour2dPlan& plan);

// ---------- Helpers the tests call directly ----------
// The depths from top to bottom (DEPTHS above): z_1 > ... > z_n = bottom. Empty when top <= bottom or n > kMaxDepths.
std::vector<Nm> depthLevels(Nm top, Nm bottom, Nm maxStepdown);
// The arc length of the point of `p` nearest to `q` (the first such point along the path).
double nearestS(const geom2d::Path& p, geom2d::Pt q);
// The moves of a planar pass along `path` at height z with tabs (TABS above): rising to zt at each tab. `feed`,
// `plungeFeed`, `rampFeed` in um/min; `comp` on every move.
struct TabMoves {
    TabShape shape = TabShape::Rectangular;
    Nm zt = 0;
    double width = 0;  // nm
    double radius = 0; // the tool radius, nm
    bool centreCutting = false;
    double rampAngleDeg = 0;
};
Result<std::vector<Move>> tabbedMoves(const geom2d::Path& path, Nm z, const std::vector<Tab>& tabs, const TabMoves& t,
                                      std::int32_t feed, std::int32_t plungeFeed, std::int32_t rampFeed,
                                      std::uint8_t comp);

std::unique_ptr<IOperationType> makeContour2dOperationType();

} // namespace ray::cam::ops::contour2d

namespace ray::cam::ops {
using contour2d::makeContour2dOperationType;
}
