// Backplot internals (task T-037): the limits and assumptions of buildBackplot, and test hooks. The layout of the
// result is documented in include/ray/sim.h; how each move kind becomes line vertices is documented in backplot.cpp.
#pragma once

#include <ray/sim.h>

#include <cstdint>

namespace ray::sim::detail {

// The moveIndex of vertex 0 (the toolpath start), which ends no segment and belongs to no move.
inline constexpr std::uint32_t kBackplotNoMove = 0xFFFFFFFFu;

// A chord tolerance below this (or not a number) is raised to it. 0.1 um is finer than a float coordinate can hold a
// few hundred millimetres from the origin, so a finer tolerance would only add vertices.
inline constexpr double kMinChordTolMm = 1e-4;
// No chord spans more than a quarter turn, so a circle far smaller than the tolerance is still drawn as a square
// rather than a single zero-length chord.
inline constexpr double kMaxArcStepRad = 1.5707963267948966; // pi / 2
// At most this many chords per arc move. Reached only for radii above about 900 m at 1 um, so in practice never.
inline constexpr std::uint32_t kMaxArcSegments = 65536;
// ---------- Canned-cycle assumptions (the one place to change them) ----------
// cam.h does not fully say what a cycle move means; the backplot reads it as below, and src/post (T-039) expands the
// cycles a post does not declare with the same reading, so the two agree. [design], T-037; to be settled in cam.h.
//   - cycleRetract is the R plane, an absolute WCS Z; the tool returns to it after each hole (G99). There is no
//     initial-level return (G98): a generator that wants one emits a Rapid after the cycle.
//   - The tool first rapids in X and Y at its current Z, then in Z to R (Fanuc order).
//   - cyclePeck is the peck depth Q, measured from R; Q <= 0, or a bottom at or above R, drills in one feed.
//   - CycleDrill: G81 (G82 with a dwell), rapid out. CycleTap: G84, feed out at the same feed. CycleBore and
//     CycleReam: G85 (G89 with a dwell), feed out at the same feed. Dwells and the spindle reversal draw nothing.
//   - The tool ends the move at (x, y, R), where the next move starts (Move::z of a cycle is the bottom).
//   - The two distances below are control parameters on a real machine (not in the IR).
struct CycleAssumptions {
    // G83: the rapid re-entry after a full retract stops this far above the previous peck depth.
    static constexpr Nm peckReentryClearanceNm = 500'000; // 0.5 mm
    // G73: a chip-break peck backs off this far.
    static constexpr Nm chipBreakRetractNm = 500'000; // 0.5 mm
    // At most this many pecks per hole (a display bound); the last one goes to the bottom whatever is left.
    static constexpr std::uint32_t maxPecks = 4096;
};

// Number of chords for an arc that sweeps `sweepRad` (0, 2 pi] at chord tolerance `tol` (already clamped), where
// `rEff` (arcEffectiveRadius, in the same unit as tol) bounds the second derivative of the path by the angle. A chord
// over an angle h of a path P(phi) is never farther from it than h^2 / 8 max|P''| (the error of linear interpolation),
// so steps of at most 2 sqrt(2 tol / rEff) keep every chord within tol. For a circle, rEff = r and the bound is the
// sagitta r (1 - cos(h / 2)) <= r h^2 / 8: below the quarter-turn cap it costs at most 3% more chords than the exact 2
// acos(1 - tol / r), and it needs no trigonometry.
std::uint32_t arcSegmentCount(double sweepRad, double rEff, double tol);

// max|P''| of an arc whose radius goes linearly from r0 to r1 over `sweepRad` while the axis moves linearly (a helix
// adds nothing): P'' = (r'' - r) e + 2 r' e_perp with r' = (r1 - r0) / sweep, so sqrt(rMax^2 + 4 r'^2); rMax when
// r0 == r1.
double arcEffectiveRadius(double r0, double r1, double sweepRad);

// Signed sweep from in-plane start vector (su, sv) to end vector (eu, ev) around the centre, in (0, 2 pi], for a
// counter-clockwise (ccw) or clockwise arc. An end on the start ray (start equals end: a full circle) gives 2 pi.
double arcSweep(double su, double sv, double eu, double ev, bool ccw);

// buildBackplot with an explicit vertex limit (the public one uses 2^32 - 1, what a uint32 moveFirstVertex can
// address). When the exact result would need more vertices it is built COARSE: every arc at kMaxArcStepRad chords
// and every peck cycle as one plunge. Returns whether it was coarse.
Backplot buildBackplotLimited(const cam::Toolpath&, const Xform& modelFromWcs, double chordTol,
                              std::uint64_t maxVertices, bool* coarse);

} // namespace ray::sim::detail
