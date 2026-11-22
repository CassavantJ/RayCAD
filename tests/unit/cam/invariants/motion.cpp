// The tool's motion as the invariant checker sees it (motion.h).
#include "motion.h"

#include <algorithm>
#include <cmath>

namespace ray::cam::invariants {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;

bool isCycleKind(MoveKind k)
{
    return k == MoveKind::CycleDrill || k == MoveKind::CyclePeck || k == MoveKind::CycleChipBreak ||
           k == MoveKind::CycleTap || k == MoveKind::CycleBore || k == MoveKind::CycleReam;
}

// Angle a brought into [0, 2 pi).
double wrap(double a)
{
    a = std::fmod(a, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

} // namespace

double Piece::xyLength() const
{
    if (arc) {
        return ra * std::abs(sweep);
    }
    return std::hypot(b.x - a.x, b.y - a.y);
}

bool Piece::vertical() const
{
    return !arc && a.x == b.x && a.y == b.y;
}

Vec3 Piece::at(double t) const
{
    const double z = a.z + t * (b.z - a.z);
    if (arc) {
        const double th = th0 + t * sweep;
        return Vec3{cx + ra * std::cos(th), cy + ra * std::sin(th), z};
    }
    return Vec3{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), z};
}

Vec2 Piece::direction(double t) const
{
    if (arc) {
        const double th = th0 + t * sweep;
        const double s = sweep > 0 ? 1.0 : -1.0;
        return Vec2{-std::sin(th) * s, std::cos(th) * s};
    }
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len <= 0) {
        return Vec2{0, 0};
    }
    return Vec2{(b.x - a.x) / len, (b.y - a.y) / len};
}

Piece::Near Piece::nearest(double qx, double qy) const
{
    Near n;
    if (arc) {
        const double dx = qx - cx, dy = qy - cy;
        const double rq = std::hypot(dx, dy);
        double s = 0; // param angle from th0 along the sweep
        if (rq > 0) {
            const double phi = std::atan2(dy, dx);
            s = sweep > 0 ? wrap(phi - th0) : wrap(th0 - phi);
        }
        const double total = std::abs(sweep);
        if (s <= total) {
            n.t = total > 0 ? s / total : 0;
            n.d = std::abs(rq - ra);
        } else {
            const Vec3 p0 = at(0), p1 = at(1);
            const double d0 = std::hypot(qx - p0.x, qy - p0.y), d1 = std::hypot(qx - p1.x, qy - p1.y);
            n.t = d0 <= d1 ? 0 : 1;
            n.d = std::min(d0, d1);
        }
        n.z = a.z + n.t * (b.z - a.z);
        return n;
    }
    const double vx = b.x - a.x, vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    if (len2 <= 0) {
        n.t = a.z <= b.z ? 0 : 1;
        n.d = std::hypot(qx - a.x, qy - a.y);
        n.z = std::min(a.z, b.z);
        return n;
    }
    n.t = std::clamp(((qx - a.x) * vx + (qy - a.y) * vy) / len2, 0.0, 1.0);
    n.d = std::hypot(qx - (a.x + n.t * vx), qy - (a.y + n.t * vy));
    n.z = a.z + n.t * (b.z - a.z);
    return n;
}

std::pair<double, double> Piece::zRange(double qx, double qy, double rho, const Near& n) const
{
    if (a.z == b.z) {
        return {a.z - inflate, a.z + inflate};
    }
    const auto [t0, t1] = reach(qx, qy, rho, n);
    const double z0 = a.z + t0 * (b.z - a.z), z1 = a.z + t1 * (b.z - a.z);
    return {std::min(z0, z1) - inflate, std::max(z0, z1) + inflate};
}

std::pair<double, double> Piece::reach(double qx, double qy, double rho, const Near& n) const
{
    double t0 = n.t, t1 = n.t;
    if (arc) {
        const double dx = qx - cx, dy = qy - cy;
        const double rq = std::hypot(dx, dy);
        const double total = std::abs(sweep);
        if (rq + ra <= rho || rq == 0) {
            t0 = 0;
            t1 = 1;
        } else if (std::abs(rq - ra) <= rho) {
            const double c = std::clamp((ra * ra + rq * rq - rho * rho) / (2 * ra * rq), -1.0, 1.0);
            const double half = std::acos(c);
            const double phi = std::atan2(dy, dx);
            const double s = sweep > 0 ? wrap(phi - th0) : wrap(th0 - phi);
            for (const double k : {-kTwoPi, 0.0, kTwoPi}) {
                const double lo = std::max(0.0, s + k - half), hi = std::min(total, s + k + half);
                if (lo <= hi && total > 0) {
                    t0 = std::min(t0, lo / total);
                    t1 = std::max(t1, hi / total);
                }
            }
        }
    } else {
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double A = vx * vx + vy * vy;
        if (A <= 0) {
            t0 = 0;
            t1 = 1;
        } else {
            const double wx = a.x - qx, wy = a.y - qy;
            const double B = 2 * (wx * vx + wy * vy);
            const double C = wx * wx + wy * wy - rho * rho;
            const double disc = B * B - 4 * A * C;
            if (disc >= 0) {
                const double sq = std::sqrt(disc);
                const double r0 = (-B - sq) / (2 * A), r1 = (-B + sq) / (2 * A);
                const double lo = std::max(0.0, r0), hi = std::min(1.0, r1);
                if (lo <= hi) {
                    t0 = std::min(t0, lo);
                    t1 = std::max(t1, hi);
                }
            }
        }
    }
    return {t0, t1};
}

void Piece::box(double& x0, double& y0, double& x1, double& y1) const
{
    if (arc) {
        x0 = cx - ra;
        x1 = cx + ra;
        y0 = cy - ra;
        y1 = cy + ra;
        return;
    }
    x0 = std::min(a.x, b.x);
    x1 = std::max(a.x, b.x);
    y0 = std::min(a.y, b.y);
    y1 = std::max(a.y, b.y);
}

Piece Piece::part(double t0, double t1) const
{
    Piece p = *this;
    p.a = at(t0);
    p.b = at(t1);
    if (arc) {
        p.th0 = th0 + t0 * sweep;
        p.sweep = (t1 - t0) * sweep;
    }
    return p;
}

Motion motionOf(const Toolpath& path)
{
    Motion out;
    Vec3 cur{mmOf(path.startX), mmOf(path.startY), mmOf(path.startZ)};
    const auto line = [&](std::size_t n, const Move& m, Vec3 to, bool rapid, MoveRole role) {
        Piece p;
        p.move = n;
        p.kind = m.kind;
        p.role = role;
        p.rapid = rapid;
        p.a = cur;
        p.b = to;
        if (p.a.x != p.b.x || p.a.y != p.b.y || p.a.z != p.b.z) {
            out.pieces.push_back(p);
        }
        cur = to;
    };
    for (std::size_t n = 0; n < path.moves.size(); ++n) {
        const Move& m = path.moves[n];
        out.startOf.push_back(cur);
        const Vec3 end{mmOf(m.x), mmOf(m.y), mmOf(m.z)};
        switch (m.kind) {
        case MoveKind::Rapid:
            line(n, m, end, true, m.role);
            break;
        case MoveKind::Linear:
            line(n, m, end, false, m.role);
            break;
        case MoveKind::ArcCW:
        case MoveKind::ArcCCW: {
            const bool ccw = m.kind == MoveKind::ArcCCW;
            // (u, v) the plane's axes, w the third: XY (x, y | z), XZ (z, x | y), YZ (y, z | x).
            const auto uvw = [&](const Vec3& p) -> Vec3 {
                switch (m.plane) {
                case ArcPlane::XZ:
                    return Vec3{p.z, p.x, p.y};
                case ArcPlane::YZ:
                    return Vec3{p.y, p.z, p.x};
                default:
                    return p;
                }
            };
            const auto xyz = [&](const Vec3& q) -> Vec3 {
                switch (m.plane) {
                case ArcPlane::XZ:
                    return Vec3{q.y, q.z, q.x};
                case ArcPlane::YZ:
                    return Vec3{q.z, q.x, q.y};
                default:
                    return q;
                }
            };
            const Vec3 s = uvw(cur), e = uvw(end), c = uvw(Vec3{mmOf(m.i), mmOf(m.j), mmOf(m.k)});
            const double ra = std::hypot(s.x - c.x, s.y - c.y);
            const double th0 = std::atan2(s.y - c.y, s.x - c.x);
            const double th1 = std::atan2(e.y - c.y, e.x - c.x);
            const bool full = s.x == e.x && s.y == e.y;
            double sweep = 0;
            if (ccw) {
                sweep = full ? kTwoPi : wrap(th1 - th0);
                if (sweep == 0) {
                    sweep = kTwoPi;
                }
            } else {
                sweep = full ? -kTwoPi : -wrap(th0 - th1);
                if (sweep == 0) {
                    sweep = -kTwoPi;
                }
            }
            if (ra <= 0) {
                line(n, m, end, false, m.role);
                break;
            }
            if (m.plane == ArcPlane::XY) {
                Piece p;
                p.move = n;
                p.kind = m.kind;
                p.role = m.role;
                p.arc = true;
                p.a = cur;
                p.b = end;
                p.cx = c.x;
                p.cy = c.y;
                p.ra = ra;
                p.th0 = th0;
                p.sweep = sweep;
                out.pieces.push_back(p);
                cur = end;
                break;
            }
            // XZ, YZ: chords with a sagitta of at most kChordSagittaMm.
            const double step = ra > kChordSagittaMm ? 2 * std::acos(1 - kChordSagittaMm / ra) : kPi;
            const int count = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / step)));
            const double dth = sweep / count;
            const double sag = ra * (1 - std::cos(dth / 2));
            for (int k = 1; k <= count; ++k) {
                const double th = th0 + dth * k;
                const double w = s.z + (e.z - s.z) * k / count;
                const Vec3 q = k == count ? end : xyz(Vec3{c.x + ra * std::cos(th), c.y + ra * std::sin(th), w});
                Piece p;
                p.move = n;
                p.kind = m.kind;
                p.role = m.role;
                p.a = cur;
                p.b = q;
                p.inflate = sag;
                out.pieces.push_back(p);
                cur = q;
            }
            break;
        }
        case MoveKind::Dwell:
        case MoveKind::CycleCancel:
            break;
        default:
            if (isCycleKind(m.kind)) {
                const double clear = mmOf(m.cycleClearance), r = mmOf(m.cycleRetract);
                line(n, m, Vec3{end.x, end.y, cur.z}, true, MoveRole::Link);
                if (cur.z > clear) {
                    line(n, m, Vec3{end.x, end.y, clear}, true, MoveRole::Approach);
                }
                line(n, m, Vec3{end.x, end.y, r}, true, MoveRole::Approach);
                line(n, m, end, false, MoveRole::Plunge);
                const bool feedOut =
                    m.kind == MoveKind::CycleTap || m.kind == MoveKind::CycleBore || m.kind == MoveKind::CycleReam;
                line(n, m, Vec3{end.x, end.y, r}, !feedOut, MoveRole::Retract);
                if (m.cycleReturn == CycleReturn::Initial) {
                    line(n, m, Vec3{end.x, end.y, clear}, true, MoveRole::Retract);
                }
            }
            break;
        }
    }
    out.startOf.push_back(cur);
    return out;
}

} // namespace ray::cam::invariants
