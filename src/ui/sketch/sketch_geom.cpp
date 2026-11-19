#include "sketch_geom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace ray::ui::sketchui {

double length(Vec2 a)
{
    return std::hypot(a.x, a.y);
}

double distance(Vec2 a, Vec2 b)
{
    return length(sub(a, b));
}

Vec2 unit(Vec2 a)
{
    const double l = length(a);
    return l > 0 ? Vec2{a.x / l, a.y / l} : Vec2{0, 0};
}

Vec2 perpLeft(Vec2 a)
{
    return {-a.y, a.x};
}

Vec2 polar(Vec2 center, double radius, double angle)
{
    return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

double angleOf(Vec2 v)
{
    return std::atan2(v.y, v.x);
}

double normAngle(double a)
{
    const double twoPi = 2 * kPi;
    a = std::fmod(a, twoPi);
    if (a < 0)
        a += twoPi;
    if (a >= twoPi)
        a -= twoPi;
    return a;
}

// ---------- polylines ----------
std::vector<Vec2> arcPolyline(Vec2 center, double r, double start, double sweep)
{
    // About 5 degrees a step, at least 4 steps.
    const int n = std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 36))));
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        out.push_back(polar(center, r, start + sweep * i / n));
    return out;
}

std::vector<Vec2> splinePolyline(const std::vector<Vec2>& control, int degree, bool closed)
{
    std::vector<Vec2> cps = control;
    if (closed && !cps.empty())
        cps.push_back(cps.front());
    const int n = static_cast<int>(cps.size());
    if (n < 2)
        return cps;
    const int p = std::clamp(degree, 1, n - 1);
    // Clamped uniform knots: p+1 zeros, interior 1..n-p-1, p+1 times (n-p).
    std::vector<double> knots;
    for (int i = 0; i <= p; ++i)
        knots.push_back(0);
    for (int i = 1; i < n - p; ++i)
        knots.push_back(i);
    for (int i = 0; i <= p; ++i)
        knots.push_back(n - p);
    const double tMax = n - p;
    const int steps = std::max(16, 12 * (n - 1));
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(steps) + 1);
    for (int s = 0; s <= steps; ++s) {
        const double t = tMax * s / steps;
        // The knot span.
        int k = p;
        while (k < n - 1 && t >= knots[static_cast<std::size_t>(k + 1)])
            ++k;
        // De Boor.
        std::vector<Vec2> d(static_cast<std::size_t>(p + 1));
        for (int j = 0; j <= p; ++j)
            d[static_cast<std::size_t>(j)] = cps[static_cast<std::size_t>(j + k - p)];
        for (int r = 1; r <= p; ++r) {
            for (int j = p; j >= r; --j) {
                const double lo = knots[static_cast<std::size_t>(j + k - p)];
                const double hi = knots[static_cast<std::size_t>(j + 1 + k - r)];
                const double alpha = hi > lo ? (t - lo) / (hi - lo) : 0.0;
                const Vec2 a = d[static_cast<std::size_t>(j - 1)];
                const Vec2 b = d[static_cast<std::size_t>(j)];
                d[static_cast<std::size_t>(j)] = {a.x * (1 - alpha) + b.x * alpha, a.y * (1 - alpha) + b.y * alpha};
            }
        }
        out.push_back(d[static_cast<std::size_t>(p)]);
    }
    return out;
}

std::vector<Vec2> slotPolyline(Vec2 a, Vec2 b, double r)
{
    const Vec2 dir = unit(sub(b, a));
    if (length(dir) == 0) {
        auto circle = arcPolyline(a, r, 0, 2 * kPi);
        return circle;
    }
    const double ang = angleOf(dir);
    // Side a->b on the right, cap round b, side b->a on the left, cap round a.
    std::vector<Vec2> out;
    for (const Vec2& p : arcPolyline(b, r, ang - kPi / 2, kPi))
        out.push_back(p);
    for (const Vec2& p : arcPolyline(a, r, ang + kPi / 2, kPi))
        out.push_back(p);
    out.push_back(out.front());
    return out;
}

// ---------- the view ----------
SketchView::SketchView(const sketch::Sketch& model)
{
    std::map<SketchEntId, std::size_t> index;
    for (const sketch::Entity& e : model.entities) {
        if (const auto* p = std::get_if<sketch::Point>(&e.geom)) {
            index[e.id] = points_.size();
            points_.push_back(ViewPoint{e.id, p->p, e.construction, false, false});
        }
    }
    std::sort(points_.begin(), points_.end(), [](const ViewPoint& a, const ViewPoint& b) { return a.id < b.id; });
    index.clear();
    for (std::size_t i = 0; i < points_.size(); ++i)
        index[points_[i].id] = i;
    const auto at = [&](const SketchEntId& id) -> std::optional<Vec2> {
        const auto it = index.find(id);
        return it == index.end() ? std::nullopt : std::optional<Vec2>(points_[it->second].p);
    };
    const auto mark = [&](const SketchEntId& id, bool center) {
        const auto it = index.find(id);
        if (it != index.end()) {
            points_[it->second].used = true;
            points_[it->second].center = points_[it->second].center || center;
        }
    };
    for (const sketch::Entity& e : model.entities) {
        ViewCurve c;
        c.id = e.id;
        c.construction = e.construction;
        if (const auto* l = std::get_if<sketch::Line>(&e.geom)) {
            const auto a = at(l->a), b = at(l->b);
            if (!a || !b)
                continue;
            c.kind = ViewCurve::Kind::Line;
            c.a = *a;
            c.b = *b;
            c.points = {l->a, l->b};
            c.poly = {*a, *b};
            mark(l->a, false);
            mark(l->b, false);
        } else if (const auto* arc = std::get_if<sketch::Arc>(&e.geom)) {
            const auto ctr = at(arc->center), a = at(arc->start), b = at(arc->end);
            if (!ctr || !a || !b)
                continue;
            c.kind = ViewCurve::Kind::Arc;
            c.c = *ctr;
            c.a = *a;
            c.b = *b;
            c.r = distance(*a, *ctr);
            c.ccw = arc->ccw;
            c.start = normAngle(angleOf(sub(*a, *ctr)));
            const double end = normAngle(angleOf(sub(*b, *ctr)));
            c.sweep = normAngle(arc->ccw ? end - c.start : c.start - end);
            if (c.sweep <= 1e-12)
                c.sweep = 2 * kPi;
            c.points = {arc->center, arc->start, arc->end};
            c.poly = arcPolyline(c.c, c.r, c.start, arc->ccw ? c.sweep : -c.sweep);
            c.poly.front() = *a;
            c.poly.back() = *b;
            mark(arc->center, true);
            mark(arc->start, false);
            mark(arc->end, false);
        } else if (const auto* circle = std::get_if<sketch::Circle>(&e.geom)) {
            const auto ctr = at(circle->center);
            if (!ctr)
                continue;
            c.kind = ViewCurve::Kind::Circle;
            c.c = *ctr;
            c.r = circle->radius;
            c.points = {circle->center};
            c.poly = arcPolyline(c.c, c.r, 0, 2 * kPi);
            mark(circle->center, true);
        } else if (const auto* sp = std::get_if<sketch::Spline>(&e.geom)) {
            std::vector<Vec2> cps;
            for (const SketchEntId& id : sp->controlPoints) {
                if (auto p = at(id))
                    cps.push_back(*p);
                mark(id, false);
            }
            if (cps.size() < 2)
                continue;
            c.kind = ViewCurve::Kind::Spline;
            c.points = sp->controlPoints;
            c.a = cps.front();
            c.b = cps.back();
            c.poly = splinePolyline(cps, sp->degree, sp->closed);
        } else if (const auto* sl = std::get_if<sketch::Slot>(&e.geom)) {
            const auto a = at(sl->a), b = at(sl->b);
            if (!a || !b)
                continue;
            c.kind = ViewCurve::Kind::Slot;
            c.a = *a;
            c.b = *b;
            c.r = sl->width / 2;
            c.points = {sl->a, sl->b};
            c.poly = slotPolyline(*a, *b, c.r);
            mark(sl->a, true);
            mark(sl->b, true);
        } else {
            continue;
        }
        curves_.push_back(std::move(c));
    }
}

const ViewPoint* SketchView::pointEntry(const SketchEntId& id) const
{
    const auto it = std::lower_bound(points_.begin(), points_.end(), id,
                                     [](const ViewPoint& p, const SketchEntId& k) { return p.id < k; });
    return it != points_.end() && it->id == id ? &*it : nullptr;
}

std::optional<Vec2> SketchView::point(const SketchEntId& id) const
{
    const ViewPoint* p = pointEntry(id);
    return p != nullptr ? std::optional<Vec2>(p->p) : std::nullopt;
}

const ViewCurve* SketchView::curve(const SketchEntId& id) const
{
    for (const ViewCurve& c : curves_) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

std::vector<const ViewCurve*> SketchView::curvesAt(const SketchEntId& point) const
{
    std::vector<const ViewCurve*> out;
    for (const ViewCurve& c : curves_) {
        if (std::find(c.points.begin(), c.points.end(), point) != c.points.end())
            out.push_back(&c);
    }
    return out;
}

std::optional<std::pair<Vec2, Vec2>> SketchView::bounds() const
{
    bool any = false;
    Vec2 lo{0, 0}, hi{0, 0};
    const auto grow = [&](Vec2 p) {
        if (!any) {
            lo = hi = p;
            any = true;
            return;
        }
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    };
    for (const ViewPoint& p : points_)
        grow(p.p);
    for (const ViewCurve& c : curves_) {
        for (const Vec2& p : c.poly)
            grow(p);
    }
    if (!any)
        return std::nullopt;
    return std::make_pair(lo, hi);
}

SketchView::Hit SketchView::hitPoint(Vec2 q, double tol, const std::set<SketchEntId>& exclude) const
{
    Hit best;
    for (const ViewPoint& p : points_) {
        if (exclude.count(p.id))
            continue;
        const double d = distance(p.p, q);
        if (d <= tol && (best.kind == Hit::Kind::None || d < best.d))
            best = Hit{Hit::Kind::Point, p.id, p.p, d};
    }
    return best;
}

SketchView::Hit SketchView::hitCurve(Vec2 q, double tol, const std::set<SketchEntId>& exclude) const
{
    Hit best;
    for (const ViewCurve& c : curves_) {
        if (exclude.count(c.id))
            continue;
        const Vec2 n = nearestOn(c, q);
        const double d = distance(n, q);
        if (d <= tol && (best.kind == Hit::Kind::None || d < best.d))
            best = Hit{Hit::Kind::Curve, c.id, n, d};
    }
    return best;
}

SketchView::Hit SketchView::hit(Vec2 q, double tol, const std::set<SketchEntId>& exclude) const
{
    Hit p = hitPoint(q, tol, exclude);
    return p.kind != Hit::Kind::None ? p : hitCurve(q, tol, exclude);
}

// ---------- curves ----------
namespace {

Vec2 nearestOnSegment(Vec2 a, Vec2 b, Vec2 q)
{
    const Vec2 d = sub(b, a);
    const double l2 = dot(d, d);
    if (!(l2 > 0))
        return a;
    const double t = std::clamp(dot(sub(q, a), d) / l2, 0.0, 1.0);
    return add(a, mul(d, t));
}

Vec2 nearestOnPolyline(const std::vector<Vec2>& poly, Vec2 q)
{
    if (poly.empty())
        return q;
    Vec2 best = poly.front();
    double bestD = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const Vec2 n = nearestOnSegment(poly[i], poly[i + 1], q);
        const double d = distance(n, q);
        if (d < bestD) {
            bestD = d;
            best = n;
        }
    }
    return best;
}

} // namespace

Vec2 nearestOn(const ViewCurve& c, Vec2 q)
{
    switch (c.kind) {
    case ViewCurve::Kind::Line:
        return nearestOnSegment(c.a, c.b, q);
    case ViewCurve::Kind::Circle: {
        const Vec2 v = sub(q, c.c);
        return length(v) > 0 ? add(c.c, mul(unit(v), c.r)) : add(c.c, Vec2{c.r, 0});
    }
    case ViewCurve::Kind::Arc: {
        const Vec2 v = sub(q, c.c);
        const double ang = normAngle(angleOf(v));
        const double along = normAngle(c.ccw ? ang - c.start : c.start - ang);
        if (along <= c.sweep)
            return polar(c.c, c.r, ang);
        return distance(q, c.a) < distance(q, c.b) ? c.a : c.b;
    }
    default:
        return nearestOnPolyline(c.poly, q);
    }
}

std::optional<Vec2> tangentOut(const ViewCurve& c, const SketchEntId& endPoint)
{
    if (c.kind == ViewCurve::Kind::Line && c.points.size() == 2) {
        if (endPoint == c.points[1])
            return unit(sub(c.b, c.a));
        if (endPoint == c.points[0])
            return unit(sub(c.a, c.b));
        return std::nullopt;
    }
    if (c.kind == ViewCurve::Kind::Arc && c.points.size() == 3) {
        // Travelling direction at the end (ccw: left of the radius vector); at the start, the reverse.
        const bool atEnd = endPoint == c.points[2];
        const bool atStart = endPoint == c.points[1];
        if (!atEnd && !atStart)
            return std::nullopt;
        const Vec2 at = atEnd ? c.b : c.a;
        Vec2 travel = perpLeft(unit(sub(at, c.c)));
        if (!c.ccw)
            travel = mul(travel, -1);
        return atEnd ? travel : mul(travel, -1);
    }
    return std::nullopt;
}

std::optional<Vec2> circleThrough(Vec2 a, Vec2 b, Vec2 c)
{
    const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    const double scale = std::max({distance(a, b), distance(b, c), distance(a, c), 1e-12});
    if (std::abs(d) < 1e-9 * scale * scale)
        return std::nullopt;
    const double a2 = dot(a, a), b2 = dot(b, b), c2 = dot(c, c);
    return Vec2{(a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d};
}

std::optional<ArcFit> tangentArc(Vec2 start, Vec2 dir, Vec2 end)
{
    // The centre lies on the normal at `start`: c = start + n t with |c - end| = |t|.
    const Vec2 n = perpLeft(unit(dir));
    const Vec2 w = sub(end, start);
    const double den = 2 * dot(w, n);
    if (std::abs(den) < 1e-12 * std::max(1.0, length(w)))
        return std::nullopt;
    const double t = dot(w, w) / den;
    ArcFit f;
    f.center = add(start, mul(n, t));
    f.r = std::abs(t);
    f.ccw = t > 0; // the centre on the left: turning left
    return f;
}

std::vector<Vec2> crossAxisLine(const ViewCurve& c, bool horizontal, double value)
{
    std::vector<Vec2> out;
    if (c.kind == ViewCurve::Kind::Line) {
        const Vec2 d = sub(c.b, c.a);
        const double comp = horizontal ? d.y : d.x;
        if (std::abs(comp) < 1e-12)
            return out;
        const double t = ((horizontal ? value - c.a.y : value - c.a.x)) / comp;
        out.push_back(add(c.a, mul(d, t)));
        return out;
    }
    if (c.kind == ViewCurve::Kind::Arc || c.kind == ViewCurve::Kind::Circle) {
        const double off = horizontal ? value - c.c.y : value - c.c.x;
        const double h2 = c.r * c.r - off * off;
        if (h2 < 0)
            return out;
        const double h = std::sqrt(h2);
        for (const double s : {-h, h}) {
            out.push_back(horizontal ? Vec2{c.c.x + s, value} : Vec2{value, c.c.y + s});
            if (h == 0)
                break;
        }
    }
    return out;
}

} // namespace ray::ui::sketchui
