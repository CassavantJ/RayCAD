// Closed-region detection (task T-091). See loops.h for what a region is and how it is returned.
//
// HOW. (1) Every non-construction curve becomes a source curve; a slot becomes its two lines and two arcs. (2) Curves
// are cut where they cross, and where an end point of one lies on another (a T-junction), using exact formulas for
// lines, arcs and circles; splines are only joined at their end points. (3) The pieces' end points are clustered
// within kJoinTolerance into nodes; pieces that lie on top of each other are kept once. (4) Pieces hanging off the
// drawing (a node with one piece) are pruned, then pieces whose removal disconnects the drawing (bridges). (5) The
// faces of the planar graph are traced by turning to the next piece clockwise at every node; a face traced counter-
// clockwise is a bounded face, one traced clockwise is the outside of a connected component. (6) Each component
// outside is a hole of the smallest bounded face of another component that contains it (winding number with exact
// arc geometry).
#include "loops.h"

#include "loops_curves.h"
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>
#include <utility>

namespace ray::sketch {

using namespace detail;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2 * std::numbers::pi;
constexpr double kTol = kJoinTolerance;
constexpr double kAngleTie = 1e-8; // rad: two pieces leaving a node this close in direction are ordered by curvature
constexpr double kMinArea2 = 1e-9; // mm^2 (twice the area): a face with less is degenerate
constexpr int kSplineFlatten = 64; // polyline points per Bezier segment for crossing and containment tests
constexpr double kSplineEndZone = 1e-3; // mm: contact this close to a spline's end is its joint, not a crossing

std::uint64_t low64(const Hash256& h)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | h.bytes[static_cast<std::size_t>(i)];
    }
    return v;
}

enum class CK { Line, Arc, Circle, Spline };

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    void add(Vec2 p)
    {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    bool overlaps(const Box& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
    bool contains(Vec2 p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
};
Box boxAround(Vec2 p)
{
    return {p.x, p.y, p.x, p.y};
}

struct Split {
    double s; // line: 0..1; arc and circle: angle travelled from the start point
    Vec2 p;
};

// One curve of the sketch (a slot gives four).
struct Source {
    CK kind = CK::Line;
    SketchEntId entity;
    std::uint64_t tag = 0;
    Vec2 a, b;                          // line: a -> b; arc: start, end; spline: first and last point of the curve
    Vec2 c;                             // arc, circle: centre
    double r = 0;                       // arc, circle: radius (arc: |a - c|)
    bool ccw = true;                    // arc: direction from a to b
    double sweep = 0;                   // arc: angle from a to b in that direction, (0, 2 pi); circle: 2 pi
    std::vector<std::vector<Vec2>> bez; // spline: Bezier segments in order
    std::vector<Vec2> poles;            // spline: control points
    int degree = 0;
    bool whole = false;  // spline: the kernel's Bspline is exactly this curve
    bool closed = false; // spline: ends where it starts
    Box box;
    bool skip = false;
    std::vector<Split> splits;
    std::vector<Vec2> poly; // spline: flattened, for crossing and containment tests
};

double lineParam(const Source& k, Vec2 q)
{
    const Vec2 d = k.b - k.a;
    return dot(q - k.a, d) / dot(d, d);
}

// Angle travelled from the arc's start (circle: from angle 0) to q, in the arc's direction, in [0, 2 pi).
double arcParam(const Source& k, Vec2 q)
{
    const Vec2 v0 = k.a - k.c;
    const Vec2 v = q - k.c;
    double ang = dmath::atan2(cross(v0, v), dot(v0, v));
    if (!k.ccw) {
        ang = -ang;
    }
    if (ang < 0) {
        ang += kTwoPi;
    }
    if (ang >= kTwoPi) {
        ang -= kTwoPi;
    }
    return ang;
}

// Length along the curve from its start to parameter s.
double along(const Source& k, double s)
{
    if (k.kind == CK::Line) {
        return s * dist(k.a, k.b);
    }
    return s * k.r;
}
double totalLength(const Source& k)
{
    if (k.kind == CK::Line) {
        return dist(k.a, k.b);
    }
    return k.sweep * k.r;
}

// True when parameter s lies inside the curve, more than kTol (along the curve) from its ends. Circles have no ends.
bool interior(const Source& k, double s)
{
    if (k.kind == CK::Circle) {
        return true;
    }
    const double l = along(k, s);
    return l > kTol && l < totalLength(k) - kTol;
}

Vec2 rotate(Vec2 v, double angle)
{
    const double c = dmath::cos(angle);
    const double s = dmath::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// ---------- Source curves ----------

struct SourceSet {
    std::vector<Source> curves;
    std::vector<OpenCurve> open;
};

void addLine(SourceSet& set, SketchEntId id, std::uint64_t tag, Vec2 a, Vec2 b)
{
    Source k;
    k.kind = CK::Line;
    k.entity = id;
    k.tag = tag;
    k.a = a;
    k.b = b;
    if (dist(a, b) <= kTol) {
        set.open.push_back({id, OpenCurve::Why::Degenerate});
        return;
    }
    k.box = boxAround(a);
    k.box.add(b);
    set.curves.push_back(std::move(k));
}

void boxArc(Source& k)
{
    k.box = boxAround(k.a);
    k.box.add(k.b);
    // The quadrant points the arc passes.
    for (int q = 0; q < 4; ++q) {
        const Vec2 dir = q == 0 ? Vec2{1, 0} : q == 1 ? Vec2{0, 1} : q == 2 ? Vec2{-1, 0} : Vec2{0, -1};
        const Vec2 p = k.c + k.r * dir;
        if (k.kind == CK::Circle || arcParam(k, p) <= k.sweep) {
            k.box.add(p);
        }
    }
}

void addArc(SourceSet& set, SketchEntId id, std::uint64_t tag, Vec2 c, Vec2 a, Vec2 b, bool ccw)
{
    Source k;
    k.kind = CK::Arc;
    k.entity = id;
    k.tag = tag;
    k.c = c;
    k.a = a;
    k.b = b;
    k.ccw = ccw;
    k.r = dist(a, c);
    if (!(k.r > kTol) || dist(a, b) <= kTol) {
        set.open.push_back({id, OpenCurve::Why::Degenerate});
        return;
    }
    k.sweep = arcParam(k, b);
    if (!(k.sweep * k.r > kTol)) {
        set.open.push_back({id, OpenCurve::Why::Degenerate});
        return;
    }
    boxArc(k);
    set.curves.push_back(std::move(k));
}

void addCircle(SourceSet& set, SketchEntId id, std::uint64_t tag, Vec2 c, double r)
{
    Source k;
    k.kind = CK::Circle;
    k.entity = id;
    k.tag = tag;
    k.c = c;
    k.r = r;
    k.ccw = true;
    k.sweep = kTwoPi;
    k.a = {c.x + r, c.y};
    k.b = k.a;
    if (!(r > kTol)) {
        set.open.push_back({id, OpenCurve::Why::Degenerate});
        return;
    }
    boxArc(k);
    set.curves.push_back(std::move(k));
}

void addSpline(SourceSet& set, SketchEntId id, std::uint64_t tag, const std::vector<Vec2>& poles, int degree,
               bool closed)
{
    Source k;
    k.kind = CK::Spline;
    k.entity = id;
    k.tag = tag;
    k.poles = poles;
    k.degree = degree;
    k.bez = sketchSplineBeziers(poles, degree, closed);
    if (k.bez.empty()) {
        set.open.push_back({id, OpenCurve::Why::Degenerate});
        return;
    }
    k.whole = kernelTakesWhole(poles.size(), degree, closed);
    k.a = k.bez.front().front();
    k.b = k.bez.back().back();
    k.closed = closed || dist(k.a, k.b) <= kTol;
    k.box = boxAround(k.a);
    for (const auto& seg : k.bez) {
        for (int i = 0; i <= kSplineFlatten; ++i) {
            if (i == 0 && !k.poly.empty()) {
                continue;
            }
            const Vec2 p = bezierPoint(seg, static_cast<double>(i) / kSplineFlatten);
            k.poly.push_back(p);
            k.box.add(p);
        }
        for (const Vec2& p : seg) {
            k.box.add(p); // the control polygon bounds the curve
        }
    }
    set.curves.push_back(std::move(k));
}

Result<SourceSet> collectSources(const Sketch& s)
{
    const EntityIndex index(s);
    SourceSet set;
    auto pt = [&](SketchEntId id) {
        return *pointOf(index, id);
    };
    for (const Entity& e : s.entities) {
        if (e.construction) {
            continue;
        }
        const std::uint64_t tag = curveTag(e.id);
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            addLine(set, e.id, tag, pt(l->a), pt(l->b));
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            addArc(set, e.id, tag, pt(a->center), pt(a->start), pt(a->end), a->ccw);
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            addCircle(set, e.id, tag, pt(c->center), c->radius);
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            std::vector<Vec2> poles;
            for (SketchEntId p : sp->controlPoints) {
                poles.push_back(pt(p));
            }
            addSpline(set, e.id, tag, poles, sp->degree, sp->closed);
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            const Vec2 A = pt(sl->a);
            const Vec2 B = pt(sl->b);
            const double len = dist(A, B);
            const double r = sl->width / 2;
            if (!(len > kTol) || !(r > kTol)) {
                set.open.push_back({e.id, OpenCurve::Why::Degenerate});
                continue;
            }
            const Vec2 n = (r / len) * perpLeft(B - A);
            const Vec2 a0 = A - n;
            const Vec2 b0 = B - n;
            const Vec2 b1 = B + n;
            const Vec2 a1 = A + n;
            addLine(set, e.id, slotPartTag(e.id, 0), a0, b0);
            addArc(set, e.id, slotPartTag(e.id, 1), B, b0, b1, true);
            addLine(set, e.id, slotPartTag(e.id, 2), b1, a1);
            addArc(set, e.id, slotPartTag(e.id, 3), A, a1, a0, true);
        }
    }
    return set;
}

// ---------- Cutting ----------

// Distance from q to the curve and the parameter of the nearest point (lines, arcs and circles only).
std::pair<double, double> project(const Source& k, Vec2 q)
{
    if (k.kind == CK::Line) {
        const double t = std::clamp(lineParam(k, q), 0.0, 1.0);
        return {dist(lerp(k.a, k.b, t), q), t};
    }
    const double s = arcParam(k, q);
    const double radial = std::fabs(dist(q, k.c) - k.r);
    if (k.kind == CK::Circle || s <= k.sweep) {
        return {radial, s};
    }
    const double toStart = dist(q, k.a);
    const double toEnd = dist(q, k.b);
    return toStart < toEnd ? std::pair{toStart, 0.0} : std::pair{toEnd, k.sweep};
}

// A T-junction: an end point of `from` on the inside of `onto`.
void endOnCurve(const Source& from, Source& onto)
{
    if (onto.kind == CK::Spline || (from.kind == CK::Circle) || (from.kind == CK::Spline && from.closed)) {
        return;
    }
    const Box near{onto.box.x0 - kTol, onto.box.y0 - kTol, onto.box.x1 + kTol, onto.box.y1 + kTol};
    for (const Vec2 q : {from.a, from.b}) {
        if (!near.contains(q)) {
            continue;
        }
        const auto [d, s] = project(onto, q);
        if (d <= kTol && interior(onto, s)) {
            onto.splits.push_back({s, q});
        }
    }
}

void crossLineLine(Source& k1, Source& k2)
{
    const Vec2 d = k1.b - k1.a;
    const Vec2 e = k2.b - k2.a;
    const double den = cross(d, e);
    if (std::fabs(den) <= 1e-14 * norm(d) * norm(e)) {
        return; // parallel: overlaps are cut by the end-point test
    }
    const Vec2 w = k2.a - k1.a;
    const double t = cross(w, e) / den;
    const double u = cross(w, d) / den;
    if (interior(k1, t) && interior(k2, u)) {
        const Vec2 p = k1.a + t * d;
        k1.splits.push_back({t, p});
        k2.splits.push_back({u, p});
    }
}

void crossLineArc(Source& line, Source& arc)
{
    const Vec2 d = line.b - line.a;
    const double dd = dot(d, d);
    const Vec2 f = line.a - arc.c;
    const double t0 = -dot(f, d) / dd;
    const Vec2 foot = line.a + t0 * d;
    const double h = dist(foot, arc.c);
    if (h > arc.r + kTol) {
        return;
    }
    std::vector<double> ts;
    if (h >= arc.r - kTol) {
        ts.push_back(t0);
    } else {
        const double w = dmath::sqrt(arc.r * arc.r - h * h) / dmath::sqrt(dd);
        ts.push_back(t0 - w);
        ts.push_back(t0 + w);
    }
    for (const double t : ts) {
        if (!interior(line, t)) {
            continue;
        }
        const Vec2 p = line.a + t * d;
        const double s = arcParam(arc, p);
        if ((arc.kind == CK::Circle || s <= arc.sweep) && interior(arc, s)) {
            line.splits.push_back({t, p});
            arc.splits.push_back({s, p});
        }
    }
}

void crossArcArc(Source& k1, Source& k2)
{
    const Vec2 u0 = k2.c - k1.c;
    const double d = norm(u0);
    if (d <= kTol) {
        return; // concentric: overlaps are cut by the end-point test
    }
    const double r1 = k1.r;
    const double r2 = k2.r;
    if (d > r1 + r2 + kTol || d < std::fabs(r1 - r2) - kTol) {
        return;
    }
    const Vec2 u = (1 / d) * u0;
    std::vector<Vec2> pts;
    if (std::fabs(d - (r1 + r2)) <= kTol) {
        pts.push_back(k1.c + r1 * u);
    } else if (std::fabs(d - std::fabs(r1 - r2)) <= kTol) {
        pts.push_back(r1 >= r2 ? k1.c + r1 * u : k1.c - r1 * u);
    } else {
        const double a = (d * d + r1 * r1 - r2 * r2) / (2 * d);
        const double h = dmath::sqrt(std::max(0.0, r1 * r1 - a * a));
        pts.push_back(k1.c + a * u + h * perpLeft(u));
        pts.push_back(k1.c + a * u - h * perpLeft(u));
    }
    for (const Vec2& p : pts) {
        const double s1 = arcParam(k1, p);
        const double s2 = arcParam(k2, p);
        const bool on1 = k1.kind == CK::Circle || s1 <= k1.sweep;
        const bool on2 = k2.kind == CK::Circle || s2 <= k2.sweep;
        if (on1 && on2 && interior(k1, s1) && interior(k2, s2)) {
            k1.splits.push_back({s1, p});
            k2.splits.push_back({s2, p});
        }
    }
}

// Polyline of a line, arc or circle for the spline crossing test.
std::vector<Vec2> flatten(const Source& k)
{
    if (k.kind == CK::Spline) {
        return k.poly;
    }
    if (k.kind == CK::Line) {
        return {k.a, k.b};
    }
    const int n = std::max(8, static_cast<int>(k.sweep / (kPi / 360)));
    std::vector<Vec2> out;
    const Vec2 v0 = k.a - k.c;
    for (int i = 0; i <= n; ++i) {
        const double ang = (k.ccw ? 1 : -1) * k.sweep * i / n;
        out.push_back(k.c + rotate(v0, ang));
    }
    if (k.kind == CK::Arc) {
        out.back() = k.b;
    }
    return out;
}

bool segmentsTouch(Vec2 p, Vec2 p2, Vec2 q, Vec2 q2, Vec2& hit)
{
    const Vec2 d = p2 - p;
    const Vec2 e = q2 - q;
    const double den = cross(d, e);
    const Vec2 w = q - p;
    if (den == 0) {
        return false;
    }
    const double t = cross(w, e) / den;
    const double u = cross(w, d) / den;
    if (t < 0 || t > 1 || u < 0 || u > 1) {
        return false;
    }
    hit = p + t * d;
    return true;
}

double pointPolylineDistance(const std::vector<Vec2>& poly, Vec2 q)
{
    double best = dist(poly.front(), q);
    for (std::size_t i = 1; i < poly.size(); ++i) {
        const Vec2 d = poly[i] - poly[i - 1];
        const double dd = dot(d, d);
        const double t = dd > 0 ? std::clamp(dot(q - poly[i - 1], d) / dd, 0.0, 1.0) : 0.0;
        best = std::min(best, dist(poly[i - 1] + t * d, q));
    }
    return best;
}

// True when `other` crosses or touches spline `sp` away from the spline's end points.
bool crossesSpline(const Source& sp, const Source& other)
{
    const auto nearEnd = [&](Vec2 p) {
        return !sp.closed && (dist(p, sp.a) <= kSplineEndZone || dist(p, sp.b) <= kSplineEndZone);
    };
    const std::vector<Vec2> q = flatten(other);
    for (std::size_t i = 1; i < sp.poly.size(); ++i) {
        Box seg = boxAround(sp.poly[i - 1]);
        seg.add(sp.poly[i]);
        for (std::size_t j = 1; j < q.size(); ++j) {
            Box seg2 = boxAround(q[j - 1]);
            seg2.add(q[j]);
            Vec2 hit;
            if (seg.overlaps(seg2) && segmentsTouch(sp.poly[i - 1], sp.poly[i], q[j - 1], q[j], hit) && !nearEnd(hit)) {
                return true;
            }
        }
    }
    if (other.kind != CK::Circle && !(other.kind == CK::Spline && other.closed)) {
        for (const Vec2 e : {other.a, other.b}) {
            if (!nearEnd(e) && pointPolylineDistance(sp.poly, e) <= 10 * kTol) {
                return true;
            }
        }
    }
    return false;
}

void cutAll(std::vector<Source>& curves, std::vector<OpenCurve>& open)
{
    std::vector<std::size_t> order(curves.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (curves[a].box.x0 != curves[b].box.x0) {
            return curves[a].box.x0 < curves[b].box.x0;
        }
        return a < b;
    });
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const Source& k1 = curves[order[oi]];
        for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
            const Source& k2 = curves[order[oj]];
            if (k2.box.x0 > k1.box.x1 + kTol) {
                break;
            }
            if (k1.box.y0 <= k2.box.y1 + kTol && k2.box.y0 <= k1.box.y1 + kTol) {
                pairs.emplace_back(std::min(order[oi], order[oj]), std::max(order[oi], order[oj]));
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    // Splines cannot be cut: one that is crossed is left out.
    for (const auto& [i, j] : pairs) {
        for (const auto& [s, o] : {std::pair{i, j}, std::pair{j, i}}) {
            if (curves[s].kind == CK::Spline && !curves[s].skip && crossesSpline(curves[s], curves[o])) {
                curves[s].skip = true;
                open.push_back({curves[s].entity, OpenCurve::Why::SplineCrossed});
            }
        }
    }
    for (const auto& [i, j] : pairs) {
        Source& k1 = curves[i];
        Source& k2 = curves[j];
        if (k1.skip || k2.skip) {
            continue;
        }
        endOnCurve(k1, k2);
        endOnCurve(k2, k1);
        const bool line1 = k1.kind == CK::Line;
        const bool line2 = k2.kind == CK::Line;
        const bool round1 = k1.kind == CK::Arc || k1.kind == CK::Circle;
        const bool round2 = k2.kind == CK::Arc || k2.kind == CK::Circle;
        if (line1 && line2) {
            crossLineLine(k1, k2);
        } else if (line1 && round2) {
            crossLineArc(k1, k2);
        } else if (round1 && line2) {
            crossLineArc(k2, k1);
        } else if (round1 && round2) {
            crossArcArc(k1, k2);
        }
    }
}

// ---------- Pieces and nodes ----------

struct Piece {
    std::size_t src = 0;
    CK kind = CK::Line;
    Vec2 p0, p1; // as cut, before joining
    int n0 = -1, n1 = -1;
    double sweep = 0;  // arc pieces: angle in the source's direction
    bool loop = false; // starts and ends at the same node: a whole circle or a closed spline
    bool dead = false;
};

void makePieces(const std::vector<Source>& curves, std::vector<Piece>& pieces)
{
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const Source& k = curves[i];
        if (k.skip) {
            continue;
        }
        if (k.kind == CK::Spline) {
            Piece p;
            p.src = i;
            p.kind = CK::Spline;
            p.p0 = k.a;
            p.p1 = k.b;
            p.loop = k.closed;
            pieces.push_back(p);
            continue;
        }
        std::vector<Split> sp = k.splits;
        std::sort(sp.begin(), sp.end(), [](const Split& x, const Split& y) {
            if (x.s != y.s) {
                return x.s < y.s;
            }
            return x.p.x != y.p.x ? x.p.x < y.p.x : x.p.y < y.p.y;
        });
        std::vector<Split> cuts;
        for (const Split& x : sp) {
            if (cuts.empty() || along(k, x.s) - along(k, cuts.back().s) > kTol) {
                cuts.push_back(x);
            }
        }
        const auto piece = [&](Vec2 a, Vec2 b, double sweep, bool loop) {
            Piece p;
            p.src = i;
            p.kind = k.kind == CK::Line ? CK::Line : CK::Arc;
            p.p0 = a;
            p.p1 = b;
            p.sweep = sweep;
            p.loop = loop;
            pieces.push_back(p);
        };
        if (k.kind == CK::Circle) {
            if (cuts.empty()) {
                piece(k.a, k.a, kTwoPi, true);
                continue;
            }
            if (cuts.size() > 1 && (cuts.front().s + kTwoPi - cuts.back().s) * k.r <= kTol) {
                cuts.pop_back(); // the same point seen on both sides of angle 0
            }
            for (std::size_t c = 0; c < cuts.size(); ++c) {
                const Split& from = cuts[c];
                const Split& to = cuts[(c + 1) % cuts.size()];
                double sweep = to.s - from.s;
                if (c + 1 == cuts.size()) {
                    sweep += kTwoPi;
                }
                piece(from.p, to.p, sweep, cuts.size() == 1);
            }
            continue;
        }
        Vec2 prev = k.a;
        double prevS = 0;
        for (const Split& x : cuts) {
            piece(prev, x.p, x.s - prevS, false);
            prev = x.p;
            prevS = x.s;
        }
        piece(prev, k.b, (k.kind == CK::Line ? 1.0 : k.sweep) - prevS, false);
    }
}

// Clusters piece end points into nodes; returns node positions.
std::vector<Vec2> makeNodes(std::vector<Piece>& pieces)
{
    struct End {
        Vec2 p;
        std::size_t piece;
        int which;
    };
    std::vector<End> ends;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        ends.push_back({pieces[i].p0, i, 0});
        if (!pieces[i].loop) {
            ends.push_back({pieces[i].p1, i, 1});
        }
    }
    std::vector<std::size_t> order(ends.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Vec2 pa = ends[a].p;
        const Vec2 pb = ends[b].p;
        if (pa.x != pb.x) {
            return pa.x < pb.x;
        }
        if (pa.y != pb.y) {
            return pa.y < pb.y;
        }
        return a < b;
    });
    std::vector<int> node(ends.size(), -1);
    std::vector<Vec2> nodes;
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const std::size_t i = order[oi];
        if (node[i] >= 0) {
            continue;
        }
        const int id = static_cast<int>(nodes.size());
        nodes.push_back(ends[i].p);
        node[i] = id;
        for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
            const std::size_t j = order[oj];
            if (ends[j].p.x - ends[i].p.x > kTol) {
                break;
            }
            if (node[j] < 0 && dist(ends[j].p, ends[i].p) <= kTol) {
                node[j] = id;
            }
        }
    }
    for (std::size_t e = 0; e < ends.size(); ++e) {
        Piece& p = pieces[ends[e].piece];
        (ends[e].which == 0 ? p.n0 : p.n1) = node[e];
    }
    for (Piece& p : pieces) {
        if (p.loop) {
            p.n1 = p.n0;
        }
    }
    return nodes;
}

// ---------- The graph ----------

struct Graph {
    const std::vector<Source>* curves = nullptr;
    std::vector<Piece> pieces;
    std::vector<Vec2> nodes;

    const Source& src(std::size_t e) const { return (*curves)[pieces[e].src]; }
    int origin(std::size_t h) const { return (h & 1) ? pieces[h / 2].n1 : pieces[h / 2].n0; }
    int dest(std::size_t h) const { return origin(h ^ 1); }
    Vec2 at(int n) const { return nodes[static_cast<std::size_t>(n)]; }

    // Arc pieces: their direction of travel when walked along half-edge h.
    bool ccwAlong(std::size_t h) const { return src(h / 2).ccw != ((h & 1) != 0); }

    // Direction and signed curvature with which half-edge h leaves its origin.
    std::pair<Vec2, double> leaving(std::size_t h) const
    {
        const Piece& p = pieces[h / 2];
        const Source& k = src(h / 2);
        const bool back = (h & 1) != 0;
        switch (p.kind) {
        case CK::Line: {
            const Vec2 d = at(p.n1) - at(p.n0);
            return {back ? -1.0 * d : d, 0.0};
        }
        case CK::Arc:
        case CK::Circle: {
            const bool ccw = ccwAlong(h);
            const Vec2 radial = at(origin(h)) - k.c;
            return {ccw ? perpLeft(radial) : -1.0 * perpLeft(radial), (ccw ? 1.0 : -1.0) / k.r};
        }
        case CK::Spline:
            if (!back) {
                return {bezierEndDirection(k.bez.front(), false), bezierEndCurvature(k.bez.front(), false)};
            }
            return {-1.0 * bezierEndDirection(k.bez.back(), true), -bezierEndCurvature(k.bez.back(), true)};
        }
        return {};
    }

    // Signed sweep of an arc piece along half-edge h.
    double arcSweep(std::size_t h) const
    {
        const double sweep = pieces[h / 2].sweep;
        return ccwAlong(h) ? sweep : -sweep;
    }

    // The integral of x dy - y dx along half-edge h.
    double areaTerm(std::size_t h) const
    {
        const Piece& p = pieces[h / 2];
        const Vec2 a = at(origin(h));
        const Vec2 b = at(dest(h));
        switch (p.kind) {
        case CK::Line:
            return cross(a, b);
        case CK::Arc:
        case CK::Circle: {
            const Source& k = src(h / 2);
            return k.c.x * (b.y - a.y) - k.c.y * (b.x - a.x) + k.r * k.r * arcSweep(h);
        }
        case CK::Spline: {
            double sum = 0;
            for (const auto& seg : src(h / 2).bez) {
                sum += bezierAreaTerm(seg).value_or(0.0);
            }
            return (h & 1) ? -sum : sum;
        }
        }
        return 0;
    }

    // The angle that half-edge h subtends seen from q (part of a winding number).
    double subtended(std::size_t h, Vec2 q) const
    {
        const Piece& p = pieces[h / 2];
        const Vec2 a = at(origin(h));
        const Vec2 b = at(dest(h));
        const auto chord = [&](Vec2 u, Vec2 v) {
            return dmath::atan2(cross(u - q, v - q), dot(u - q, v - q));
        };
        if (p.kind == CK::Line) {
            return chord(a, b);
        }
        if (p.kind == CK::Spline) {
            const Source& k = src(h / 2);
            double sum = 0;
            for (std::size_t i = 1; i < k.poly.size(); ++i) {
                sum += chord(k.poly[i - 1], k.poly[i]);
            }
            return (h & 1) ? -sum : sum;
        }
        const Source& k = src(h / 2);
        const double sweep = arcSweep(h);
        if (p.loop) {
            return dist(q, k.c) < k.r ? (sweep > 0 ? kTwoPi : -kTwoPi) : 0.0;
        }
        double ang = chord(a, b);
        // q inside the circular segment between the arc and its chord adds a whole turn.
        const Vec2 mid = k.c + rotate(a - k.c, sweep / 2);
        const double side = cross(b - a, mid - a);
        const double qSide = cross(b - a, q - a);
        if (dist(q, k.c) < k.r && ((side > 0 && qSide > 0) || (side < 0 && qSide < 0))) {
            ang += sweep > 0 ? kTwoPi : -kTwoPi;
        }
        return ang;
    }
};

// Removes pieces that hang off the drawing: repeatedly, every piece at a node that has only one.
void pruneDangling(Graph& g, std::vector<OpenCurve>& open)
{
    std::vector<int> degree(g.nodes.size(), 0);
    std::vector<std::vector<std::size_t>> at(g.nodes.size());
    for (std::size_t e = 0; e < g.pieces.size(); ++e) {
        const Piece& p = g.pieces[e];
        if (p.dead) {
            continue;
        }
        degree[static_cast<std::size_t>(p.n0)] += 1;
        degree[static_cast<std::size_t>(p.n1)] += 1;
        at[static_cast<std::size_t>(p.n0)].push_back(e);
        if (p.n1 != p.n0) {
            at[static_cast<std::size_t>(p.n1)].push_back(e);
        }
    }
    std::vector<std::size_t> queue;
    for (std::size_t n = 0; n < degree.size(); ++n) {
        if (degree[n] == 1) {
            queue.push_back(n);
        }
    }
    while (!queue.empty()) {
        const std::size_t n = queue.back();
        queue.pop_back();
        for (const std::size_t e : at[n]) {
            Piece& p = g.pieces[e];
            if (p.dead) {
                continue;
            }
            p.dead = true;
            open.push_back({g.src(e).entity, OpenCurve::Why::OpenChain});
            for (const int m : {p.n0, p.n1}) {
                const auto mi = static_cast<std::size_t>(m);
                degree[mi] -= 1;
                if (degree[mi] == 1) {
                    queue.push_back(mi);
                }
            }
        }
    }
}

// Removes bridges: pieces whose removal disconnects their component (iterative Tarjan over the multigraph).
void removeBridges(Graph& g, std::vector<OpenCurve>& open)
{
    const std::size_t nn = g.nodes.size();
    std::vector<std::vector<std::size_t>> adj(nn); // half-edges leaving each node
    for (std::size_t e = 0; e < g.pieces.size(); ++e) {
        const Piece& p = g.pieces[e];
        if (p.dead || p.n0 == p.n1) {
            continue;
        }
        adj[static_cast<std::size_t>(p.n0)].push_back(2 * e);
        adj[static_cast<std::size_t>(p.n1)].push_back(2 * e + 1);
    }
    std::vector<int> disc(nn, -1);
    std::vector<int> low(nn, 0);
    int time = 0;
    std::vector<std::size_t> bridges;
    struct Frame {
        std::size_t node;
        std::size_t viaEdge; // piece index used to enter, or SIZE_MAX at a root
        std::size_t next;    // position in adj[node]
    };
    for (std::size_t root = 0; root < nn; ++root) {
        if (disc[root] >= 0 || adj[root].empty()) {
            continue;
        }
        std::vector<Frame> stack{{root, SIZE_MAX, 0}};
        disc[root] = low[root] = time++;
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.next < adj[f.node].size()) {
                const std::size_t h = adj[f.node][f.next++];
                if (h / 2 == f.viaEdge) {
                    continue;
                }
                const auto to = static_cast<std::size_t>(g.dest(h));
                if (disc[to] < 0) {
                    disc[to] = low[to] = time++;
                    stack.push_back({to, h / 2, 0});
                } else {
                    low[f.node] = std::min(low[f.node], disc[to]);
                }
                continue;
            }
            const Frame done = f;
            stack.pop_back();
            if (!stack.empty()) {
                Frame& parent = stack.back();
                low[parent.node] = std::min(low[parent.node], low[done.node]);
                if (low[done.node] > disc[parent.node]) {
                    bridges.push_back(done.viaEdge);
                }
            }
        }
    }
    std::sort(bridges.begin(), bridges.end());
    for (const std::size_t e : bridges) {
        g.pieces[e].dead = true;
        open.push_back({g.src(e).entity, OpenCurve::Why::Bridge});
    }
}

// Keeps one of each set of pieces that lie on top of each other.
void removeDuplicates(Graph& g, std::vector<OpenCurve>& open)
{
    std::vector<std::size_t> order;
    for (std::size_t e = 0; e < g.pieces.size(); ++e) {
        if (!g.pieces[e].dead) {
            order.push_back(e);
        }
    }
    const auto key = [&](std::size_t e) {
        const Piece& p = g.pieces[e];
        return std::pair{std::min(p.n0, p.n1), std::max(p.n0, p.n1)};
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    const auto midpoint = [&](std::size_t e) {
        const Piece& p = g.pieces[e];
        const Source& k = g.src(e);
        if (p.kind == CK::Line) {
            return lerp(g.at(p.n0), g.at(p.n1), 0.5);
        }
        if (p.kind == CK::Spline) {
            return bezierPoint(k.bez[k.bez.size() / 2], k.bez.size() % 2 ? 0.5 : 0.0);
        }
        return k.c + rotate(g.at(p.n0) - k.c, (k.ccw ? 0.5 : -0.5) * p.sweep);
    };
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::size_t e = order[i];
        if (g.pieces[e].dead) {
            continue;
        }
        for (std::size_t j = i + 1; j < order.size() && key(order[j]) == key(e); ++j) {
            const std::size_t f = order[j];
            Piece& q = g.pieces[f];
            const Piece& p = g.pieces[e];
            if (q.dead || q.kind != p.kind) {
                continue;
            }
            bool same = dist(midpoint(e), midpoint(f)) <= kTol;
            if (same && p.kind != CK::Line) {
                const Source& a = g.src(e);
                const Source& b = g.src(f);
                if (p.kind == CK::Arc) {
                    same = dist(a.c, b.c) <= kTol && std::fabs(a.r - b.r) <= kTol;
                } else {
                    same = a.poles.size() == b.poles.size() && a.degree == b.degree;
                    for (std::size_t s = 0; same && s < a.bez.size(); ++s) {
                        same = dist(bezierPoint(a.bez[s], 0.37), bezierPoint(b.bez[s], 0.37)) <= kTol;
                    }
                }
            }
            if (same) {
                q.dead = true;
                open.push_back({g.src(f).entity, OpenCurve::Why::Duplicate});
            }
        }
    }
}

struct Cycle {
    std::vector<std::size_t> half; // half-edges in walk order
    double area2 = 0;
    int component = 0;
    Box box;
};

std::vector<Cycle> traceFaces(const Graph& g)
{
    const std::size_t nn = g.nodes.size();
    std::vector<std::vector<std::size_t>> out(nn);
    for (std::size_t e = 0; e < g.pieces.size(); ++e) {
        if (g.pieces[e].dead) {
            continue;
        }
        out[static_cast<std::size_t>(g.origin(2 * e))].push_back(2 * e);
        out[static_cast<std::size_t>(g.origin(2 * e + 1))].push_back(2 * e + 1);
    }
    std::vector<std::size_t> pos(g.pieces.size() * 2, 0);
    for (auto& list : out) {
        struct Dir {
            std::size_t h;
            double angle;
            double curvature;
        };
        std::vector<Dir> dirs;
        for (const std::size_t h : list) {
            const auto [d, k] = g.leaving(h);
            double ang = dmath::atan2(d.y, d.x);
            if (ang > kPi - kAngleTie) {
                ang -= kTwoPi;
            }
            dirs.push_back({h, ang, k});
        }
        std::sort(dirs.begin(), dirs.end(), [](const Dir& a, const Dir& b) {
            if (a.angle != b.angle) {
                return a.angle < b.angle;
            }
            return a.h < b.h;
        });
        // Pieces leaving in the same direction: the one bending right comes first counter-clockwise.
        for (std::size_t i = 0; i < dirs.size();) {
            std::size_t j = i + 1;
            while (j < dirs.size() && dirs[j].angle - dirs[j - 1].angle <= kAngleTie) {
                ++j;
            }
            std::sort(dirs.begin() + static_cast<std::ptrdiff_t>(i), dirs.begin() + static_cast<std::ptrdiff_t>(j),
                      [](const Dir& a, const Dir& b) {
                          if (a.curvature != b.curvature) {
                              return a.curvature < b.curvature;
                          }
                          if (a.angle != b.angle) {
                              return a.angle < b.angle;
                          }
                          return a.h < b.h;
                      });
            i = j;
        }
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            list[i] = dirs[i].h;
            pos[dirs[i].h] = i;
        }
    }
    std::vector<char> seen(g.pieces.size() * 2, 0);
    std::vector<Cycle> cycles;
    for (std::size_t start = 0; start < seen.size(); ++start) {
        if (seen[start] || g.pieces[start / 2].dead) {
            continue;
        }
        Cycle c;
        std::size_t h = start;
        while (!seen[h]) {
            seen[h] = 1;
            c.half.push_back(h);
            const auto& list = out[static_cast<std::size_t>(g.dest(h))];
            const std::size_t twin = h ^ 1;
            h = list[(pos[twin] + list.size() - 1) % list.size()];
        }
        for (const std::size_t x : c.half) {
            c.area2 += g.areaTerm(x);
        }
        cycles.push_back(std::move(c));
    }
    return cycles;
}

// ---------- Output ----------

using kernel::ProfileCurve;
using kernel::ProfileLoop;

// Centre moved onto the perpendicular bisector of start and end, so both lie at one radius from it.
Vec2 balancedCentre(Vec2 s, Vec2 e, Vec2 c)
{
    const Vec2 m = lerp(s, e, 0.5);
    const Vec2 dir = perpLeft(e - s);
    const double dd = dot(dir, dir);
    if (!(dd > 0)) {
        return c;
    }
    return m + (dot(c - m, dir) / dd) * dir;
}

struct Emitted {
    std::vector<ProfileCurve> curves;
    std::vector<std::pair<std::uint64_t, int>> sense; // (tag, travels along the curve's own direction)
    std::string problem;
};

void emitHalfEdge(const Graph& g, std::size_t h, bool alone, Emitted& out)
{
    const Piece& p = g.pieces[h / 2];
    const Source& k = g.src(h / 2);
    const bool back = (h & 1) != 0;
    const Vec2 s = g.at(g.origin(h));
    const Vec2 e = g.at(g.dest(h));
    out.sense.emplace_back(k.tag, back ? 0 : 1);
    switch (p.kind) {
    case CK::Line:
        out.curves.push_back({ProfileCurve::Kind::Line, {s, e}, 0.0, false, k.tag});
        return;
    case CK::Arc:
    case CK::Circle: {
        const bool ccw = g.ccwAlong(h);
        if (p.loop) {
            if (alone) {
                // `ccw` tells regionsOf() whether the circle is an outer loop or a hole; the kernel ignores it.
                out.curves.push_back({ProfileCurve::Kind::Circle, {k.c}, k.r, ccw, k.tag});
                return;
            }
            const Vec2 opposite = 2.0 * k.c - s;
            out.curves.push_back({ProfileCurve::Kind::Arc, {s, opposite, k.c}, 0.0, ccw, k.tag});
            out.curves.push_back({ProfileCurve::Kind::Arc, {opposite, s, k.c}, 0.0, ccw, k.tag});
            return;
        }
        out.curves.push_back({ProfileCurve::Kind::Arc, {s, e, balancedCentre(s, e, k.c)}, 0.0, ccw, k.tag});
        return;
    }
    case CK::Spline: {
        if (k.whole && !(p.loop && !alone)) {
            std::vector<Vec2> pts = k.poles;
            if (back) {
                std::reverse(pts.begin(), pts.end());
            }
            pts.front() = s;
            pts.back() = e;
            out.curves.push_back({ProfileCurve::Kind::Bspline, std::move(pts), 0.0, false, k.tag});
            return;
        }
        std::vector<std::vector<Vec2>> segs = k.bez;
        if (back) {
            std::reverse(segs.begin(), segs.end());
            for (auto& seg : segs) {
                std::reverse(seg.begin(), seg.end());
            }
        }
        segs.front().front() = s;
        segs.back().back() = e;
        if (k.degree > 3) {
            out.problem = "spline " + idText(k.entity.value) + " has degree " + std::to_string(k.degree) +
                          "; a profile can only take splines of degree 3 or less exactly";
            return;
        }
        for (auto& seg : segs) {
            out.curves.push_back({ProfileCurve::Kind::Bspline, std::move(seg), 0.0, false, k.tag});
        }
        return;
    }
    }
}

std::uint64_t loopTagOf(std::vector<std::pair<std::uint64_t, int>> sense, std::uint64_t salt)
{
    std::sort(sense.begin(), sense.end());
    Hasher h;
    h.str("raycad.sketch.loop");
    for (const auto& [tag, fwd] : sense) {
        h.i64(static_cast<std::int64_t>(tag));
        h.i64(fwd);
    }
    if (salt) {
        h.i64(static_cast<std::int64_t>(salt));
    }
    return low64(h.finish());
}

std::string describe(const Sketch& s, const OpenCurve& oc)
{
    const EntityIndex index(s);
    const Entity* e = index.find(oc.entity);
    std::string what = std::string(e ? kindName(kindOf(*e)) : "curve") + " " + idText(oc.entity.value);
    switch (oc.why) {
    case OpenCurve::Why::OpenChain:
        return what + " is not part of a closed loop (open chain or dangling end)";
    case OpenCurve::Why::Bridge:
        return what + " only links loops and bounds no region";
    case OpenCurve::Why::Duplicate:
        return what + " lies on top of another curve and was used once";
    case OpenCurve::Why::SplineCrossed:
        return what + " is crossed or touched by another curve away from its ends; splines are not cut, so it was "
                      "left out";
    case OpenCurve::Why::Degenerate:
        return what + " has no length, or its ends meet (an arc that closes on itself), and was left out";
    }
    return what;
}

} // namespace

std::uint64_t curveTag(SketchEntId entity)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span(entity.value.bytes)));
    return low64(h.finish());
}

std::uint64_t slotPartTag(SketchEntId slot, int part)
{
    Hasher h;
    h.bytes(std::as_bytes(std::span(slot.value.bytes)));
    const auto b = static_cast<std::byte>(part);
    h.bytes(std::span(&b, 1));
    return low64(h.finish());
}

double loopSignedArea2(const kernel::ProfileLoop& loop)
{
    double sum = 0;
    for (const ProfileCurve& c : loop.curves) {
        switch (c.kind) {
        case ProfileCurve::Kind::Line:
            if (c.pts.size() == 2) {
                sum += cross(c.pts[0], c.pts[1]);
            }
            break;
        case ProfileCurve::Kind::Arc:
            if (c.pts.size() == 3) {
                const Vec2 a = c.pts[0];
                const Vec2 b = c.pts[1];
                const Vec2 m = c.pts[2];
                double sweep = dmath::atan2(cross(a - m, b - m), dot(a - m, b - m));
                if (c.ccw && sweep <= 0) {
                    sweep += kTwoPi;
                } else if (!c.ccw && sweep >= 0) {
                    sweep -= kTwoPi;
                }
                const double r = dist(a, m);
                sum += m.x * (b.y - a.y) - m.y * (b.x - a.x) + r * r * sweep;
            }
            break;
        case ProfileCurve::Kind::Circle:
            sum += kTwoPi * c.radius * c.radius * (c.ccw ? 1 : -1);
            break;
        case ProfileCurve::Kind::Bspline: {
            const int n = static_cast<int>(c.pts.size());
            if (n >= 2) {
                for (const auto& seg : sketchSplineBeziers(c.pts, std::min(3, n - 1), false)) {
                    sum += bezierAreaTerm(seg).value_or(0.0);
                }
            }
            break;
        }
        }
    }
    return sum;
}

std::vector<std::vector<kernel::ProfileLoop>> regionsOf(const std::vector<kernel::ProfileLoop>& loops)
{
    std::vector<std::vector<kernel::ProfileLoop>> out;
    for (const kernel::ProfileLoop& l : loops) {
        // A single circle carries its direction in `ccw`; any other loop in its area's sign.
        if (out.empty() || loopSignedArea2(l) > 0) {
            out.emplace_back();
        }
        out.back().push_back(l);
    }
    return out;
}

Result<LoopAnalysis> analyzeLoops(const Sketch& input)
{
    if (auto ok = validateSketch(input); !ok) {
        return tl::unexpected(ok.error());
    }
    const Sketch s = canonicalSketch(input);
    auto sources = collectSources(s);
    if (!sources) {
        return tl::unexpected(sources.error());
    }
    std::vector<OpenCurve> open = std::move(sources->open);
    std::vector<Source> curves = std::move(sources->curves);
    cutAll(curves, open);

    Graph g;
    g.curves = &curves;
    makePieces(curves, g.pieces);
    g.nodes = makeNodes(g.pieces);
    for (Piece& p : g.pieces) {
        if (!p.loop && p.n0 == p.n1) {
            // Ends joined into one point: a sliver left by cutting next to a joint, or an arc that closes on itself
            // (nearly a whole circle). Only the second loses real geometry, so only it is reported.
            p.dead = true;
            if (p.kind == CK::Arc && p.sweep * curves[p.src].r > 10 * kTol) {
                open.push_back({curves[p.src].entity, OpenCurve::Why::Degenerate});
            }
        }
    }
    removeDuplicates(g, open);
    pruneDangling(g, open);
    removeBridges(g, open);
    pruneDangling(g, open);

    std::vector<Cycle> cycles = traceFaces(g);
    // Components (union-find over nodes) and boxes.
    std::vector<int> parent(g.nodes.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<int>(i);
    }
    const auto find = [&](int x) {
        while (parent[static_cast<std::size_t>(x)] != x) {
            parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    };
    for (const Piece& p : g.pieces) {
        if (!p.dead) {
            const int a = find(p.n0);
            const int b = find(p.n1);
            if (a != b) {
                parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
            }
        }
    }
    for (Cycle& c : cycles) {
        c.component = find(g.origin(c.half.front()));
        c.box = boxAround(g.at(g.origin(c.half.front())));
        for (const std::size_t h : c.half) {
            c.box.add(g.at(g.origin(h)));
            const Source& k = g.src(h / 2);
            if (k.kind == CK::Arc || k.kind == CK::Circle || k.kind == CK::Spline) {
                c.box.add({k.box.x0, k.box.y0});
                c.box.add({k.box.x1, k.box.y1});
            }
        }
    }
    std::vector<std::size_t> faces;
    std::vector<std::size_t> outsides;
    for (std::size_t i = 0; i < cycles.size(); ++i) {
        if (cycles[i].area2 > kMinArea2) {
            faces.push_back(i);
        } else if (cycles[i].area2 < -kMinArea2) {
            outsides.push_back(i);
        }
    }
    // Each component's outside is a hole of the smallest face of another component that contains it.
    std::vector<std::vector<std::size_t>> holes(cycles.size());
    for (const std::size_t o : outsides) {
        const Vec2 q = g.at(g.origin(cycles[o].half.front()));
        std::size_t best = SIZE_MAX;
        for (const std::size_t f : faces) {
            const Cycle& fc = cycles[f];
            if (fc.component == cycles[o].component || !fc.box.contains(q)) {
                continue;
            }
            if (best != SIZE_MAX && fc.area2 >= cycles[best].area2) {
                continue;
            }
            double turn = 0;
            for (const std::size_t h : fc.half) {
                turn += g.subtended(h, q);
            }
            if (std::fabs(turn) > kPi) {
                best = f;
            }
        }
        if (best != SIZE_MAX) {
            holes[best].push_back(o);
        }
    }

    LoopAnalysis result;
    struct Built {
        LoopRegion region;
        std::vector<std::pair<std::uint64_t, int>> outerSense;
        std::vector<std::vector<std::pair<std::uint64_t, int>>> holeSense;
        double area2 = 0;
        Vec2 anchor;
    };
    std::vector<Built> built;
    for (const std::size_t f : faces) {
        Built b;
        bool ok = true;
        const auto emitCycle = [&](const Cycle& c, ProfileLoop& loop,
                                   std::vector<std::pair<std::uint64_t, int>>& sense) {
            Emitted em;
            for (const std::size_t h : c.half) {
                emitHalfEdge(g, h, c.half.size() == 1, em);
                if (!em.problem.empty()) {
                    result.rejected.push_back("A region could not be passed to the kernel exactly: " + em.problem +
                                              ".");
                    ok = false;
                    return;
                }
            }
            loop.curves = std::move(em.curves);
            sense = std::move(em.sense);
        };
        emitCycle(cycles[f], b.region.outer, b.outerSense);
        double area2 = cycles[f].area2;
        std::vector<std::size_t> hs = holes[f];
        std::sort(hs.begin(), hs.end());
        for (const std::size_t h : hs) {
            if (!ok) {
                break;
            }
            ProfileLoop loop;
            std::vector<std::pair<std::uint64_t, int>> sense;
            emitCycle(cycles[h], loop, sense);
            b.region.holes.push_back(std::move(loop));
            b.holeSense.push_back(std::move(sense));
            area2 += cycles[h].area2;
        }
        if (!ok) {
            continue;
        }
        b.region.area = area2 / 2;
        b.area2 = cycles[f].area2;
        b.anchor = g.at(g.origin(cycles[f].half.front()));
        built.push_back(std::move(b));
    }
    // Loop tags; a tie between two loops is broken by a counter in order of area and position.
    struct TagSlot {
        std::uint64_t base;
        double area2;
        Vec2 anchor;
        std::uint64_t* tag;
    };
    std::vector<TagSlot> slots;
    for (Built& b : built) {
        slots.push_back({loopTagOf(b.outerSense, 0), b.area2, b.anchor, &b.region.outer.tag});
        for (std::size_t i = 0; i < b.region.holes.size(); ++i) {
            const double a2 = loopSignedArea2(b.region.holes[i]);
            const Vec2 anchor =
                b.region.holes[i].curves.empty() ? Vec2{} : b.region.holes[i].curves.front().pts.front();
            slots.push_back({loopTagOf(b.holeSense[i], 0), a2, anchor, &b.region.holes[i].tag});
        }
    }
    std::sort(slots.begin(), slots.end(), [](const TagSlot& a, const TagSlot& b) {
        if (a.base != b.base) {
            return a.base < b.base;
        }
        if (a.area2 != b.area2) {
            return a.area2 < b.area2;
        }
        if (a.anchor.x != b.anchor.x) {
            return a.anchor.x < b.anchor.x;
        }
        return a.anchor.y < b.anchor.y;
    });
    for (std::size_t i = 0; i < slots.size(); ++i) {
        std::uint64_t counter = 0;
        for (std::size_t j = i; j > 0 && slots[j - 1].base == slots[i].base; --j) {
            ++counter;
        }
        *slots[i].tag = counter == 0 ? slots[i].base : loopTagOf({{slots[i].base, 2}}, counter);
    }
    for (Built& b : built) {
        result.regions.push_back(std::move(b.region));
    }
    std::sort(result.regions.begin(), result.regions.end(),
              [](const LoopRegion& a, const LoopRegion& b) { return a.outer.tag < b.outer.tag; });

    std::sort(open.begin(), open.end(), [](const OpenCurve& a, const OpenCurve& b) {
        if (a.entity != b.entity) {
            return a.entity < b.entity;
        }
        return a.why < b.why;
    });
    open.erase(
        std::unique(open.begin(), open.end(),
                    [](const OpenCurve& a, const OpenCurve& b) { return a.entity == b.entity && a.why == b.why; }),
        open.end());
    result.open = std::move(open);
    for (const OpenCurve& oc : result.open) {
        result.warnings.push_back("Sketch " + describe(s, oc) + ".");
    }
    for (const std::string& r : result.rejected) {
        result.warnings.push_back(r);
    }
    return result;
}

Result<std::vector<kernel::ProfileLoop>> closedLoops(const Sketch& s)
{
    auto analysis = analyzeLoops(s);
    if (!analysis) {
        return tl::unexpected(analysis.error());
    }
    if (!analysis->rejected.empty()) {
        Error err{ErrorCode::InvalidArgument, analysis->rejected.front(), {"sketch loops"}};
        for (std::size_t i = 1; i < analysis->rejected.size(); ++i) {
            err.context.push_back(analysis->rejected[i]);
        }
        return tl::unexpected(std::move(err));
    }
    if (!analysis->open.empty()) {
        std::string msg = std::to_string(analysis->open.size()) +
                          " sketch curve(s) are not used in any closed region: " + analysis->warnings.front();
        if (analysis->open.size() > 1) {
            msg += " (and " + std::to_string(analysis->open.size() - 1) + " more)";
        }
        ray::log(LogLevel::Warn, "sketch", msg);
    }
    std::vector<kernel::ProfileLoop> loops;
    for (LoopRegion& r : analysis->regions) {
        loops.push_back(std::move(r.outer));
        for (kernel::ProfileLoop& h : r.holes) {
            loops.push_back(std::move(h));
        }
    }
    return loops;
}

} // namespace ray::sketch
