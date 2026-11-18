// Curve arithmetic for loops and DXF (task T-091). See loops_curves.h.
#include "loops_curves.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ray::sketch::detail {
namespace {

// Gauss-Legendre rule with 10 points on [-1, 1]: exact for polynomials up to degree 19.
constexpr std::array<double, 5> kGaussX = {0.1488743389816312108848260, 0.4333953941292471907992659,
                                           0.6794095682990244062343274, 0.8650633666889845107320967,
                                           0.9739065285171717200779640};
constexpr std::array<double, 5> kGaussW = {0.2955242247147528701738930, 0.2692667193099963550912269,
                                           0.2190863625159820439955349, 0.1494513491505805931457763,
                                           0.0666713443086881375935688};

void insertKnot(std::vector<HPoint>& poles, std::vector<double>& knots, int p, double u)
{
    const int n = static_cast<int>(poles.size());
    // k: the last index with knots[k] <= u; s: how often u is a knot already.
    int k = static_cast<int>(std::upper_bound(knots.begin(), knots.end(), u) - knots.begin()) - 1;
    const int s = static_cast<int>(std::count(knots.begin(), knots.end(), u));
    std::vector<HPoint> out(static_cast<std::size_t>(n + 1));
    for (int i = 0; i <= k - p; ++i) {
        out[static_cast<std::size_t>(i)] = poles[static_cast<std::size_t>(i)];
    }
    for (int i = std::max(k - s, 0); i < n; ++i) {
        out[static_cast<std::size_t>(i + 1)] = poles[static_cast<std::size_t>(i)];
    }
    for (int i = std::max(k - p + 1, 1); i <= k - s; ++i) {
        const double ui = knots[static_cast<std::size_t>(i)];
        const double alpha = (u - ui) / (knots[static_cast<std::size_t>(i + p)] - ui);
        const HPoint& a = poles[static_cast<std::size_t>(i)];
        const HPoint& b = poles[static_cast<std::size_t>(i - 1)];
        out[static_cast<std::size_t>(i)] = {alpha * a.x + (1 - alpha) * b.x, alpha * a.y + (1 - alpha) * b.y,
                                            alpha * a.w + (1 - alpha) * b.w};
    }
    knots.insert(knots.begin() + (k + 1), u);
    poles = std::move(out);
}

} // namespace

Vec2 bezierPoint(const std::vector<Vec2>& pts, double t)
{
    std::vector<Vec2> work = pts;
    for (std::size_t r = 1; r < work.size(); ++r) {
        for (std::size_t i = 0; i + r < work.size(); ++i) {
            work[i] = lerp(work[i], work[i + 1], t);
        }
    }
    return work.empty() ? Vec2{} : work[0];
}

Vec2 bezierDerivative(const std::vector<Vec2>& pts, double t)
{
    if (pts.size() < 2) {
        return {};
    }
    const double p = static_cast<double>(pts.size() - 1);
    std::vector<Vec2> diff;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        diff.push_back(p * (pts[i + 1] - pts[i]));
    }
    return bezierPoint(diff, t);
}

Vec2 bezierEndDirection(const std::vector<Vec2>& pts, bool atEnd)
{
    const std::size_t n = pts.size();
    for (std::size_t i = 1; i < n; ++i) {
        const Vec2 d = atEnd ? pts[n - 1] - pts[n - 1 - i] : pts[i] - pts[0];
        if (d.x != 0 || d.y != 0) {
            return d;
        }
    }
    return {};
}

double bezierEndCurvature(const std::vector<Vec2>& pts, bool atEnd)
{
    const std::size_t n = pts.size();
    if (n < 3) {
        return 0;
    }
    const double p = static_cast<double>(n - 1);
    Vec2 d1;
    Vec2 d2;
    if (atEnd) {
        d1 = p * (pts[n - 1] - pts[n - 2]);
        d2 = (p * (p - 1)) * ((pts[n - 1] - 2.0 * pts[n - 2]) + pts[n - 3]);
    } else {
        d1 = p * (pts[1] - pts[0]);
        d2 = (p * (p - 1)) * ((pts[2] - 2.0 * pts[1]) + pts[0]);
    }
    const double speed = norm(d1);
    if (!(speed > 0)) {
        return 0;
    }
    return cross(d1, d2) / (speed * speed * speed);
}

std::optional<double> bezierAreaTerm(const std::vector<Vec2>& pts)
{
    if (pts.size() < 2) {
        return 0.0;
    }
    if (pts.size() > 11) {
        return std::nullopt;
    }
    double sum = 0;
    for (std::size_t i = 0; i < kGaussX.size(); ++i) {
        for (const double sign : {-1.0, 1.0}) {
            const double t = 0.5 * (1 + sign * kGaussX[i]);
            const Vec2 q = bezierPoint(pts, t);
            const Vec2 d = bezierDerivative(pts, t);
            sum += 0.5 * kGaussW[i] * cross(q, d);
        }
    }
    return sum;
}

std::pair<std::vector<Vec2>, std::vector<Vec2>> bezierSplit(const std::vector<Vec2>& pts, double t)
{
    std::vector<Vec2> work = pts;
    std::vector<Vec2> left;
    std::vector<Vec2> right(pts.size());
    const std::size_t n = pts.size();
    for (std::size_t r = 0; r < n; ++r) {
        left.push_back(work[0]);
        right[n - 1 - r] = work[n - 1 - r];
        for (std::size_t i = 0; i + 1 < n - r; ++i) {
            work[i] = lerp(work[i], work[i + 1], t);
        }
    }
    return {left, right};
}

std::vector<double> clampedUniformKnots(int n, int p)
{
    std::vector<double> knots;
    for (int i = 0; i <= p; ++i) {
        knots.push_back(0.0);
    }
    const int spans = n - p;
    for (int i = 1; i < spans; ++i) {
        knots.push_back(static_cast<double>(i) / static_cast<double>(spans));
    }
    for (int i = 0; i <= p; ++i) {
        knots.push_back(1.0);
    }
    return knots;
}

namespace {

// Bezier segments by knot insertion over the whole curve (quadratic in its size; used on one span at a time).
std::optional<std::vector<std::vector<HPoint>>> decomposeAll(std::vector<HPoint> poles, std::vector<double> knots,
                                                             int p)
{
    const int n = static_cast<int>(poles.size());
    if (p < 1 || n < p + 1 || knots.size() != static_cast<std::size_t>(n + p + 1)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < knots.size(); ++i) {
        if (!std::isfinite(knots[i]) || (i > 0 && knots[i] < knots[i - 1])) {
            return std::nullopt;
        }
    }
    for (const HPoint& q : poles) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.w) || !(q.w > 0)) {
            return std::nullopt;
        }
    }
    const double a = knots[static_cast<std::size_t>(p)];
    const double b = knots[static_cast<std::size_t>(n)];
    if (!(b > a)) {
        return std::nullopt;
    }
    const double eps = 1e-12 * (b - a);
    for (std::size_t i = 1; i < knots.size(); ++i) {
        if (knots[i] - knots[i - 1] <= eps) {
            knots[i] = knots[i - 1];
        }
    }
    std::vector<double> values;
    for (const double u : knots) {
        if (u >= a && u <= b && (values.empty() || values.back() != u)) {
            values.push_back(u);
        }
    }
    for (const double u : values) {
        auto mult = std::count(knots.begin(), knots.end(), u);
        while (mult < p) {
            insertKnot(poles, knots, p, u);
            ++mult;
        }
    }
    std::vector<std::vector<HPoint>> segments;
    for (std::size_t j = 0; j + 1 < values.size(); ++j) {
        const int k = static_cast<int>(std::upper_bound(knots.begin(), knots.end(), values[j]) - knots.begin()) - 1;
        if (k - p < 0 || k >= static_cast<int>(poles.size())) {
            return std::nullopt;
        }
        segments.emplace_back(poles.begin() + (k - p), poles.begin() + (k + 1));
    }
    return segments;
}

} // namespace

std::optional<std::vector<std::vector<HPoint>>> bezierDecompose(std::vector<HPoint> poles, std::vector<double> knots,
                                                                int p)
{
    const int n = static_cast<int>(poles.size());
    if (p < 1 || n < p + 1 || knots.size() != static_cast<std::size_t>(n + p + 1)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < knots.size(); ++i) {
        if (!std::isfinite(knots[i]) || (i > 0 && knots[i] < knots[i - 1])) {
            return std::nullopt;
        }
    }
    const double a = knots[static_cast<std::size_t>(p)];
    const double b = knots[static_cast<std::size_t>(n)];
    if (!(b > a)) {
        return std::nullopt;
    }
    const double eps = 1e-12 * (b - a);
    for (std::size_t i = 1; i < knots.size(); ++i) {
        if (knots[i] - knots[i - 1] <= eps) {
            knots[i] = knots[i - 1];
        }
    }
    // Span k (knots[k] < knots[k + 1], p <= k < n) depends only on poles k-p..k and knots k-p..k+p+1: decompose that
    // local curve on its own, so the work grows linearly with the number of poles.
    std::vector<std::vector<HPoint>> segments;
    for (int k = p; k < n; ++k) {
        const auto ks = static_cast<std::size_t>(k);
        if (!(knots[ks] < knots[ks + 1])) {
            continue;
        }
        std::vector<HPoint> local(poles.begin() + (k - p), poles.begin() + (k + 1));
        std::vector<double> localKnots(knots.begin() + (k - p), knots.begin() + (k + p + 2));
        auto seg = decomposeAll(std::move(local), std::move(localKnots), p);
        if (!seg || seg->size() != 1) {
            return std::nullopt;
        }
        segments.push_back(std::move(seg->front()));
    }
    // Neighbouring segments share their joint: make it the same value.
    for (std::size_t i = 1; i < segments.size(); ++i) {
        segments[i].front() = segments[i - 1].back();
    }
    return segments;
}

std::vector<std::vector<Vec2>> sketchSplineBeziers(const std::vector<Vec2>& controlPoints, int degree, bool closed)
{
    const int n = static_cast<int>(controlPoints.size());
    if (degree < 1 || n < degree + 1) {
        return {};
    }
    std::vector<HPoint> poles;
    for (const Vec2& q : controlPoints) {
        poles.push_back({q.x, q.y, 1.0});
    }
    std::vector<double> knots;
    if (closed) {
        for (int i = 0; i < degree; ++i) {
            poles.push_back(poles[static_cast<std::size_t>(i)]);
        }
        for (int i = 0; i <= n + 2 * degree; ++i) {
            knots.push_back(static_cast<double>(i));
        }
    } else {
        knots = clampedUniformKnots(n, degree);
    }
    const auto segments = bezierDecompose(std::move(poles), std::move(knots), degree);
    std::vector<std::vector<Vec2>> out;
    if (!segments) {
        return out;
    }
    for (const auto& seg : *segments) {
        std::vector<Vec2> pts;
        for (const HPoint& q : seg) {
            pts.push_back({q.x / q.w, q.y / q.w});
        }
        out.push_back(std::move(pts));
    }
    if (!closed && !out.empty()) {
        // A clamped curve starts and ends exactly at its end control points.
        out.front().front() = controlPoints.front();
        out.back().back() = controlPoints.back();
    }
    if (closed) {
        // Neighbouring segments share their joint exactly.
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[(i + 1) % out.size()].front() = out[i].back();
        }
    }
    return out;
}

bool kernelTakesWhole(std::size_t controlPointCount, int degree, bool closed)
{
    return !closed && controlPointCount >= 2 && degree == std::min(3, static_cast<int>(controlPointCount) - 1);
}

} // namespace ray::sketch::detail
