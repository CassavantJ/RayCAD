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
// KEYFRAMES. A snapshot (IStockModel::snapshot, copy-on-write tiles) is kept at motion 0, then at the end of the
// first batch after every kKeyframeRays of work, and at the end. A batch ends at twice the spacing from the last
// keyframe at the latest, so a seek recuts at most 2 kKeyframeRays (or one motion that is more): tens of milliseconds
// on the reference machine at any cell. A kept snapshot pins the tiles whose material later cuts change (tridexel.h,
// SNAPSHOTS). With the CPU tri-dexel model, before each keyframe the tiles cut since the last one are made one with any
// equal tile met before (detail::internTiles): a facing pass, a pocket's floor, uncut stock and emptied layers hold
// thousands of equal tiles, which then cost one, where each keyframe before a facing pass used to pin a whole Z grid
// (124 MB on S6's stock at 0.05 mm, 460 MB on a 300 x 200 plate). The pinned bytes are counted exactly as the run goes,
// every tile once however many keyframes and places hold it (detail::KeyframeLedger); when they pass the budget
// (keyframeBudgetFor the cell) every other keyframe is dropped (the first and the last kept) and the spacing doubles,
// so memory stays inside the budget and the seek time grows instead. Another stock model's memory is not known: its
// keyframes are neither interned nor thinned. Off the end of the timeline the end keyframe pins up to one more copy of
// the changed tiles, and a partial move one more snapshot.
//   Measured (simulator_bench_test.cpp: ray_sim_simulator_bench, and the hidden "[stress]" programs, seeking into EVERY
//   interval, the best of two tries each; reference machine when quiet, two or three runs each, ranges over them).
//   S6 (112k moves): High 375 keyframes pinning 812 MB (before interning: 290 and 1524 MB), worst seek 59 to 63 ms;
//   Medium 151 MB, 60 ms; Low 30 MB, 43 ms. The stress programs: facing (a 300 x 200 plate faced in twelve 0.5 mm
//   passes with a 50.8 mm face mill) pins at most 1 MB at every tier, worst seeks Low 34, Medium 58, High 49 ms
//   (before: High thinned twice, 1874 MB, 250 to 370 ms); large (a 500 x 350 plate at High: faced, pocketed, one ball
//   raster) 702 MB, not thinned, 52 to 66 ms (before: thinned five times, 1.0 to 1.5 s); a long program (263k moves:
//   facing, adaptive and pocket finish of a 260 x 160 pocket, three ball rasters at 0.75 to 0.225 mm) at Medium
//   967 MB, not thinned, 74 ms (before: thinned once, 145 to 184 ms, over Medium's budget).
//   Where the budget is NOT met: ball-end finishing at 0.05 mm over a large area. Every raster line changes the band
//   of tiles under the ball's width (a curved surface: no equal tiles to share), each tile over ten or more lines, so
//   every keyframe pins most of a band and one pass over 220 x 120 mm pins gigabytes. Within High's 2 GB such a
//   program is thinned and its seeks slow down: the finishing program (a 260 x 160 pocket roughed in twelve 0.5 mm
//   levels, then three rasters over 220 x 120 mm; 183k moves) thinned twice, worst 312 to 316 ms (before: four times,
//   0.75 s); the long program thinned three times, worst 0.6 s (before: seven, 4.3 to 5.6 s). Both meet Low's and
//   Medium's budgets. With no budget (hidden test "... High without a keyframe budget") the finishing program would
//   pin 4.1 GB and seek within 83 ms; the long program 7.6 GB and still up to 240 ms: its last raster (a 6 mm ball
//   along X, about 11 mm below the stock top) takes 3 to 6 ns per ray on 8 workers (S6's scallops 1.3 to 1.9), so there
//   even a keyframe at every spacing is too far apart for 100 ms.
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
// Error event, events after that move are dropped, and that move's end is the run's end (runEnd()). The setup's events
// (verify.h, SETUP: fixtures against the part and the stock; moveIndex kSetupEvent) come before all of them, and
// stopOnError passes over them: they belong to no move, and the program is worth seeing cut all the same.
//
// SEEK REPORT (added after T-074; ISimulator::seekReport). thinned: SimRunInfo::thinnings > 0. worstSeekMs: the run
// times each batch's cutBatch with the steady clock (nothing else reads those times) and sums them per keyframe
// interval as the keyframes stand at the end (batches end at keyframes, so a batch lies in one interval); the largest
// sum is the estimate: a seek to just before a keyframe restores the one before and recuts that interval, the same
// motions through the same stock model on the same workers. The thinned programs above (finishing, long) are where it
// passes High's 100 ms.
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
// scallops) and 2 to 4 (facing, where every ray loses material). On ONE worker, timed in CPU cycles (the hidden test
// "Simulator work measure calibration on the stress programs"), a ray costs about the same in facing, pocketing and
// ball rasters (within a factor 1.5 of each other), whether or not it loses material: the measure needs no term for
// writes; what varies is how well a batch spreads over the workers and, on this machine, other load. Keyframe spacing,
// batch size and interning never change a result (tested); they only set seek time, memory and cancel latency.
inline constexpr double kMotionRays = 2048.0;
// A batch: at most this many motions (S6: batches of 512 keep the workers busy; smaller ones cost up to 30 % more
// time) and this much work (25 to 50 ms in a pocket, up to about 150 ms of facing at 0.05 mm on 8 workers).
inline constexpr double kBatchRays = 64.0e6;
inline constexpr std::size_t kMaxBatchMotions = 512;
// A keyframe at the end of the first batch after this much work since the last one, and batches end at twice it, so a
// seek recuts between this and twice it: 10 to 70 ms on the reference machine when quiet depending on the moves
// (section 9: High 100 ms; Low 250 and Medium 150 on slower machines). Before the bound a batch could add up to
// kBatchRays more (S6's slowest seeks at High were single 64 M-ray batches of ball scallops).
inline constexpr double kKeyframeRays = 24.0e6;
// Memory the keyframes may pin (CPU tri-dexel model; see KEYFRAMES above): half of section 9's "memory, typical part
// with full CAM program" for the tier whose cell this is (Low 1.5 GB at 0.2 mm, Medium 2.5 GB at 0.1, High 4 GB at
// 0.05 and finer); the other half is the document, meshes, the stock and the checks' model. A cap: S6's 112k-move
// program pins 30, 151 and 812 MB at the three cells with interned tiles (77, 356 and 1524 MB before; none thinned).
// High's cap stays at 2 GB: the finishing programs above would need several times that (see KEYFRAMES).
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
    bool internTiles = true;               // equal tiles made one at each keyframe (tridexel.h, KEYFRAMES)
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
