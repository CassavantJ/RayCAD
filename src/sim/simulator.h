// The simulator (task T-072): makeSimulator and ISimulator (include/ray/sim.h states the contract; this file says how
// this one works, the judgment constants, and gives test hooks).
//
// ONE EXPANSION. The program's IR moves become the straight sub-motions of the safety checks (verify_motion.cpp:
// expandMotions, which follows buildBackplot), so a move index means the same in the backplot, the events and here.
// Arcs are chords within kVerifyArcSagittaMm; canned cycles the rapids and feeds the control makes; each toolpath
// starts with a zero-length rapid at its start point (move 0). Every sub-motion goes into the stock model as a line.
//
// THE RUN. run(): (1) the safety checks (verify.h, unless SimOptions::safetyChecks is off), (2) gouges (below, unless
// SimOptions::gouges is off), (3) the material removal: the stock model is reset from SimProgram::stockSolid in its
// bounding box (the setup WCS) at SimOptions::resolution, and the sub-motions go through cutBatch in batches of one
// operation's motions, at most kMaxBatchMotions and kBatchRays of work (rayCost below). Cancel is polled between
// batches (and inside the checks and the gouge test); progress covers the three steps. A cancelled or
// failed run leaves the simulator EMPTY (as before the first run: totalMoves 0, no events, seek refused) and usable.
//
// KEYFRAMES. A snapshot (IStockModel::snapshot, copy-on-write strips) is kept at motion 0, then at the end of the
// first batch after every kKeyframeRays of work (tens of milliseconds on the reference machine at any cell, so a seek
// recuts at most that), and at the end. A kept snapshot pins the strips later cuts change: with T-070's strips (rows
// of 1 mm of y across the whole stock for the Z grid, layers of 0.5 mm of z for the X and Y grids) one keyframe pins
// about 16 MB at 0.1 mm and 60 to 110 MB at 0.05 mm on the benchmark, and halving the spacing barely halves that (each
// keyframe of a pocket level copies every layer the tool spans). With the CPU tri-dexel model the
// pinned bytes are counted exactly as the run goes (detail::snapshotPrivateBytes against the next keyframe); when they
// pass the budget (keyframeBudgetFor the cell) every other keyframe is dropped (the first and the last kept) and the
// spacing doubles, so memory stays inside the budget and the seek time grows instead. Another stock model's memory is
// not known: its keyframes are not thinned. Off the end of the timeline the end keyframe pins up to one more copy of
// the changed strips, and a partial move one more snapshot.
//   Measured on the S6 benchmark (112k moves, ray_sim_simulator_bench and the hidden budget sweep, reference machine
//   when quiet): Low 0.2 mm, 4 workers: 51 keyframes pinning 225 MB, seeks at most 36 to 42 ms (budget 250). Medium
//   0.1 mm, 4 workers: thinned once to 75 keyframes, 1216 MB; seeks at most 74 to 79 ms (budget 150; with 640 MB, 21
//   keyframes and 300 ms). High 0.05 mm, 8 workers: thinned 5 times to 19 keyframes, 1736 MB; seeks up to 0.8 s
//   (budget 100 ms: NOT met; 1 GB gives 7 keyframes and 2.5 s, 3 GB 34 and 0.43 s). On 11k moves High holds 9
//   keyframes, 698 MB, seeks up to 0.14 s. A finer copy-on-write unit in the stock model (tiles rather than whole rows
//   and layers) is what would bring High within budget at this memory.
//
// SEEK. A position (op, move, t01) is: every sub-motion of the moves before it, and of its move the path up to the
// fraction t01 of its length (zero-length sub-motions at its start included). seek() continues from the current
// state when that is before the target and no keyframe lies between, else restores the last keyframe at or before the
// target; then cuts up to the target's last whole sub-motion with cutBatch, then (when t01 falls inside a sub-motion)
// the part of that sub-motion with cut(), after keeping a snapshot of the state before it, which the next seek restores
// first. So the stock at a position is always the same bits, however it was reached: a straight run, a forward seek, a
// keyframe and a recut (the stock model's results do not depend on how moves are split across calls).
//
// GOUGES. A gouge is the cutting part of the tool (ToolShape::cuttingProfile) going into the PART beyond the
// tolerance. Measured exactly against the part's triangles, per sub-motion, not on the stock cells (so it is the same
// at every tier and is not resolution-dependent): the depth of a sub-motion is the largest s for which the cutting
// profile shrunk by s (as shrunkProfile: every radius less s, both ends pulled in by s, which holds the tool eroded by
// a ball of radius s; but no radius below kGougeNeedleMm) still meets the part when swept along it (sweepHitsSoup:
// touches a triangle or lies inside). That is the thickness of part material the tool's surface went through, measured
// from the tool's side: on a floor or a wall it is the distance below the surface. Past the tool's radius a needle
// along the axis is left, so a tool that went wholly into the part (a drill too deep) reports how far along its axis,
// up to half its cutting length ("at least" when even that is inside); the message says which. Allowance (stock to
// leave) plays no part: the finished part must not be cut.
//   The part soup's chords lie up to kGougePartChordMm from the design surface (inside it on a concave surface, so a
//   perfect finishing pass meets the soup by up to that), makeToolShape's chords up to kArcChordTolMm inside the real
//   tool, and an arc's chords up to kVerifyArcSagittaMm inside the arc. A sub-motion is reported when its depth
//   measured that way exceeds tol - kGougePartChordMm - kArcChordTolMm (an arc's chord: less its sagitta too), and the
//   depth reported (SimEvent::amountMm) is the measured depth plus those margins: an UPPER bound of the true depth,
//   at most about 0.015 mm above it. So no gouge deeper than the tolerance is missed, and a pass that stays on the
//   design surface is never reported, provided the tolerance is at least kGougeMinTolMm (a smaller one is raised).
//   Taps and thread mills are swept at their major diameter, which cuts the thread form beyond a hole modelled at its
//   tap-drill size by design: their gouge test uses the tool narrowed by kThreadDepthPerPitch times the pitch
//   (lib::Tool::threadPitch; not known: the full tool, which may report).
//   Each sub-motion is tested on its own, against the design part, whether or not earlier moves already removed the
//   material there (a pass through a gouge an earlier move made is reported too). Events: one per run of consecutive IR
//   moves of one operation that gouge, moveIndex the first, amountMm the deepest of the run, position the tool tip
//   where the first move first goes deeper than the tolerance; the message names the deepest move. Severity::Error.
//
// EVENTS. The checks' events and the gouges, sorted by operation (program order), move index, then the checks' kinds
// in their order before Gouge. With SimOptions::stopOnError the removal stops at the end of the IR move of the first
// Error event, events after that move are dropped, and that move's end is the run's end (runEnd()).
//
// THREADS. One thread at a time calls the non-const functions (run, seek, verifyFast); the const ones may run
// together, but not during a run or a seek (stock() is the live model). The checks, gouges and cuts use the job system
// (parallelChunks); results do not depend on the worker count.
#pragma once

#include "verify_impl.h"

#include <ray/sim.h>

#include <cstdint>
#include <vector>

namespace ray::sim::detail {

// ---------- Judgment constants (each a shop-practice decision; the report of T-072 lists them) ----------
// The chord error the part soup may have (the same requirement as the checks' part skin: kPartSkinMm less the tool's
// chords): a SimProgram's partSolid must be triangulated within this of the design part.
inline constexpr double kGougePartChordMm = kPartSkinMm - kArcChordTolMm;
// The smallest gouge tolerance used: below it a pass on the design surface could read as a gouge through the chords of
// the part, the tool and an arc (2 x 0.009 + 0.001 + 2 x 0.005 = 0.029 mm). A smaller SimOptions::gougeTolMm is raised
// to it, and the message says so.
inline constexpr double kGougeMinTolMm = 0.03;
// Radial depth of a thread per pitch that a tap or thread mill cuts beyond the hole (ISO 68-1 external thread depth
// 17/24 H = 0.6134 P, deeper than the internal thread's 5/8 H = 0.5413 P, so a hole modelled at any tap-drill size
// reads clean).
inline constexpr double kThreadDepthPerPitch = 0.6134;
// The depth of a gouge is found by bisection to this.
inline constexpr double kGougeDepthStepMm = 1e-4;
// The radius of the needle a tool shrinks to past its own radius (see GOUGES).
inline constexpr double kGougeNeedleMm = 1e-3;

// ---------- Work, batches and keyframes ----------
// Work is counted in RAYS (rayCost): the dexel rays a motion's swept cutting part can reach in the three grids, from
// its sweep box clipped to the stock, plus kMotionRays for each motion. Measured on the S6 benchmark on the reference
// machine (the hidden test "Simulator work measure calibration"), the stock model takes 0.5 to 1.0 ns per ray over the
// whole run at every cell (4 or 8 workers), by part 0.4 to 0.8 (adaptive pocket, mostly re-sweeps), 1 to 3 (ball
// scallops) and 2 to 4 (facing, where every ray loses material). Keyframe spacing and batch size never change a result
// (tested); they only set seek time, memory and cancel latency.
inline constexpr double kMotionRays = 2048.0;
// A batch: at most this many motions (S6: batches of 512 keep the workers busy; smaller ones cost up to 30 % more
// time) and this much work (25 to 50 ms in a pocket, up to about 150 ms of facing at 0.05 mm on 8 workers).
inline constexpr double kBatchRays = 64.0e6;
inline constexpr std::size_t kMaxBatchMotions = 512;
// A keyframe after this much work since the last one: a seek recuts at most that, 10 to 100 ms on the reference
// machine depending on the moves (section 9: High 100 ms; Low 250 and Medium 150 on slower machines).
inline constexpr double kKeyframeRays = 24.0e6;
// Memory the keyframes may pin (CPU tri-dexel model; see KEYFRAMES above): half of section 9's "memory, typical part
// with full CAM program" for the tier whose cell this is (Low 1.5 GB at 0.2 mm, Medium 2.5 GB at 0.1, High 4 GB at
// 0.05 and finer); the other half is the document, meshes, the stock and the checks' model. A cap: a program of 11k
// moves pins 33, 235 and 698 MB at the three cells; only long programs at the finer cells reach it.
inline constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
inline constexpr std::uint64_t kKeyframeBudgetLowBytes = 768 * kMiB;
inline constexpr std::uint64_t kKeyframeBudgetMediumBytes = 1280 * kMiB;
inline constexpr std::uint64_t kKeyframeBudgetHighBytes = 2048 * kMiB;
inline constexpr std::uint64_t keyframeBudgetFor(double cellMm)
{
    return cellMm >= 0.15    ? kKeyframeBudgetLowBytes
           : cellMm >= 0.075 ? kKeyframeBudgetMediumBytes
                             : kKeyframeBudgetHighBytes;
}

// Tests and benchmarks may change these (setSimTuning) to prove that results do not depend on them.
struct SimTuning {
    double batchRays = kBatchRays;
    std::size_t maxBatchMotions = kMaxBatchMotions;
    double keyframeRays = kKeyframeRays;
    std::uint32_t keyframeMotions = 0; // > 0: a keyframe every this many motions instead (at a batch end; batches are
                                       // cut there), the rays ignored
    std::uint64_t keyframeBudgetBytes = 0; // 0: keyframeBudgetFor(the run's cell)
};
// The simulator must come from makeSimulator. Takes effect at the next run.
void setSimTuning(ISimulator& sim, const SimTuning& tuning);

// What the last run did (tests, benchmarks, the report).
struct SimRunInfo {
    bool ready = false;
    std::uint64_t motions = 0;           // sub-motions of the program
    std::uint64_t runMotions = 0;        // cut by the run (fewer when stopOnError stopped it)
    std::vector<double> removedMm3;      // per sub-motion, as cutBatch measured it (runMotions entries)
    std::vector<std::uint32_t> motionOp; // per sub-motion, its operation index
    std::vector<std::uint32_t> motionMove;
    std::vector<std::uint64_t> keyframeMotions; // where the kept keyframes are
    std::uint64_t keyframeBytes = 0;            // pinned by the kept keyframes, exactly; 0: not known (not a CPU model)
    std::uint64_t keyframeBudgetBytes = 0;
    int thinnings = 0;       // times the keyframes were halved
    double keyframeRays = 0; // the spacing in rays at the end
    std::uint64_t batches = 0;
    std::uint64_t gougeMotionsTested = 0, gougeMotionsFlagged = 0;
    double gougeTolMm = 0; // the tolerance used (raised to kGougeMinTolMm)
    double cellMm = 0;     // SimOptions::resolution
};
SimRunInfo simRunInfo(const ISimulator& sim);

// The rays a motion's cutting part can reach (the work measure above). `cell` is the snapped cell per axis.
double rayCost(const Motion& m, double radius, double height, const Box3& stock, const Vec3& cell);

// ---------- Gouges (simulator_gouge.cpp) ----------
struct GougeEvent {
    std::uint32_t op = 0; // index into VerifyInput::ops
    SimEvent event;
};
struct GougeStats {
    std::uint64_t tested = 0, flagged = 0;
};
// The gouges of `motions` (expandMotions(in)) against in.part at tolerance tolMm (raised to kGougeMinTolMm), sorted
// by operation and move. Cancellable; progress on ctx.progress. Results do not depend on the worker count.
Result<std::vector<GougeEvent>> findGouges(const VerifyInput& in, const std::vector<Motion>& motions, double tolMm,
                                           IJobSystem& jobs, JobContext& ctx, GougeStats* stats = nullptr);
// The cutting profile the gouge test sweeps for an operation: ToolShape::cuttingProfile, narrowed for a tap or a thread
// mill whose pitch is known (see GOUGES above).
std::vector<Vec2> gougeProfile(const ToolShape& shape, const lib::Tool* tool);

} // namespace ray::sim::detail
