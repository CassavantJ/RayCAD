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

// Constants of the host's own motion, in nanometres. Documented in post_api.d.ts terms in the engine header.
constexpr Nm kLinearizeToleranceNm = 1'000;      // chord error when an arc is replaced by lines
constexpr std::int64_t kMaxSegmentsPerArc = 65'536;
constexpr Nm kPeckReentryNm = 500'000;           // expanded peck: rapid back down to 0.5 mm above the last depth
constexpr Nm kChipBreakBackoffNm = 200'000;      // expanded chip break: back off 0.2 mm between pecks

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
// other move, or the end of the section; mapped to the cancel move or the run's last hole). The run's clearance is
// the tool's Z before its first hole (at least the retract plane); after each hole the host assumes the tool is back
// at that level at the hole, as with G98.
// Cycles it does not declare: rapid to the hole at the clearance level, rapid to the retract plane, then
//   drill      feed to the bottom, dwell, rapid out to the clearance level
//   peck       feed in steps of `peck`, rapid to the retract plane after each step and back down to
//              kPeckReentryNm above the last depth; at the bottom dwell, rapid out
//   chipBreak  feed in steps of `peck`, backing off kChipBreakBackoffNm by rapid between steps; dwell, rapid out
//   bore, ream feed to the bottom, dwell, feed back to the retract plane, rapid out
// A peck or chip break with peck 0 is drilled in one feed.
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
