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

std::string_view cycleKindName(CycleKind kind);                     // "drill", "peck", "chipBreak", ...
std::optional<CycleKind> cycleKindFromName(std::string_view name);   // inverse; nullopt for anything else
std::optional<CycleKind> cycleKindOfMove(cam::MoveKind kind);        // CycleDrill..CycleReam

// What the post declared (definePost capabilities), as the expander needs it.
struct MotionCaps {
    ArcOutput arcs = ArcOutput::IJK;
    bool helical = true;
    double maxArcDegrees = 360.0;             // (0, 360]
    std::array<bool, kCycleKindCount> cycles{}; // canned cycles the post emits itself
    bool cutterComp = false;
};

// Constants of the host's own arc linearisation, in nanometres.
constexpr Nm kLinearizeToleranceNm = 1'000;      // chord error when an arc is replaced by lines
constexpr std::int64_t kMaxSegmentsPerArc = 65'536;

// ---------- Canned-cycle conventions: ONE place, shared with the backplot (T-037) ----------
// cam.h does not yet say how a cycle Move is to be read. Until it does, the post host and the backplot assume the
// same, and every one of these assumptions lives here so that it changes in one edit:
//  - Move::cycleRetract is the absolute Z of the R plane; Move::z is the hole bottom; Move::(x, y) the hole.
//  - Peck depth (Move::cyclePeck, Q) is measured down from the R plane.
//  - After each hole the tool is back at the R plane (G99): it ends the cycle at (x, y, R), not at the bottom
//    the move's z names, and moves to the next hole at R.
//  - Expanded peck (G83): after each peck, rapid out to R and back down to kPeckReentryNm above the last depth.
//  - Expanded chip break (G73): after each peck, back off kChipBreakBackoffNm by rapid, then feed on.
//  - Bore and ream are G85 (feed in, feed out to R), or G89 (with a dwell at the bottom) when Move::cycleDwellMs > 0.
// Neither clearance distance is in the IR.
namespace cycle_conventions {
constexpr bool kReturnToRetractPlane = true;     // G99; false would return to the level before the run (G98)
constexpr Nm kPeckReentryNm = 500'000;           // 0.5 mm
constexpr Nm kChipBreakBackoffNm = 500'000;      // 0.5 mm
} // namespace cycle_conventions
using cycle_conventions::kChipBreakBackoffNm;
using cycle_conventions::kPeckReentryNm;

enum class EventKind : std::uint8_t { Rapid, Linear, Arc, Cycle, CycleEnd, Dwell };

// One call of a script handler. Every event names the IR move it came from (Toolpath::moves index); an arc split in
// four or a drill cycle expanded into nine moves gives several events with the same index.
struct MotionEvent {
    EventKind kind = EventKind::Rapid;
    std::uint32_t move = 0;
    Nm x = 0, y = 0, z = 0;      // end point; for Cycle the hole (x, y) and z = bottom
    std::int32_t feed = 0;       // micrometres per minute (Linear, Arc, Cycle)
    cam::MoveRole role = cam::MoveRole::Cut;
    std::uint8_t comp = 0;       // 0 off, 1 left, 2 right
    // Arc: direction as G2/G3 (seen from the positive end of the plane normal: +Z for XY, +Y for XZ, +X for YZ).
    bool cw = false;
    cam::ArcPlane plane = cam::ArcPlane::XY;
    Nm cx = 0, cy = 0, cz = 0;   // absolute centre; the out-of-plane coordinate is the start's
    Nm i = 0, j = 0, k = 0;      // centre minus start (0 along the plane normal)
    double radiusNm = 0;         // distance from the start to the centre
    double sweepDegrees = 0;     // (0, 360]
    // Cycle
    CycleKind cycle = CycleKind::Drill;
    Nm top = 0, retract = 0, clearance = 0, peck = 0;
    std::int32_t dwellMs = 0;
    std::int32_t pitchNm = 0;
    bool first = false;          // first hole of a run, or a hole whose cycle parameters changed
};

class MotionSink {
public:
    virtual ~MotionSink() = default;
    virtual bool onEvent(const MotionEvent& event) = 0; // false stops the expansion (the script failed)
};

// Turns one section's toolpath into handler events, in order. topZ is the operation's top height (CycleMove.top).
// Returns true when every move was handed on, false when the sink stopped, and PostFailed for a toolpath the post
// cannot take: cutter compensation in the control without capabilities.cutterComp, a tapping cycle the post does
// not declare (it needs spindle reversal; it is never expanded), a feed move without a feed, a cycle whose bottom
// is not below its retract plane.
//
// Cycles the post declares: one Cycle event per hole, a CycleEnd event when the run ends (a CycleCancel move, any
// other move, or the end of the section; mapped to the cancel move or the run's last hole). The run's clearance
// (CycleMove.clearance) is the tool's Z before its first hole, at least the retract plane. After each hole the host
// takes the tool to be where cycle_conventions says (the R plane with G99).
// Cycles it does not declare (cycle_conventions above): from the tool's Z (raised to R if below it) rapid to the
// hole, rapid down to R, then
//   drill      feed to the bottom, dwell, rapid out to R                            (G81, G82)
//   peck       feed in steps of `peck`, rapid to R after each step and back down to kPeckReentryNm above the last
//              depth; at the bottom dwell, rapid out to R                         (G83)
//   chipBreak  feed in steps of `peck`, backing off kChipBreakBackoffNm by rapid between steps; dwell, rapid out (G73)
//   bore, ream feed to the bottom, dwell, feed back out to R                        (G85, G89)
// and with kReturnToRetractPlane false, a final rapid up to the clearance. A peck or chip break with peck 0 is
// drilled in one feed.
// Arcs: split into equal pieces of at most maxArcDegrees (with arcs "R", every piece under 180 degrees); with arcs
// "none", or a helical arc and helical off, replaced by lines within kLinearizeToleranceNm.
Result<bool> expandSection(const cam::Toolpath& path, Nm topZ, const MotionCaps& caps, MotionSink& sink);

// Sweep of an arc move from `start`, in radians, (0, 2 pi]; also its radius (start to centre) in nanometres.
struct ArcGeometry {
    double sweep = 0;
    double radiusNm = 0;
    bool fullCircle = false;
};
ArcGeometry arcGeometry(const cam::Move& arc, Nm startX, Nm startY, Nm startZ);

// Numbers for the Section object: Z range of the moves (and the start point), length of the feed moves, and a
// time estimate: feed moves at their feed, rapids at `rapidMmPerMin` (none counted when 0), dwells.
struct SectionStats {
    Nm minZ = 0, maxZ = 0;
    double cutLengthNm = 0;
    double seconds = 0;
};
SectionStats sectionStats(const cam::Toolpath& path, double rapidMmPerMin);

} // namespace ray::post
