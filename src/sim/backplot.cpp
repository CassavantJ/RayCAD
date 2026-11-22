// Backplot line data from the toolpath IR (task T-037). The layout of the result is documented in include/ray/sim.h.
//
// What each move becomes (every move gets at least one vertex, and its vertices are contiguous):
//   Rapid, Linear   one vertex at the end point. A rapid is drawn straight, although a control may dogleg G0.
//   ArcCW, ArcCCW   chords (vertex kind stays ArcCW / ArcCCW), the last vertex exactly at the end point. The plane
//                   picks the in-plane axes (u, v) and the axis w, right-handed so that counter-clockwise is a positive
//                   turn about the plane normal, as G2/G3 are defined: XY (u, v, w) = (x, y, z), G17; XZ = (z, x, y),
//                   G18, seen from +Y; YZ = (y, z, x), G19, seen from +X. The sweep is in (0, 360] degrees: an end on
//                   the start ray (start equals end) is a full circle, as a control reads it. A change of w makes a
//                   helix (w linear in the angle). When the start and end radii differ (a rounded or inconsistent
//                   centre) the radius changes linearly with the angle, the spiral a control interpolates within its
//                   radius tolerance; chords are counted so that this spiral too stays within the tolerance
//                   (backplot.h, arcSegmentCount). A start or end exactly at the centre is no arc: one straight
//                   segment.
//   Dwell,          no motion (x, y, z of the move are ignored): one zero-length vertex at the current position with
//   CycleCancel     the move's own kind and role.
//   Cycles          the motion the control makes, as cam.h "Canned cycles" defines it (G81-G89, G98 or G99 per move).
//                   R = cycleRetract, C = cycleClearance, Q = cyclePeck, c = the peck clearance (cam::peckClearanceNm
//                   with the machine given), the return level = R (RPlane, G99) or C (Initial, G98). Sub-motions get
//                   kind Rapid or Linear (the motion) and these roles:
//                     1. rapid in X and Y at the current Z to the hole                   Rapid  Link
//                     2. if above C: rapid down to C                                     Rapid  Approach
//                     3. rapid down (or up) to R                                         Rapid  Approach
//                     4. by cycle:
//                        CycleDrill (G81, G82)  feed to the bottom                       Linear Plunge
//                                               rapid to the return level                Rapid  Retract
//                        CyclePeck (G83)   feed to R - Q; then per peck: rapid to R      Rapid  Retract
//                                          rapid to c above the last depth               Rapid  Approach
//                                          feed Q deeper (the last peck to the bottom)   Linear Plunge
//                                          rapid to the return level                     Rapid  Retract
//                        CycleChipBreak (G73) feed to R - Q; then per peck: rapid up c   Rapid  Retract
//                                          feed Q deeper (the last peck to the bottom)   Linear Plunge
//                                          rapid to the return level                     Rapid  Retract
//                        CycleTap (G84, G74), CycleBore and CycleReam (G85, G89)
//                                          feed to the bottom, feed back to R at the
//                                          same feed                                    Linear Plunge, then Retract
//                                          with G98, rapid to C                          Rapid  Retract
//                   Re-entries and back-offs stop at R. A peck cycle with Q <= 0 or a bottom at or above R is drilled
//                   in one feed. Dwells and the spindle reversal draw nothing. Sub-motions of zero length are left
//                   out. The tool ends the move at (x, y, return level), where the next move starts. The backplot
//                   draws what the control would do even for a cycle that breaks cam.h's rules (levels out of order,
//                   a run started below C); the invariant checker and the post host refuse those.
//
// Determinism: plain IEEE-754 double arithmetic in a fixed order (the module is built with strict floating point and
// no contraction), trigonometry only through ray::dmath, and float conversion by round-to-nearest. Intermediate arc
// points are the start radius vector turned by a fixed rotation (one dmath::sin and dmath::cos per arc), so the cost
// per chord is a few multiplications; the drift of that recurrence after 65536 steps is about 1e-11 of the radius.
//
// Speed: two passes over the moves. The first counts vertices and plans each arc (sweep and chord count, 16 bytes per
// arc, the only temporary); the second writes the vertex array, allocated once at its exact size. Nothing is allocated
// per move. Measured (T-037 report, with the 96-byte moves of then; they are 112 bytes now) the time goes mostly to
// memory: reading the moves twice and first-touching the 24-byte vertices; the one dmath::atan2 per arc is the largest
// computing cost.
#include "backplot.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ray::sim {
namespace detail {

double arcEffectiveRadius(double r0, double r1, double sweepRad)
{
    const double rMax = std::max(r0, r1);
    if (r0 == r1) {
        return rMax;
    }
    const double slope = (r1 - r0) / sweepRad;
    return dmath::sqrt(rMax * rMax + 4.0 * slope * slope);
}

std::uint32_t arcSegmentCount(double sweepRad, double rEff, double tol)
{
    double step = kMaxArcStepRad;
    if (rEff > 0.0) {
        const double bound = 2.0 * dmath::sqrt(2.0 * tol / rEff);
        if (bound < step) {
            step = bound;
        }
    }
    const double n = std::ceil(sweepRad / step);
    if (!(n >= 1.0)) {
        return 1; // also a NaN sweep
    }
    if (n >= static_cast<double>(kMaxArcSegments)) {
        return kMaxArcSegments;
    }
    return static_cast<std::uint32_t>(n);
}

double arcSweep(double su, double sv, double eu, double ev, bool ccw)
{
    constexpr double kTwoPi = 6.283185307179586;
    // The counter-clockwise angle from start to end, in (-pi, pi].
    const double a = dmath::atan2(su * ev - sv * eu, su * eu + sv * ev);
    if (ccw) {
        return a > 0.0 ? a : a + kTwoPi;
    }
    return a < 0.0 ? -a : kTwoPi - a;
}

} // namespace detail

namespace {

using cam::Move;
using cam::MoveKind;
using cam::MoveRole;

struct Pos {
    Nm x, y, z;
    bool operator==(const Pos&) const = default;
};

// Coordinates of a cycle are clamped so that their sums and differences cannot overflow (2^61 nm is 2.3e12 mm).
constexpr Nm kNmClamp = Nm{1} << 61;
Nm clampNm(Nm v)
{
    return std::clamp(v, -kNmClamp, kNmClamp);
}

double mm(Nm v)
{
    return static_cast<double>(v) / kNmPerMm; // the same expression as ray::toMm
}

float feedMm(std::int32_t umPerMin)
{
    return static_cast<float>(static_cast<double>(umPerMin) / 1000.0); // um/min -> mm/min
}

// In-plane axes and axis of an arc: index into (x, y, z) of u, v and w.
struct PlaneAxes {
    int u, v, w;
};
PlaneAxes axesOf(cam::ArcPlane p)
{
    switch (p) {
    case cam::ArcPlane::XZ:
        return {2, 0, 1};
    case cam::ArcPlane::YZ:
        return {1, 2, 0};
    case cam::ArcPlane::XY:
    default:
        return {0, 1, 2};
    }
}

Nm component(const Pos& p, int axis)
{
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

// What the counting pass found for each arc (in move order), so the emitting pass does not compute it again.
struct ArcPlan {
    double sweep;
    std::uint32_t chords;
};

// One walk over the moves. Counting (kEmit false) and emitting (kEmit true) share every decision, so the count of
// the first pass is exactly the size of the second; arcs are planned in the first and replayed in the second.
template <bool kEmit>
class Walker {
public:
    Walker(const cam::Toolpath& tp, const lib::MachineDef* machine, const Xform& xf, double tolMm, bool coarse,
           std::vector<ArcPlan>& arcs, Backplot* out)
        : tp_(tp), machine_(machine), xf_(xf), tolNm_(tolMm * kNmPerMm), coarse_(coarse), arcs_(arcs), out_(out)
    {
        if constexpr (!kEmit) {
            arcs_.clear();
        }
    }

    std::uint64_t run()
    {
        cur_ = {tp_.startX, tp_.startY, tp_.startZ};
        move_ = detail::kBackplotNoMove;
        vertexNm(cur_, MoveKind::Rapid, MoveRole::Approach, 0);
        const std::size_t n = tp_.moves.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Move& mv = tp_.moves[i];
            move_ = static_cast<std::uint32_t>(i);
            const std::uint64_t first = count_;
            if constexpr (kEmit) {
                out_->moveFirstVertex[i] = static_cast<std::uint32_t>(first);
            }
            switch (mv.kind) {
            case MoveKind::Rapid:
                to({mv.x, mv.y, mv.z}, MoveKind::Rapid, mv.role, 0);
                break;
            case MoveKind::Linear:
                to({mv.x, mv.y, mv.z}, MoveKind::Linear, mv.role, mv.feedMicronPerMin);
                break;
            case MoveKind::ArcCW:
            case MoveKind::ArcCCW:
                arc(mv);
                break;
            case MoveKind::CycleDrill:
            case MoveKind::CyclePeck:
            case MoveKind::CycleChipBreak:
            case MoveKind::CycleTap:
            case MoveKind::CycleBore:
            case MoveKind::CycleReam:
                cycle(mv);
                break;
            case MoveKind::Dwell:
            case MoveKind::CycleCancel:
            default:
                break;
            }
            if (count_ == first) { // no motion: one zero-length vertex, so every move has a vertex
                vertexNm(cur_, mv.kind, mv.role, mv.kind == MoveKind::Rapid ? 0 : mv.feedMicronPerMin);
            }
        }
        if constexpr (kEmit) {
            out_->moveFirstVertex[n] = static_cast<std::uint32_t>(count_);
            out_->bounds = {{lo_[0], lo_[1], lo_[2]}, {hi_[0], hi_[1], hi_[2]}};
        }
        return count_;
    }

private:
    void vertexMm([[maybe_unused]] double x, [[maybe_unused]] double y, [[maybe_unused]] double z,
                  [[maybe_unused]] MoveKind kind, [[maybe_unused]] MoveRole role, [[maybe_unused]] std::int32_t feedUm)
    {
        ++count_;
        if constexpr (kEmit) {
            const double* m = xf_.m;
            BackplotVertex v;
            v.x = static_cast<float>(m[0] * x + m[1] * y + m[2] * z + m[3]);
            v.y = static_cast<float>(m[4] * x + m[5] * y + m[6] * z + m[7]);
            v.z = static_cast<float>(m[8] * x + m[9] * y + m[10] * z + m[11]);
            v.role = static_cast<std::uint8_t>(role);
            v.kind = static_cast<std::uint8_t>(kind);
            v.pad = 0;
            v.feed = feedMm(feedUm);
            v.moveIndex = move_;
            out_->lineVertices.push_back(v);
            lo_[0] = std::min(lo_[0], v.x), lo_[1] = std::min(lo_[1], v.y), lo_[2] = std::min(lo_[2], v.z);
            hi_[0] = std::max(hi_[0], v.x), hi_[1] = std::max(hi_[1], v.y), hi_[2] = std::max(hi_[2], v.z);
        }
    }

    // Feeds are passed in um/min and converted only when a vertex is written: the module is built with strict
    // floating point, under which the compiler keeps even unused floating-point operations.
    void vertexNm(const Pos& p, MoveKind kind, MoveRole role, std::int32_t feed)
    {
        if constexpr (kEmit) {
            vertexMm(mm(p.x), mm(p.y), mm(p.z), kind, role, feed);
        } else {
            ++count_;
        }
    }

    // A straight sub-motion; one of zero length is left out.
    void to(const Pos& p, MoveKind kind, MoveRole role, std::int32_t feed)
    {
        if (p == cur_) {
            return;
        }
        vertexNm(p, kind, role, feed);
        cur_ = p;
    }

    void arc(const Move& mv)
    {
        const Pos end{mv.x, mv.y, mv.z};
        const Pos centre{mv.i, mv.j, mv.k};
        const PlaneAxes ax = axesOf(mv.plane);
        const Nm su = component(cur_, ax.u), sv = component(cur_, ax.v);
        const Nm eu = component(end, ax.u), ev = component(end, ax.v);
        const Nm cu = component(centre, ax.u), cv = component(centre, ax.v);
        if ((su == cu && sv == cv) || (eu == cu && ev == cv)) {
            to(end, mv.kind, mv.role, mv.feedMicronPerMin); // no radius at one end: a straight segment
            return;
        }
        // Start and end from the centre, in the plane, in nanometres (exact up to 2^53 nm).
        const double au = static_cast<double>(su) - static_cast<double>(cu);
        const double av = static_cast<double>(sv) - static_cast<double>(cv);
        const double bu = static_cast<double>(eu) - static_cast<double>(cu);
        const double bv = static_cast<double>(ev) - static_cast<double>(cv);
        const double r0 = dmath::sqrt(au * au + av * av);
        const double r1 = dmath::sqrt(bu * bu + bv * bv);
        const bool ccw = mv.kind == MoveKind::ArcCCW;
        if constexpr (kEmit) {
            const std::int32_t feed = mv.feedMicronPerMin;
            const double sweep = arcs_[nextArc_].sweep;
            const std::uint32_t n = arcs_[nextArc_].chords;
            ++nextArc_;
            const double nD = static_cast<double>(n);
            const double step = ccw ? sweep / nD : -(sweep / nD);
            const double c = dmath::cos(step), s = dmath::sin(step);
            double du = au / r0, dv = av / r0; // unit start direction, turned by `step` per chord
            const double cuMm = mm(cu), cvMm = mm(cv);
            const double r0Mm = r0 / kNmPerMm;
            const double dr = (r1 - r0) / kNmPerMm / nD;
            const double sw = mm(component(cur_, ax.w));
            const double dw = (mm(component(end, ax.w)) - sw) / nD;
            double p[3];
            for (std::uint32_t k = 1; k < n; ++k) {
                const double nu = du * c - dv * s;
                const double nv = du * s + dv * c;
                du = nu;
                dv = nv;
                const double kD = static_cast<double>(k);
                const double r = r0Mm + dr * kD;
                p[ax.u] = cuMm + r * du;
                p[ax.v] = cvMm + r * dv;
                p[ax.w] = sw + dw * kD;
                vertexMm(p[0], p[1], p[2], mv.kind, mv.role, feed);
            }
            vertexNm(end, mv.kind, mv.role, feed);
        } else {
            const double sweep = detail::arcSweep(au, av, bu, bv, ccw);
            const std::uint32_t n = detail::arcSegmentCount(sweep, detail::arcEffectiveRadius(r0, r1, sweep), tolNm_);
            arcs_.push_back({sweep, n});
            count_ += n;
        }
        cur_ = end;
    }

    void cycle(const Move& mv)
    {
        const Nm x = mv.x, y = mv.y;
        const Nm bottom = clampNm(mv.z);
        const Nm r = clampNm(mv.cycleRetract);
        const Nm clearance = clampNm(mv.cycleClearance);
        const Nm back = clampNm(cam::peckClearanceNm(mv, machine_));
        const Nm out = mv.cycleReturn == cam::CycleReturn::Initial ? clearance : r; // the return level
        const std::int32_t feed = mv.feedMicronPerMin;
        to({x, y, cur_.z}, MoveKind::Rapid, MoveRole::Link, 0);
        if (cur_.z > clearance) {
            to({x, y, clearance}, MoveKind::Rapid, MoveRole::Approach, 0);
        }
        to({x, y, r}, MoveKind::Rapid, MoveRole::Approach, 0);
        const bool peck = mv.kind == MoveKind::CyclePeck || mv.kind == MoveKind::CycleChipBreak;
        const Nm q = clampNm(mv.cyclePeck);
        if (peck && !coarse_ && q > 0 && bottom < r) {
            const bool fullRetract = mv.kind == MoveKind::CyclePeck;
            Nm depth = r;
            for (std::uint32_t k = 1;; ++k) {
                const Nm next = (depth - bottom <= q || k >= detail::kMaxPecksDrawn) ? bottom : depth - q;
                if (fullRetract && k > 1) {
                    to({x, y, std::min(r, depth + back)}, MoveKind::Rapid, MoveRole::Approach, 0);
                }
                to({x, y, next}, MoveKind::Linear, MoveRole::Plunge, feed);
                if (next == bottom) {
                    break;
                }
                const Nm up = fullRetract ? r : std::min(r, next + back);
                to({x, y, up}, MoveKind::Rapid, MoveRole::Retract, 0);
                depth = next;
            }
            to({x, y, out}, MoveKind::Rapid, MoveRole::Retract, 0);
            return;
        }
        to({x, y, bottom}, MoveKind::Linear, MoveRole::Plunge, feed);
        const bool feedOut =
            mv.kind == MoveKind::CycleTap || mv.kind == MoveKind::CycleBore || mv.kind == MoveKind::CycleReam;
        if (feedOut) {
            to({x, y, r}, MoveKind::Linear, MoveRole::Retract, feed);
        }
        to({x, y, out}, MoveKind::Rapid, MoveRole::Retract, 0);
    }

    const cam::Toolpath& tp_;
    const lib::MachineDef* machine_;
    const Xform& xf_;
    double tolNm_; // chord tolerance in nanometres, as arcs are measured
    bool coarse_;
    std::vector<ArcPlan>& arcs_;
    std::size_t nextArc_ = 0;
    Backplot* out_;
    Pos cur_{};
    std::uint32_t move_ = 0;
    std::uint64_t count_ = 0;
    // Bounds of the vertices written (as floats, so exact); vertex 0 always exists.
    float lo_[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity()};
    float hi_[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()};
};

double clampTolerance(double chordTol)
{
    return chordTol >= detail::kMinChordTolMm ? chordTol : detail::kMinChordTolMm; // NaN too
}

} // namespace

namespace detail {

Backplot buildBackplotLimited(const cam::Toolpath& tp, const Xform& modelFromWcs, double chordTol,
                              const lib::MachineDef* machine, std::uint64_t maxVertices, bool* coarseOut)
{
    double tol = clampTolerance(chordTol);
    bool coarse = false;
    std::vector<ArcPlan> arcs;
    std::uint64_t count = Walker<false>(tp, machine, modelFromWcs, tol, false, arcs, nullptr).run();
    if (count > maxVertices) {
        // Only an absurd toolpath gets here (billions of chords or pecks). Every arc at quarter-turn chords and every
        // peck cycle as one plunge: at most six vertices per move.
        coarse = true;
        tol = std::numeric_limits<double>::infinity();
        count = Walker<false>(tp, machine, modelFromWcs, tol, true, arcs, nullptr).run();
    }
    if (coarseOut != nullptr) {
        *coarseOut = coarse;
    }
    Backplot out;
    out.moveFirstVertex.resize(tp.moves.size() + 1);
    out.lineVertices.reserve(static_cast<std::size_t>(count));
    Walker<true>(tp, machine, modelFromWcs, tol, coarse, arcs, &out).run();
    return out;
}

} // namespace detail

Backplot buildBackplot(const cam::Toolpath& tp, const Xform& modelFromWcs, double chordTol,
                       const lib::MachineDef* machine)
{
    return detail::buildBackplotLimited(tp, modelFromWcs, chordTol, machine, std::numeric_limits<std::uint32_t>::max(),
                                        nullptr);
}

} // namespace ray::sim
