// The "drill" operation type (task T-035), private to src/cam and its tests: spot drilling, drilling, peck drilling,
// chip breaking, tapping, boring and reaming as canned-cycle moves (cam.h "Canned cycles"), one move per hole.
// Parameters: schemas/ops/drill.schema.json. docs/operation_types.md describes the pattern every operation type in
// src/cam/ops follows; this file is its first instance.
//
// HOW A HOLE IS DRILLED. The operation's geometry references are faces of holes; each selects the recognised hole
// (cam.h recognizeHoles, with the thread pickup holeOptionsFor) whose faces include it. A selected element that is not
// a face of a hole recognised along setup Z is an error naming where it is. Holes are drilled in the shortest-path
// order from the work origin (orderHolesShortestPath from (0, 0)), or in selection order (param holeOrder).
//
// LEVELS, per hole, absolute WCS Z (Operation::heights relative to each hole's top, cam.h Heights):
//   cycleTop = hole top + heights.top          cycleRetract (R) = hole top + heights.retract
//   cycleClearance (initial level) = the highest hole top of the operation + heights.clearance, one level for the
//   whole operation, so the operation is one cycle run. heights.feedPlane and heights.bottom are not read.
// RETURN (G98 / G99): after each hole the tool goes to the next at the return level. G99 (the R plane) only when that
// R plane is at or above everything the setup has: the top of the stock box, of every part body and of every fixture
// body in the WCS (their boxes, so tilted bodies count conservatively); otherwise, and after the last hole, G98 (the
// initial level). The first move is a rapid at the initial level to the first hole, so the run starts there.
//
// DEPTH: the Z of the tool tip, `z` of the cycle move. All arithmetic after the tool's point length is in integer
// nanometres.
//   Point length L of a tool of diameter D, tip angle a and tip flat d:  L = ((D - d) / 2) / tan(a / 2), computed in
//   IEEE double as  half = (D - d) / 2,  t = dmath::tan((a * kRadPerDeg) / 2),  then rounded once to nanometres
//   with toNm (round half to even): pointLengthNm(). A tool with a <= 0 or a >= 180 (flat: end mills, flat-bottom
//   drills, a tool without a point) or with d >= D has L = 0.
//   depthFrom "value": depthReference fullDiameter: tip = hole top - depth - L; tip: tip = hole top - depth.
//   depthFrom "hole", by hole bottom (B = the bore, the hole's lowest cylinder; cam.h RecognizedHole):
//     through:  tip = exit - breakthrough - L (the full diameter goes `breakthrough` past the exit).
//     conical:  if the tool is pointed and its tip angle is within kSameAngleDeg of the hole's cone, the tip goes to
//               the cone's apex (the modelled point); otherwise the full diameter goes to the bottom of B, tip = B
//               bottom - L, and a tip that ends below the modelled apex is reported as a warning with the distance.
//     flat:     the tip stops at the floor (warning: the floor keeps a drill point of L) unless allowBelowFloor, when
//               the full diameter reaches the floor and tip = floor - L.
//   Spot: depthFrom "hole": the spot diameter S (param spotDiameter, else the top diameter of the hole's entry cone
//     when it is chamfered or countersunk, else B's diameter) at the hole top: tip = hole top - pointLengthNm(S, a, d).
//     The spot drill must be pointed and at least S wide.
//   Tap (the tap's pitch P, lead = toNm(tapLeadPitches * P)): depthFrom "hole": with a thread depth T from the
//     hole's thread, tip = B top - T - lead; without one, blind: tip = B bottom (the drilled full-diameter depth);
//     through: tip = exit - breakthrough - lead. depthFrom "value": as above with L = lead. Always: a blind hole's tap
//     tip must not go below B's bottom (the drilled depth).
// FEED: feedMicronPerMin = feedCut x 1000 rounded half to even; taps: threadPitchNm x rpm / 1000 rounded half to even
// (cam.h) with threadPitchNm = toNm(P) and the whole-number rpm. Bore and ream feed out at the same feed.
// CYCLE: spot and drill CycleDrill; peck CyclePeck and chipBreak CycleChipBreak with cyclePeck = toNm(peck) (a peck
// of 0 drills in one feed: CycleDrill) and cyclePeckClearance = toNm(peckClearance) (0: the control's); tap CycleTap
// (left-hand from the hole's thread spec, "LH" in it, else param leftHand); bore CycleBore; ream CycleReam.
// cycleDwellMs = dwell x 1000 rounded half to even, on every kind.
//
// VALIDATE runs the same plan as generate (planDrill) and returns its first error; the warnings of the plan go into
// Toolpath::warnings when generate runs (Status cannot carry them). Errors and warnings: docs/operation_types.md.
#pragma once

#include <ray/cam.h>

#include <memory>
#include <string>
#include <vector>

namespace ray::cam::ops {

inline constexpr double kRadPerDeg = 0.017453292519943295; // pi / 180, rounded to double
inline constexpr double kSameAngleDeg = 0.5;               // a tool point matches a hole's cone within this
inline constexpr double kDiameterTolMm = 0.001;            // a tool counts as larger than a hole beyond this
inline constexpr double kThreadDiameterTolMm = 0.01;       // tap and thread nominal diameters match within this
inline constexpr Nm kThreadPitchTolNm = 1000;              // tap and thread pitches match within 1 um
inline constexpr double kHolderClearanceMm = 1.0;          // the holder stays this far above a hole's top

enum class DrillCycle { Spot, Drill, Peck, ChipBreak, Tap, Bore, Ream };

// The point length L of a tool (see DEPTH above), in nanometres.
Nm pointLengthNm(double diameterMm, double tipAngleDeg, double tipDiameterMm);
// The tapping feed: threadPitchNm x rpm / 1000 rounded half to even (cam.h); rpm must be a whole number >= 1.
std::int64_t tapFeedMicronPerMin(Nm threadPitchNm, std::int64_t rpm);
// True when a thread designation names a left-hand thread: "LH" as a word of it ("M8x1-LH", "1/4-20 UNC-2B LH").
bool isLeftHandThread(std::string_view spec);

// One hole as it is drilled.
struct PlannedHole {
    std::size_t part = 0; // index into ResolvedGeometry::partShapes
    RecognizedHole hole;
    Nm top = 0, retract = 0, tip = 0;
    bool leftHand = false;
    CycleReturn ret = CycleReturn::Initial;
};

// Everything generate needs, worked out (and checked) once.
struct DrillPlan {
    DrillCycle cycle = DrillCycle::Drill;
    MoveKind kind = MoveKind::CycleDrill;
    std::vector<PlannedHole> holes; // in drilling order
    Nm clearance = 0;               // the initial level
    Nm safeTravelZ = 0;             // the top of the stock, the parts and the fixtures
    std::int32_t feed = 0;          // um/min
    std::int32_t dwellMs = 0;
    Nm peck = 0, peckClearance = 0;
    std::int32_t pitchNm = 0;
    std::vector<std::string> warnings;
};

// The plan of an operation, or the first problem in words a machinist reads (InvalidArgument for what the user must
// change, ToolpathFailed for geometry that cannot be drilled, KernelFailure / Cancelled from recognition).
Result<DrillPlan> planDrill(const GenerateInput& in, JobContext& ctx);
// The toolpath of a plan (deterministic; no kernel calls).
Toolpath drillToolpath(const DrillPlan& plan);

std::unique_ptr<IOperationType> makeDrillOperationType();

} // namespace ray::cam::ops
