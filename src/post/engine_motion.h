// Host services between the toolpath IR and the script's move handlers (PLAN.md T-039): arcs split to the post's
// maxArcDegrees or linearised, canned cycles the post does not declare expanded into rapid, linear and dwell moves,
// modal cycle runs tracked. Works in integer nanometres; unit conversion to the script happens in the engine.
// Internal to src/post; tested directly by tests/unit/post.
#pragma once
#include <ray/cam.h>
#include <ray/core.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ray::post {

enum class ArcOutput : std::uint8_t { IJK, R, None }; // capabilities.arcs
enum class CycleKind : std::uint8_t { Drill, Peck, ChipBreak, Tap, Bore, Ream };
constexpr std::size_t kCycleKindCount = 6;

std::string_view cycleKindName(CycleKind kind);                    // "drill", "peck", "chipBreak", ...
std::optional<CycleKind> cycleKindFromName(std::string_view name); // inverse; nullopt for anything else
std::optional<CycleKind> cycleKindOfMove(cam::MoveKind kind);      // CycleDrill..CycleReam

// What the post declared (definePost capabilities), as the expander needs it.
struct MotionCaps {
    ArcOutput arcs = ArcOutput::IJK;
    bool helical = true;
    double maxArcDegrees = 360.0;               // (0, 360]
    std::array<bool, kCycleKindCount> cycles{}; // canned cycles the post emits itself
    bool cutterComp = false;
};

// Constants of the host's own arc linearisation, in nanometres.
constexpr Nm kLinearizeToleranceNm = 1'000; // chord error when an arc is replaced by lines
constexpr std::int64_t kMaxSegmentsPerArc = 65'536;

// Canned cycles are read as cam.h "Canned cycles" defines them; nothing about them is assumed here. The backplot
// (src/sim) expands them the same way, and tests/unit/cycles checks that the two agree.

enum class EventKind : std::uint8_t { Rapid, Linear, Arc, Cycle, CycleEnd, Dwell };

// One call of a script handler. Every event names the IR move it came from (Toolpath::moves index); an arc split in
// four or a drill cycle expanded into nine moves gives several events with the same index.
struct MotionEvent {
    EventKind kind = EventKind::Rapid;
    std::uint32_t move = 0;
    Nm x = 0, y = 0, z = 0; // end point; for Cycle the hole (x, y) and z = bottom
    std::int32_t feed = 0;  // micrometres per minute (Linear, Arc, Cycle)
    cam::MoveRole role = cam::MoveRole::Cut;
    std::uint8_t comp = 0; // 0 off, 1 left, 2 right
    // Arc: direction as G2/G3 (seen from the positive end of the plane normal: +Z for XY, +Y for XZ, +X for YZ).
    bool cw = false;
    cam::ArcPlane plane = cam::ArcPlane::XY;
    Nm cx = 0, cy = 0, cz = 0; // absolute centre; the out-of-plane coordinate is the start's
    Nm i = 0, j = 0, k = 0;    // centre minus start (0 along the plane normal)
    double radiusNm = 0;       // distance from the start to the centre
    double sweepDegrees = 0;   // (0, 360]
    // Cycle: the move's values as the IR has them (cam.h); peckClearance 0 leaves it to the control
    CycleKind cycle = CycleKind::Drill;
    Nm top = 0, retract = 0, clearance = 0, peck = 0, peckClearance = 0;
    cam::CycleReturn returnLevel = cam::CycleReturn::RPlane;
    std::int32_t dwellMs = 0;
    std::int32_t pitchNm = 0;
    bool leftHand = false;
    bool first = false; // first hole of a run, or a hole whose cycle parameters changed
};

class MotionSink {
public:
    virtual ~MotionSink() = default;
    virtual bool onEvent(const MotionEvent& event) = 0; // false stops the expansion (the script failed)
};

// Turns one section's toolpath into handler events, in order. `machine` (may be null) gives the peck clearance of
// expanded cycles whose move leaves it to the control (cam::peckClearanceNm).
// Returns true when every move was handed on, false when the sink stopped, and PostFailed for a toolpath the post
// cannot take: cutter compensation in the control without capabilities.cutterComp, a tapping cycle the post does
// not declare (it needs spindle reversal; it is never expanded), a feed move without a feed, a cycle whose levels are
// out of order (cam.h: bottom < top <= R plane <= clearance), a cycle run whose first hole starts below its clearance.
//
// Runs are cam.h's (same kind, left-hand flag and clearance). Cycles the post declares: one Cycle event per hole, a
// CycleEnd event when the run ends (a CycleCancel move, any other move, a new run, or the end of the section; mapped
// to the cancel move or the run's last hole). When the tool is above the clearance at a run's first hole, Rapid
// events take it over the hole and down to the clearance first (cam.h motion steps 1 and 2), so the control's initial
// level is the clearance. After each hole the tool is at (x, y, return level).
// Cycles it does not declare are expanded into Rapid, Linear and Dwell events with exactly the motion of cam.h
// (steps 1 to 4, a dwell at the bottom when the move has one, zero-length rapids left out), the same as the backplot.
// Arcs: split into equal pieces of at most maxArcDegrees (with arcs "R", every piece under 180 degrees); with arcs
// "none", or a helical arc and helical off, replaced by lines within kLinearizeToleranceNm.
Result<bool> expandSection(const cam::Toolpath& path, const lib::MachineDef* machine, const MotionCaps& caps,
                           MotionSink& sink);

// Sweep of an arc move from `start`, in radians, (0, 2 pi]; also its radius (start to centre) in nanometres.
struct ArcGeometry {
    double sweep = 0;
    double radiusNm = 0;
    bool fullCircle = false;
};
ArcGeometry arcGeometry(const cam::Move& arc, Nm startX, Nm startY, Nm startZ);

// Numbers for the Section object: Z range of the moves (and the start point; a cycle adds its R plane and
// clearance), length of the feed moves, and a time estimate: feed moves at their feed, rapids at `rapidMmPerMin`
// (none counted when 0), dwells. A cycle counts as feeding in and out between its R plane and bottom and leaves the
// tool at its return level.
struct SectionStats {
    Nm minZ = 0, maxZ = 0;
    double cutLengthNm = 0;
    double seconds = 0;
};
SectionStats sectionStats(const cam::Toolpath& path, double rapidMmPerMin);

} // namespace ray::post
