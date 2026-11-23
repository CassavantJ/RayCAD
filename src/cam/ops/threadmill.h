// The "threadmill" operation type (task T-056), private to src/cam and its tests: helical thread milling of internal
// threads (in drilled holes) and external threads (on bosses and pins), single-form or multi-form, climb or
// conventional, in radial passes, with a half-circle arc lead-in and lead-out. Parameters:
// schemas/ops/threadmill.schema.json; the pattern every type follows: docs/operation_types.md.
//
// FEATURES. Each selected element must be a face of a part body:
//   internal: a face of a hole recognised along the setup's Z (recognizeHoles with the thread pickup holeOptionsFor);
//     its main bore B (RecognizedHole::bore) is the drilled hole: centre (x, y), diameter d, top B.zTop;
//   external: a cylinder whose axis is the setup's Z (within 1e-9) and whose material is inside it (a boss or pin);
//     centre, diameter d and top and bottom (its edges' heights, kernel::edgeCurves).
//   Features are machined in the shortest-path order from the work origin (orderHolesShortestPath from (0, 0)).
// THREAD DATA, per feature (integer nanometres, toNm, from here on):
//   pitch P: param pitch > 0, else the hole's thread pitch (internal); none: refused ("no thread data"). A pitch given
//     and a hole thread whose pitch differs by more than 1 um: refused.
//   major diameter D: param diameter > 0 (a hole thread's nominal diameter that differs by more than 0.01 mm:
//     refused), else internal: the hole thread's nominal diameter (none: refused); external: d.
//   hand: a hole with a thread spec: left-hand when "LH" is a word of it (drill.h isLeftHandThread); else param
//     leftHand.
//   length L from the top T of the thread (internal: B.zTop; external: the cylinder's top): param threadLength > 0,
//     else internal: the hole thread's depth when > 0, else the whole bore (B.zTop - B.zBottom); external: the
//     cylinder's length. Bottom Zb = T - L, which must not be below B's bottom (internal) or the cylinder's bottom.
//   radial depth h: param threadDepth > 0, else the ISO 68-1 basic profile with H = sqrt(3) / 2 P: inside 5/8 H
//     (= (D - D1) / 2, D1 the basic minor diameter), outside 17/24 H (to the external minor diameter d3), rounded to
//     nm.
// PATH RADII (the tool centre's circle about the feature's centre; Dt = toNm(tool diameter)):
//   internal: the tool's teeth reach D at the last pass: Rf = (D - Dt) / 2; the passes k = 1..n (param radialPasses)
//     at R_k = Rf - i_k; external: they reach D - 2h: Rf = (D - 2h + Dt) / 2, R_k = Rf + i_k; the inset
//     i_k = 2 round(h (n - k) / (2 n)), an even number of nm so every R_k has the parity of Rf (the leads inside a hole
//     then all start and end at one point). Each rounding is half to even, so the last pass is at Rf exactly and the
//     thread's diameter
//     is exact whenever D - Dt (inside) or D - 2h + Dt (outside) is an even number of nanometres. springPass adds one
//     more pass at Rf.
// DIRECTION: with the spindle clockwise (M3) climb milling travels counter-clockwise inside a hole and clockwise
//   around a boss; conventional, or a counter-clockwise spindle, reverses it (each one flip). A right-hand thread
//   rises with counter-clockwise travel, so the tool travels up when (counter-clockwise == right-hand), else down:
//   inside, right-hand, climb: counter-clockwise from the bottom up (G3); outside, right-hand, climb: clockwise from
//   the top down (G2). The spindle must turn clockwise (every library tool cuts right hand), as for drilling.
// THE HELIX of a pass at radius R: it starts at angle 0, (x + R, y), and every revolution is two arcs about (x, y)
//   (geom2d's moves split at 180 degrees): to (x - R, y) at a Z change of hp = P / 2 (rounded half to even) and back
//   to (x + R, y) at a Z change of exactly P. So every revolution rises or falls exactly P, the arcs' ends lie exactly
//   on the circle, and every pass and step of a feature lies on one helix.
//   single-form: N = ceil(L / P) revolutions from Zb up to Zb + N P (up) or down from Zb + N P to Zb.
//   multi-form: the tool has t = floor(flute length / P) teeth (t >= 1); steps of t P, s = ceil(L / (t P)) of them,
//     step j at Zb_j = Zb + j t P; with the overlap o (degrees) and oz = P o / 360 rounded half to even: up, from Zb_j
//     at angle 0, one revolution to Zb_j + P, then o degrees on to Zb_j + P + oz; down, from Zb_j + P + oz at angle 0,
//     one revolution, then o degrees on to Zb_j. The overlap's end point is rounded onto the circle. An overlap whose
//     chord on the thread's smallest pass would be under kMinOverlapChordNm is left out (o = 0): a shorter arc is a
//     full turn or nothing to a control. Steps are cut in
//     the direction of travel (up: j = 0, 1, ...; down: from the top step).
//   Passes are cut radial pass by radial pass (the spring pass last), each over every step.
// LEADS (link::Lead::Arc, sweep 180, at the pass's start and end height, never made smaller): internal, radius R / 2
//   on the side of the centre, so the lead-in starts and the lead-out ends at the hole's centre (a radial lead-in);
//   external, radius (Dt + h) / 2 on the outside, so the tool starts and ends a tool diameter plus the thread depth
//   outside the pass. The room for the leads: inside, the pass's circle; outside, everything outside it.
// FEEDS: the cut feed F = feedCut x 1000 um/min rounded half to even; with edgeFeed the tool centre's feed of a pass is
//   F R / (R + Dt / 2) inside and F R / (R - Dt / 2) outside (the cutting edge then moves at F), rounded half to even.
//   Lead-in, lead-out and plunge feeds: the operation's (0: the pass's feed).
// LEVELS: Top = the highest feature top (internal: the hole's top; external: the cylinder's top); clearance, retract
//   and feed plane = Top + heights.clearance, retract, feedPlane (in that order, feedPlane >= 0). A pass may not rise
//   above the feed plane (a single-form helix ends up to one pitch above the thread's top).
// LINKING (link::linkPasses, the given order): the drilled hole is Stock::cleared (its bore's disc, free from its
//   bottom up: the linker does not know the drilling operation); around a boss, the stock box less the part's slice
//   at the boss's bottom (geom2d::sliceRegions; the part is taken as already machined there). The tool goes down to
//   the lead-in's start through that free area. Inside a hole consecutive passes stay down (maxStayDown 1 um: the link
//   is a move along Z at the hole's centre, where the lead-out ends and the next lead-in starts); every other link
//   retracts (outside, and from hole to hole). A feature that cannot be entered that way is refused by linking. A link
//   under 1 um (a lead-out's end and the next lead-in's start are rounded points) followed by a straight move is merged
//   into that move.
//   (Links between holes are not left to linking's cut-area model on purpose: geom2d::sweptArea is wrong for a pass
//   whose radius equals the tool's radius together with another path, a known problem reported with T-056.)
// REFUSED, each naming its cause: a tool that is not a thread mill; a tool pitch that does not match the thread's;
//   multi-form with a tool without a pitch or shorter than one pitch; no thread data; a thread mill that does not fit
//   the hole (not smaller than the bore) or whose path radius would be under 0.01 mm; a bore not smaller than the
//   thread; a thread depth that leaves no minor diameter; a neck (above the flutes: the narrower of the cutting and
//   shank diameters, libraries.h having no neck diameter) or a shank (above the shoulder) that goes into the thread's
//   length and would rub the bore or hit the boss's thread; flutes or stick-out too short; heights out of order; a
//   thread longer than the bore or the boss.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr Nm kMinThreadPathRadiusNm = 10'000; // 0.01 mm: the smallest tool-centre circle thread milled
inline constexpr Nm kMinOverlapChordNm = 10'000;     // 0.01 mm: a shorter overlap arc is left out (geom2d kMinArcChord)

// One feature as it is threaded.
struct PlannedThread {
    bool internal = true;
    Nm x = 0, y = 0;     // the centre
    Nm top = 0;          // T: the top of the thread
    Nm bottom = 0;       // Zb
    Nm featureTop = 0;   // the hole's or the cylinder's top (for the levels)
    Nm boreDiameter = 0; // internal: the drilled bore; external: the cylinder
    Nm boreBottom = 0;   // internal: where the drilled hole ends (free above)
    Nm pitch = 0, majorDiameter = 0, depth = 0;
    bool leftHand = false;
    bool ccw = true, up = true; // the direction of travel (DIRECTION above)
    std::vector<Nm> radii;      // the radial passes (and the spring pass), first to last
    std::string where;          // "the hole at X.. Y.." for messages
};

struct ThreadmillPlan {
    std::vector<PlannedThread> threads; // in machining order
    bool multiForm = false;
    std::int64_t teeth = 1; // multi-form
    double overlapDeg = 0;
    Nm clearance = 0, retract = 0, feedPlane = 0;
    link::Linked linked;
    std::vector<std::string> warnings;
};

// The direction of travel of a thread (DIRECTION above).
struct ThreadTravel {
    bool ccw = true;
    bool up = true;
};
ThreadTravel threadTravel(bool internal, bool rightHand, bool climb, bool spindleClockwise);
// The basic radial thread depth of pitch P (nm) inside (5/8 H) or outside (17/24 H), rounded half to even.
Nm basicThreadDepthNm(Nm pitch, bool internal);
// The moves of one helical pass about (x, y) at radius R (THE HELIX above): from z0 at angle 0, `revolutions` whole
// revolutions, then `overlapDeg` more, the Z changing by `pitch` per revolution (signed: negative goes down) and
// `overlapZ` over the overlap (signed); feed and role Cut. The pass starts at (x + R, y, z0).
std::vector<Move> helixMoves(Nm x, Nm y, Nm radius, bool ccw, Nm z0, Nm pitch, std::int64_t revolutions,
                             double overlapDeg, Nm overlapZ, std::int32_t feedMicronPerMin);

// The plan, or the first problem in words a machinist reads (InvalidArgument for what the user must change,
// ToolpathFailed for geometry that cannot be threaded or linked, KernelFailure / Cancelled from the kernel).
Result<ThreadmillPlan> planThreadmill(const GenerateInput& in, JobContext& ctx);
// The toolpath of a plan (deterministic; no kernel calls).
Toolpath threadmillToolpath(const ThreadmillPlan& plan);

std::unique_ptr<IOperationType> makeThreadmillOperationType();

} // namespace ray::cam::ops
