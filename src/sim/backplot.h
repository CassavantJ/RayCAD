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
// ---------- Canned cycles ----------
// What a cycle move means is settled in cam.h ("Canned cycles"); the backplot draws that motion, and src/post expands
// the cycles a post does not declare in the same way (tests/unit/cycles checks that the two agree). The peck
// clearance comes from cam::peckClearanceNm (the move's, else the machine's, else cam::kDefaultPeckClearanceNm).
// What remains the backplot's own: at most this many pecks per hole are drawn (a display bound); the last one goes to
// the bottom whatever is left.
inline constexpr std::uint32_t kMaxPecksDrawn = 4096;

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
                              const lib::MachineDef* machine, std::uint64_t maxVertices, bool* coarse);

} // namespace ray::sim::detail
