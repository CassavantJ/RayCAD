// fitArcs (task T-050): lines and arcs through a polyline. geom2d.h states the rules and thresholds; geom2d.cpp
// "TOLERANCES" why each threshold has its value.
//
// From point i, the longest line run and the longest arc run are found by exponential search on the end index j (1,
// 2, 4, ... points) and a binary search back from the first failure; the search stops at kMaxRun points. The arc run is
// tested on its shape (every point and every polyline edge within tolerance, monotone turning, sweep and radius) during
// the search and on its quality (chord, sagitta) at the end; the longer of the two runs wins, a line on a tie. An arc's
// circle starts from the least-squares (Kasa) circle of the run's points; its centre is the integer point with the
// smallest largest deviation found among the nine round it (ties: smaller x, then smaller y) and then by descent over
// the eight neighbours at steps of 8, 4, 2 and 1 nm; its radius is the rounded middle of the smallest and largest
// distance. When that circle leaves either end of the run more than 1.25 nm off it (or the two ends' distances more
// than 1.5 nm apart), the centre is moved to the integer point nearest its projection onto the perpendicular bisector
// of the ends and the radius becomes the rounded mean of the ends' distances (ends within 1.21 nm, 1.42 nm apart at
// most); if that circle misses a point by more than the tolerance, a descent along the bisector (steps from a quarter
// of the chord down to 1 nm) looks for one that does not. The run must still pass every test below with that circle;
// so a fitted arc is a valid toolpath arc as it stands. On short, flat arcs many integer centres fit equally well (the
// samples' rounding allows it), so the centre of a sampled known arc is found exactly in about 85% of cases and
// otherwise within the rounding's reach (the property test measures it: at most 131 nm on a 10 degree arc of 100 mm
// radius), always within the tolerance. Every test is integer-exact where it decides a direction (crossSign) and plain
// IEEE double otherwise; nothing depends on the platform.
#include "geom2d_internal.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace ray::cam::geom2d {

using namespace detail;

namespace {

constexpr std::size_t kMaxRun = 4096;
// A fitted arc's ends (geom2d.h fitArcs): each within kEndOnCircle of its circle, their distances from the centre
// within kEndsApart of each other (the toolpath IR and its invariant checker allow 2 nm between them).
constexpr double kEndOnCircle = 1.25;
constexpr double kEndsApart = 1.5;

struct Circle {
    Pt c;
    Coord r = 0;
    bool ccw = true;
};

class Fitter {
public:
    Fitter(const std::vector<Pt>& pts, const FitOptions& opt)
        : p_(pts), opt_(opt), tol_(static_cast<double>(opt.tolerance))
    {
    }

    bool lineOk(std::size_t i, std::size_t j) const
    {
        const D2 a = d2(p_[i]);
        const D2 d = d2(p_[j]) - a;
        const double len = norm(d);
        if (len <= 0.0)
            return false;
        const D2 u = d * (1.0 / len);
        double last = -tol_;
        for (std::size_t k = i + 1; k < j; ++k) {
            const D2 q = d2(p_[k]) - a;
            const double along = dot(q, u);
            if (along < last - tol_ || along < -tol_ || along > len + tol_)
                return false;
            last = std::max(last, along);
            const double off = std::abs(cross(u, q));
            const double outside = along < 0.0 ? -along : (along > len ? along - len : 0.0);
            if (dmath::sqrt(off * off + outside * outside) > tol_)
                return false;
        }
        return true;
    }

    // The arc's circle and shape test; nullopt when the run cannot be one arc.
    std::optional<Circle> arcShape(std::size_t i, std::size_t j) const
    {
        if (j < i + 2)
            return std::nullopt;
        const auto centre = leastSquaresCentre(i, j);
        if (!centre)
            return std::nullopt;
        // The integer centre with the smallest largest deviation: the nine points round the least-squares centre,
        // then a descent over the eight neighbours at steps 8, 4, 2, 1 nm (ties keep the current point, so the
        // result does not depend on the order neighbours are tried in beyond the fixed order below).
        Circle best;
        double bestDev = std::numeric_limits<double>::infinity();
        const auto evaluate = [&](Pt c) {
            double lo = std::numeric_limits<double>::infinity(), hi = 0.0;
            for (std::size_t k = i; k <= j; ++k) {
                const double d = distance(d2(p_[k]), d2(c));
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
            const Coord r = roundNm(0.5 * (lo + hi));
            return std::make_pair(std::max(hi - static_cast<double>(r), static_cast<double>(r) - lo), r);
        };
        const Pt c0{roundNm(centre->x), roundNm(centre->y)};
        for (Coord dx = -1; dx <= 1; ++dx)
            for (Coord dy = -1; dy <= 1; ++dy) {
                const Pt c{c0.x + dx, c0.y + dy};
                const auto [dev, r] = evaluate(c);
                if (dev < bestDev || (dev == bestDev && (c.x < best.c.x || (c.x == best.c.x && c.y < best.c.y)))) {
                    bestDev = dev;
                    best.c = c;
                    best.r = r;
                }
            }
        for (Coord step = 8; step >= 1; step /= 2) {
            for (int moves = 0; moves < 64; ++moves) {
                bool moved = false;
                for (Coord dx = -step; dx <= step && !moved; dx += step)
                    for (Coord dy = -step; dy <= step && !moved; dy += step) {
                        if (dx == 0 && dy == 0)
                            continue;
                        const Pt c{best.c.x + dx, best.c.y + dy};
                        const auto [dev, r] = evaluate(c);
                        if (dev < bestDev) {
                            bestDev = dev;
                            best.c = c;
                            best.r = r;
                            moved = true;
                        }
                    }
                if (!moved)
                    break;
            }
        }
        // Both ends on the circle as the toolpath IR wants them (geom2d.h fitArcs): when the best centre leaves them
        // farther, the centre moves onto the perpendicular bisector of the ends and the radius is their mean distance
        // (first the projection of the best centre; if that misses a point by more than the tolerance, a descent along
        // the bisector on the largest deviation, steps from a quarter of the chord down to 1 nm); the arc must then
        // still be within the tolerance of every point.
        if (!endsOnCircle(i, j, best)) {
            if (p_[i] == p_[j])
                return std::nullopt;
            const Bisector b = bisectorOf(i, j);
            double t = b.param(best.c);
            best = circleOn(i, j, b, t);
            bestDev = deviationOf(i, j, best);
            if (bestDev > tol_) {
                for (double step = std::max(1.0, std::floor(b.chord / 4)); step >= 1.0; step = std::floor(step / 2)) {
                    for (int moves = 0; moves < 64; ++moves) {
                        bool moved = false;
                        for (const double dt : {step, -step}) {
                            const Circle c = circleOn(i, j, b, t + dt);
                            const double dev = deviationOf(i, j, c);
                            if (dev < bestDev) {
                                t += dt;
                                best = c;
                                bestDev = dev;
                                moved = true;
                                break;
                            }
                        }
                        if (!moved)
                            break;
                    }
                }
            }
        }
        if (best.r <= 0 || best.r > opt_.maxArcRadius || bestDev > tol_)
            return std::nullopt;
        // Monotone turning about the centre, each step below 90 degrees, all one way.
        int way = 0;
        double sweep = 0.0;
        const double r = static_cast<double>(best.r);
        for (std::size_t k = i; k < j; ++k) {
            const Pt u{p_[k].x - best.c.x, p_[k].y - best.c.y};
            const Pt w{p_[k + 1].x - best.c.x, p_[k + 1].y - best.c.y};
            const int s = crossSign(u.x, u.y, w.x, w.y);
            if (s == 0 || (way != 0 && s != way) || dotSign(u.x, u.y, w.x, w.y) <= 0)
                return std::nullopt;
            way = s;
            const D2 du = d2(u), dw = d2(w);
            sweep += std::abs(dmath::atan2(cross(du, dw), dot(du, dw)));
            // The polyline edge against the arc between its ends: the arc's sagitta over the edge plus the larger
            // deviation of the ends.
            const double half = 0.5 * distance(du, dw);
            if (half >= r)
                return std::nullopt;
            const double sag = r - dmath::sqrt((r - half) * (r + half));
            const double ends = std::max(std::abs(norm(du) - r), std::abs(norm(dw) - r));
            if (sag + ends > tol_)
                return std::nullopt;
        }
        if (sweep > opt_.maxSweep + 1e-12)
            return std::nullopt;
        best.ccw = way > 0;
        return best;
    }

    bool arcQuality(std::size_t i, std::size_t j, const Circle& c) const
    {
        if (distance(d2(p_[i]), d2(p_[j])) < static_cast<double>(opt_.minArcChord))
            return false;
        double sweep = 0.0;
        for (std::size_t k = i; k < j; ++k) {
            const D2 u = d2(p_[k]) - d2(c.c), w = d2(p_[k + 1]) - d2(c.c);
            sweep += std::abs(dmath::atan2(cross(u, w), dot(u, w)));
        }
        const double sagitta = static_cast<double>(c.r) * (1.0 - dmath::cos(0.5 * sweep));
        return sagitta > tol_;
    }

private:
    // The distances of the run's end points from c (dmath::hypot, as onBisector measures them).
    std::pair<double, double> endDistances(std::size_t i, std::size_t j, Pt c) const
    {
        const double cx = static_cast<double>(c.x), cy = static_cast<double>(c.y);
        return {dmath::hypot(static_cast<double>(p_[i].x) - cx, static_cast<double>(p_[i].y) - cy),
                dmath::hypot(static_cast<double>(p_[j].x) - cx, static_cast<double>(p_[j].y) - cy)};
    }
    bool endsOnCircle(std::size_t i, std::size_t j, const Circle& c) const
    {
        const auto [di, dj] = endDistances(i, j, c.c);
        const double r = static_cast<double>(c.r);
        return std::abs(di - r) <= kEndOnCircle && std::abs(dj - r) <= kEndOnCircle && std::abs(di - dj) <= kEndsApart;
    }
    // The perpendicular bisector of the run's ends (distinct): their middle m and the unit normal n of the chord.
    struct Bisector {
        double mx = 0, my = 0, nx = 0, ny = 0, chord = 0;
        double param(Pt c) const { return (static_cast<double>(c.x) - mx) * nx + (static_cast<double>(c.y) - my) * ny; }
    };
    Bisector bisectorOf(std::size_t i, std::size_t j) const
    {
        const double ax = static_cast<double>(p_[i].x), ay = static_cast<double>(p_[i].y);
        const double bx = static_cast<double>(p_[j].x), by = static_cast<double>(p_[j].y);
        Bisector b;
        b.mx = (ax + bx) / 2;
        b.my = (ay + by) / 2;
        b.nx = -(by - ay);
        b.ny = bx - ax;
        b.chord = dmath::hypot(b.nx, b.ny);
        b.nx /= b.chord;
        b.ny /= b.chord;
        return b;
    }
    // The integer point nearest m + t n, with the rounded mean of the two end distances as radius. Rounding moves the
    // centre by at most 0.71 nm, so the two distances differ by at most 1.42 nm and each lies within 1.21 nm of the
    // radius.
    Circle circleOn(std::size_t i, std::size_t j, const Bisector& b, double t) const
    {
        Circle c;
        c.c = Pt{roundNm(b.mx + t * b.nx), roundNm(b.my + t * b.ny)};
        const auto [di, dj] = endDistances(i, j, c.c);
        c.r = roundNm((di + dj) / 2);
        return c;
    }
    double deviationOf(std::size_t i, std::size_t j, const Circle& c) const
    {
        double dev = 0.0;
        for (std::size_t k = i; k <= j; ++k)
            dev = std::max(dev, std::abs(distance(d2(p_[k]), d2(c.c)) - static_cast<double>(c.r)));
        return dev;
    }

    std::optional<D2> leastSquaresCentre(std::size_t i, std::size_t j) const
    {
        const D2 o = d2(p_[i]);
        double scale = 0.0;
        for (std::size_t k = i; k <= j; ++k) {
            const D2 q = d2(p_[k]) - o;
            scale = std::max({scale, std::abs(q.x), std::abs(q.y)});
        }
        if (scale <= 0.0)
            return std::nullopt;
        // Minimise sum (x^2 + y^2 + D x + E y + F)^2 over the scaled points.
        double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxz = 0, syz = 0, sz = 0, n = 0;
        for (std::size_t k = i; k <= j; ++k) {
            const D2 q = (d2(p_[k]) - o) * (1.0 / scale);
            const double z = q.x * q.x + q.y * q.y;
            sxx += q.x * q.x;
            sxy += q.x * q.y;
            syy += q.y * q.y;
            sx += q.x;
            sy += q.y;
            sxz += q.x * z;
            syz += q.y * z;
            sz += z;
            n += 1.0;
        }
        // Solve [sxx sxy sx; sxy syy sy; sx sy n] [D E F] = -[sxz syz sz] by Cramer's rule.
        const double m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n}};
        const double b[3] = {-sxz, -syz, -sz};
        const auto det3 = [](const double a[3][3]) {
            return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                   a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        };
        const double det = det3(m);
        if (!(std::abs(det) > 1e-18))
            return std::nullopt;
        double mx[3][3], my[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                mx[r][c] = c == 0 ? b[r] : m[r][c];
                my[r][c] = c == 1 ? b[r] : m[r][c];
            }
        const double dd = det3(mx) / det, ee = det3(my) / det;
        const double cx = -0.5 * dd, cy = -0.5 * ee;
        const D2 centre{cx * scale + o.x, cy * scale + o.y};
        if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || std::abs(centre.x) > 4e12 ||
            std::abs(centre.y) > 4e12)
            return std::nullopt;
        return centre;
    }

    const std::vector<Pt>& p_;
    const FitOptions& opt_;
    double tol_;
};

// The largest j in [minJ, last] (at most kMaxRun points from i) for which ok(j) holds, found by doubling the step
// and searching back from the first failure; 0 if ok(minJ) fails.
template <class Ok>
std::size_t longest(std::size_t i, std::size_t minJ, std::size_t last, Ok ok)
{
    if (minJ > last || !ok(minJ))
        return 0;
    std::size_t good = minJ;
    std::size_t step = 1;
    const std::size_t cap = std::min(last, i + kMaxRun);
    while (good < cap) {
        const std::size_t candidate = std::min(cap, good + step);
        if (ok(candidate)) {
            good = candidate;
            step *= 2;
            continue;
        }
        std::size_t lo = good + 1, hi = candidate - 1;
        while (lo <= hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (ok(mid)) {
                good = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        break;
    }
    return good;
}

} // namespace

Path fitArcs(std::span<const Pt> input, bool closed, const FitOptions& opt)
{
    std::vector<Pt> pts;
    pts.reserve(input.size() + 1);
    for (const Pt& p : input)
        if (pts.empty() || pts.back() != p)
            pts.push_back(p);
    if (closed && pts.size() > 1 && pts.back() == pts.front())
        pts.pop_back();
    Path out;
    out.closed = closed;
    if (pts.empty())
        return out;
    out.start = pts[0];
    if (closed)
        pts.push_back(pts[0]);
    if (pts.size() < 2)
        return out;
    const Fitter fitter(pts, opt);
    const std::size_t last = pts.size() - 1;
    std::size_t i = 0;
    while (i < last) {
        const std::size_t jl =
            std::max<std::size_t>(i + 1, longest(i, i + 1, last, [&](std::size_t j) { return fitter.lineOk(i, j); }));
        const std::size_t ja =
            longest(i, i + 2, last, [&](std::size_t j) { return fitter.arcShape(i, j).has_value(); });
        if (ja > jl) {
            const auto circle = fitter.arcShape(i, ja);
            if (circle && fitter.arcQuality(i, ja, *circle)) {
                out.spans.push_back(
                    Span{circle->ccw ? SpanKind::ArcCCW : SpanKind::ArcCW, pts[ja], circle->c, circle->r});
                i = ja;
                continue;
            }
        }
        out.spans.push_back(Span{SpanKind::Line, pts[jl], {}, 0});
        i = jl;
    }
    return out;
}

} // namespace ray::cam::geom2d
