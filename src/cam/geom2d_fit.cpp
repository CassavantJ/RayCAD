// fitArcs (task T-050): lines and arcs through a polyline. geom2d.h states the rules and thresholds; geom2d.cpp
// "TOLERANCES" why each threshold has its value.
//
// From point i, the longest line run and the longest arc run are found by exponential search on the end index j (1,
// 2, 4, ... points) and a binary search back from the first failure; the search stops at kMaxRun points. The arc run is
// tested on its shape (every point and every polyline edge within tolerance, monotone turning, sweep and radius) during
// the search and on its quality (chord, sagitta) at the end; the longer of the two runs wins, a line on a tie. An arc's
// circle is the least-squares (Kasa) circle of the run's points, its centre moved to the integer point among the nine
// around it that gives the smallest largest deviation (ties: smaller x, then smaller y), its radius the rounded middle
// of the smallest and largest distance. Every test is integer-exact where it decides a direction (crossSign) and plain
// IEEE double otherwise; nothing depends on the platform.
#include "geom2d_internal.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace ray::cam::geom2d {

using namespace detail;

namespace {

constexpr std::size_t kMaxRun = 4096;

struct Circle {
    Pt c;
    Coord r = 0;
    bool ccw = true;
};

class Fitter {
public:
    Fitter(const std::vector<Pt>& pts, const FitOptions& opt) : p_(pts), opt_(opt), tol_(static_cast<double>(opt.tolerance)) {}

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
        Circle best;
        double bestDev = std::numeric_limits<double>::infinity();
        const Pt c0{roundNm(centre->x), roundNm(centre->y)};
        for (Coord dx = -1; dx <= 1; ++dx)
            for (Coord dy = -1; dy <= 1; ++dy) {
                const Pt c{c0.x + dx, c0.y + dy};
                double lo = std::numeric_limits<double>::infinity(), hi = 0.0;
                for (std::size_t k = i; k <= j; ++k) {
                    const double d = distance(d2(p_[k]), d2(c));
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
                const Coord r = roundNm(0.5 * (lo + hi));
                const double dev = std::max(hi - static_cast<double>(r), static_cast<double>(r) - lo);
                if (dev < bestDev || (dev == bestDev && (c.x < best.c.x || (c.x == best.c.x && c.y < best.c.y)))) {
                    bestDev = dev;
                    best.c = c;
                    best.r = r;
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
            return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
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
        const D2 centre{-0.5 * dd * scale + o.x, -0.5 * ee * scale + o.y};
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
        const std::size_t jl = std::max<std::size_t>(i + 1, longest(i, i + 1, last, [&](std::size_t j) {
            return fitter.lineOk(i, j);
        }));
        const std::size_t ja = longest(i, i + 2, last, [&](std::size_t j) { return fitter.arcShape(i, j).has_value(); });
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
