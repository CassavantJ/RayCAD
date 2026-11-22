// Cycle-time estimate (task T-059): cam.h estimateCycleTime.
//
// THE MODEL. Units here are mm, s, mm/s and mm/s^2. Each axis a of X, Y, Z has a rapid rate V_a, a maximum feed F_a
// and an acceleration A_a (lib::AxisDef; a 0 is "not known", see DEFAULTS).
//
//  Profile   Every motion of length L runs a trapezoidal velocity profile: from its entry speed vs it accelerates at A
//            up to its cruise speed v, cruises, and brakes at A to its exit speed ve; when L is too short to reach v
//            it peaks at vp = sqrt(A L + (vs^2 + ve^2) / 2) (a triangle). Time:
//              reaches v:  t = (v - vs)/A + (v - ve)/A + (L - (v^2 - vs^2)/2A - (v^2 - ve^2)/2A) / v
//              triangle:   t = (vp - vs)/A + (vp - ve)/A
//            No jerk limit: acceleration switches on and off at once.
//  Straight  For a straight motion along the unit direction u, each axis carries the share |u_a| of the speed and of
//            the acceleration, so v = min_a F_a / |u_a| (and the programmed feed) and A = min_a A_a / |u_a| over the
//            axes that move.
//  Rapids    G0 starts and ends at rest (a control's G0 stops in position before the next block). With
//            CycleTimeOptions::rapidInterpolated false (the default) each axis runs its own profile from rest to rest
//            at V_a and A_a and the rapid takes as long as the slowest axis: t = max_a t_a(|d_a|). With it true the
//            rapid is one straight motion at v = min_a V_a / |u_a|, A = min_a A_a / |u_a|. Either way rapidMm counts
//            the straight line.
//  Feeds     G1 runs at the programmed feed F (mm/min / 60), held under min_a F_a / |u_a|. A feed move without a feed
//            (0) takes the one before it in the toolpath, as a modal F word does, else no limit but the machine's
//            (CycleTimeAssumptions::feedMissing).
//  Arcs      G2/G3 in the plane (u, v) with axis w (cam.h ArcPlane: XY, XZ, YZ). The length is the swept angle times
//  the
//            mean of the start and end radii in the plane, combined with the travel along w for a helix:
//            L = sqrt((sweep (r0 + r1)/2)^2 + dw^2); the in-plane share is f = sweep (r0 + r1)/2 / L. In the plane the
//            direction turns through every angle, so the plane is held to its weaker axis, A_p = min(A_u, A_v) and
//            F_p = min(F_u, F_v): v = min(F, F_p / f, F_w / |dw/L|, sqrt(A_p R) / f) with R = min(r0, r1), the last
//            being the speed whose centripetal acceleration (v f)^2 / R equals A_p; A = min(A_p / f, A_w / |dw/L|) for
//            the speeding up and slowing down along the path. The centripetal and tangential accelerations are each
//            held to A_p, not their vector sum.
//  Corners   Consecutive feed moves (straight or arc) form a chain the tool runs through without stopping wherever the
//            direction at the end of one and at the start of the next differ by at most cornerStopDeg (default 10
//            degrees); there the junction speed is the lower of the two cruise speeds. A larger turn is a full stop.
//            A chain also stops at its ends: at a rapid, a dwell, a cycle, the end of an operation. The speeds through
//            a chain come from the usual two passes: backward, each junction no faster than the tool can brake from
//            to the next one (v_i^2 <= v_{i+1}^2 + 2 A L); forward, no faster than it can speed up to from the one
//            before.
//  Cycles    A canned-cycle move runs as cam.h "Canned cycles" expands it (cycletime.h expandCycle, every peck, every
//            re-entry and retract): each of its motions on its own profile from rest to rest, the rapids as rapids,
//            the feeds as feeds, its dwell at the bottom. A tap's spindle reversal takes no time.
//  Dwells    Dwell moves and cycle dwells add their milliseconds.
//  Tools     Each tool change adds MachineDef::toolChangeSec and covers the motion to the change position and back to
//            the next toolpath's start; the first operation counts one (the program loads its first tool). An
//            operation with the same tool as the one before rapids from that one's end to its own start.
//  Spindle   Not modelled: MachineDef has no spindle acceleration, so a spindle start, a speed change and a tap's
//            reversal take no time; nor do coolant, M-codes, block processing or the control's look-ahead.
//
// DEFAULTS (cam.h kCycleTimeDefault*), used where the machine definition has 0 (not known), each reported in
// CycleTimeAssumptions: acceleration 1000 mm/s^2 per axis (about 0.1 g, well under the 2.5 to 10 m/s^2 usual on
// machining centres), rapid rate 10 m/min per axis (well under the 25 to 54 m/min of the shipped machine definitions),
// maximum feed = the axis's rapid rate, tool change 10 s. Each is chosen slow on purpose, so an estimate made without
// the machine's data errs long, never short. They are round numbers, not data about any machine.
//
// WHAT MAKES IT WRONG ON A REAL CONTROL. The estimate leaves out: jerk limits (S-curve acceleration lengthens every
// speed change, so short moves take longer than here); look-ahead and smoothing modes (Fanuc AICC/HPCC, Haas
// "smoothing", Heidenhain M120/cycle 32) which round corners within a tolerance and keep speed through turns this model
// stops at, or slow down through dense short segments it runs at full speed (the block processing rate is not
// modelled); exact-stop modes (G61, G09) and in-position checks, which stop at junctions this model runs through;
// feed and rapid overrides; the real G0 path of a dogleg rapid; a tool changer's own sequence (the Z home move and the
// carousel, which vary with the tool distance); spindle run-up and orientation, coolant and other M-codes. Calibrate
// against a real machine (tests/data/cycletime, M8) before trusting an estimate to better than about 10 to 20 percent.
//
// DETERMINISM. Plain IEEE-754 double arithmetic in a fixed order (the module is built with strict floating point and
// no contraction), square roots and trigonometry through ray::dmath, sums in move order: the same inputs give the same
// bits on every platform.
//
// SPEED. One pass over the moves; a chain of feed moves is kept (32 bytes per move) until it stops, then planned and
// timed. Nothing else is allocated per move.
#include "cycletime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace ray::cam {
namespace {

using cycletime::Point;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPi = 3.14159265358979323846;
// The notes (estimateCycleTime) state these values in words.
static_assert(kCycleTimeDefaultAccelMmS2 == 1000 && kCycleTimeDefaultRapidMmMin == 10000 &&
              kCycleTimeDefaultToolChangeSec == 10);

double mmOf(Nm a, Nm b)
{
    return (static_cast<double>(a) - static_cast<double>(b)) / kNmPerMm;
}

double feedMmS(std::int32_t umPerMin)
{
    return static_cast<double>(umPerMin) / 60000.0; // um/min -> mm/s
}

struct AxisLimit {
    double rapid = 0;   // mm/s
    double maxFeed = 0; // mm/s
    double accel = 0;   // mm/s^2
};

struct MachineLimits {
    AxisLimit axis[3];
    double toolChangeSec = 0;
    bool accelUnknown = false, rapidUnknown = false, maxFeedUnknown = false, toolChangeUnknown = false;
};

bool known(double v)
{
    return v > 0 && v < kInf; // false for 0, negatives and NaN
}

MachineLimits limitsOf(const lib::MachineDef& m)
{
    MachineLimits out;
    for (int a = 0; a < 3; ++a) {
        const char upper = static_cast<char>('X' + a), lower = static_cast<char>('x' + a);
        const lib::AxisDef* def = nullptr;
        for (const lib::AxisDef& ax : m.axes) {
            if (ax.letter == upper || ax.letter == lower) {
                def = &ax;
                break;
            }
        }
        AxisLimit& lim = out.axis[a];
        const double rapid = def != nullptr ? def->rapidMmMin : 0;
        const double feed = def != nullptr ? def->maxFeedMmMin : 0;
        const double accel = def != nullptr ? def->accelMmS2 : 0;
        lim.rapid = (known(rapid) ? rapid : kCycleTimeDefaultRapidMmMin) / 60.0;
        out.rapidUnknown |= !known(rapid);
        lim.maxFeed = known(feed) ? feed / 60.0 : lim.rapid;
        out.maxFeedUnknown |= !known(feed);
        lim.accel = known(accel) ? accel : kCycleTimeDefaultAccelMmS2;
        out.accelUnknown |= !known(accel);
    }
    out.toolChangeUnknown = !known(m.toolChangeSec);
    out.toolChangeSec = known(m.toolChangeSec) ? m.toolChangeSec : kCycleTimeDefaultToolChangeSec;
    return out;
}

// The profile of the model's "Profile" paragraph: `len` from speed vs to speed ve, cruising at most at vmax,
// accelerating and braking at acc. vs and ve are reachable (|ve^2 - vs^2| <= 2 acc len, up to rounding).
double profileTime(double len, double vs, double ve, double vmax, double acc)
{
    if (!(len > 0))
        return 0;
    const double vs2 = vs * vs, ve2 = ve * ve, vm2 = vmax * vmax;
    const double dAcc = (vm2 - vs2) / (2.0 * acc);
    const double dDec = (vm2 - ve2) / (2.0 * acc);
    if (dAcc + dDec <= len)
        return (vmax - vs) / acc + (vmax - ve) / acc + (len - dAcc - dDec) / vmax;
    double vp = dmath::sqrt(acc * len + 0.5 * (vs2 + ve2));
    vp = std::max(vp, std::max(vs, ve)); // only rounding can put it below them
    return (vp - vs) / acc + (vp - ve) / acc;
}

// In-plane axes and axis of an arc: index into (x, y, z) of u, v and w (G17 XY, G18 XZ = (z, x, y), G19 YZ).
struct PlaneAxes {
    int u, v, w;
};
PlaneAxes axesOf(ArcPlane p)
{
    switch (p) {
    case ArcPlane::XZ:
        return {2, 0, 1};
    case ArcPlane::YZ:
        return {1, 2, 0};
    case ArcPlane::XY:
    default:
        return {0, 1, 2};
    }
}

Nm component(const Point& p, int axis)
{
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

// The counter-clockwise (ccw) or clockwise angle swept from start vector (su, sv) to end vector (eu, ev), in (0, 2 pi]:
// an end on the start ray is a full circle, as a control reads G2/G3.
double arcSweep(double su, double sv, double eu, double ev, bool ccw)
{
    const double a = dmath::atan2(su * ev - sv * eu, su * eu + sv * ev); // (-pi, pi]
    if (ccw)
        return a > 0.0 ? a : a + 2.0 * kPi;
    return a < 0.0 ? -a : 2.0 * kPi - a;
}

class Estimator {
public:
    Estimator(const MachineLimits& lim, const lib::MachineDef& machine, const CycleTimeOptions& opt)
        : lim_(lim), machine_(machine), interpolated_(opt.rapidInterpolated)
    {
        double deg = opt.cornerStopDeg;
        if (!(deg >= 0)) // NaN or negative: every corner stops
            deg = 0;
        deg = std::min(deg, 180.0);
        cosStop_ = dmath::cos(deg * (kPi / 180.0));
    }

    bool feedMissing() const { return feedMissing_; }

    // Times every motion from here into `part` (an operation's).
    void setPart(CycleTimePart* part) { part_ = part; }

    void startToolpath() { modalFeed_ = 0; }

    void rapid(const Point& from, const Point& to)
    {
        flush();
        const double d[3] = {mmOf(to.x, from.x), mmOf(to.y, from.y), mmOf(to.z, from.z)};
        const double len = dmath::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 0))
            return;
        double t = 0;
        if (interpolated_) {
            double v = kInf, acc = kInf;
            for (int a = 0; a < 3; ++a) {
                const double share = std::fabs(d[a]) / len;
                if (share > 0) {
                    v = std::min(v, lim_.axis[a].rapid / share);
                    acc = std::min(acc, lim_.axis[a].accel / share);
                }
            }
            t = profileTime(len, 0, 0, v, acc);
        } else {
            for (int a = 0; a < 3; ++a)
                t = std::max(t, profileTime(std::fabs(d[a]), 0, 0, lim_.axis[a].rapid, lim_.axis[a].accel));
        }
        part_->rapidSec += t;
        part_->rapidMm += len;
    }

    void line(const Point& from, const Point& to, std::int32_t feedUm)
    {
        const double d[3] = {mmOf(to.x, from.x), mmOf(to.y, from.y), mmOf(to.z, from.z)};
        const double len = dmath::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 0))
            return;
        double v = programmedFeed(feedUm), acc = kInf;
        double u[3];
        for (int a = 0; a < 3; ++a) {
            u[a] = d[a] / len;
            const double share = std::fabs(u[a]);
            if (share > 0) {
                v = std::min(v, lim_.axis[a].maxFeed / share);
                acc = std::min(acc, lim_.axis[a].accel / share);
            }
        }
        feed(len, v, acc, u, u);
    }

    void arc(const Point& from, const Move& mv)
    {
        const Point end{mv.x, mv.y, mv.z};
        const Point centre{mv.i, mv.j, mv.k};
        const PlaneAxes ax = axesOf(mv.plane);
        const Nm su = component(from, ax.u), sv = component(from, ax.v);
        const Nm eu = component(end, ax.u), ev = component(end, ax.v);
        const Nm cu = component(centre, ax.u), cv = component(centre, ax.v);
        if ((su == cu && sv == cv) || (eu == cu && ev == cv)) {
            line(from, end, mv.feedMicronPerMin); // no radius at one end: a straight motion
            return;
        }
        const double au = mmOf(su, cu), av = mmOf(sv, cv), bu = mmOf(eu, cu), bv = mmOf(ev, cv);
        const double r0 = dmath::sqrt(au * au + av * av), r1 = dmath::sqrt(bu * bu + bv * bv);
        const bool ccw = mv.kind == MoveKind::ArcCCW;
        const double sweep = arcSweep(au, av, bu, bv, ccw);
        const double inPlane = sweep * (0.5 * (r0 + r1));
        const double dw = mmOf(component(end, ax.w), component(from, ax.w));
        const double len = dmath::sqrt(inPlane * inPlane + dw * dw);
        if (!(len > 0))
            return;
        const double f = inPlane / len;        // in-plane share of the direction (> 0: r0, r1 and sweep are)
        const double fw = std::fabs(dw) / len; // along w
        const AxisLimit& lu = lim_.axis[ax.u];
        const AxisLimit& lv = lim_.axis[ax.v];
        const AxisLimit& lw = lim_.axis[ax.w];
        const double planeAccel = std::min(lu.accel, lv.accel);
        const double planeFeed = std::min(lu.maxFeed, lv.maxFeed);
        double v = programmedFeed(mv.feedMicronPerMin);
        v = std::min(v, planeFeed / f);
        v = std::min(v, dmath::sqrt(planeAccel * std::min(r0, r1)) / f); // centripetal
        double acc = planeAccel / f;
        if (fw > 0) {
            v = std::min(v, lw.maxFeed / fw);
            acc = std::min(acc, lw.accel / fw);
        }
        // Unit tangents at the ends: the in-plane tangent (the radius turned a quarter in the arc's sense) scaled by f,
        // with the helix's share along w.
        const double s = ccw ? 1.0 : -1.0;
        double t0[3], t1[3];
        t0[ax.u] = -s * av / r0 * f;
        t0[ax.v] = s * au / r0 * f;
        t0[ax.w] = dw / len;
        t1[ax.u] = -s * bv / r1 * f;
        t1[ax.v] = s * bu / r1 * f;
        t1[ax.w] = dw / len;
        feed(len, v, acc, t0, t1);
    }

    void dwell(std::int32_t ms)
    {
        flush();
        if (ms > 0)
            part_->dwellSec += static_cast<double>(ms) / 1000.0;
    }

    // A canned-cycle move: each motion on its own from rest to rest.
    Point cycle(const Point& from, const Move& mv)
    {
        flush();
        Point at = from;
        return cycletime::expandCycle(mv, from, &machine_, [&](const cycletime::CycleStep& st) {
            switch (st.kind) {
            case cycletime::StepKind::Rapid:
                rapid(at, st.to);
                break;
            case cycletime::StepKind::Feed:
                line(at, st.to, st.feed);
                flush();
                break;
            case cycletime::StepKind::Dwell:
                dwell(st.dwellMs);
                break;
            }
            at = st.to;
        });
    }

    // Plans and times the chain of feed moves, which stops here.
    void flush()
    {
        const std::size_t n = chain_.size();
        if (n == 0)
            return;
        // chain_[i].vIn is the junction speed before move i (0 for the first); after the last the tool stops.
        double vNext = 0;
        for (std::size_t k = n; k-- > 0;) { // backward: no faster than the tool can brake from
            Seg& sg = chain_[k];
            sg.vIn = std::min(sg.vIn, dmath::sqrt(vNext * vNext + 2.0 * sg.acc * sg.len));
            vNext = sg.vIn;
        }
        double t = 0;
        for (std::size_t k = 0; k < n; ++k) { // forward: no faster than it can speed up to; then the time
            const Seg& sg = chain_[k];
            double vOut = k + 1 < n ? chain_[k + 1].vIn : 0.0;
            vOut = std::min(vOut, dmath::sqrt(sg.vIn * sg.vIn + 2.0 * sg.acc * sg.len));
            if (k + 1 < n)
                chain_[k + 1].vIn = vOut;
            t += profileTime(sg.len, sg.vIn, vOut, sg.vmax, sg.acc);
        }
        part_->cutSec += t;
        chain_.clear();
    }

private:
    struct Seg {
        double len, vmax, acc, vIn;
    };

    double programmedFeed(std::int32_t feedUm)
    {
        if (feedUm > 0) {
            modalFeed_ = feedUm;
            return feedMmS(feedUm);
        }
        feedMissing_ = true;
        return modalFeed_ > 0 ? feedMmS(modalFeed_) : kInf;
    }

    void feed(double len, double vmax, double acc, const double (&t0)[3], const double (&t1)[3])
    {
        double vIn = 0;
        if (!chain_.empty()) {
            const double c = tangent_[0] * t0[0] + tangent_[1] * t0[1] + tangent_[2] * t0[2];
            if (c >= cosStop_)
                vIn = std::min(chain_.back().vmax, vmax);
            else
                flush();
        }
        chain_.push_back({len, vmax, acc, vIn});
        tangent_[0] = t1[0], tangent_[1] = t1[1], tangent_[2] = t1[2];
        part_->cutMm += len;
    }

    const MachineLimits& lim_;
    const lib::MachineDef& machine_;
    bool interpolated_;
    double cosStop_ = 1;
    CycleTimePart* part_ = nullptr;
    std::vector<Seg> chain_;
    double tangent_[3] = {0, 0, 0}; // at the end of the chain's last move
    std::int32_t modalFeed_ = 0;
    bool feedMissing_ = false;
};

void addInto(CycleTimePart& sum, const CycleTimePart& p)
{
    sum.cutSec += p.cutSec;
    sum.rapidSec += p.rapidSec;
    sum.toolChangeSec += p.toolChangeSec;
    sum.dwellSec += p.dwellSec;
    sum.totalSec += p.totalSec;
    sum.cutMm += p.cutMm;
    sum.rapidMm += p.rapidMm;
    sum.toolChanges += p.toolChanges;
}

} // namespace

CycleTime estimateCycleTime(std::span<const CycleTimeOperation> ops, const lib::MachineDef& machine,
                            const CycleTimeOptions& options)
{
    const MachineLimits lim = limitsOf(machine);
    Estimator est(lim, machine, options);
    CycleTime out;
    out.operations.reserve(ops.size());
    bool loaded = false; // a tool is in the spindle
    int tool = 0;
    Point pos;
    for (const CycleTimeOperation& op : ops) {
        CycleTimePart part;
        if (op.toolpath != nullptr) {
            const Toolpath& tp = *op.toolpath;
            est.setPart(&part);
            est.startToolpath();
            const Point start{tp.startX, tp.startY, tp.startZ};
            if (!loaded || op.toolNumber <= 0 || op.toolNumber != tool) {
                part.toolChangeSec = lim.toolChangeSec;
                part.toolChanges = 1;
            } else {
                est.rapid(pos, start);
            }
            loaded = true;
            tool = op.toolNumber;
            pos = start;
            for (const Move& mv : tp.moves) {
                const Point end{mv.x, mv.y, mv.z};
                switch (mv.kind) {
                case MoveKind::Rapid:
                    est.rapid(pos, end);
                    pos = end;
                    break;
                case MoveKind::Linear:
                    est.line(pos, end, mv.feedMicronPerMin);
                    pos = end;
                    break;
                case MoveKind::ArcCW:
                case MoveKind::ArcCCW:
                    est.arc(pos, mv);
                    pos = end;
                    break;
                case MoveKind::Dwell:
                    est.dwell(mv.cycleDwellMs);
                    break;
                case MoveKind::CycleCancel:
                    break;
                default:
                    if (cycletime::isCycleMotion(mv.kind))
                        pos = est.cycle(pos, mv);
                    break;
                }
            }
            est.flush();
            part.totalSec = part.cutSec + part.rapidSec + part.toolChangeSec + part.dwellSec;
        }
        addInto(out, part);
        out.operations.push_back(part);
    }
    CycleTimeAssumptions& as = out.assumptions;
    as.accelerationUnknown = lim.accelUnknown;
    as.rapidUnknown = lim.rapidUnknown;
    as.maxFeedUnknown = lim.maxFeedUnknown;
    as.toolChangeUnknown = lim.toolChangeUnknown && out.toolChanges > 0;
    as.feedMissing = est.feedMissing();
    if (as.accelerationUnknown)
        out.notes.emplace_back("The machine's axis accelerations are not known: 1000 mm/s^2 was assumed.");
    if (as.rapidUnknown)
        out.notes.emplace_back("The machine's rapid rates are not known: 10000 mm/min was assumed.");
    if (as.maxFeedUnknown)
        out.notes.emplace_back("The machine's maximum feeds are not known: feeds were held to the rapid rate.");
    if (as.toolChangeUnknown)
        out.notes.emplace_back("The machine's tool change time is not known: 10 s was assumed.");
    if (as.feedMissing)
        out.notes.emplace_back("A feed move has no feed: the feed before it, or else the machine's maximum, was used.");
    return out;
}

CycleTime estimateCycleTime(std::span<const Toolpath* const> toolpaths, const lib::MachineDef& machine)
{
    std::vector<CycleTimeOperation> ops;
    ops.reserve(toolpaths.size());
    for (const Toolpath* tp : toolpaths)
        ops.push_back({tp, 0});
    return estimateCycleTime(ops, machine);
}

} // namespace ray::cam
