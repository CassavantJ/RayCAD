// The toolpath IR as straight sub-motions of the tool tip for the safety checks (task T-071). Each move becomes what
// buildBackplot draws for it (backplot.cpp says what every kind becomes; tests/unit/sim/verify_motion_test.cpp checks
// the two agree), but in doubles, from the integer nanometres, without the float rounding of display vertices:
//   Rapid, Linear   one straight motion (none when it does not move);
//   ArcCW, ArcCCW   chords with their ends on the arc (a spiral when the end radius differs, a helix when the axis
//                   coordinate changes), counted by arcSegmentCount at kVerifyArcSagittaMm, so no point of the arc is
//                   farther than that from its chord; a start or end at the centre is one straight motion;
//   cycles          the rapids and feeds of cam.h "Canned cycles", with cam::peckClearanceNm (move, machine, default);
//   Dwell, CycleCancel  nothing.
// Each toolpath begins with a zero-length rapid at its start point (move 0): how the tool gets there from the tool
// change is the post's, outside the IR; that it stands clear there is checked.
// Trigonometry through ray::dmath, a fixed order of operations: the same motions on every platform.
#include "backplot.h"
#include "verify_impl.h"

#include <algorithm>
#include <cmath>

namespace ray::sim::detail {

namespace {

using cam::Move;
using cam::MoveKind;

struct Pos {
    Nm x, y, z;
    bool operator==(const Pos&) const = default;
};

constexpr Nm kNmClamp = Nm{1} << 61; // as backplot.cpp: sums and differences of cycle levels cannot overflow
Nm clampNm(Nm v)
{
    return std::clamp(v, -kNmClamp, kNmClamp);
}
double mm(Nm v)
{
    return static_cast<double>(v) / kNmPerMm;
}
Vec3 mm(const Pos& p)
{
    return {mm(p.x), mm(p.y), mm(p.z)};
}

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

class Walker {
public:
    Walker(const cam::Toolpath& tp, const lib::MachineDef* machine, std::uint32_t op, std::vector<Motion>& out)
        : tp_(tp), machine_(machine), op_(op), out_(out)
    {
    }

    void run()
    {
        cur_ = {tp_.startX, tp_.startY, tp_.startZ};
        // The tool standing at the start (where the post's tool change and approach leave it): a zero-length rapid of
        // move 0, so a start inside material is reported.
        Motion start;
        start.a = start.b = mm(cur_);
        start.op = op_;
        start.move = 0;
        start.rapid = true;
        out_.push_back(start);
        for (std::size_t i = 0; i < tp_.moves.size(); ++i) {
            const Move& mv = tp_.moves[i];
            move_ = static_cast<std::uint32_t>(i);
            switch (mv.kind) {
            case MoveKind::Rapid:
                to({mv.x, mv.y, mv.z}, true);
                break;
            case MoveKind::Linear:
                to({mv.x, mv.y, mv.z}, false);
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
        }
    }

private:
    void push(const Vec3& a, const Vec3& b, bool rapid, bool chord)
    {
        if (a.x == b.x && a.y == b.y && a.z == b.z) {
            return;
        }
        Motion m;
        m.a = a;
        m.b = b;
        m.op = op_;
        m.move = move_;
        m.rapid = rapid;
        m.chord = chord;
        out_.push_back(m);
    }
    void to(const Pos& p, bool rapid)
    {
        if (p == cur_) {
            return;
        }
        push(mm(cur_), mm(p), rapid, false);
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
            to(end, false);
            return;
        }
        const double au = static_cast<double>(su) - static_cast<double>(cu);
        const double av = static_cast<double>(sv) - static_cast<double>(cv);
        const double bu = static_cast<double>(eu) - static_cast<double>(cu);
        const double bv = static_cast<double>(ev) - static_cast<double>(cv);
        const double r0 = dmath::sqrt(au * au + av * av);
        const double r1 = dmath::sqrt(bu * bu + bv * bv);
        const bool ccw = mv.kind == MoveKind::ArcCCW;
        const double sweep = arcSweep(au, av, bu, bv, ccw);
        const std::uint32_t n =
            arcSegmentCount(sweep, arcEffectiveRadius(r0, r1, sweep), kVerifyArcSagittaMm * kNmPerMm);
        const double nD = static_cast<double>(n);
        const double step = ccw ? sweep / nD : -(sweep / nD);
        const double c = dmath::cos(step), s = dmath::sin(step);
        double du = au / r0, dv = av / r0;
        const double cuMm = mm(cu), cvMm = mm(cv);
        const double r0Mm = r0 / kNmPerMm;
        const double dr = (r1 - r0) / kNmPerMm / nD;
        const double sw = mm(component(cur_, ax.w));
        const double dw = (mm(component(end, ax.w)) - sw) / nD;
        Vec3 prev = mm(cur_);
        for (std::uint32_t k = 1; k < n; ++k) {
            const double nu = du * c - dv * s;
            const double nv = du * s + dv * c;
            du = nu;
            dv = nv;
            const double kD = static_cast<double>(k);
            const double r = r0Mm + dr * kD;
            double p[3];
            p[ax.u] = cuMm + r * du;
            p[ax.v] = cvMm + r * dv;
            p[ax.w] = sw + dw * kD;
            const Vec3 next{p[0], p[1], p[2]};
            push(prev, next, false, true);
            prev = next;
        }
        push(prev, mm(end), false, true);
        cur_ = end;
    }

    void cycle(const Move& mv)
    {
        const Nm x = mv.x, y = mv.y;
        const Nm bottom = clampNm(mv.z);
        const Nm r = clampNm(mv.cycleRetract);
        const Nm clearance = clampNm(mv.cycleClearance);
        const Nm back = clampNm(cam::peckClearanceNm(mv, machine_));
        const Nm out = mv.cycleReturn == cam::CycleReturn::Initial ? clearance : r;
        to({x, y, cur_.z}, true);
        if (cur_.z > clearance) {
            to({x, y, clearance}, true);
        }
        to({x, y, r}, true);
        const bool peck = mv.kind == MoveKind::CyclePeck || mv.kind == MoveKind::CycleChipBreak;
        const Nm q = clampNm(mv.cyclePeck);
        if (peck && q > 0 && bottom < r) {
            const bool fullRetract = mv.kind == MoveKind::CyclePeck;
            Nm depth = r;
            for (std::uint32_t k = 1;; ++k) {
                const Nm next = (depth - bottom <= q || k >= kMaxPecksDrawn) ? bottom : depth - q;
                if (fullRetract && k > 1) {
                    to({x, y, std::min(r, depth + back)}, true);
                }
                to({x, y, next}, false);
                if (next == bottom) {
                    break;
                }
                const Nm up = fullRetract ? r : std::min(r, next + back);
                to({x, y, up}, true);
                depth = next;
            }
            to({x, y, out}, true);
            return;
        }
        to({x, y, bottom}, false);
        const bool feedOut =
            mv.kind == MoveKind::CycleTap || mv.kind == MoveKind::CycleBore || mv.kind == MoveKind::CycleReam;
        if (feedOut) {
            to({x, y, r}, false);
        }
        to({x, y, out}, true);
    }

    const cam::Toolpath& tp_;
    const lib::MachineDef* machine_;
    std::uint32_t op_;
    std::vector<Motion>& out_;
    Pos cur_{};
    std::uint32_t move_ = 0;
};

} // namespace

std::vector<Motion> expandMotions(const VerifyInput& in)
{
    std::vector<Motion> out;
    for (std::size_t k = 0; k < in.ops.size(); ++k) {
        const cam::Toolpath* tp = in.ops[k].path;
        if (tp == nullptr) {
            continue;
        }
        Walker(*tp, in.machine, static_cast<std::uint32_t>(k), out).run();
    }
    return out;
}

} // namespace ray::sim::detail
