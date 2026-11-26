// Pick-time evidence (territory.h): territories, point-on-element probes, pick neighbours.
// Deterministic module: no <cmath> trig (dmath), std::sqrt and std::abs only; no unordered containers.
#include "territory.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::naming::detail {

namespace {

using kernel::SubId;
using kernel::SubKind;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.283185307179586476925286766559;
// A chord of an arc lies within this of the arc, mm.
constexpr double kSagitta = 1e-5;
// Half the size of the box around a point that kernel::minDistance measures to, mm.
constexpr double kProbeHalf = 1e-5;
// Same-carrier tolerances: positions (mm) and unit-vector components.
constexpr double kCarrierPosition = 1e-4;
constexpr double kCarrierParallel = 1e-9;

double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(const Vec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}
bool unitOf(const Vec3& v, Vec3& out)
{
    const double n = norm(v);
    if (!(n > 0.0))
        return false;
    out = mul(v, 1.0 / n);
    return true;
}
double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 d = sub(b, a);
    const double l2 = dot(d, d);
    double t = l2 > 0.0 ? dot(sub(p, a), d) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return norm(sub(p, add(a, mul(d, t))));
}
void grow(Box3& box, const Vec3& p, bool first)
{
    if (first) {
        box.min = box.max = p;
        return;
    }
    box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
    box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
}
bool insideBox(const Vec3& p, const Box3& box, double tol)
{
    return p.x >= box.min.x - tol && p.x <= box.max.x + tol && p.y >= box.min.y - tol && p.y <= box.max.y + tol &&
           p.z >= box.min.z - tol && p.z <= box.max.z + tol;
}

// Distance from p to element `sub` by the kernel (a small box around p); nullopt when it cannot be computed.
std::optional<kernel::Distance> kernelDistance(const kernel::Shape& shape, SubId element, const Vec3& p)
{
    const Vec3 h{kProbeHalf, kProbeHalf, kProbeHalf};
    auto box = kernel::makeBox(sub(p, h), add(p, h));
    if (!box)
        return std::nullopt;
    auto d = kernel::minDistance(shape, element, *box, SubId{SubKind::Solid, 0});
    if (!d)
        return std::nullopt;
    return *d;
}

// ---------------------------------------------------------------- curves
struct Curve {
    kernel::CurveInfo info{};
    bool known = false; // line, or circle whose arc direction was found
    bool line = false;
    Vec3 axis{}, r0{}, r1{}; // circle: unit axis, radius vectors at angle 0 and 90 degrees
    double sweep = 0;        // circle: signed angle from start to end
};

std::optional<Curve> curveOf(const kernel::Shape& shape, SubId edge)
{
    auto info = kernel::curveInfo(shape, edge);
    if (!info)
        return std::nullopt;
    Curve c;
    c.info = *info;
    if (info->type == kernel::CurveType::Line) {
        c.line = true;
        c.known = norm(sub(info->end, info->start)) > 0.0;
        return c;
    }
    if (info->type != kernel::CurveType::Circle || !(info->radius > 0.0) || !unitOf(info->axis, c.axis))
        return c;
    c.r0 = sub(info->start, info->center);
    c.r1 = cross(c.axis, c.r0);
    c.sweep = info->closed ? kTwoPi : info->length / info->radius;
    auto at = [&](double a) {
        return add(info->center, add(mul(c.r0, dmath::cos(a)), mul(c.r1, dmath::sin(a))));
    };
    const double endTolerance = 1e-4 * std::max(1.0, info->radius);
    if (!info->closed && norm(sub(at(c.sweep), info->end)) > endTolerance) {
        c.sweep = -c.sweep;
        if (norm(sub(at(c.sweep), info->end)) > endTolerance)
            return c;
    }
    c.known = true;
    return c;
}

Vec3 curvePoint(const Curve& c, double t)
{
    if (c.line)
        return add(c.info.start, mul(sub(c.info.end, c.info.start), t));
    const double a = c.sweep * t;
    return add(c.info.center, add(mul(c.r0, dmath::cos(a)), mul(c.r1, dmath::sin(a))));
}

// Distance from p to a known curve.
double curveDistance(const Curve& c, const Vec3& p)
{
    if (c.line)
        return segmentDistance(p, c.info.start, c.info.end);
    const Vec3 v = sub(p, c.info.center);
    const double along = dot(v, c.axis);
    const Vec3 radial = sub(v, mul(c.axis, along));
    const double rl = norm(radial);
    const double ends = std::min(norm(sub(p, c.info.start)), norm(sub(p, c.info.end)));
    if (!(rl > 0.0))
        return std::min(ends, std::sqrt(along * along + c.info.radius * c.info.radius));
    const double r = c.info.radius;
    double angle = dmath::atan2(dot(radial, c.r1) / r, dot(radial, c.r0) / r);
    if (c.sweep < 0)
        angle = -angle;
    if (angle < 0)
        angle += kTwoPi;
    if (angle <= std::abs(c.sweep))
        return std::sqrt(along * along + (rl - r) * (rl - r));
    return ends;
}

// Points along a known curve, every chord within kSagitta of it (a line: its two ends).
std::vector<Vec3> polyline(const Curve& c)
{
    if (c.line)
        return {c.info.start, c.info.end};
    const double r = c.info.radius;
    const double step = 2.0 * std::sqrt(2.0 * kSagitta / r); // sagitta r (1 - cos(step / 2)) ~ r step^2 / 8
    const int n = std::clamp(static_cast<int>(std::abs(c.sweep) / step) + 1, 8, 20000);
    std::vector<Vec3> out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        out.push_back(i == n && !c.info.closed ? c.info.end : curvePoint(c, static_cast<double>(i) / n));
    return out;
}

// ---------------------------------------------------------------- planar outlines
// The outline of a planar face bounded by lines and arcs, as 2D chords in a frame of its plane.
struct Outline {
    Vec3 origin{}, n{}, e1{}, e2{};
    std::vector<std::array<double, 4>> chords; // (u0, v0, u1, v1)
    double umin = 0, umax = 0, vmin = 0, vmax = 0;

    std::array<double, 2> uv(const Vec3& p) const
    {
        const Vec3 d = sub(p, origin);
        return {dot(d, e1), dot(d, e2)};
    }
    double off(const Vec3& p) const { return dot(sub(p, origin), n); }
    Vec3 at(double u, double v) const { return add(origin, add(mul(e1, u), mul(e2, v))); }
    // Even-odd crossings of a ray from (u, v) in a fixed direction not along either axis.
    bool inside(double u, double v) const
    {
        constexpr double ca = 0.8150192, sa = 0.5794376; // a fixed unit direction (about 35.4 degrees)
        int crossings = 0;
        for (const auto& c : chords) {
            // Rotate so the ray runs along +x.
            const double ax = (c[0] - u) * ca + (c[1] - v) * sa, ay = -(c[0] - u) * sa + (c[1] - v) * ca;
            const double bx = (c[2] - u) * ca + (c[3] - v) * sa, by = -(c[2] - u) * sa + (c[3] - v) * ca;
            if ((ay > 0) == (by > 0))
                continue;
            const double x = ax + (bx - ax) * (0 - ay) / (by - ay);
            if (x > 0)
                ++crossings;
        }
        return crossings % 2 == 1;
    }
    double boundaryDistance(double u, double v) const
    {
        double best = 1e300;
        for (const auto& c : chords) {
            const double dx = c[2] - c[0], dy = c[3] - c[1];
            const double l2 = dx * dx + dy * dy;
            double t = l2 > 0.0 ? ((u - c[0]) * dx + (v - c[1]) * dy) / l2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double px = c[0] + dx * t - u, py = c[1] + dy * t - v;
            best = std::min(best, px * px + py * py);
        }
        return std::sqrt(best);
    }
};

std::optional<Outline> outlineOf(const kernel::Shape& shape, SubId face, const kernel::SurfaceInfo& info)
{
    if (info.type != kernel::SurfaceType::Plane)
        return std::nullopt;
    Outline o;
    if (!unitOf(info.axis, o.n))
        return std::nullopt;
    o.origin = info.origin;
    const Vec3 t = std::abs(o.n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    if (!unitOf(cross(t, o.n), o.e1))
        return std::nullopt;
    o.e2 = cross(o.n, o.e1);
    bool first = true;
    for (const SubId& edge : kernel::adjacent(shape, face, SubKind::Edge)) {
        const auto c = curveOf(shape, edge);
        if (!c || !c->known)
            return std::nullopt;
        const std::vector<Vec3> pts = polyline(*c);
        std::array<double, 2> prev = o.uv(pts.front());
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const std::array<double, 2> next = o.uv(pts[i]);
            if (first) {
                o.umin = o.umax = next[0];
                o.vmin = o.vmax = next[1];
                first = false;
            }
            o.umin = std::min(o.umin, next[0]);
            o.umax = std::max(o.umax, next[0]);
            o.vmin = std::min(o.vmin, next[1]);
            o.vmax = std::max(o.vmax, next[1]);
            if (i > 0)
                o.chords.push_back({prev[0], prev[1], next[0], next[1]});
            prev = next;
        }
    }
    if (o.chords.empty())
        return std::nullopt;
    return o;
}

// ---------------------------------------------------------------- cylinders
// A cylindrical face bounded by lines along its axis and circles about it (the only lines and circles a cylinder can
// hold): in (angle, height) its boundary is vertical and horizontal segments, and a point on the surface is inside
// when an odd number of its circles cross the vertical ray from the point upwards (lines run along the ray).
struct CylinderDomain {
    Vec3 origin{}, axis{}, e1{}, e2{};
    double radius = 0;
    struct Arc {
        double h, from, sweep; // height; start angle; positive sweep (from .. from + sweep)
    };
    std::vector<Arc> arcs;
    std::vector<Curve> edges;

    double angleOf(const Vec3& p) const
    {
        const Vec3 w = sub(p, origin);
        double a = dmath::atan2(dot(w, e2), dot(w, e1));
        if (a < 0)
            a += kTwoPi;
        return a;
    }
    bool inside(const Vec3& p) const
    {
        const double h = dot(sub(p, origin), axis);
        const double a = angleOf(p);
        int crossings = 0;
        for (const Arc& arc : arcs) {
            if (!(arc.h > h))
                continue;
            double rel = a - arc.from;
            while (rel < 0)
                rel += kTwoPi;
            while (rel >= kTwoPi)
                rel -= kTwoPi;
            if (rel < arc.sweep)
                ++crossings;
        }
        return crossings % 2 == 1;
    }
    double boundaryDistance(const Vec3& p) const
    {
        double best = 1e300;
        for (const Curve& c : edges)
            best = std::min(best, curveDistance(c, p));
        return best;
    }
};

std::optional<CylinderDomain> cylinderDomainOf(const kernel::Shape& shape, SubId face, const kernel::SurfaceInfo& info)
{
    if (info.type != kernel::SurfaceType::Cylinder || !(info.radius > 0))
        return std::nullopt;
    CylinderDomain d;
    if (!unitOf(info.axis, d.axis))
        return std::nullopt;
    d.origin = info.origin;
    d.radius = info.radius;
    const Vec3 t = std::abs(d.axis.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    if (!unitOf(cross(t, d.axis), d.e1))
        return std::nullopt;
    d.e2 = cross(d.axis, d.e1);
    for (const SubId& edge : kernel::adjacent(shape, face, SubKind::Edge)) {
        const auto c = curveOf(shape, edge);
        if (!c || !c->known)
            return std::nullopt;
        if (c->line) {
            // Along the axis, on the surface.
            const Vec3 dir = sub(c->info.end, c->info.start);
            const double l = norm(dir);
            if (!(l > 0) || std::abs(std::abs(dot(dir, d.axis)) / l - 1.0) > 1e-9)
                return std::nullopt;
        } else {
            // About the axis, of the surface's radius, centred on the axis.
            const Vec3 w = sub(c->info.center, d.origin);
            if (std::abs(std::abs(dot(c->axis, d.axis)) - 1.0) > 1e-9 || std::abs(c->info.radius - d.radius) > 1e-7 ||
                norm(sub(w, mul(d.axis, dot(w, d.axis)))) > 1e-7)
                return std::nullopt;
            const double h = dot(w, d.axis);
            // The arc runs `sweep` about its own axis from its start: about the surface's axis the other way when the
            // two axes are opposite.
            const double s = dot(c->axis, d.axis) > 0 ? c->sweep : -c->sweep;
            const double a0 = d.angleOf(c->info.start);
            const double from = s >= 0 ? a0 : a0 + s;
            d.arcs.push_back({h, from < 0 ? from + kTwoPi : from, std::abs(s)});
        }
        d.edges.push_back(*c);
    }
    if (d.arcs.empty())
        return std::nullopt;
    return d;
}

// ---------------------------------------------------------------- choosing points
struct Candidate {
    Vec3 p;
    double depth; // distance from the boundary
};

// Farthest-first: the deepest candidate, then repeatedly the one furthest from those chosen (ties: the first).
std::vector<Vec3> spread(const std::vector<Candidate>& candidates)
{
    std::vector<Vec3> out;
    if (candidates.empty())
        return out;
    std::size_t best = 0;
    for (std::size_t i = 1; i < candidates.size(); ++i)
        if (candidates[i].depth > candidates[best].depth)
            best = i;
    out.push_back(candidates[best].p);
    std::vector<double> nearest(candidates.size(), 1e300);
    while (out.size() < kTerritoryPoints) {
        std::size_t pick = candidates.size();
        double far = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            nearest[i] = std::min(nearest[i], norm(sub(candidates[i].p, out.back())));
            if (nearest[i] > far + 1e-12) {
                far = nearest[i];
                pick = i;
            }
        }
        if (pick == candidates.size() || far < kTerritoryMargin)
            break;
        out.push_back(candidates[pick].p);
    }
    return out;
}

void setPoints(Territory& t, const std::vector<Vec3>& points)
{
    t.points.clear();
    for (const Vec3& p : points)
        t.points.push_back(toQuanta(p));
}

void setBox(Territory& t, const Box3& box)
{
    t.qMin = toQuanta(box.min);
    t.qMax = toQuanta(box.max);
}

Territory edgeTerritory(const kernel::Shape& shape, SubId edge)
{
    Territory t;
    const auto c = curveOf(shape, edge);
    if (!c)
        return t;
    t.qMeasure = quantise(c->info.length, kLengthQuantaPerMm);
    const double length = c->info.length;
    Box3 box{};
    std::vector<Vec3> points;
    constexpr double fractions[] = {0.5, 0.3, 0.7, 0.15, 0.85};
    const double reach = c->info.closed ? 0.0 : kTerritoryMargin;
    auto farFromEnds = [&](const Vec3& p) {
        return c->info.closed || (norm(sub(p, c->info.start)) >= reach && norm(sub(p, c->info.end)) >= reach);
    };
    if (c->known) {
        if (c->line) {
            grow(box, c->info.start, true);
            grow(box, c->info.end, false);
        } else {
            for (int i = 0; i <= 128; ++i)
                grow(box, curvePoint(*c, i / 128.0), i == 0);
        }
        for (const double f : fractions) {
            const Vec3 p = curvePoint(*c, f);
            if (farFromEnds(p) && f * length >= kTerritoryMargin && (1 - f) * length >= kTerritoryMargin)
                points.push_back(p);
        }
    } else {
        // Another curve: the feet on it of points of its chord at the fractions (the kernel gives its ends only).
        grow(box, c->info.start, true);
        grow(box, c->info.end, false);
        for (const double f : fractions) {
            const Vec3 q = add(c->info.start, mul(sub(c->info.end, c->info.start), f));
            if (const auto d = kernelDistance(shape, edge, q)) {
                grow(box, d->p1, false);
                if (farFromEnds(d->p1))
                    points.push_back(d->p1);
            }
        }
    }
    setBox(t, box);
    setPoints(t, points);
    return t;
}

Territory faceTerritory(const kernel::Shape& shape, SubId face)
{
    Territory t;
    const auto info = kernel::surfaceInfo(shape, face);
    if (!info)
        return t;
    t.qMeasure = quantise(info->area, kLengthQuantaPerMm);
    setBox(t, info->bounds);
    if (const auto o = outlineOf(shape, face, *info)) {
        for (int n = 9; n <= 81; n *= 3) {
            std::vector<Candidate> candidates;
            const double du = (o->umax - o->umin) / n, dv = (o->vmax - o->vmin) / n;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double u = o->umin + (i + 0.5) * du, v = o->vmin + (j + 0.5) * dv;
                    if (!o->inside(u, v))
                        continue;
                    const double depth = o->boundaryDistance(u, v);
                    if (depth >= kTerritoryMargin)
                        candidates.push_back({o->at(u, v), depth});
                }
            if (candidates.size() >= kTerritoryPoints || n == 81) {
                setPoints(t, spread(candidates));
                break;
            }
        }
        return t;
    }
    if (const auto d = cylinderDomainOf(shape, face, *info)) {
        // Points of a grid over (angle, height) inside the domain, chosen as on a plane.
        double hmin = 1e300, hmax = -1e300;
        for (const CylinderDomain::Arc& arc : d->arcs) {
            hmin = std::min(hmin, arc.h);
            hmax = std::max(hmax, arc.h);
        }
        for (int n = 9; n <= 81; n *= 3) {
            std::vector<Candidate> candidates;
            for (int i = 0; i < 2 * n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double a = kTwoPi * (i + 0.5) / (2 * n), h = hmin + (hmax - hmin) * (j + 0.5) / n;
                    const Vec3 p = add(add(d->origin, mul(d->axis, h)), add(mul(d->e1, d->radius * dmath::cos(a)),
                                                                            mul(d->e2, d->radius * dmath::sin(a))));
                    if (!d->inside(p))
                        continue;
                    const double depth = d->boundaryDistance(p);
                    if (depth >= kTerritoryMargin)
                        candidates.push_back({p, depth});
                }
            if (candidates.size() >= kTerritoryPoints || n == 81) {
                setPoints(t, spread(candidates));
                break;
            }
        }
        return t;
    }
    // Any other face: the feet on it of nine points of its box (its centroid, then a 2 x 2 x 2 grid), kept when far
    // enough from every edge, the first kTerritoryPoints that are. On a cylinder or sphere the foot is the point of
    // the surface straight out from the axis or centre, kept when it lies on the face; else the kernel's nearest point.
    std::vector<std::optional<Curve>> edges;
    std::vector<SubId> edgeIds = kernel::adjacent(shape, face, SubKind::Edge);
    for (const SubId& e : edgeIds)
        edges.push_back(curveOf(shape, e));
    std::vector<Vec3> seeds{info->centroid};
    const Box3& b = info->bounds;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                seeds.push_back(Vec3{b.min.x + (b.max.x - b.min.x) * (0.25 + 0.5 * i),
                                     b.min.y + (b.max.y - b.min.y) * (0.25 + 0.5 * j),
                                     b.min.z + (b.max.z - b.min.z) * (0.25 + 0.5 * k)});
    Vec3 axis{};
    const bool cylinder = info->type == kernel::SurfaceType::Cylinder && unitOf(info->axis, axis) && info->radius > 0;
    const bool sphere = info->type == kernel::SurfaceType::Sphere && info->radius > 0;
    std::vector<Vec3> points;
    for (const Vec3& seed : seeds) {
        if (points.size() >= kTerritoryPoints)
            break;
        std::optional<Vec3> foot;
        if (cylinder || sphere) {
            const Vec3 w = sub(seed, info->origin);
            const Vec3 radial = cylinder ? sub(w, mul(axis, dot(w, axis))) : w;
            Vec3 dir{};
            if (!unitOf(radial, dir))
                continue;
            const Vec3 onSurface = add(sub(seed, radial), mul(dir, info->radius));
            if (const auto d = kernelDistance(shape, face, onSurface); d && d->value <= 1e-6)
                foot = onSurface;
        } else if (const auto d = kernelDistance(shape, face, seed)) {
            foot = d->p1;
        }
        if (!foot)
            continue;
        bool duplicate = false;
        for (const Vec3& p : points)
            duplicate = duplicate || norm(sub(p, *foot)) < kTerritoryMargin;
        if (duplicate)
            continue;
        double depth = 1e300;
        for (std::size_t i = 0; i < edges.size() && depth >= kTerritoryMargin; ++i) {
            if (edges[i] && edges[i]->known) {
                depth = std::min(depth, curveDistance(*edges[i], *foot));
            } else if (const auto de = kernelDistance(shape, edgeIds[i], *foot)) {
                depth = std::min(depth, de->value);
            } else {
                depth = 0;
            }
        }
        if (depth >= kTerritoryMargin)
            points.push_back(*foot);
    }
    setPoints(t, points);
    return t;
}

} // namespace

Vec3 fromQuanta(const std::array<std::int64_t, 3>& q)
{
    return {static_cast<double>(q[0]) / kLengthQuantaPerMm, static_cast<double>(q[1]) / kLengthQuantaPerMm,
            static_cast<double>(q[2]) / kLengthQuantaPerMm};
}

std::array<std::int64_t, 3> toQuanta(const Vec3& p)
{
    return {quantise(p.x, kLengthQuantaPerMm), quantise(p.y, kLengthQuantaPerMm), quantise(p.z, kLengthQuantaPerMm)};
}

PickOutline pickOutlineOf(const kernel::Shape& shape, SubId face)
{
    PickOutline out;
    try {
        const auto info = kernel::surfaceInfo(shape, face);
        if (!info || info->type != kernel::SurfaceType::Plane)
            return out;
        std::vector<Curve> curves;
        std::size_t arcs = 0;
        for (const SubId& edge : kernel::adjacent(shape, face, SubKind::Edge)) {
            const auto c = curveOf(shape, edge);
            if (!c || !c->known)
                return out;
            arcs += c->line ? 0 : 1;
            curves.push_back(*c);
        }
        const std::size_t lines = curves.size() - arcs;
        if (curves.empty() || lines + 4 * arcs > kMaxOutlineChords)
            return out;
        const std::size_t perArc = arcs == 0 ? 0 : std::max<std::size_t>(4, (kMaxOutlineChords - lines) / arcs);
        for (const Curve& c : curves) {
            std::vector<Vec3> pts;
            double slack = 0.0;
            if (c.line) {
                pts = {c.info.start, c.info.end};
            } else {
                for (std::size_t i = 0; i <= perArc; ++i)
                    pts.push_back(i == perArc && !c.info.closed
                                      ? c.info.end
                                      : curvePoint(c, static_cast<double>(i) / static_cast<double>(perArc)));
                slack = c.info.radius * (1.0 - dmath::cos(std::abs(c.sweep) / (2.0 * static_cast<double>(perArc))));
            }
            const std::int64_t qSlack = c.line ? 0 : quantise(slack, kLengthQuantaPerMm) + 1;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                const auto a = toQuanta(pts[i - 1]), b = toQuanta(pts[i]);
                out.chords.push_back({a[0], a[1], a[2], b[0], b[1], b[2], qSlack});
            }
        }
    } catch (...) {
        out = PickOutline{};
    }
    return out;
}

std::vector<SubId> piecesInsideOutline(const kernel::Shape& shape, const std::vector<SubId>& pieces,
                                       const PickOutline& outline)
{
    try {
        if (!outline.known() || pieces.size() < 2 || pieces.front().kind != SubKind::Face)
            return pieces;
        // The outline's plane: a chord, and the first chord not along it.
        Outline o;
        const auto start = [&](std::size_t i) {
            return fromQuanta({outline.chords[i][0], outline.chords[i][1], outline.chords[i][2]});
        };
        const auto end = [&](std::size_t i) {
            return fromQuanta({outline.chords[i][3], outline.chords[i][4], outline.chords[i][5]});
        };
        o.origin = start(0);
        Vec3 d0{};
        if (!unitOf(sub(end(0), start(0)), d0))
            return pieces;
        bool plane = false;
        for (std::size_t i = 1; i < outline.chords.size() && !plane; ++i)
            plane = unitOf(cross(d0, sub(end(i), start(i))), o.n) || unitOf(cross(d0, sub(end(i), o.origin)), o.n);
        if (!plane)
            return pieces;
        o.e1 = d0;
        o.e2 = cross(o.n, o.e1);
        std::vector<double> slack;
        double planeMargin = kOnTolerance;
        for (std::size_t i = 0; i < outline.chords.size(); ++i) {
            const auto a = o.uv(start(i)), b = o.uv(end(i));
            o.chords.push_back({a[0], a[1], b[0], b[1]});
            slack.push_back(static_cast<double>(outline.chords[i][6]) / kLengthQuantaPerMm);
            planeMargin = std::max(planeMargin, slack.back() + kOnTolerance);
        }
        // Distance from (u, v) past each chord's slack: a point within a chord's slack of it may lie on either side
        // of the boundary the chord stands for.
        const auto clearance = [&](double u, double v) {
            double best = 1e300;
            for (std::size_t i = 0; i < o.chords.size(); ++i) {
                const auto& c = o.chords[i];
                const double dx = c[2] - c[0], dy = c[3] - c[1];
                const double l2 = dx * dx + dy * dy;
                const double t = l2 > 0.0 ? std::clamp(((u - c[0]) * dx + (v - c[1]) * dy) / l2, 0.0, 1.0) : 0.0;
                const double px = c[0] + dx * t - u, py = c[1] + dy * t - v;
                best = std::min(best, std::sqrt(px * px + py * py) - slack[i]);
            }
            return best;
        };
        const double margin = kOnTolerance;
        const TerritoryCache territories(shape);
        std::vector<SubId> kept;
        bool anyInside = false;
        for (const SubId& piece : pieces) {
            const Territory& t = territories.of(piece);
            bool outside = t.known();
            for (const auto& q : t.points) {
                const Vec3 p = fromQuanta(q);
                const auto uv = o.uv(p);
                const bool onPlane = std::abs(o.off(p)) <= planeMargin;
                const bool in = !onPlane || o.inside(uv[0], uv[1]) || clearance(uv[0], uv[1]) <= margin;
                if (in)
                    outside = false;
                if (onPlane && o.inside(uv[0], uv[1]))
                    anyInside = true;
            }
            if (!outside)
                kept.push_back(piece);
        }
        if (!anyInside || kept.empty())
            return pieces;
        return kept;
    } catch (...) {
        return pieces;
    }
}

std::vector<SubId> otherFacesInsideOutline(const kernel::Shape& shape, SubId element, const TopoRef& ref,
                                           const TerritoryCache& territories)
{
    std::vector<SubId> out;
    try {
        const PickOutline& outline = ref.outline;
        if (element.kind != SubKind::Face || !outline.known() || !ref.territory.known())
            return out;
        // The outline in a frame of its plane, with each chord's slack (as piecesInsideOutline).
        Outline o;
        const auto start = [&](std::size_t i) {
            return fromQuanta({outline.chords[i][0], outline.chords[i][1], outline.chords[i][2]});
        };
        const auto end = [&](std::size_t i) {
            return fromQuanta({outline.chords[i][3], outline.chords[i][4], outline.chords[i][5]});
        };
        o.origin = start(0);
        Vec3 d0{};
        if (!unitOf(sub(end(0), start(0)), d0))
            return out;
        bool plane = false;
        for (std::size_t i = 1; i < outline.chords.size() && !plane; ++i)
            plane = unitOf(cross(d0, sub(end(i), start(i))), o.n) || unitOf(cross(d0, sub(end(i), o.origin)), o.n);
        if (!plane)
            return out;
        o.e1 = d0;
        o.e2 = cross(o.n, o.e1);
        std::vector<double> slack;
        double planeMargin = kOnTolerance;
        for (std::size_t i = 0; i < outline.chords.size(); ++i) {
            const auto a = o.uv(start(i)), b = o.uv(end(i));
            o.chords.push_back({a[0], a[1], b[0], b[1]});
            slack.push_back(static_cast<double>(outline.chords[i][6]) / kLengthQuantaPerMm);
            planeMargin = std::max(planeMargin, slack.back() + kOnTolerance);
        }
        const auto clearlyInside = [&](const Vec3& p) {
            const auto uv = o.uv(p);
            if (std::abs(o.off(p)) > planeMargin || !o.inside(uv[0], uv[1]))
                return false;
            double best = 1e300;
            for (std::size_t i = 0; i < o.chords.size(); ++i) {
                const auto& c = o.chords[i];
                const double dx = c[2] - c[0], dy = c[3] - c[1];
                const double l2 = dx * dx + dy * dy;
                const double t =
                    l2 > 0.0 ? std::clamp(((uv[0] - c[0]) * dx + (uv[1] - c[1]) * dy) / l2, 0.0, 1.0) : 0.0;
                const double px = c[0] + dx * t - uv[0], py = c[1] + dy * t - uv[1];
                best = std::min(best, std::sqrt(px * px + py * py) - slack[i]);
            }
            return best > kOnTolerance;
        };
        // The pick stayed: the element holds most of the picked territory.
        if (!piecesHoldMostOf(shape, {element}, ref.territory))
            return out;
        for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
            const SubId face{SubKind::Face, i};
            if (face == element || !territories.sameCarrier(element, face))
                continue;
            const Territory& t = territories.of(face);
            bool all = t.known() && !t.points.empty();
            for (const auto& q : t.points)
                all = all && clearlyInside(fromQuanta(q));
            if (!all)
                continue;
            const Probe probe(shape, face);
            bool neighbours = false;
            for (const PickNeighbour& n : ref.pickNeighbours)
                neighbours = neighbours || (n.territory.known() && probe.count(n.territory) > 0);
            if (!neighbours)
                out.push_back(face);
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

InPlace stillInPlace(const kernel::Shape& shape, SubId element, const TopoRef& ref)
{
    try {
        if (!ref.territory.known() || ref.territory.points.empty())
            return InPlace::No;
        const Probe probe(shape, element);
        const std::size_t on = probe.count(ref.territory), n = ref.territory.points.size();
        if (on < n)
            return InPlace::No;
        for (const PickNeighbour& nb : ref.pickNeighbours)
            if (nb.territory.known() && probe.count(nb.territory) > 0)
                return InPlace::TookNeighbour;
        return InPlace::Yes;
    } catch (...) {
        return InPlace::No;
    }
}

bool piecesHoldMostOf(const kernel::Shape& shape, const std::vector<SubId>& pieces, const Territory& territory)
{
    try {
        if (territory.points.empty())
            return true;
        std::vector<std::unique_ptr<Probe>> probes;
        for (const SubId& p : pieces)
            probes.push_back(std::make_unique<Probe>(shape, p));
        std::size_t held = 0;
        for (const auto& q : territory.points) {
            const Vec3 p = fromQuanta(q);
            for (const auto& probe : probes)
                if (probe->on(p)) {
                    ++held;
                    break;
                }
        }
        return 2 * held >= territory.points.size();
    } catch (...) {
        return false;
    }
}

Box3 boxOf(const Territory& t)
{
    return Box3{fromQuanta(t.qMin), fromQuanta(t.qMax)};
}

Vec3 centreOf(const Box3& box)
{
    return mul(add(box.min, box.max), 0.5);
}

Territory territoryOf(const kernel::Shape& shape, SubId element)
{
    try {
        switch (element.kind) {
        case SubKind::Vertex: {
            Territory t;
            if (const auto p = kernel::vertexPoint(shape, element)) {
                t.qMin = t.qMax = toQuanta(*p);
                t.points.push_back(t.qMin);
            }
            return t;
        }
        case SubKind::Edge:
            return edgeTerritory(shape, element);
        case SubKind::Face:
            return faceTerritory(shape, element);
        case SubKind::Solid:
            break;
        }
    } catch (...) {
    }
    return Territory{};
}

std::optional<Box3> elementBox(const kernel::Shape& shape, SubId element)
{
    try {
        switch (element.kind) {
        case SubKind::Vertex:
            if (const auto p = kernel::vertexPoint(shape, element))
                return Box3{*p, *p};
            return std::nullopt;
        case SubKind::Edge: {
            const auto c = curveOf(shape, element);
            if (!c)
                return std::nullopt;
            if (c->known && !c->line) {
                Box3 box{};
                for (int i = 0; i <= 128; ++i)
                    grow(box, curvePoint(*c, i / 128.0), i == 0);
                return box;
            }
            if (c->known)
                return Box3{{std::min(c->info.start.x, c->info.end.x), std::min(c->info.start.y, c->info.end.y),
                             std::min(c->info.start.z, c->info.end.z)},
                            {std::max(c->info.start.x, c->info.end.x), std::max(c->info.start.y, c->info.end.y),
                             std::max(c->info.start.z, c->info.end.z)}};
            return boxOf(edgeTerritory(shape, element));
        }
        case SubKind::Face:
            if (const auto info = kernel::surfaceInfo(shape, element))
                return info->bounds;
            return std::nullopt;
        case SubKind::Solid:
            break;
        }
    } catch (...) {
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- Probe
struct Probe::Impl {
    Impl(const kernel::Shape& s, SubId e) : shape(s), element(e) {}
    const kernel::Shape& shape;
    SubId element;
    bool readable = false;
    Vec3 point{};               // vertex
    std::optional<Curve> curve; // edge
    Box3 bounds{};              // face
    std::optional<Outline> outline;
    std::optional<CylinderDomain> cylinder;
    kernel::SurfaceType surface = kernel::SurfaceType::Other;
    Vec3 origin{}, axis{};
    double radius = 0;
};

Probe::Probe(const kernel::Shape& shape, SubId element) : impl_(new Impl(shape, element))
{
    try {
        switch (element.kind) {
        case SubKind::Vertex:
            if (const auto p = kernel::vertexPoint(shape, element)) {
                impl_->point = *p;
                impl_->readable = true;
            }
            break;
        case SubKind::Edge:
            impl_->curve = curveOf(shape, element);
            impl_->readable = impl_->curve.has_value();
            break;
        case SubKind::Face:
            if (const auto info = kernel::surfaceInfo(shape, element)) {
                impl_->bounds = info->bounds;
                impl_->outline = outlineOf(shape, element, *info);
                impl_->cylinder = cylinderDomainOf(shape, element, *info);
                impl_->surface = info->type;
                impl_->origin = info->origin;
                impl_->radius = info->radius;
                if (!unitOf(info->axis, impl_->axis))
                    impl_->surface = kernel::SurfaceType::Other;
                impl_->readable = true;
            }
            break;
        case SubKind::Solid:
            break;
        }
    } catch (...) {
        impl_->readable = false;
    }
}

Probe::~Probe() = default;

bool Probe::on(const Vec3& p, double tol) const
{
    try {
        const Impl& m = *impl_;
        if (!m.readable)
            return false;
        switch (m.element.kind) {
        case SubKind::Vertex:
            return norm(sub(p, m.point)) <= tol;
        case SubKind::Edge:
            if (m.curve->known)
                return curveDistance(*m.curve, p) <= tol;
            break;
        case SubKind::Face:
            if (!insideBox(p, m.bounds, tol))
                return false;
            if (m.outline) {
                if (std::abs(m.outline->off(p)) > tol)
                    return false;
                const auto uv = m.outline->uv(p);
                return m.outline->inside(uv[0], uv[1]) || m.outline->boundaryDistance(uv[0], uv[1]) <= tol;
            }
            // Off the carrier surface: not on the face (no kernel call needed).
            if (m.surface == kernel::SurfaceType::Plane && std::abs(dot(sub(p, m.origin), m.axis)) > tol)
                return false;
            if (m.surface == kernel::SurfaceType::Cylinder) {
                const Vec3 w = sub(p, m.origin);
                if (std::abs(norm(sub(w, mul(m.axis, dot(w, m.axis)))) - m.radius) > tol)
                    return false;
                if (m.cylinder)
                    return m.cylinder->inside(p) || m.cylinder->boundaryDistance(p) <= tol;
            }
            if (m.surface == kernel::SurfaceType::Sphere && std::abs(norm(sub(p, m.origin)) - m.radius) > tol)
                return false;
            break;
        case SubKind::Solid:
            return false;
        }
        const auto d = kernelDistance(m.shape, m.element, p);
        return d && d->value <= tol;
    } catch (...) {
        return false;
    }
}

std::size_t Probe::count(const Territory& territory) const
{
    std::size_t n = 0;
    for (const auto& q : territory.points)
        if (on(fromQuanta(q)))
            ++n;
    return n;
}

// ---------------------------------------------------------------- TerritoryCache
struct TerritoryCache::Carrier {
    bool known = false;
    int type = -1; // 0 plane, 1 cylinder, 2 line, 3 circle
    Vec3 p{}, a{};
    double r = 0;
};

TerritoryCache::TerritoryCache(const kernel::Shape& shape) : shape_(shape) {}
TerritoryCache::~TerritoryCache() = default;

const Territory& TerritoryCache::of(SubId element) const
{
    {
        const std::lock_guard<std::mutex> guard(lock_);
        const auto it = made_.find(element);
        if (it != made_.end())
            return it->second;
    }
    Territory t = territoryOf(shape_, element); // outside the lock: slow, and deterministic
    const std::lock_guard<std::mutex> guard(lock_);
    return made_.emplace(element, std::move(t)).first->second;
}

const TerritoryCache::Carrier& TerritoryCache::carrier(SubId element) const
{
    {
        const std::lock_guard<std::mutex> guard(lock_);
        const auto it = carriers_.find(element);
        if (it != carriers_.end())
            return *it->second;
    }
    auto c = std::make_unique<Carrier>();
    try {
        if (element.kind == SubKind::Face) {
            if (const auto info = kernel::surfaceInfo(shape_, element); info && unitOf(info->axis, c->a)) {
                c->p = info->origin;
                c->r = info->radius;
                if (info->type == kernel::SurfaceType::Plane) {
                    c->type = 0;
                    c->known = true;
                    if (info->reversed)
                        c->a = mul(c->a, -1.0);
                } else if (info->type == kernel::SurfaceType::Cylinder) {
                    c->type = 1;
                    c->known = true;
                }
            }
        } else if (element.kind == SubKind::Edge) {
            if (const auto info = kernel::curveInfo(shape_, element)) {
                if (info->type == kernel::CurveType::Line && unitOf(sub(info->end, info->start), c->a)) {
                    c->type = 2;
                    c->p = info->start;
                    c->known = true;
                } else if (info->type == kernel::CurveType::Circle && unitOf(info->axis, c->a)) {
                    c->type = 3;
                    c->p = info->center;
                    c->r = info->radius;
                    c->known = true;
                }
            }
        }
    } catch (...) {
        c->known = false;
    }
    const std::lock_guard<std::mutex> guard(lock_);
    return *carriers_.emplace(element, std::move(c)).first->second;
}

std::string TerritoryCache::carrierKey(SubId element) const
{
    const Carrier& c = carrier(element);
    if (!c.known || (c.type != 0 && c.type != 1))
        return {};
    Vec3 a = c.a;
    // A cylinder's axis has no inherent sign; a plane's normal is its outward side.
    if (c.type == 1 && (a.x < -1e-9 || (std::abs(a.x) <= 1e-9 && (a.y < -1e-9 || (std::abs(a.y) <= 1e-9 && a.z < 0)))))
        a = mul(a, -1.0);
    // The point of the carrier's axis line (cylinder) or plane nearest the origin.
    const Vec3 foot = c.type == 1 ? sub(c.p, mul(a, dot(c.p, a))) : mul(a, dot(c.p, a));
    auto q = [](double v, double per) {
        return std::to_string(quantise(v, per));
    };
    return std::to_string(c.type) + ":" + q(a.x, 1e4) + "," + q(a.y, 1e4) + "," + q(a.z, 1e4) + ":" + q(foot.x, 1e3) +
           "," + q(foot.y, 1e3) + "," + q(foot.z, 1e3) + ":" + q(c.r, 1e3);
}

bool TerritoryCache::sameCarrier(SubId a, SubId b) const
{
    const Carrier& ca = carrier(a);
    const Carrier& cb = carrier(b);
    if (!ca.known || !cb.known || ca.type != cb.type)
        return false;
    const double d = dot(ca.a, cb.a);
    auto offLine = [](const Vec3& p, const Vec3& o, const Vec3& u) {
        const Vec3 w = sub(p, o);
        return norm(sub(w, mul(u, dot(w, u))));
    };
    switch (ca.type) {
    case 0: // plane, same outward side
        return d >= 1.0 - kCarrierParallel && std::abs(dot(sub(cb.p, ca.p), ca.a)) <= kCarrierPosition;
    case 1: // cylinder
        return std::abs(d) >= 1.0 - kCarrierParallel && std::abs(ca.r - cb.r) <= kCarrierPosition &&
               offLine(cb.p, ca.p, ca.a) <= kCarrierPosition;
    case 2: // line
        return std::abs(d) >= 1.0 - kCarrierParallel && offLine(cb.p, ca.p, ca.a) <= kCarrierPosition;
    case 3: // circle
        return std::abs(d) >= 1.0 - kCarrierParallel && std::abs(ca.r - cb.r) <= kCarrierPosition &&
               norm(sub(ca.p, cb.p)) <= kCarrierPosition;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- pick neighbours
std::vector<PickNeighbour> pickNeighboursOf(const ElementMap& map, const kernel::Shape& shape, SubId pick,
                                            const TerritoryCache& territories)
{
    std::vector<PickNeighbour> out;
    try {
        if (!map.impl || (pick.kind != SubKind::Face && pick.kind != SubKind::Edge && pick.kind != SubKind::Vertex))
            return out;
        std::set<SubId> siblings, carriers;
        // A shared name family, or a shared split parent.
        std::set<Hash256> families, splitParents;
        for (const Hash256& name : map.namesOf(pick))
            if (const TopoName* record = map.record(name)) {
                families.insert(record->family);
                if (record->role == "split")
                    splitParents.insert(record->parents.begin(), record->parents.end());
            }
        for (const Hash256& family : families)
            for (const Hash256& member : map.family(family))
                if (const auto s = map.find(member); s && *s != pick && s->kind == pick.kind)
                    siblings.insert(*s);
        if (!splitParents.empty())
            for (const TopoName& record : map.impl->records) {
                if (record.role != "split" || record.kind != pick.kind)
                    continue;
                bool shares = false;
                for (const Hash256& parent : record.parents)
                    shares = shares || splitParents.count(parent) != 0;
                if (const auto s = map.find(record.id); shares && s && *s != pick)
                    siblings.insert(*s);
            }
        // An edge or vertex on exactly the faces the picked one lies on (another piece of the same meeting), where a
        // face that a feature generated (a blend face) stands for every face that feature generated with the same role,
        // and a piece of a split face for every piece of it: the blend faces of the pieces of one blended edge are the
        // pieces of one blend (T-081a Linux holdout3 17:0:
        // two such faces traded places when a cut moved, and the edges on them with them).
        if (pick.kind != SubKind::Face) {
            auto classOf = [&](SubId face) {
                std::vector<std::string> keys;
                for (const Hash256& name : map.namesOf(face)) {
                    const TopoName* record = map.record(name);
                    if (record == nullptr)
                        continue;
                    if (record->role.find(".g") != std::string::npos) {
                        // The pieces of one blend lie on one surface; the blends of other edges do not.
                        const std::string carrier = territories.carrierKey(face);
                        if (!carrier.empty())
                            keys.push_back("g" + record->origin.value.toString() + record->role + carrier);
                    } else if (record->role == "split") {
                        // A piece of a split face stands for every piece of that face.
                        std::string key = "s";
                        for (const Hash256& parent : record->parents)
                            key += parent.hex();
                        keys.push_back(key);
                    }
                }
                if (keys.empty())
                    keys.push_back("f" + std::to_string(face.index));
                std::sort(keys.begin(), keys.end());
                return keys.front();
            };
            auto classesOf = [&](SubId element) {
                std::vector<std::string> out;
                for (const SubId& f : kernel::adjacent(shape, element, SubKind::Face))
                    out.push_back(classOf(f));
                std::sort(out.begin(), out.end());
                return out;
            };
            const std::vector<SubId> faces = kernel::adjacent(shape, pick, SubKind::Face);
            const std::vector<std::string> own = classesOf(pick);
            std::set<SubId> candidates;
            for (const SubId& f : faces) {
                const std::string c = classOf(f);
                if (c.front() == 'f')
                    continue;
                // Every face of the same class: the other pieces of the blend or of the split face.
                for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i)
                    if (const SubId g{SubKind::Face, i}; g != f && classOf(g) == c)
                        for (const SubId& e : kernel::adjacent(shape, g, pick.kind))
                            candidates.insert(e);
            }
            if (!faces.empty())
                for (const SubId& e : kernel::adjacent(shape, faces.front(), pick.kind))
                    candidates.insert(e);
            for (const SubId& other : candidates)
                if (other != pick && classesOf(other) == own)
                    siblings.insert(other);
        }
        // Faces or edges on the same carrier.
        if (pick.kind != SubKind::Vertex) {
            const std::uint32_t n = shape.count(pick.kind);
            for (std::uint32_t i = 0; i < n; ++i) {
                const SubId other{pick.kind, i};
                if (other != pick && siblings.count(other) == 0 && territories.sameCarrier(pick, other))
                    carriers.insert(other);
            }
        }
        const Box3 own = boxOf(territories.of(pick));
        auto gap = [&](SubId s) {
            const Box3 b = boxOf(territories.of(s));
            const double gx = std::max({0.0, b.min.x - own.max.x, own.min.x - b.max.x});
            const double gy = std::max({0.0, b.min.y - own.max.y, own.min.y - b.max.y});
            const double gz = std::max({0.0, b.min.z - own.max.z, own.min.z - b.max.z});
            return quantise(std::sqrt(gx * gx + gy * gy + gz * gz), kLengthQuantaPerMm);
        };
        auto ordered = [&](const std::set<SubId>& set) {
            std::vector<std::pair<std::int64_t, SubId>> v;
            for (const SubId& s : set)
                if (!map.namesOf(s).empty())
                    v.emplace_back(gap(s), s);
            std::sort(v.begin(), v.end());
            return v;
        };
        const auto sib = ordered(siblings);
        const auto car = ordered(carriers);
        std::vector<std::pair<PickNeighbour::Relation, SubId>> chosen;
        std::size_t si = 0, ci = 0;
        for (; si < sib.size() && chosen.size() < 5; ++si)
            chosen.emplace_back(PickNeighbour::Relation::Sibling, sib[si].second);
        for (; ci < car.size() && chosen.size() < kMaxPickNeighbours; ++ci)
            chosen.emplace_back(PickNeighbour::Relation::SameCarrier, car[ci].second);
        for (; si < sib.size() && chosen.size() < kMaxPickNeighbours; ++si)
            chosen.emplace_back(PickNeighbour::Relation::Sibling, sib[si].second);
        for (const auto& [relation, s] : chosen) {
            PickNeighbour n;
            n.relation = relation;
            const std::span<const Hash256> names = map.namesOf(s);
            n.names.assign(names.begin(), names.end());
            std::sort(n.names.begin(), n.names.end());
            n.territory = territories.of(s);
            out.push_back(std::move(n));
        }
    } catch (...) {
    }
    return out;
}

} // namespace ray::naming::detail
