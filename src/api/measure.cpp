// Measure and analysis tools (task T-022). See ray_api/measure.h.
#include <ray_api/measure.h>

#include "json.h"

#include <ray/kernel.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace ray::api::measure {

namespace {

// ---------- vector helpers ----------
Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(Vec3 a)
{
    return std::sqrt(dot(a, a));
}
std::optional<Vec3> unit(Vec3 a)
{
    const double n = norm(a);
    if (!(n > 1e-300) || !std::isfinite(n))
        return std::nullopt;
    return mul(a, 1.0 / n);
}
Vec3 unitOr(Vec3 a, Vec3 fallback)
{
    const auto u = unit(a);
    return u ? *u : fallback;
}
Vec3 mid(Vec3 a, Vec3 b)
{
    return mul(add(a, b), 0.5);
}
double clampUnit(double v)
{
    return std::clamp(v, -1.0, 1.0);
}
// The part of `v` perpendicular to the unit vector `u`.
Vec3 perpendicular(Vec3 v, Vec3 u)
{
    return sub(v, mul(u, dot(v, u)));
}
// Any unit vector perpendicular to the unit vector `u`.
Vec3 anyPerpendicular(Vec3 u)
{
    const Vec3 helper = std::abs(u.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return unitOr(cross(u, helper), Vec3{0, 0, 1});
}

constexpr double kPi = 3.14159265358979323846;
// Two directions count as parallel when the sine of the angle between them is below this.
constexpr double kParallel = 1e-7;
// Two points count as the same (a shared corner) closer than this, in mm.
constexpr double kSamePoint = 1e-6;
constexpr int kArcSegments = 32;

bool parallel(Vec3 u, Vec3 v)
{
    return norm(cross(u, v)) < kParallel;
}

// ---------- values ----------
Value length(std::string key, std::string label, double mm, bool primary = false)
{
    Value v;
    v.key = std::move(key);
    v.label = std::move(label);
    v.quantity = Quantity::Length;
    v.number = mm;
    v.primary = primary;
    return v;
}
Value scalar(std::string key, std::string label, Quantity q, double number, bool primary = false)
{
    Value v = length(std::move(key), std::move(label), number, primary);
    v.quantity = q;
    return v;
}
Value vec(std::string key, std::string label, Quantity q, Vec3 vector, bool primary = false)
{
    Value v;
    v.key = std::move(key);
    v.label = std::move(label);
    v.quantity = q;
    v.vector = vector;
    v.primary = primary;
    return v;
}
Value text(std::string key, std::string label, std::string value)
{
    Value v;
    v.key = std::move(key);
    v.label = std::move(label);
    v.quantity = Quantity::Text;
    v.text = std::move(value);
    return v;
}

void clearPrimary(Measurement& m)
{
    for (Value& v : m.values)
        v.primary = false;
}

void addLabel(Witness& w, Vec3 at, std::string key)
{
    w.labels.push_back(Label{at, std::move(key)});
}

// Arc of radius `r` about `centre`, the short way from unit direction `from` to unit direction `to`.
void addArc(Witness& w, Vec3 centre, Vec3 from, Vec3 to, double r)
{
    const double sweep = std::acos(clampUnit(dot(from, to)));
    const auto axis = unit(cross(from, to));
    if (!axis || !(sweep > 1e-9) || !(r > 0))
        return;
    const Vec3 side = cross(*axis, from);
    Vec3 last = add(centre, mul(from, r));
    for (int i = 1; i <= kArcSegments; ++i) {
        const double t = sweep * i / kArcSegments;
        const Vec3 p = add(centre, mul(add(mul(from, std::cos(t)), mul(side, std::sin(t))), r));
        w.lines.push_back(Segment{last, p, false});
        last = p;
    }
}

void addBoxOutline(Witness& w, const Box3& b)
{
    const auto corner = [&](int i) {
        return Vec3{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
    };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0)
                w.lines.push_back(Segment{corner(i), corner(i | bit), true});
        }
    }
}

std::string_view subKindName(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Vertex:
        return "Vertex";
    case kernel::SubKind::Edge:
        return "Edge";
    case kernel::SubKind::Face:
        return "Face";
    case kernel::SubKind::Solid:
        break;
    }
    return "Body";
}

std::string_view surfaceName(kernel::SurfaceType type)
{
    switch (type) {
    case kernel::SurfaceType::Plane:
        return "Plane";
    case kernel::SurfaceType::Cylinder:
        return "Cylinder";
    case kernel::SurfaceType::Cone:
        return "Cone";
    case kernel::SurfaceType::Sphere:
        return "Sphere";
    case kernel::SurfaceType::Torus:
        return "Torus";
    case kernel::SurfaceType::Bspline:
        return "B-spline";
    case kernel::SurfaceType::Other:
        break;
    }
    return "Other";
}

std::string_view curveName(kernel::CurveType type)
{
    switch (type) {
    case kernel::CurveType::Line:
        return "Line";
    case kernel::CurveType::Circle:
        return "Circle";
    case kernel::CurveType::Ellipse:
        return "Ellipse";
    case kernel::CurveType::Bspline:
        return "B-spline";
    case kernel::CurveType::Other:
        break;
    }
    return "Other";
}

// ---------- what an element is, geometrically ----------
enum class GeoType { Point, Line, Plane, Axis, Circle, Sphere, Torus, Body, Other };

struct Geo {
    GeoType type = GeoType::Other;
    kernel::SubKind kind = kernel::SubKind::Solid;
    Vec3 origin{};   // Point: the point; Line: start; Plane: a point of it; Axis: a point of the axis; Circle, Sphere,
                     // Torus: the centre
    Vec3 dir{};      // Line: unit direction start to end; Plane: outward unit normal; Axis, Circle, Torus: unit axis
    Vec3 centroid{}; // faces: area centroid; edges: midpoint of the ends (a circle: its centre); vertex: the point
    double radius = 0;
    bool cone = false;
    std::optional<kernel::SurfaceInfo> surface;
    std::optional<kernel::CurveInfo> curve;
};

bool hasDirection(const Geo& g)
{
    return g.type == GeoType::Line || g.type == GeoType::Plane || g.type == GeoType::Axis || g.type == GeoType::Circle;
}
// Round elements with an axis line (a cylinder or cone face, a circular edge).
bool axial(const Geo& g)
{
    return g.type == GeoType::Axis || g.type == GeoType::Circle;
}
// Round elements with a centre point.
bool centred(const Geo& g)
{
    return g.type == GeoType::Circle || g.type == GeoType::Sphere;
}
// Line-like for angles: straight edges and the axes of round elements.
bool lineLike(const Geo& g)
{
    return g.type == GeoType::Line || axial(g);
}

// Whether `p` is in the body's material: a tiny probe box there is at distance 0 from the solid (BRepExtrema reports
// a shape inside a solid as an inner solution, distance 0). nullopt when that cannot be told (no solid, a failure).
std::optional<bool> inMaterial(const kernel::Shape& body, Vec3 p, double probe)
{
    if (body.count(kernel::SubKind::Solid) == 0)
        return std::nullopt;
    const Vec3 h{probe, probe, probe};
    Result<kernel::Shape> box = kernel::makeBox(sub(p, h), add(p, h));
    if (!box)
        return std::nullopt;
    Result<kernel::Distance> d = kernel::minDistance(body, kernel::SubId{kernel::SubKind::Solid, 0}, *box,
                                                     kernel::SubId{kernel::SubKind::Solid, 0});
    if (!d)
        return std::nullopt;
    return d->value <= probe * 1e-3;
}

// The outward normal of a planar face. kernel::SurfaceInfo gives the plane's axis and whether the face is reversed,
// but the axis of a gp_Pln is the surface normal only when its coordinate system is right-handed (OCCT's chamfer
// faces, for one, are not), so the side is checked: a point just off the face along the normal must be outside the
// material and the point just behind it inside. Undecided (a sheet body, a point at a corner): the kernel's guess.
Vec3 outwardNormal(const Element& e, const kernel::SurfaceInfo& s, Vec3 guess)
{
    const Box3& b = s.bounds;
    const double size = norm(sub(b.max, b.min));
    if (!(size > 0))
        return guess;
    const double step = std::max(size * 1e-4, 1e-5);
    const double probe = step * 0.25;
    // A point on the face: the one nearest its centroid (the centroid itself may lie off a non-convex face).
    Vec3 on = s.centroid;
    {
        const Vec3 h{probe, probe, probe};
        Result<kernel::Shape> box = kernel::makeBox(sub(on, h), add(on, h));
        if (box) {
            Result<kernel::Distance> d =
                kernel::minDistance(e.shape, e.sub, *box, kernel::SubId{kernel::SubKind::Solid, 0});
            if (d)
                on = d->p1;
        }
    }
    const std::optional<bool> front = inMaterial(e.shape, add(on, mul(guess, step)), probe);
    const std::optional<bool> back = inMaterial(e.shape, sub(on, mul(guess, step)), probe);
    if (front && back && *front && !*back)
        return mul(guess, -1.0);
    return guess;
}

Result<Geo> describe(const Element& e)
{
    Geo g;
    g.kind = e.sub.kind;
    switch (e.sub.kind) {
    case kernel::SubKind::Vertex: {
        Result<Vec3> p = kernel::vertexPoint(e.shape, e.sub);
        if (!p)
            return tl::unexpected(p.error());
        g.type = GeoType::Point;
        g.origin = g.centroid = *p;
        return g;
    }
    case kernel::SubKind::Edge: {
        Result<kernel::CurveInfo> c = kernel::curveInfo(e.shape, e.sub);
        if (!c)
            return tl::unexpected(c.error());
        g.curve = *c;
        g.centroid = mid(c->start, c->end);
        if (c->type == kernel::CurveType::Line) {
            g.type = GeoType::Line;
            g.origin = c->start;
            g.dir = unitOr(c->axis, unitOr(sub(c->end, c->start), Vec3{1, 0, 0}));
        } else if (c->type == kernel::CurveType::Circle) {
            g.type = GeoType::Circle;
            g.origin = g.centroid = c->center;
            g.dir = unitOr(c->axis, Vec3{0, 0, 1});
            g.radius = c->radius;
        }
        return g;
    }
    case kernel::SubKind::Face: {
        Result<kernel::SurfaceInfo> s = kernel::surfaceInfo(e.shape, e.sub);
        if (!s)
            return tl::unexpected(s.error());
        g.surface = *s;
        g.centroid = s->centroid;
        g.origin = s->origin;
        g.radius = s->radius;
        switch (s->type) {
        case kernel::SurfaceType::Plane: {
            g.type = GeoType::Plane;
            const Vec3 n = unitOr(s->axis, Vec3{0, 0, 1});
            // The material side is behind the outward normal.
            g.dir = outwardNormal(e, *s, s->reversed ? mul(n, -1.0) : n);
            break;
        }
        case kernel::SurfaceType::Cylinder:
        case kernel::SurfaceType::Cone:
            g.type = GeoType::Axis;
            g.cone = s->type == kernel::SurfaceType::Cone;
            g.dir = unitOr(s->axis, Vec3{0, 0, 1});
            break;
        case kernel::SurfaceType::Sphere:
            g.type = GeoType::Sphere;
            g.dir = unitOr(s->axis, Vec3{0, 0, 1});
            break;
        case kernel::SurfaceType::Torus:
            g.type = GeoType::Torus;
            g.dir = unitOr(s->axis, Vec3{0, 0, 1});
            break;
        default:
            break;
        }
        return g;
    }
    case kernel::SubKind::Solid:
        break;
    }
    g.type = GeoType::Body;
    return g;
}

// The range of an axial element along the unit axis `u`, measured from `origin`: a face's bounding box corners
// projected on the axis (exact for an axis-aligned cylinder), a circle's centre.
std::pair<double, double> axialRange(const Geo& g, Vec3 origin, Vec3 u)
{
    if (g.type == GeoType::Circle || !g.surface) {
        const double t = dot(sub(g.origin, origin), u);
        return {t, t};
    }
    const Box3& b = g.surface->bounds;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    for (int i = 0; i < 8; ++i) {
        const Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
        const double t = dot(sub(c, origin), u);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    return {lo, hi};
}

// The axis direction turned to point "up" (+Z, else +Y, else +X), so "the top end" means the same for every element.
Vec3 upwards(Vec3 u)
{
    const double lead = std::abs(u.z) > 1e-9 ? u.z : (std::abs(u.y) > 1e-9 ? u.y : u.x);
    return lead < 0 ? mul(u, -1.0) : u;
}

// The point of an axial element's axis nearest its centroid (a circle: its centre).
Vec3 axisPoint(const Geo& g)
{
    return add(g.origin, mul(g.dir, dot(sub(g.centroid, g.origin), g.dir)));
}

Status checkCancel(JobContext& ctx)
{
    return ctx.cancel.check();
}

// ---------- the round measures of one element (shared by radius() and element()) ----------
// Adds radius, diameter and the like for a round element; false if it is not round.
bool addRoundValues(Measurement& m, const Geo& g, bool primaryDiameter)
{
    const auto radiusLine = [&](Vec3 centre, Vec3 onCurve) {
        m.witness.points.push_back(centre);
        m.witness.lines.push_back(Segment{centre, onCurve, false});
        addLabel(m.witness, mid(centre, onCurve), primaryDiameter ? "diameter" : "radius");
    };
    if (g.type == GeoType::Circle && g.curve) {
        const kernel::CurveInfo& c = *g.curve;
        m.values.push_back(length("radius", "Radius", c.radius, !primaryDiameter));
        m.values.push_back(length("diameter", "Diameter", 2 * c.radius, primaryDiameter));
        m.values.push_back(vec("center", "Centre", Quantity::Point, c.center));
        m.values.push_back(vec("axis", "Axis", Quantity::Direction, g.dir));
        if (!c.closed && c.radius > 0)
            m.values.push_back(scalar("arcAngle", "Arc angle", Quantity::Angle, c.length / c.radius));
        radiusLine(c.center, c.start);
        return true;
    }
    if (!g.surface)
        return false;
    const kernel::SurfaceInfo& s = *g.surface;
    switch (s.type) {
    case kernel::SurfaceType::Cylinder: {
        m.values.push_back(length("radius", "Radius", s.radius, !primaryDiameter));
        m.values.push_back(length("diameter", "Diameter", 2 * s.radius, primaryDiameter));
        m.values.push_back(vec("axis", "Axis", Quantity::Direction, g.dir));
        const Vec3 u = upwards(g.dir);
        const Vec3 base = axisPoint(g);
        const auto [lo, hi] = axialRange(g, base, u);
        m.witness.lines.push_back(Segment{add(base, mul(u, lo)), add(base, mul(u, hi)), true});
        // A radius at the top end, towards the face (a partial cylinder: towards its centroid).
        const Vec3 top = add(base, mul(u, hi));
        const Vec3 towards = unitOr(perpendicular(sub(g.centroid, base), u), anyPerpendicular(u));
        radiusLine(top, add(top, mul(towards, s.radius)));
        return true;
    }
    case kernel::SurfaceType::Cone: {
        m.values.push_back(scalar("halfAngle", "Half angle", Quantity::Angle, s.semiAngle, true));
        m.values.push_back(scalar("includedAngle", "Included angle", Quantity::Angle, 2 * s.semiAngle));
        m.values.push_back(length("referenceRadius", "Reference radius", s.radius));
        m.values.push_back(vec("axis", "Axis", Quantity::Direction, g.dir));
        const Vec3 u = upwards(g.dir);
        const Vec3 base = axisPoint(g);
        const auto [lo, hi] = axialRange(g, base, u);
        m.witness.lines.push_back(Segment{add(base, mul(u, lo)), add(base, mul(u, hi)), true});
        addLabel(m.witness, add(base, mul(u, hi)), "halfAngle");
        return true;
    }
    case kernel::SurfaceType::Sphere: {
        m.values.push_back(length("radius", "Radius", s.radius, !primaryDiameter));
        m.values.push_back(length("diameter", "Diameter", 2 * s.radius, primaryDiameter));
        m.values.push_back(vec("center", "Centre", Quantity::Point, s.origin));
        const Vec3 towards = unitOr(sub(g.centroid, s.origin), anyPerpendicular(g.dir));
        radiusLine(s.origin, add(s.origin, mul(towards, s.radius)));
        return true;
    }
    case kernel::SurfaceType::Torus: {
        m.values.push_back(length("majorRadius", "Major radius", s.radius, true));
        m.values.push_back(vec("center", "Centre", Quantity::Point, s.origin));
        m.values.push_back(vec("axis", "Axis", Quantity::Direction, g.dir));
        m.notes.push_back("The tube (minor) radius of a torus is not available yet.");
        m.witness.points.push_back(s.origin);
        addLabel(m.witness, s.origin, "majorRadius");
        return true;
    }
    default:
        break;
    }
    return false;
}

// ---------- the distance specials ----------
// Adds the plane, line, point and axis distances that apply to the pair; returns true if one became primary.
bool addDistanceSpecials(Measurement& m, const Geo& a, const Geo& b)
{
    Witness& w = m.witness;
    const auto primary = [&](Value v, Vec3 from, Vec3 to) {
        clearPrimary(m);
        v.primary = true;
        w.lines.push_back(Segment{from, to, false});
        w.points.push_back(from);
        w.points.push_back(to);
        addLabel(w, mid(from, to), v.key);
        m.values.push_back(std::move(v));
        return true;
    };

    // Two planes.
    if (a.type == GeoType::Plane && b.type == GeoType::Plane) {
        if (!parallel(a.dir, b.dir))
            return false;
        const double d = dot(sub(b.origin, a.origin), a.dir);
        const Vec3 from = a.centroid;
        const Vec3 to = add(a.centroid, mul(a.dir, d));
        return primary(length("planeDistance", "Plane to plane", std::abs(d)), from, to);
    }
    // Two straight edges.
    if (a.type == GeoType::Line && b.type == GeoType::Line) {
        if (!parallel(a.dir, b.dir))
            return false;
        const Vec3 off = perpendicular(sub(b.origin, a.origin), a.dir);
        const Vec3 from = a.centroid;
        return primary(length("lineDistance", "Line to line", norm(off)), from, add(from, off));
    }
    // A straight edge or an axis, and a plane, parallel to each other.
    for (const auto& [line, plane] : {std::pair<const Geo*, const Geo*>{&a, &b}, {&b, &a}}) {
        if (!(lineLike(*line) && plane->type == GeoType::Plane))
            continue;
        if (std::abs(dot(line->dir, plane->dir)) >= kParallel)
            return false;
        const Vec3 from = axial(*line) ? axisPoint(*line) : line->centroid;
        const double d = dot(sub(from, plane->origin), plane->dir);
        const bool isAxis = axial(*line);
        return primary(
            length(isAxis ? "axisToPlane" : "lineToPlane", isAxis ? "Axis to plane" : "Line to plane", std::abs(d)),
            from, sub(from, mul(plane->dir, d)));
    }
    // A point and a plane, a line, an axis or a centre.
    for (const auto& [pt, other] : {std::pair<const Geo*, const Geo*>{&a, &b}, {&b, &a}}) {
        if (pt->type != GeoType::Point)
            continue;
        const Vec3 p = pt->origin;
        if (other->type == GeoType::Plane) {
            const double d = dot(sub(p, other->origin), other->dir);
            m.values.push_back(length("pointToPlane", "Point to plane", std::abs(d)));
            w.lines.push_back(Segment{p, sub(p, mul(other->dir, d)), true});
            return false;
        }
        if (other->type == GeoType::Line) {
            const Vec3 off = perpendicular(sub(p, other->origin), other->dir);
            m.values.push_back(length("pointToLine", "Point to line", norm(off)));
            w.lines.push_back(Segment{p, sub(p, off), true});
            return false;
        }
        if (centred(*other) && other->type == GeoType::Circle) {
            const Vec3 off = perpendicular(sub(p, other->origin), other->dir);
            m.values.push_back(length("pointToAxis", "Point to axis", norm(off)));
            return primary(length("pointToCenter", "Point to centre", norm(sub(p, other->origin))), other->origin, p);
        }
        if (other->type == GeoType::Axis) {
            const Vec3 off = perpendicular(sub(p, other->origin), other->dir);
            return primary(length("pointToAxis", "Point to axis", norm(off)), sub(p, off), p);
        }
        if (other->type == GeoType::Sphere)
            return primary(length("pointToCenter", "Point to centre", norm(sub(p, other->origin))), other->origin, p);
        return false;
    }
    // A straight edge parallel to an axis.
    for (const auto& [line, ax] : {std::pair<const Geo*, const Geo*>{&a, &b}, {&b, &a}}) {
        if (line->type != GeoType::Line || !axial(*ax) || !parallel(line->dir, ax->dir))
            continue;
        const Vec3 from = line->centroid;
        const Vec3 off = perpendicular(sub(from, ax->origin), ax->dir);
        return primary(length("lineToAxis", "Line to axis", norm(off)), sub(from, off), from);
    }
    // Two round elements: centre to centre, axis to axis.
    if (axial(a) && axial(b)) {
        const Vec3 u = upwards(a.dir);
        const Vec3 aBase = axisPoint(a);
        if (parallel(a.dir, b.dir)) {
            const Vec3 w0 = perpendicular(sub(axisPoint(b), aBase), u);
            const double d = norm(w0);
            m.values.push_back(length("centerDx", "Centre \u0394X", w0.x));
            m.values.push_back(length("centerDy", "Centre \u0394Y", w0.y));
            m.values.push_back(length("centerDz", "Centre \u0394Z", w0.z));
            if (a.type == GeoType::Circle && b.type == GeoType::Circle)
                m.values.push_back(length("centerToCenter", "Centre to centre (3D)", norm(sub(b.origin, a.origin))));
            if (d < kSamePoint)
                m.notes.push_back("The axes are the same line (coaxial).");
            // The dimension line at the top end of both, between the axes; the axes as construction lines.
            const auto [aLo, aHi] = axialRange(a, aBase, u);
            const auto [bLo, bHi] = axialRange(b, aBase, u);
            const double top = std::max(aHi, bHi);
            const Vec3 from = add(aBase, mul(u, top));
            const Vec3 to = add(from, w0);
            if (aHi - aLo > kSamePoint || top - aHi > kSamePoint)
                w.lines.push_back(Segment{add(aBase, mul(u, aLo)), from, true});
            if (bHi - bLo > kSamePoint || top - bHi > kSamePoint)
                w.lines.push_back(Segment{add(add(aBase, w0), mul(u, bLo)), to, true});
            return primary(length("centerDistance", "Centre distance", d), from, to);
        }
        // Skew axes: their common perpendicular.
        const Vec3 n = cross(a.dir, b.dir);
        const Vec3 r = sub(axisPoint(b), aBase);
        const double nn = dot(n, n);
        const double ta = dot(cross(r, b.dir), n) / nn;
        const double tb = dot(cross(r, a.dir), n) / nn;
        const Vec3 pa = add(aBase, mul(a.dir, ta));
        const Vec3 pb = add(axisPoint(b), mul(b.dir, tb));
        m.values.push_back(length("axisDistance", "Axis to axis", norm(sub(pb, pa))));
        w.lines.push_back(Segment{pa, pb, true});
        m.notes.push_back("The axes are not parallel: the axis distance is along their common perpendicular.");
        if (a.type == GeoType::Circle && b.type == GeoType::Circle)
            return primary(length("centerToCenter", "Centre to centre", norm(sub(b.origin, a.origin))), a.origin,
                           b.origin);
        return false;
    }
    // Spheres and circles: centre to centre.
    if (centred(a) && centred(b))
        return primary(length("centerToCenter", "Centre to centre", norm(sub(b.origin, a.origin))), a.origin, b.origin);
    return false;
}

// ---------- the angle between two directed elements ----------
// The line of an element for angles: a point and a unit direction, and the point to measure "towards".
struct LineOf {
    Vec3 origin, dir, towards;
    std::optional<std::pair<Vec3, Vec3>> ends; // a straight edge's two ends
};
LineOf lineOf(const Geo& g)
{
    if (g.type == GeoType::Line && g.curve)
        return LineOf{g.origin, g.dir, g.centroid, std::make_pair(g.curve->start, g.curve->end)};
    const Vec3 p = axisPoint(g);
    return LineOf{p, g.dir, p, std::nullopt};
}

void addAnglePair(Measurement& m, double acute, bool primaryAcute)
{
    m.values.push_back(scalar("angle", "Acute angle", Quantity::Angle, acute, primaryAcute));
    m.values.push_back(scalar("supplement", "Obtuse angle", Quantity::Angle, kPi - acute));
}

// Fills `m` with the angle between `a` and `b` (both hasDirection()); `sharedEdge` is the edge they share when they
// are faces of one shape. True when they are parallel (the angle is 0, with no arc).
bool angleBetween(Measurement& m, const Geo& a, const Geo& b, const std::optional<kernel::CurveInfo>& sharedEdge)
{
    Witness& w = m.witness;
    // ---- two planes ----
    if (a.type == GeoType::Plane && b.type == GeoType::Plane) {
        const double between = std::acos(clampUnit(dot(a.dir, b.dir))); // between the outward normals
        const double acute = std::min(between, kPi - between);
        if (parallel(a.dir, b.dir)) {
            addAnglePair(m, 0.0, true);
            m.notes.push_back("The faces are parallel.");
            return true;
        }
        const Vec3 e = unitOr(cross(a.dir, b.dir), anyPerpendicular(a.dir));
        // The point of the planes' common line nearest the middle of the two centroids.
        const Vec3 c = mid(a.centroid, b.centroid);
        const double d1 = dot(a.dir, a.origin);
        const double d2 = dot(b.dir, b.origin);
        const double d3 = dot(e, c);
        const double det = dot(a.dir, cross(b.dir, e));
        Vec3 p = c;
        if (std::abs(det) > 1e-300) {
            p = mul(add(add(mul(cross(b.dir, e), d1), mul(cross(e, a.dir), d2)), mul(cross(a.dir, b.dir), d3)),
                    1.0 / det);
        }
        if (sharedEdge && sharedEdge->type == kernel::CurveType::Line)
            p = mid(sharedEdge->start, sharedEdge->end); // on the shared edge: its middle
        const Vec3 ta = perpendicular(sub(a.centroid, p), e);
        const Vec3 tb = perpendicular(sub(b.centroid, p), e);
        const auto ua = unit(ta);
        const auto ub = unit(tb);
        bool edgeAnglePrimary = false;
        if (sharedEdge && ua && ub) {
            // The material lies behind both outward normals: at a convex edge face a runs into the material side of
            // plane b.
            const bool convex = dot(*ua, b.dir) < 0;
            const double material = convex ? kPi - between : kPi + between;
            m.values.push_back(scalar("edgeAngle", convex ? "Edge angle (material)" : "Edge angle (reflex)",
                                      Quantity::Angle, material, true));
            edgeAnglePrimary = true;
        }
        addAnglePair(m, acute, !edgeAnglePrimary);
        m.values.push_back(scalar("normalAngle", "Between normals", Quantity::Angle, between));
        if (ua && ub) {
            // Big enough to see next to a narrow face (a chamfer), small beside the wide one.
            const double r = std::max(0.4 * std::min(norm(ta), norm(tb)), 0.15 * std::max(norm(ta), norm(tb)));
            // At a convex edge the arc goes over the face extensions, outside the material.
            const bool convex = dot(*ua, b.dir) < 0;
            const Vec3 fa = convex ? mul(*ua, -1.0) : *ua;
            const Vec3 fb = convex ? mul(*ub, -1.0) : *ub;
            w.lines.push_back(Segment{p, add(p, mul(fa, r * 1.25)), true});
            w.lines.push_back(Segment{p, add(p, mul(fb, r * 1.25)), true});
            addArc(w, p, fa, fb, r);
            w.points.push_back(p);
            addLabel(w, add(p, mul(unitOr(add(fa, fb), fa), r * 1.15)), edgeAnglePrimary ? "edgeAngle" : "angle");
        }
        return false;
    }
    // ---- a line and a plane ----
    for (const auto& [lg, pg] : {std::pair<const Geo*, const Geo*>{&a, &b}, {&b, &a}}) {
        if (!(lineLike(*lg) && pg->type == GeoType::Plane))
            continue;
        const LineOf l = lineOf(*lg);
        const double s = std::abs(dot(l.dir, pg->dir));
        const double acute = std::asin(clampUnit(s));
        addAnglePair(m, acute, true);
        m.values.push_back(scalar("toNormal", "To the plane's normal", Quantity::Angle, kPi / 2 - acute));
        if (s < kParallel) {
            m.notes.push_back("The line is parallel to the plane.");
            return true;
        }
        const double t = dot(sub(pg->origin, l.origin), pg->dir) / dot(l.dir, pg->dir);
        const Vec3 p = add(l.origin, mul(l.dir, t));
        Vec3 f = sub(l.towards, p);
        if (norm(f) < kSamePoint)
            f = l.dir;
        const Vec3 fu = unitOr(f, l.dir);
        const auto proj = unit(perpendicular(fu, pg->dir));
        if (proj) {
            const double r = std::max(0.4 * norm(sub(l.towards, p)), 1e-3);
            w.lines.push_back(Segment{p, l.towards, true});
            w.lines.push_back(Segment{p, add(p, mul(*proj, r * 1.25)), true});
            addArc(w, p, fu, *proj, r);
            w.points.push_back(p);
            addLabel(w, add(p, mul(unitOr(add(fu, *proj), fu), r * 1.15)), "angle");
        }
        return false;
    }
    // ---- two lines (straight edges or axes) ----
    const LineOf la = lineOf(a);
    const LineOf lb = lineOf(b);
    const double between = std::acos(clampUnit(dot(la.dir, lb.dir)));
    const double acute = std::min(between, kPi - between);
    // A shared end: the included angle at that corner, from the corner along each edge.
    std::optional<Vec3> corner;
    Vec3 fa = la.dir, fb = lb.dir;
    if (la.ends && lb.ends) {
        const std::array<Vec3, 2> ea{la.ends->first, la.ends->second};
        const std::array<Vec3, 2> eb{lb.ends->first, lb.ends->second};
        for (int i = 0; i < 2 && !corner; ++i) {
            for (int j = 0; j < 2 && !corner; ++j) {
                if (norm(sub(ea[i], eb[j])) < kSamePoint) {
                    corner = ea[i];
                    fa = unitOr(sub(ea[1 - i], ea[i]), la.dir);
                    fb = unitOr(sub(eb[1 - j], eb[j]), lb.dir);
                }
            }
        }
    }
    if (corner) {
        m.values.push_back(
            scalar("includedAngle", "Included angle", Quantity::Angle, std::acos(clampUnit(dot(fa, fb))), true));
        addAnglePair(m, acute, false);
    } else {
        addAnglePair(m, acute, true);
    }
    if (parallel(la.dir, lb.dir)) {
        m.notes.push_back(axial(a) || axial(b) ? "The axes are parallel." : "The lines are parallel.");
        return true;
    }
    Vec3 p;
    if (corner) {
        p = *corner;
    } else {
        // Middle of the closest points of the two lines.
        const Vec3 n = cross(la.dir, lb.dir);
        const Vec3 r = sub(lb.origin, la.origin);
        const double nn = dot(n, n);
        const Vec3 pa = add(la.origin, mul(la.dir, dot(cross(r, lb.dir), n) / nn));
        const Vec3 pb = add(lb.origin, mul(lb.dir, dot(cross(r, la.dir), n) / nn));
        p = mid(pa, pb);
        if (norm(sub(pa, pb)) > kSamePoint)
            m.notes.push_back("The lines do not meet: the angle is between their directions.");
        if (dot(sub(la.towards, p), fa) < 0)
            fa = mul(fa, -1.0);
        if (dot(sub(lb.towards, p), fb) < 0)
            fb = mul(fb, -1.0);
        w.lines.push_back(Segment{pa, la.towards, true});
        w.lines.push_back(Segment{pb, lb.towards, true});
    }
    double r = 0.4 * std::min(norm(sub(la.towards, p)), norm(sub(lb.towards, p)));
    if (la.ends && lb.ends && corner) {
        r = 0.3 * std::min(norm(sub(la.ends->second, la.ends->first)), norm(sub(lb.ends->second, lb.ends->first)));
    }
    if (!(r > 1e-6))
        r = 1.0;
    addArc(w, p, fa, fb, r);
    w.points.push_back(p);
    addLabel(w, add(p, mul(unitOr(add(fa, fb), fa), r * 1.15)), corner ? "includedAngle" : "angle");
    return false;
}

// The straight or curved edge two faces of one shape share, if any.
std::optional<kernel::CurveInfo> sharedEdge(const Element& a, const Element& b)
{
    if (a.shape.impl != b.shape.impl || a.sub.kind != kernel::SubKind::Face || b.sub.kind != kernel::SubKind::Face)
        return std::nullopt;
    const std::vector<kernel::SubId> ea = kernel::adjacent(a.shape, a.sub, kernel::SubKind::Edge);
    const std::vector<kernel::SubId> eb = kernel::adjacent(b.shape, b.sub, kernel::SubKind::Edge);
    std::vector<kernel::SubId> common;
    std::set_intersection(ea.begin(), ea.end(), eb.begin(), eb.end(), std::back_inserter(common));
    for (const kernel::SubId& e : common) {
        Result<kernel::CurveInfo> c = kernel::curveInfo(a.shape, e);
        if (c && c->length > 0)
            return *c;
    }
    return std::nullopt;
}

// ---------- formatting ----------
std::string fixed(double v, int decimals)
{
    decimals = std::clamp(decimals, 0, 15);
    if (!std::isfinite(v))
        return "-";
    char buffer[400];
    const auto r = std::to_chars(buffer, buffer + sizeof buffer, v, std::chars_format::fixed, decimals);
    if (r.ec != std::errc())
        return "-";
    std::string s(buffer, r.ptr);
    if (!s.empty() && s.front() == '-' && s.find_first_not_of("0.", 1) == std::string::npos)
        s.erase(0, 1);
    return s;
}

constexpr double kMmPerInch = 25.4;
constexpr double kGramsPerPound = 453.59237;
// g/cm³ to lb/in³: (1 g / 453.59237) per (1 cm³ / 16.387064).
constexpr double kLbPerIn3PerGPerCm3 = 16.387064 / 453.59237;

std::string triple(Vec3 v, UnitFormat f, const char* separator)
{
    const bool inch = f.system == UnitSystem::Inch;
    const double s = inch ? 1.0 / kMmPerInch : 1.0;
    return fixed(v.x * s, f.decimals) + separator + fixed(v.y * s, f.decimals) + separator +
           fixed(v.z * s, f.decimals) + (inch ? " in" : " mm");
}

json::Value point(Vec3 p)
{
    return json::Value::array({json::Value::number(p.x), json::Value::number(p.y), json::Value::number(p.z)});
}

json::Value valueOf(const Value& v)
{
    switch (v.quantity) {
    case Quantity::Point:
    case Quantity::Size:
    case Quantity::Direction:
        return point(v.vector);
    case Quantity::Text:
        return json::Value::string(v.text);
    default:
        break;
    }
    return json::Value::number(v.number);
}

} // namespace

// ---------- Measurement ----------
const Value* Measurement::find(std::string_view key) const
{
    for (const Value& v : values) {
        if (v.key == key)
            return &v;
    }
    return nullptr;
}

const Value* Measurement::primary() const
{
    for (const Value& v : values) {
        if (v.primary)
            return &v;
    }
    return values.empty() ? nullptr : &values.front();
}

// ---------- distance ----------
Result<Measurement> distance(const Element& a, const Element& b, JobContext& ctx)
{
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Result<kernel::Distance> d = kernel::minDistance(a.shape, a.sub, b.shape, b.sub);
    if (!d)
        return tl::unexpected(d.error());
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Result<Geo> ga = describe(a);
    if (!ga)
        return tl::unexpected(ga.error());
    Result<Geo> gb = describe(b);
    if (!gb)
        return tl::unexpected(gb.error());

    Measurement m;
    m.kind = Kind::Distance;
    m.title = "Distance";
    const Vec3 delta = sub(d->p2, d->p1);
    m.values.push_back(length("distance", "Minimum distance", d->value, true));
    m.values.push_back(length("dx", "\u0394X", delta.x));
    m.values.push_back(length("dy", "\u0394Y", delta.y));
    m.values.push_back(length("dz", "\u0394Z", delta.z));
    m.values.push_back(vec("p1", "Closest point A", Quantity::Point, d->p1));
    m.values.push_back(vec("p2", "Closest point B", Quantity::Point, d->p2));
    const bool special = addDistanceSpecials(m, *ga, *gb);
    // The minimum distance itself: the headline, or a construction line beside the special one.
    if (d->value > kSamePoint || !special) {
        m.witness.lines.push_back(Segment{d->p1, d->p2, special});
        m.witness.points.push_back(d->p1);
        m.witness.points.push_back(d->p2);
    }
    if (!special && d->value > kSamePoint)
        addLabel(m.witness, mid(d->p1, d->p2), "distance");
    if (d->value <= kSamePoint)
        m.notes.push_back("The elements touch.");
    return m;
}

// ---------- angle ----------
Result<Measurement> angle(const Element& a, const Element& b, JobContext& ctx)
{
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Result<Geo> ga = describe(a);
    if (!ga)
        return tl::unexpected(ga.error());
    Result<Geo> gb = describe(b);
    if (!gb)
        return tl::unexpected(gb.error());
    if (!hasDirection(*ga) || !hasDirection(*gb)) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument,
                                    "An angle needs two planar faces, straight edges or round faces and edges (their "
                                    "axes), or one line and one plane.",
                                    {"api::measure::angle"}});
    }
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Measurement m;
    m.kind = Kind::Angle;
    m.title = "Angle";
    angleBetween(m, *ga, *gb, sharedEdge(a, b));
    return m;
}

// ---------- radius ----------
Result<Measurement> radius(const Element& e, JobContext& ctx)
{
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Result<Geo> g = describe(e);
    if (!g)
        return tl::unexpected(g.error());
    Measurement m;
    m.kind = Kind::Radius;
    m.title = "Radius";
    if (g->type == GeoType::Other && g->curve && g->curve->type == kernel::CurveType::Ellipse) {
        m.values.push_back(length("majorRadius", "Major radius", g->curve->radius, true));
        m.values.push_back(vec("center", "Centre", Quantity::Point, g->curve->center));
        m.notes.push_back("An ellipse's minor radius is not available yet.");
        m.witness.points.push_back(g->curve->center);
        addLabel(m.witness, g->curve->center, "majorRadius");
        return m;
    }
    if (!addRoundValues(m, *g, false)) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument,
                                    "A radius needs a circular edge or a cylindrical, conical, spherical or toroidal "
                                    "face.",
                                    {"api::measure::radius"}});
    }
    return m;
}

// ---------- mass properties and bounding box ----------
Result<Measurement> massProperties(std::span<const kernel::Shape> shapes, double density, JobContext& ctx)
{
    if (!(density > 0) || !std::isfinite(density))
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "The density must be a positive number.", {}});
    double volume = 0, area = 0;
    Vec3 moment{};
    int counted = 0;
    for (const kernel::Shape& s : shapes) {
        if (auto ok = checkCancel(ctx); !ok)
            return tl::unexpected(ok.error());
        if (s.isNull())
            continue;
        Result<kernel::MassProps> p = kernel::massProps(s);
        if (!p)
            return tl::unexpected(p.error());
        volume += p->volume;
        area += p->area;
        moment = add(moment, mul(p->centroid, p->volume));
        ++counted;
    }
    if (counted == 0)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "There is no body to measure.", {}});
    Measurement m;
    m.kind = Kind::MassProperties;
    m.title = "Mass properties";
    const Vec3 centroid = volume > 0 ? mul(moment, 1.0 / volume) : Vec3{};
    m.values.push_back(scalar("volume", "Volume", Quantity::Volume, volume, true));
    m.values.push_back(scalar("area", "Surface area", Quantity::Area, area));
    m.values.push_back(scalar("mass", "Mass", Quantity::Mass, volume * 1e-3 * density));
    m.values.push_back(scalar("density", "Density", Quantity::Density, density));
    m.values.push_back(vec("centroid", "Centroid", Quantity::Point, centroid));
    if (counted > 1)
        m.values.push_back(scalar("bodies", "Bodies", Quantity::Count, counted));
    if (volume <= 0)
        m.notes.push_back("There is no closed solid: the volume and the mass are zero.");
    m.witness.points.push_back(centroid);
    addLabel(m.witness, centroid, "volume");
    return m;
}

Result<Measurement> boundingBox(std::span<const kernel::Shape> shapes, JobContext& ctx)
{
    std::optional<Box3> box;
    for (const kernel::Shape& s : shapes) {
        if (auto ok = checkCancel(ctx); !ok)
            return tl::unexpected(ok.error());
        if (s.isNull())
            continue;
        const Box3 b = kernel::bounds(s);
        if (!box) {
            box = b;
            continue;
        }
        box->min = Vec3{std::min(box->min.x, b.min.x), std::min(box->min.y, b.min.y), std::min(box->min.z, b.min.z)};
        box->max = Vec3{std::max(box->max.x, b.max.x), std::max(box->max.y, b.max.y), std::max(box->max.z, b.max.z)};
    }
    if (!box)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "There is no body to measure.", {}});
    Measurement m;
    m.kind = Kind::BoundingBox;
    m.title = "Bounding box";
    const Vec3 size = sub(box->max, box->min);
    m.values.push_back(vec("size", "Size (X \u00d7 Y \u00d7 Z)", Quantity::Size, size, true));
    m.values.push_back(length("sizeX", "Size X", size.x));
    m.values.push_back(length("sizeY", "Size Y", size.y));
    m.values.push_back(length("sizeZ", "Size Z", size.z));
    m.values.push_back(vec("min", "Minimum", Quantity::Point, box->min));
    m.values.push_back(vec("max", "Maximum", Quantity::Point, box->max));
    m.values.push_back(vec("center", "Centre", Quantity::Point, mid(box->min, box->max)));
    m.values.push_back(length("diagonal", "Diagonal", norm(size)));
    addBoxOutline(m.witness, *box);
    addLabel(m.witness, box->max, "size");
    return m;
}

// ---------- element ----------
Result<Measurement> element(const Element& e, double density, JobContext& ctx)
{
    if (e.sub.kind == kernel::SubKind::Solid) {
        const kernel::Shape shapes[] = {e.shape};
        Result<Measurement> mass = massProperties(shapes, density, ctx);
        if (!mass)
            return mass;
        Result<Measurement> box = boundingBox(shapes, ctx);
        if (!box)
            return box;
        Measurement m = std::move(*mass);
        m.kind = Kind::Element;
        m.title = "Body";
        for (Value& v : box->values) {
            v.primary = false;
            if (v.key == "center") {
                v.key = "boxCenter";
                v.label = "Box centre";
            }
            m.values.push_back(std::move(v));
        }
        m.witness.lines = std::move(box->witness.lines);
        for (std::string& n : box->notes)
            m.notes.push_back(std::move(n));
        return m;
    }
    if (auto ok = checkCancel(ctx); !ok)
        return tl::unexpected(ok.error());
    Result<Geo> g = describe(e);
    if (!g)
        return tl::unexpected(g.error());
    Measurement m;
    m.kind = Kind::Element;
    m.title = std::string(subKindName(e.sub.kind));
    switch (e.sub.kind) {
    case kernel::SubKind::Vertex: {
        const Vec3 p = g->origin;
        m.values.push_back(vec("position", "Position", Quantity::Point, p, true));
        m.values.push_back(length("x", "X", p.x));
        m.values.push_back(length("y", "Y", p.y));
        m.values.push_back(length("z", "Z", p.z));
        m.witness.points.push_back(p);
        addLabel(m.witness, p, "position");
        break;
    }
    case kernel::SubKind::Edge: {
        const kernel::CurveInfo& c = *g->curve;
        m.values.push_back(text("curve", "Curve", std::string(curveName(c.type))));
        const bool round = g->type == GeoType::Circle;
        m.values.push_back(length("length", "Length", c.length, !round));
        if (round) {
            addRoundValues(m, *g, c.closed);
        } else {
            m.values.push_back(vec("start", "Start", Quantity::Point, c.start));
            m.values.push_back(vec("end", "End", Quantity::Point, c.end));
            if (g->type == GeoType::Line)
                m.values.push_back(vec("direction", "Direction", Quantity::Direction, g->dir));
            if (c.type == kernel::CurveType::Ellipse) {
                m.values.push_back(length("majorRadius", "Major radius", c.radius));
                m.notes.push_back("An ellipse's minor radius is not available yet.");
            }
            m.witness.points.push_back(c.start);
            m.witness.points.push_back(c.end);
            addLabel(m.witness, g->type == GeoType::Line ? mid(c.start, c.end) : c.start, "length");
        }
        break;
    }
    case kernel::SubKind::Face: {
        const kernel::SurfaceInfo& s = *g->surface;
        m.values.push_back(text("surface", "Surface", std::string(surfaceName(s.type))));
        const bool round = s.type == kernel::SurfaceType::Cylinder || s.type == kernel::SurfaceType::Cone ||
                           s.type == kernel::SurfaceType::Sphere || s.type == kernel::SurfaceType::Torus;
        m.values.push_back(scalar("area", "Area", Quantity::Area, s.area, !round));
        if (round) {
            addRoundValues(m, *g, true);
        } else {
            if (s.type == kernel::SurfaceType::Plane)
                m.values.push_back(vec("normal", "Normal (outward)", Quantity::Direction, g->dir));
            m.witness.points.push_back(s.centroid);
            addLabel(m.witness, s.centroid, "area");
        }
        m.values.push_back(vec("centroid", "Centroid", Quantity::Point, s.centroid));
        break;
    }
    case kernel::SubKind::Solid:
        break;
    }
    return m;
}

// ---------- selection ----------
Result<Measurement> selection(std::span<const Element> elements, double density, JobContext& ctx)
{
    if (elements.empty())
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Select one or two elements to measure.", {}});
    if (elements.size() == 1)
        return element(elements.front(), density, ctx);
    const Element& a = elements[elements.size() - 2];
    const Element& b = elements[elements.size() - 1];
    Result<Measurement> d = distance(a, b, ctx);
    if (!d)
        return d;
    Measurement m = std::move(*d);
    if (elements.size() > 2) {
        m.notes.insert(m.notes.begin(),
                       std::to_string(elements.size()) + " elements are selected: this measures the last two.");
    }
    Result<Geo> ga = describe(a);
    Result<Geo> gb = describe(b);
    if (ga && gb && hasDirection(*ga) && hasDirection(*gb)) {
        Measurement an;
        if (angleBetween(an, *ga, *gb, sharedEdge(a, b))) {
            // Parallel: an angle of 0 says nothing the note does not.
            for (std::string& n : an.notes)
                m.notes.push_back(std::move(n));
            return m;
        }
        m.kind = Kind::Pair;
        m.title = "Distance and angle";
        // The distance stays the headline; the angle values follow, with their own label and arc.
        for (Value& v : an.values) {
            v.primary = false;
            m.values.push_back(std::move(v));
        }
        for (Segment& s : an.witness.lines)
            m.witness.lines.push_back(s);
        for (Vec3& p : an.witness.points)
            m.witness.points.push_back(p);
        for (Label& l : an.witness.labels)
            m.witness.labels.push_back(std::move(l));
        for (std::string& n : an.notes) {
            if (std::find(m.notes.begin(), m.notes.end(), n) == m.notes.end())
                m.notes.push_back(std::move(n));
        }
    }
    return m;
}

// ---------- display ----------
std::string formatValue(const Value& v, UnitFormat f)
{
    const bool inch = f.system == UnitSystem::Inch;
    switch (v.quantity) {
    case Quantity::Length:
        return formatLength(v.number, f);
    case Quantity::Angle:
        return fixed(v.number * 180.0 / kPi, 3) + "\u00b0";
    case Quantity::Area:
        return inch ? fixed(v.number / (kMmPerInch * kMmPerInch), f.decimals) + " in\u00b2"
                    : fixed(v.number, f.decimals) + " mm\u00b2";
    case Quantity::Volume:
        return inch ? fixed(v.number / (kMmPerInch * kMmPerInch * kMmPerInch), f.decimals) + " in\u00b3"
                    : fixed(v.number, f.decimals) + " mm\u00b3";
    case Quantity::Mass:
        if (inch)
            return fixed(v.number / kGramsPerPound, f.decimals) + " lb";
        return std::abs(v.number) >= 1000 ? fixed(v.number / 1000, f.decimals) + " kg"
                                          : fixed(v.number, f.decimals) + " g";
    case Quantity::Density:
        return inch ? fixed(v.number * kLbPerIn3PerGPerCm3, std::max(f.decimals, 4)) + " lb/in\u00b3"
                    : fixed(v.number, std::max(f.decimals, 2)) + " g/cm\u00b3";
    case Quantity::Point:
        return triple(v.vector, f, ", ");
    case Quantity::Size:
        return triple(v.vector, f, " \u00d7 ");
    case Quantity::Direction:
        return fixed(v.vector.x, 4) + ", " + fixed(v.vector.y, 4) + ", " + fixed(v.vector.z, 4);
    case Quantity::Count:
        return fixed(v.number, 0);
    case Quantity::Text:
        break;
    }
    return v.text;
}

// ---------- JSON ----------
std::string_view kindName(Kind k)
{
    switch (k) {
    case Kind::Element:
        return "element";
    case Kind::Distance:
        return "distance";
    case Kind::Angle:
        return "angle";
    case Kind::Radius:
        return "radius";
    case Kind::MassProperties:
        return "massProperties";
    case Kind::BoundingBox:
        return "boundingBox";
    case Kind::Pair:
        break;
    }
    return "pair";
}

std::string_view quantityName(Quantity q)
{
    switch (q) {
    case Quantity::Length:
        return "length";
    case Quantity::Angle:
        return "angle";
    case Quantity::Area:
        return "area";
    case Quantity::Volume:
        return "volume";
    case Quantity::Mass:
        return "mass";
    case Quantity::Density:
        return "density";
    case Quantity::Point:
        return "point";
    case Quantity::Size:
        return "size";
    case Quantity::Direction:
        return "direction";
    case Quantity::Count:
        return "count";
    case Quantity::Text:
        break;
    }
    return "text";
}

std::string toJson(const Measurement& m)
{
    using json::Value;
    Value values = Value::array();
    Value measures = Value::object();
    for (const measure::Value& v : m.values) {
        values.push(Value::object({{"key", Value::string(v.key)},
                                   {"label", Value::string(v.label)},
                                   {"quantity", Value::string(std::string(quantityName(v.quantity)))},
                                   {"primary", Value::boolean(v.primary)},
                                   {"value", valueOf(v)}}));
        if (measures.find(v.key) == nullptr)
            measures.set(v.key, valueOf(v));
    }
    Value points = Value::array();
    for (const Vec3& p : m.witness.points)
        points.push(point(p));
    Value lines = Value::array();
    for (const Segment& s : m.witness.lines) {
        lines.push(
            Value::object({{"a", point(s.a)}, {"b", point(s.b)}, {"construction", Value::boolean(s.construction)}}));
    }
    Value labels = Value::array();
    for (const Label& l : m.witness.labels)
        labels.push(Value::object({{"at", point(l.at)}, {"key", Value::string(l.key)}}));
    Value notes = Value::array();
    for (const std::string& n : m.notes)
        notes.push(Value::string(n));
    Value out = Value::object(
        {{"kind", Value::string(std::string(kindName(m.kind)))},
         {"title", Value::string(m.title)},
         {"values", std::move(values)},
         {"measures", std::move(measures)},
         {"witness",
          Value::object({{"points", std::move(points)}, {"lines", std::move(lines)}, {"labels", std::move(labels)}})},
         {"notes", std::move(notes)}});
    return json::canonical(out);
}

} // namespace ray::api::measure
