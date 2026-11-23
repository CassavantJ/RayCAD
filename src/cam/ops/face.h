// The "face" operation type (task T-052), private to src/cam and its tests: the whole top of the setup's stock faced
// flat by parallel strokes, zigzag or one way, at one or more depths. Parameters: schemas/ops/face.schema.json. The
// pattern every operation type follows is docs/operation_types.md; drill.h is its first instance.
//
// WHAT IS FACED. The stock box (ResolvedGeometry::stockBounds, in the WCS), not a picked face: the operation takes no
// geometry. Its top is S = stockBounds.max.z; the part top P is the highest WCS Z of the part bodies' boxes
// (kernel::boundsIn in the WCS frame), or S when the setup has no part bodies.
//
// LEVELS, absolute WCS Z in integer nanometres, every millimetre value rounded once with toNm (half to even):
//   clearance = S + heights.clearance, retract = S + heights.retract, feed plane = S + heights.feedPlane,
//   top T = S + heights.top (where the material starts), bottom B = P + heights.bottom (the faced height).
//   They must be in order: clearance >= retract >= feed plane >= T > B, retract >= S (linking's rule), B >= P (below
//   the part top the strokes would cut the part), and the clearance at or above S, P and every fixture's top.
//   Depths: n = ceil((T - B) / stepdown) passes, pass k (1..n) at z_k = T - floor(k (T - B) / n): equal steps of at
//   most the stepdown, the last exactly at B (faceLevels).
//
// STROKES. In the stroke frame: u along the strokes (X for "alongX", Y for "alongY"), v across them (the other axis);
// the stock spans [u0, u1] x [v0, v1]. R = toNm(D / 2) is the tool's radius, F = R - toNm(corner radius) the radius of
// its flat bottom (the flat width is 2F), a = toNm(overhang).
//   Along: every stroke runs from u0 - R - a to u1 + R + a (or back), so at both ends the tool is a clear of the stock:
//   it feeds down there in the air and the links between strokes stay outside the stock.
//   Across: the flat bands [v - F, v + F] of the strokes cover [v0 - a, v1 + a]. With L = (v1 + a - F) - (v0 - a + F):
//   L <= 0: one stroke at v = (v0 - a) + floor((v1 - v0 + 2a) / 2); else m = ceil(L / s) + 1 strokes at
//   v_j = (v0 - a + F) + floor(j L / (m - 1)), j = 0..m-1, where s is the stepover (param stepover; 0: floor(3 (2F) /
//   4)) and must be at most 2F. Neighbours are at most ceil(L / (m - 1)) <= s <= 2F apart, so the bands leave no gap
//   (strokePositions). The coverage of the stock rectangle by the bands of the last level is checked once more with
//   geom2d (sweptArea + subtract) before the toolpath is made; a gap is an Internal error (faceUncovered).
//   Direction: with the spindle turning clockwise (M3) a stroke is climb when the uncut stock (higher v) is on its
//   left: +u for alongX (+X, stock at +Y), -u for alongY (-Y, stock at +X). One way: every stroke climb, or every
//   stroke the other way for "conventional". Zigzag: the first stroke climb, then alternating. A zigzag level after the
//   first runs the previous level's strokes backwards (last stroke first, each reversed), so it starts where that one
//   ended.
//
// LINKING (link.h, T-051): one pass per stroke and level, passFromPath at the level's z with the cut feed, in the order
// above (keepOrder; no pass reversible). Every stroke starts beside the stock, so each entry is a feed descent in the
// air at the plunge feed (Entered::Free; feedPlunge 0: the cut feed). Zigzag: maxStayDown = s, so the link to the next
// stroke (at most s long, outside the stock) and the drop to the next level (at the same XY) stay down at the cut feed.
// One way: maxStayDown 0, so every link retracts (feed up to the feed plane, rapid to the retract plane and across,
// rapid down to the feed plane), as the way back crosses uncut stock anyway. The stock is the stock box
// (link::stockOf).
//
// CHECKS (each an error naming the cause; docs/operation_types.md): the tool a face mill, flat or bull nose end mill
// with a flat bottom (F > 0); the stepover at most 2F; the spindle speed set, within the machine's range, clockwise; a
// cut feed; the heights in order (above); a stock box; nothing of the parts above B and no fixture above B inside the
// area the tool sweeps; every pass's depth (the step, or from S down for the first pass when T < S) within the flute
// length and the stick-out less kHolderClearanceMm; at most kMaxStrokes strokes in all; the spans within the machine's
// travel where it gives them. Warning: passes wholly above the stock top (T above S by a stepdown or more) cut air.
#pragma once

#include "../geom2d.h"
#include "../link.h"

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr std::size_t kMaxStrokes = 100'000;    // strokes of one operation (all levels)
inline constexpr std::size_t kMaxFaceLevels = 10'000;  // depth passes of one operation
inline constexpr double kFaceHolderClearanceMm = 1.0;  // the holder stays this far above the material
inline constexpr double kFaceCoverageSlackNm2 = 1.0e6; // 1 um^2: what faceUncovered may report and still pass

// The pass heights from the top T down to the bottom B (T > B) in steps of at most `stepdown` (> 0), as LEVELS says.
std::vector<Nm> faceLevels(Nm top, Nm bottom, Nm stepdown);
// The across positions of the strokes whose flat bands (half width flatHalf > 0) cover [lo, hi], at most `spacing`
// (0 < spacing <= 2 flatHalf) apart, as STROKES says.
std::vector<Nm> strokePositions(Nm lo, Nm hi, Nm flatHalf, Nm spacing);

struct FaceStroke {
    geom2d::Pt from, to;
    Nm z = 0;
};

// Everything generate needs, worked out (and checked) once.
struct FacePlan {
    std::vector<FaceStroke> strokes; // in cutting order, every level
    std::vector<Nm> levels;          // the pass heights, top first; levels.back() is the faced height
    std::size_t strokesPerLevel = 0;
    Nm stockTop = 0, partTop = 0, top = 0, bottom = 0;
    Nm toolRadius = 0, flatHalf = 0, stepover = 0, overhang = 0;
    geom2d::Region stockRect; // the stock box's XY
    link::Options link;
    link::Stock stock;
    std::int32_t feed = 0; // um/min
    std::vector<std::string> warnings;
};

// The plan of an operation, or the first problem in words a machinist reads (InvalidArgument for what the user must
// change, ToolpathFailed for a setup that cannot be faced, KernelFailure from measuring the bodies).
Result<FacePlan> planFace(const GenerateInput& in, JobContext& ctx);
// The area (nm^2) of the stock rectangle the flat bottoms of the last level's strokes leave uncovered: geom2d sweptArea
// of those strokes with radius flatHalf, subtracted from the stock rectangle. 0 for a correct plan.
Result<double> faceUncovered(const FacePlan& plan, JobContext& ctx);
// The toolpath of a plan: the strokes linked by link::linkPasses (Cancelled, or linking's own errors).
Result<Toolpath> faceToolpath(const FacePlan& plan, JobContext& ctx);

std::unique_ptr<IOperationType> makeFaceOperationType();

} // namespace ray::cam::ops
