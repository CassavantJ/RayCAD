// Resolver stages 2 to 4 (task T-081a): following names through the current build. See propagate.h.
#include "propagate.h"
#include "territory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace ray::naming::detail {

namespace {

using kernel::SubId;
using kernel::SubKind;

// Split chains deeper than this are not followed (a build would need that many splits of one element).
constexpr int kMaxSplitDepth = 64;
// Geometric slack of the containment check, mm.
constexpr double kContainTolerance = 1e-3;

Resolved make(ResolveState state, std::vector<SubId> elements, std::string reason = {})
{
    std::sort(elements.begin(), elements.end());
    elements.erase(std::unique(elements.begin(), elements.end()), elements.end());
    return Resolved{state, std::move(elements), std::move(reason)};
}

Resolved broken(std::string reason)
{
    return Resolved{ResolveState::Broken, {}, std::move(reason)};
}

bool isAmbiguous(const ElementMap& map, const TopoRef& ref, std::size_t i)
{
    if (i < ref.ambiguous.size() && ref.ambiguous[i])
        return true;
    const TopoName* record = map.record(ref.names[i]);
    return record != nullptr && needsHint(*record);
}

// Records in `map` of the pieces `name` was split into in this build.
std::vector<const TopoName*> splitChildren(const ElementMap& map, const Hash256& name)
{
    std::vector<const TopoName*> out;
    for (const TopoName& record : map.impl->records)
        if (record.role == kSplitRole && std::binary_search(record.parents.begin(), record.parents.end(), name))
            out.push_back(&record);
    return out;
}

// Where `name` went: its element, or the elements of the pieces it was split into (recursively).
struct Walk {
    std::set<SubId> elements;
    bool split = false;      // reached through at least one split record
    bool shared = false;     // a name on the way is held by several elements that cannot be told apart
    bool uncertain = false;  // a piece came from an element that also held names the reference does not cover, so
                             // it may hold none of what was picked
    bool incomplete = false; // fewer pieces than were issued: some were split again or removed, and cannot be found
};

// `covered`: the names the reference stands for (its names and the names its pieces were split from), sorted.
void walk(const ElementMap& map, const Hash256& name, const std::vector<Hash256>& covered, int depth, Walk& out)
{
    if (const auto sub = map.find(name)) {
        out.elements.insert(*sub);
        return;
    }
    if (map.record(name) != nullptr) {
        out.shared = true;
        return;
    }
    if (depth >= kMaxSplitDepth)
        return;
    const std::vector<const TopoName*> children = splitChildren(map, name);
    for (const TopoName* child : children) {
        out.split = true;
        // The pieces of one split are one family, and each records the family's size when it was issued.
        if (child->family != children.front()->family || child->siblingCount != children.size())
            out.incomplete = true;
        for (const Hash256& parent : child->parents)
            if (std::find(covered.begin(), covered.end(), parent) == covered.end())
                out.uncertain = true;
        walk(map, child->id, covered, depth + 1, out);
    }
}

std::vector<Hash256> coveredBy(const TopoRef& ref)
{
    std::vector<Hash256> out(ref.names.begin(), ref.names.end());
    out.insert(out.end(), ref.splitParents.begin(), ref.splitParents.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The name that a member of this family would have if it were issued alone.
Hash256 aloneName(const TopoName& member)
{
    TopoName alone{};
    alone.kind = member.kind;
    alone.origin = member.origin;
    alone.role = member.role;
    alone.parents = member.parents;
    alone.ordinal = 0;
    alone.siblingCount = 1;
    return nameId(alone);
}

double q(std::int64_t v)
{
    return static_cast<double>(v) / kLengthQuantaPerMm;
}

double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

bool insideBox(const Vec3& p, const Box3& box, double tol)
{
    return p.x >= box.min.x - tol && p.x <= box.max.x + tol && p.y >= box.min.y - tol && p.y <= box.max.y + tol &&
           p.z >= box.min.z - tol && p.z <= box.max.z + tol;
}

// Does point p (on the face's plane) lie inside planar face `face`? Even-odd count of the crossings of a ray in the
// plane with the face's boundary edges, lines and circular arcs (an arc runs counter-clockwise about its axis from
// its start for length / radius; arcs are walked in short chords). nullopt when an edge is of another kind or its
// shape cannot be reconstructed: then the question cannot be answered.
std::optional<bool> insidePlanarFace(const kernel::Shape& shape, SubId face, const Vec3& p,
                                     const kernel::SurfaceInfo& info)
{
    const double n0 = norm(info.axis);
    if (!(n0 > 0.0))
        return std::nullopt;
    const Vec3 n{info.axis.x / n0, info.axis.y / n0, info.axis.z / n0};
    // A frame (e1, e2) of the plane.
    const Vec3 t = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 e1{t.y * n.z - t.z * n.y, t.z * n.x - t.x * n.z, t.x * n.y - t.y * n.x};
    const double l1 = norm(e1);
    e1 = Vec3{e1.x / l1, e1.y / l1, e1.z / l1};
    const Vec3 e2{n.y * e1.z - n.z * e1.y, n.z * e1.x - n.x * e1.z, n.x * e1.y - n.y * e1.x};
    auto uv = [&](const Vec3& q) {
        const Vec3 d = sub(q, p);
        return std::array<double, 2>{dot(d, e1), dot(d, e2)};
    };
    // Crossings of the ray from p along +e1 (u > 0, v = 0) with segment a-b (half-open in v).
    int crossings = 0;
    auto segment = [&](const std::array<double, 2>& a, const std::array<double, 2>& b) {
        if ((a[1] > 0) == (b[1] > 0))
            return;
        const double u = a[0] + (b[0] - a[0]) * (0 - a[1]) / (b[1] - a[1]);
        if (u > 0)
            ++crossings;
    };
    for (const SubId& edge : kernel::adjacent(shape, face, SubKind::Edge)) {
        const auto c = kernel::curveInfo(shape, edge);
        if (!c)
            return std::nullopt;
        if (c->type == kernel::CurveType::Line) {
            segment(uv(c->start), uv(c->end));
            continue;
        }
        if (c->type != kernel::CurveType::Circle) {
            // An ellipse or B-spline is taken as its chord (the kernel gives its ends only): good enough to tell a
            // point well inside the face from one outside it, which is what is asked. A closed one has no chord.
            if (c->closed || norm(sub(c->start, c->end)) <= 1e-9)
                return std::nullopt;
            segment(uv(c->start), uv(c->end));
            continue;
        }
        if (!(c->radius > 0.0))
            return std::nullopt;
        const double a0 = norm(c->axis);
        if (!(a0 > 0.0))
            return std::nullopt;
        const Vec3 ax{c->axis.x / a0, c->axis.y / a0, c->axis.z / a0};
        const Vec3 r0 = sub(c->start, c->center);
        const Vec3 r1{ax.y * r0.z - ax.z * r0.y, ax.z * r0.x - ax.x * r0.z, ax.x * r0.y - ax.y * r0.x};
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        double sweep = c->closed ? kTwoPi : c->length / c->radius;
        auto at = [&](double a) {
            const double ca = dmath::cos(a), sa = dmath::sin(a);
            return Vec3{c->center.x + r0.x * ca + r1.x * sa, c->center.y + r0.y * ca + r1.y * sa,
                        c->center.z + r0.z * ca + r1.z * sa};
        };
        // The arc must end where the edge ends, turning one way or the other about the axis; else its shape is not
        // known.
        const double endTolerance = 1e-4 * std::max(1.0, c->radius);
        if (!c->closed && norm(sub(at(sweep), c->end)) > endTolerance) {
            sweep = -sweep;
            if (norm(sub(at(sweep), c->end)) > endTolerance)
                return std::nullopt;
        }
        const int steps = std::max(8, static_cast<int>(std::abs(sweep) / 0.05));
        std::array<double, 2> prev = uv(c->start);
        for (int i = 1; i <= steps; ++i) {
            const std::array<double, 2> next = uv(i == steps && !c->closed ? c->end : at(sweep * i / steps));
            segment(prev, next);
            prev = next;
        }
    }
    return crossings % 2 == 1;
}

// Can element `c` contain the element the reference was picked on? Used before a family or split-parent rule
// answers Merged: the rule says the pieces re-joined, but the same names also appear when a piece vanished and
// the rest of the element is left whole. This keeps Merged to elements of the same type on the same carrier, at
// least as large, with the picked element's position on them. Only planes, cylinders, lines and full circles
// are checked; anything else is not given Merged.
bool canContain(const ElementMap& map, const kernel::Shape& shape, SubId c, const Fingerprint& hint,
                const Territory& territory)
{
    const Fingerprint fc = map.fingerprintOf(c);
    if (fc.surface != hint.surface || std::llabs(fc.qRadius - hint.qRadius) > 10)
        return false;
    if (std::llabs(fc.qAxisX - hint.qAxisX) > 10 || std::llabs(fc.qAxisY - hint.qAxisY) > 10 ||
        std::llabs(fc.qAxisZ - hint.qAxisZ) > 10)
        return false;
    const double slack = std::max(2.0, 1e-7 * std::abs(static_cast<double>(hint.qArea)));
    if (static_cast<double>(fc.qArea) < static_cast<double>(hint.qArea) - slack)
        return false;
    const Vec3 p{q(hint.qCx), q(hint.qCy), q(hint.qCz)};
    if (c.kind == SubKind::Face) {
        const auto info = kernel::surfaceInfo(shape, c);
        if (!info || !insideBox(p, info->bounds, kContainTolerance))
            return false;
        const double axisLength = norm(info->axis);
        if (!(axisLength > 0.0))
            return false;
        const Vec3 axis{info->axis.x / axisLength, info->axis.y / axisLength, info->axis.z / axisLength};
        const Vec3 d = sub(p, info->origin);
        if (info->type == kernel::SurfaceType::Plane) {
            // Inside its outline, not merely inside its box: a face whole again around a hole that now sits where the
            // picked piece was does not contain the piece (T-081a random 10:34).
            if (std::abs(dot(d, axis)) > kContainTolerance)
                return false;
            const auto inside = insidePlanarFace(shape, c, p, *info);
            return inside.has_value() && *inside;
        }
        if (info->type == kernel::SurfaceType::Cylinder) {
            const double along = dot(d, axis);
            const Vec3 radial{d.x - along * axis.x, d.y - along * axis.y, d.z - along * axis.z};
            const double rho = norm(radial);
            if (rho > info->radius + kContainTolerance)
                return false;
            // On the face, not merely inside its box (T-081b sorted block 5:15: a sliver of a boss's side beside a cut,
            // the cut moved over it, the rest of the side whole): the picked centroid moved out onto the cylinder lies
            // on it, or a point of the picked territory does (a piece whose centroid falls outside its own outline).
            // A centroid on the axis (a whole ring) tells no place; the box test stands.
            if (rho <= kContainTolerance)
                return true;
            const double k = info->radius / rho;
            const Vec3 onCylinder{info->origin.x + along * axis.x + k * radial.x,
                                  info->origin.y + along * axis.y + k * radial.y,
                                  info->origin.z + along * axis.z + k * radial.z};
            const Probe probe(shape, c);
            return probe.on(onCylinder, kContainTolerance) || (territory.known() && probe.count(territory) > 0);
        }
        return false;
    }
    if (c.kind == SubKind::Edge) {
        const auto info = kernel::curveInfo(shape, c);
        if (!info)
            return false;
        if (info->type == kernel::CurveType::Line) {
            const Vec3 d = sub(info->end, info->start);
            const double length = norm(d);
            if (!(length > 0.0))
                return false;
            const Vec3 u{d.x / length, d.y / length, d.z / length};
            const Vec3 w = sub(p, info->start);
            const double t = dot(w, u);
            const Vec3 off{w.x - t * u.x, w.y - t * u.y, w.z - t * u.z};
            return norm(off) <= kContainTolerance && t >= -kContainTolerance && t <= length + kContainTolerance;
        }
        if (info->type == kernel::CurveType::Circle && info->closed) {
            const double axisLength = norm(info->axis);
            if (!(axisLength > 0.0))
                return false;
            const Vec3 axis{info->axis.x / axisLength, info->axis.y / axisLength, info->axis.z / axisLength};
            const Vec3 d = sub(p, info->center);
            const double along = dot(d, axis);
            const Vec3 radial{d.x - along * axis.x, d.y - along * axis.y, d.z - along * axis.z};
            return std::abs(along) <= kContainTolerance && norm(radial) <= info->radius + kContainTolerance;
        }
        return false;
    }
    return false; // a vertex never contains another
}

// Does element `c` lie where the picked element was: same surface (or curve) type, radius and axis, and on the
// picked element's carrier (the same plane, the same line overlapping the picked segment, the same circle)? Used
// for the pieces of a Split (see piecesLieAsPicked): a separate curve on the same two faces (the exit loop of a
// lengthened cross hole, the second circle where a plane meets a torus, the second breakout line of a hole) or a
// face at another height after an un-merge is not a piece of what was picked. Planes, cylinders, cones, tori,
// spheres, lines and circles are checked; anything else fails.
bool onHintCarrier(const ElementMap& map, const kernel::Shape& shape, SubId c, const Fingerprint& hint)
{
    const Fingerprint fc = map.fingerprintOf(c);
    if (fc.surface != hint.surface || std::llabs(fc.qRadius - hint.qRadius) > 10)
        return false;
    if (std::llabs(fc.qAxisX - hint.qAxisX) > 10 || std::llabs(fc.qAxisY - hint.qAxisY) > 10 ||
        std::llabs(fc.qAxisZ - hint.qAxisZ) > 10)
        return false;
    const Vec3 p{q(hint.qCx), q(hint.qCy), q(hint.qCz)};
    if (c.kind == SubKind::Face) {
        const auto info = kernel::surfaceInfo(shape, c);
        if (!info)
            return false;
        const double axisLength = norm(info->axis);
        if (!(axisLength > 0.0))
            return false;
        const Vec3 axis{info->axis.x / axisLength, info->axis.y / axisLength, info->axis.z / axisLength};
        const Vec3 d = sub(p, info->origin);
        const double along = dot(d, axis);
        const Vec3 radial{d.x - along * axis.x, d.y - along * axis.y, d.z - along * axis.z};
        // The piece must also lie near the picked element: the picked centroid within half the picked element's
        // size of the piece's box (the lower part of a cross hole lengthened through a bore is on the same
        // cylinder, far from what was picked).
        const double half = 0.5 * std::sqrt(std::max(0.0, q(hint.qArea))); // qArea holds the area in mm^2
        const bool near = insideBox(p, info->bounds, kContainTolerance + half);
        switch (info->type) {
        case kernel::SurfaceType::Plane:
            return near && std::abs(along) <= kContainTolerance;
        case kernel::SurfaceType::Cylinder:
            return near && norm(radial) <= info->radius + kContainTolerance;
        case kernel::SurfaceType::Cone:
        case kernel::SurfaceType::Torus:
        case kernel::SurfaceType::Sphere:
            return near; // no closed-form test from a fingerprint
        default:
            return false;
        }
    }
    if (c.kind == SubKind::Edge) {
        const auto info = kernel::curveInfo(shape, c);
        if (!info)
            return false;
        if (info->type == kernel::CurveType::Line) {
            const Vec3 d = sub(info->end, info->start);
            const double length = norm(d);
            if (!(length > 0.0))
                return false;
            const Vec3 u{d.x / length, d.y / length, d.z / length};
            const Vec3 w = sub(p, info->start);
            const double t = dot(w, u);
            const Vec3 off{w.x - t * u.x, w.y - t * u.y, w.z - t * u.z};
            const double half = 0.5 * q(hint.qArea); // the picked segment runs from t - half to t + half
            // The piece must share a stretch of the picked segment, not merely touch its end: a separate segment
            // on the same line that starts where the picked one ended is not a piece of it (T-081a random 6:21).
            const double shared = std::min(length, t + half) - std::max(0.0, t - half);
            return norm(off) <= kContainTolerance && shared > std::max(kContainTolerance, 0.01 * 2.0 * half);
        }
        if (info->type == kernel::CurveType::Circle) {
            const double axisLength = norm(info->axis);
            if (!(axisLength > 0.0))
                return false;
            const Vec3 axis{info->axis.x / axisLength, info->axis.y / axisLength, info->axis.z / axisLength};
            const Vec3 d = sub(p, info->center);
            const double along = dot(d, axis);
            if (std::abs(along) > kContainTolerance)
                return false;
            // A whole circle was fingerprinted at its centre; an arc at the midpoint of its chord.
            constexpr double kTwoPi = 6.283185307179586476925286766559;
            const bool wasWhole = std::abs(q(hint.qArea) - kTwoPi * info->radius) <= 1e-3 * std::max(1.0, info->radius);
            if (wasWhole)
                return norm(d) <= kContainTolerance;
            // An arc: no point of the picked arc is further than half its length from its chord's midpoint, so a
            // piece of it has an end that near (the other arc where a plane meets a torus on the far side has not).
            const double reach = 0.5 * q(hint.qArea) + kContainTolerance;
            if (!(norm(d) <= info->radius + kContainTolerance &&
                  std::min(norm(sub(info->start, p)), norm(sub(info->end, p))) <= reach))
                return false;
            // And it must share a stretch of the picked arc, not merely touch its end (T-081a random 6:21). The picked
            // arc runs `half` radians either side of the direction of its chord's midpoint; the piece is walked in
            // short steps from its start, turning whichever way about the axis ends at its end.
            const Vec3 radial{d.x - along * axis.x, d.y - along * axis.y, d.z - along * axis.z};
            const double rl = norm(radial);
            if (!(rl > kContainTolerance) || !(info->radius > 0.0))
                return false;
            const double half = 0.5 * q(hint.qArea) / info->radius;
            // The chord's midpoint lies towards the arc's middle for an arc shorter than half the circle, away from
            // it for a longer one.
            constexpr double kHalfTurn = 3.14159265358979323846;
            const double towards = half > 0.5 * kHalfTurn ? -1.0 : 1.0;
            const Vec3 m{towards * radial.x / rl, towards * radial.y / rl, towards * radial.z / rl};
            const Vec3 r0 = sub(info->start, info->center);
            const Vec3 r1{axis.y * r0.z - axis.z * r0.y, axis.z * r0.x - axis.x * r0.z, axis.x * r0.y - axis.y * r0.x};
            auto at = [&](double a) {
                const double ca = dmath::cos(a), sa = dmath::sin(a);
                return Vec3{r0.x * ca + r1.x * sa, r0.y * ca + r1.y * sa, r0.z * ca + r1.z * sa};
            };
            double sweep = info->length / info->radius;
            const Vec3 endRadial = sub(info->end, info->center);
            if (norm(sub(at(sweep), endRadial)) > 1e-4 * std::max(1.0, info->radius)) {
                sweep = -sweep;
                if (norm(sub(at(sweep), endRadial)) > 1e-4 * std::max(1.0, info->radius))
                    return false;
            }
            constexpr double kPi = 3.14159265358979323846;
            const double margin = kContainTolerance / info->radius;
            if (half - margin >= kPi)
                return true;
            const double cosLimit = dmath::cos(std::max(0.0, half - margin));
            constexpr int kSteps = 64;
            int inside = 0;
            for (int i = 0; i < kSteps; ++i) {
                const Vec3 v = at(sweep * (i + 0.5) / kSteps);
                const double vl = norm(v);
                if (vl > 0.0 && dot(v, m) / vl > cosLimit)
                    ++inside;
            }
            const double shared = info->length * inside / kSteps;
            return shared > std::max(kContainTolerance, 0.01 * q(hint.qArea));
        }
        return false;
    }
    return false;
}

// Do two elements lie on one carrier: the same plane, the same cylinder (axis line and radius), the same line, the
// same circle? Anything else is not compared and fails.
bool sameCarrier(const kernel::Shape& shape, SubId a, SubId b)
{
    auto unitOf = [](const Vec3& v, Vec3& out) {
        const double n = norm(v);
        if (!(n > 0.0))
            return false;
        out = Vec3{v.x / n, v.y / n, v.z / n};
        return true;
    };
    auto parallel = [](const Vec3& u, const Vec3& v) {
        return std::abs(std::abs(dot(u, v)) - 1.0) <= 1e-9;
    };
    // Distance of p from the line through o along unit u.
    auto offLine = [](const Vec3& p, const Vec3& o, const Vec3& u) {
        const Vec3 w = sub(p, o);
        const double t = dot(w, u);
        return norm(Vec3{w.x - t * u.x, w.y - t * u.y, w.z - t * u.z});
    };
    if (a.kind == SubKind::Face && b.kind == SubKind::Face) {
        const auto ia = kernel::surfaceInfo(shape, a);
        const auto ib = kernel::surfaceInfo(shape, b);
        Vec3 ua{}, ub{};
        if (!ia || !ib || ia->type != ib->type || !unitOf(ia->axis, ua) || !unitOf(ib->axis, ub) || !parallel(ua, ub))
            return false;
        if (ia->type == kernel::SurfaceType::Plane)
            return std::abs(dot(sub(ib->origin, ia->origin), ua)) <= kContainTolerance;
        if (ia->type == kernel::SurfaceType::Cylinder)
            return std::abs(ia->radius - ib->radius) <= kContainTolerance &&
                   offLine(ib->origin, ia->origin, ua) <= kContainTolerance;
        return false;
    }
    if (a.kind == SubKind::Edge && b.kind == SubKind::Edge) {
        const auto ia = kernel::curveInfo(shape, a);
        const auto ib = kernel::curveInfo(shape, b);
        if (!ia || !ib || ia->type != ib->type)
            return false;
        Vec3 ua{}, ub{};
        if (ia->type == kernel::CurveType::Line)
            return unitOf(sub(ia->end, ia->start), ua) && unitOf(sub(ib->end, ib->start), ub) && parallel(ua, ub) &&
                   offLine(ib->start, ia->start, ua) <= kContainTolerance;
        if (ia->type == kernel::CurveType::Circle)
            return unitOf(ia->axis, ua) && unitOf(ib->axis, ub) && parallel(ua, ub) &&
                   std::abs(ia->radius - ib->radius) <= kContainTolerance &&
                   norm(sub(ia->center, ib->center)) <= kContainTolerance;
        return false;
    }
    return false;
}

// May `elements` be answered as the pieces of the picked element (a Split)? Together they must be no larger than the
// picked element was (a split adds no territory). Then every piece must lie on the picked element's carrier
// (onHintCarrier) or touch, through other pieces on the same carrier, one that does (a face split along an edge, an
// edge split at a vertex). Failing that, the pieces may be the picked element moved and then cut apart (a hole moved
// onto a groove breaks its rim into two arcs): they must all lie on one carrier. A separate element that only shares
// the picked element's faces (the exit loop of a lengthened cross hole, the other breakout line of a hole, a piece
// of the same face that a smaller cut brings back) fails, and the caller answers Broken.
bool piecesLieAsPicked(const ElementMap& map, const kernel::Shape& shape, const std::vector<SubId>& elements,
                       const Fingerprint& hint)
{
    if (elements.empty())
        return false;
    const std::size_t n = elements.size();
    // Pieces of what was picked are together no larger than it was: a piece of the same face that a later edit
    // brought back elsewhere (territory that was not part of the picked element) makes them larger.
    if (elements.front().kind != SubKind::Vertex) {
        double total = 0;
        for (const SubId& s : elements)
            total += q(map.fingerprintOf(s).qArea);
        if (total > q(hint.qArea) * (1.0 + 1e-5) + kContainTolerance)
            return false;
    }
    std::vector<char> ok(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        ok[i] = onHintCarrier(map, shape, elements[i], hint) ? 1 : 0;
    const SubKind via = elements.front().kind == SubKind::Face ? SubKind::Edge : SubKind::Vertex;
    std::vector<std::vector<SubId>> boundary(n);
    if (elements.front().kind != SubKind::Vertex)
        for (std::size_t i = 0; i < n; ++i)
            boundary[i] = kernel::adjacent(shape, elements[i], via);
    auto touch = [&](std::size_t i, std::size_t j) {
        for (const SubId& s : boundary[i])
            if (std::binary_search(boundary[j].begin(), boundary[j].end(), s))
                return true;
        return false;
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; ok[i] == 0 && j < n; ++j)
                if (ok[j] != 0 && touch(i, j) && sameCarrier(shape, elements[i], elements[j])) {
                    ok[i] = 1;
                    grew = true;
                }
    }
    if (std::all_of(ok.begin(), ok.end(), [](char c) { return c != 0; }))
        return true;
    if (n < 2 || elements.front().kind == SubKind::Vertex)
        return false;
    for (std::size_t i = 1; i < n; ++i)
        if (!sameCarrier(shape, elements.front(), elements[i]))
            return false;
    if (elements.front().kind == SubKind::Edge) {
        // The pieces of one moved segment or arc lie within its length of each other: a piece further along the
        // same line or circle is territory the picked element never had.
        std::vector<Vec3> ends;
        double radius = 0;
        bool line = true;
        for (const SubId& s : elements) {
            const auto info = kernel::curveInfo(shape, s);
            if (!info)
                return false;
            ends.push_back(info->start);
            ends.push_back(info->end);
            line = info->type == kernel::CurveType::Line;
            radius = info->radius;
        }
        constexpr double kPi = 3.14159265358979323846;
        const double length = q(hint.qArea);
        // A chord of the picked arc is at most this long (half a circle or more: the diameter).
        double reach = length;
        if (!line)
            reach =
                radius > 0 && length / radius < kPi ? 2.0 * radius * dmath::sin(0.5 * length / radius) : 2.0 * radius;
        for (std::size_t i = 0; i < ends.size(); ++i)
            for (std::size_t j = i + 1; j < ends.size(); ++j)
                if (norm(sub(ends[i], ends[j])) > reach + kContainTolerance)
                    return false;
        // On a circle, within a chord of each other is not enough (on a small circle every point is within its
        // diameter of every other): the pieces must fit in an arc no longer than the picked one, about their mean
        // direction (T-081a holdout 18:5: the floor rim of a moved hole in two arcs, one where the picked arc went,
        // the other on the far side of the rim).
        // (A whole circle was picked when its length is the circumference, as onHintCarrier tells it.)
        if (!line && radius > 0.0 && 2.0 * kPi * radius - length > 1e-3 * std::max(1.0, radius)) {
            const auto first = kernel::curveInfo(shape, elements.front());
            const double al = first ? norm(first->axis) : 0.0;
            if (!first || !(al > 0.0))
                return false;
            const Vec3 axis{first->axis.x / al, first->axis.y / al, first->axis.z / al};
            const Vec3 centre = first->center;
            std::vector<Vec3> dirs;
            for (const SubId& s : elements) {
                const auto info = kernel::curveInfo(shape, s);
                if (!info || !(info->radius > 0.0))
                    return false;
                const Vec3 r0 = sub(info->start, centre);
                const Vec3 r1{axis.y * r0.z - axis.z * r0.y, axis.z * r0.x - axis.x * r0.z,
                              axis.x * r0.y - axis.y * r0.x};
                auto at = [&](double a) {
                    const double ca = dmath::cos(a), sa = dmath::sin(a);
                    return Vec3{r0.x * ca + r1.x * sa, r0.y * ca + r1.y * sa, r0.z * ca + r1.z * sa};
                };
                double sweep = info->closed ? 2.0 * kPi : info->length / info->radius;
                if (norm(sub(at(sweep), sub(info->end, centre))) > 1e-4 * std::max(1.0, info->radius)) {
                    sweep = -sweep;
                    if (norm(sub(at(sweep), sub(info->end, centre))) > 1e-4 * std::max(1.0, info->radius))
                        return false;
                }
                for (int k = 0; k <= 16; ++k) {
                    const Vec3 v = at(sweep * k / 16.0);
                    const double vl = norm(v);
                    if (!(vl > 0.0))
                        return false;
                    dirs.push_back(Vec3{v.x / vl, v.y / vl, v.z / vl});
                }
            }
            Vec3 mean{0, 0, 0};
            for (const Vec3& v : dirs)
                mean = Vec3{mean.x + v.x, mean.y + v.y, mean.z + v.z};
            const double ml = norm(mean);
            if (!(ml > 1e-9))
                return false;
            mean = Vec3{mean.x / ml, mean.y / ml, mean.z / ml};
            const double half = 0.5 * length / radius + kContainTolerance / radius;
            if (half < kPi) {
                const double cosLimit = dmath::cos(half);
                for (const Vec3& v : dirs)
                    if (dot(v, mean) < cosLimit)
                        return false;
            }
        }
    }
    return true;
}

// The faces around the picked element at pick time, as they are now: the elements its neighbour names lead to, and
// for a neighbour that was split, all its pieces.
std::set<SubId> facesAroundPick(const ElementMap& map, const TopoRef& ref)
{
    std::set<SubId> out;
    for (const Hash256& neighbour : ref.neighbourNames) {
        Walk w;
        walk(map, neighbour, ref.neighbourNames, 0, w);
        out.insert(w.elements.begin(), w.elements.end());
    }
    return out;
}

// Do the face pieces of a Split stay on the picked element's side of every feature that bounded it? A piece may meet
// faces that were around the picked element, faces of the other pieces, and faces of features that did not bound it
// (the cut that split it). A piece that meets a face, not around the picked element, of a feature one of whose faces
// WAS around it lies beyond a boundary the picked element had: the part of a chamfer beyond a pocket that bounded it
// and has moved into its middle (T-081a random 11:12), the part of a boss's start face that now shows above the part
// where the picked part showed below it (3:42). Such a piece is territory the picked element never had; the module
// cannot tell how much of it, so the answer is Broken. Edges are not checked (their pieces lie on the same two faces;
// the reach check of piecesLieAsPicked bounds them).
bool facePiecesStayInside(const ElementMap& map, const kernel::Shape& shape, const std::vector<SubId>& pieces,
                          const TopoRef& ref)
{
    if (pieces.empty() || pieces.front().kind != SubKind::Face)
        return true;
    const std::set<SubId> around = facesAroundPick(map, ref);
    std::set<FeatureId> bounding;
    for (const Hash256& neighbour : ref.neighbourNames)
        if (const TopoName* record = map.record(neighbour))
            bounding.insert(record->origin);
    for (const SubId& piece : pieces)
        for (const SubId& face : kernel::adjacent(shape, piece, SubKind::Face)) {
            if (around.count(face) != 0 || std::binary_search(pieces.begin(), pieces.end(), face))
                continue;
            for (const Hash256& name : map.namesOf(face))
                if (const TopoName* record = map.record(name); record != nullptr && bounding.count(record->origin) != 0)
                    return false;
        }
    return true;
}

// Is `candidate` surrounded by exactly the faces the reference's neighbour names resolve to? Each neighbour name
// must lead to a face around the candidate, and every face around it must be reached by one.
bool surroundedAsPicked(const ElementMap& map, const kernel::Shape& shape, SubId candidate, const TopoRef& ref)
{
    if (ref.neighbourNames.empty())
        return false;
    const std::vector<SubId> around = kernel::adjacent(shape, candidate, SubKind::Face);
    if (around.empty())
        return false;
    std::set<SubId> covered;
    for (const Hash256& neighbour : ref.neighbourNames) {
        Walk w;
        walk(map, neighbour, ref.neighbourNames, 0, w);
        if (w.shared || w.uncertain || w.incomplete || w.elements.empty())
            return false;
        bool hit = false;
        for (const SubId& s : w.elements) {
            covered.insert(s);
            hit = hit || std::binary_search(around.begin(), around.end(), s);
        }
        if (!hit)
            return false;
    }
    for (const SubId& f : around)
        if (covered.count(f) == 0)
            return false;
    return true;
}

bool neighboursStillAround(const ElementMap& map, const kernel::Shape& shape, SubId element, const TopoRef& ref)
{
    if (element.kind == SubKind::Face || element.kind == SubKind::Solid)
        return true;
    const std::vector<SubId> around = kernel::adjacent(shape, element, SubKind::Face);
    if (ref.neighbourNames.empty())
        return around.empty(); // picked on no face (a free edge), and still on none
    for (const Hash256& neighbour : ref.neighbourNames) {
        Walk w;
        walk(map, neighbour, ref.neighbourNames, 0, w);
        if (w.shared || w.uncertain || w.incomplete || w.elements.empty())
            return false;
        // A face that is a piece of a split face stands for every piece of that face: an edge that a later split
        // carried from one piece to its sibling still lies on the same logical face.
        std::set<SubId> faces(w.elements.begin(), w.elements.end());
        for (const SubId& s : w.elements)
            for (const Hash256& name : map.namesOf(s)) {
                const TopoName* record = map.record(name);
                if (record == nullptr || record->role != kSplitRole)
                    continue;
                for (const Hash256& parent : record->parents)
                    for (const TopoName* sibling : splitChildren(map, parent))
                        if (const auto f = map.find(sibling->id))
                            faces.insert(*f);
            }
        bool hit = false;
        for (const SubId& s : faces)
            hit = hit || std::binary_search(around.begin(), around.end(), s);
        if (!hit)
            return false;
    }
    return true;
}

// For a face that a family or split-parent rule would answer Merged: every face that was around the picked piece
// and still exists must still be around the candidate (the face that split the picked piece off may be gone), and
// at least one must be. A whole face that lost the picked piece's region (it vanished, the rest is left) no longer
// meets the faces that bordered that region.
bool piecesNeighboursAround(const ElementMap& map, const kernel::Shape& shape, SubId candidate, const TopoRef& ref)
{
    const std::vector<SubId> around = kernel::adjacent(shape, candidate, SubKind::Face);
    std::size_t hits = 0;
    for (const Hash256& neighbour : ref.neighbourNames) {
        Walk w;
        walk(map, neighbour, ref.neighbourNames, 0, w);
        if (w.shared || w.uncertain || w.incomplete)
            return false;
        if (w.elements.empty())
            continue;
        bool hit = false;
        for (const SubId& s : w.elements)
            hit = hit || s == candidate || std::binary_search(around.begin(), around.end(), s); // or merged into it
        if (!hit)
            return false;
        ++hits;
    }
    return hits > 0;
}

// Stage 4: exactly one element matches the hint and is surrounded by exactly the faces the neighbour names
// resolve to.
std::optional<Resolved> repaired(const ElementMap& map, const kernel::Shape& shape, const TopoRef& ref)
{
    if (ref.neighbourNames.empty() || ref.hint == emptyFingerprint())
        return std::nullopt;
    std::vector<SubId> candidates;
    for (const ElementMap::Impl::Element& e : map.impl->elements)
        if (e.sub.kind == ref.kind && e.fingerprint.matches(ref.hint))
            candidates.push_back(e.sub);
    if (candidates.size() != 1)
        return std::nullopt;
    const SubId candidate = candidates.front();
    if (!surroundedAsPicked(map, shape, candidate, ref))
        return std::nullopt;
    // Not onto a sibling of the pick: an element of the same name family (the same feature, role and parent faces,
    // told apart only by discriminator or ordinal) that now lies where the pick was is most likely the pick's twin
    // after the feature moved, not the pick (T-081b round 3, randomised 8:18 on macOS: a hole moved by exactly its
    // own width puts its other crossing with a wall where the picked crossing was). Without a map of the move, no
    // candidate is offered: Broken.
    for (const Hash256& name : map.namesOf(candidate))
        if (const TopoName* record = map.record(name);
            record != nullptr &&
            std::find(ref.families.begin(), ref.families.end(), record->family) != ref.families.end())
            return std::nullopt;
    return make(ResolveState::Repaired, {candidate},
                "The picked element's name is gone. One element has the same shape and position and is surrounded "
                "by the same faces; confirm that it is the one you meant.");
}

std::optional<Resolved> resolveImpl(const ElementMap& map, const kernel::Shape& shape, const TopoRef& ref)
{
    if (!map.impl)
        return std::nullopt;
    const std::set<Hash256> refNames(ref.names.begin(), ref.names.end());
    const std::vector<Hash256> covered = coveredBy(ref);
    // An edge is the meeting of two faces: an edge that contains the picked one must lie on the same faces. A face
    // is its surface and may meet other faces than before.
    auto surroundedLikePick = [&](SubId c) {
        if (c.kind == SubKind::Face)
            return piecesNeighboursAround(map, shape, c, ref);
        return c.kind != SubKind::Edge || surroundedAsPicked(map, shape, c, ref);
    };

    // ---- stage 2: names followed through this build ----
    Walk all;
    std::size_t namesFound = 0;
    bool ambiguousFollowed = false;
    for (std::size_t i = 0; i < ref.names.size(); ++i) {
        Walk w;
        walk(map, ref.names[i], covered, 0, w);
        all.shared = all.shared || w.shared;
        all.uncertain = all.uncertain || w.uncertain;
        all.incomplete = all.incomplete || w.incomplete;
        if (w.elements.empty())
            continue;
        ++namesFound;
        all.split = all.split || w.split;
        all.elements.insert(w.elements.begin(), w.elements.end());
        ambiguousFollowed = ambiguousFollowed || isAmbiguous(map, ref, i);
    }
    if (all.shared)
        return broken("The picked element can no longer be told apart from an identical element.");
    if (all.uncertain)
        return broken("The picked element was joined with another one and then split; which pieces hold what was "
                      "picked is not known.");
    if (all.incomplete)
        return broken("The picked element was split, and some of its pieces were split again or removed; they "
                      "cannot all be found.");
    if (!all.elements.empty()) {
        if (ambiguousFollowed)
            return broken("The picked element could only be told apart from identical neighbours by its geometry, "
                          "and it has changed since.");
        for (const SubId& s : all.elements)
            if (s.kind != ref.kind)
                return broken("The picked element's name now belongs to an element of another kind.");
        std::vector<SubId> elements(all.elements.begin(), all.elements.end());
        // A piece of zero measure is not part of the answer (zeroMeasure; holdout5 1:7).
        if (all.split) {
            std::erase_if(elements, [&](const SubId& s) { return zeroMeasure(shape, s, ref.hint); });
            if (elements.empty())
                return broken("The picked element came apart, and none of its pieces has any area (or length).");
        }
        for (const SubId& s : elements)
            if (!neighboursStillAround(map, shape, s, ref))
                return broken("The picked element no longer lies on the faces it was picked on.");
        if (elements.size() == 1 && !all.split) {
            for (const Hash256& name : map.namesOf(elements.front()))
                if (refNames.count(name) == 0) {
                    if (!surroundedLikePick(elements.front()))
                        return broken("The picked element was joined with another one, and its neighbours are not "
                                      "the ones it had.");
                    return make(ResolveState::Merged, std::move(elements));
                }
            if (namesFound < ref.names.size())
                // Part of a merged element is left and the rest has no name any more: whether the rest vanished
                // or became something else is not known.
                return broken("Part of the picked element no longer exists.");
            return std::nullopt; // the Exact stage's business
        }
        // A face piece wholly outside the picked face's outline at pick time is not a piece (TopoRef::outline). But
        // only while the pieces kept hold most of the picked territory: when they hold less, the face may have moved
        // with its feature, and a piece outside the outline may hold the moved pick (T-081b holdout2 7:44 face 30: a
        // boss's end cap, the boss moved by its width; one piece left in a pocket, the other sticking out below the
        // part). Which reading is right cannot be told from the evidence: Broken.
        if (const std::vector<SubId> inside = piecesInsideOutline(shape, elements, ref.outline);
            inside.size() < elements.size()) {
            if (!piecesHoldMostOf(shape, inside, ref.territory))
                return broken("The picked face came apart; some pieces lie where it was and others where it was not, "
                              "and whether it moved cannot be told.");
            elements = inside;
        }
        // A piece the pick's feature's own move takes wholly past the pick (T-081b round 4, review holdout 1:39 edge
        // 83: a stretch of a moved cut's edge that lay inside a bore when it was picked) is ground the pick did not
        // have: another element's, or new ground the pick grew into where something that bounded it is gone (sorted
        // 17:32 edge 32, a deleted hole). Which, cannot be told here, and need not be (round 5, the reviewer's
        // suggestion): neither is ever part of the pick (new ground past its old end is not the pick, another's
        // ground is not its to take). Such pieces are left out and the pieces that hold the moved pick answered,
        // flagged (Split, however many are left).
        if (std::vector<SubId> kept = piecesInsideMovedPick(map, shape, elements, ref); kept.size() < elements.size()) {
            if (kept.empty())
                return broken("The picked element came apart, and no piece lies where it was; whether its feature "
                              "moved cannot be told (a reference made before feature placements were recorded).");
            elements = std::move(kept);
        }
        if (!piecesLieAsPicked(map, shape, elements, ref.hint))
            return broken("The picked element came apart, and not every piece lies where it was.");
        if (!facePiecesStayInside(map, shape, elements, ref))
            return broken("The picked element came apart, and a piece lies beyond a face that bounded it when it was "
                          "picked.");
        if (std::string why; !piecesFitEvidence(map, shape, elements, ref, why))
            return broken(why);
        return make(ResolveState::Split, std::move(elements));
    }

    // ---- stage 3: the family rule, then the split parent ----
    if (ref.families.size() == ref.names.size()) {
        for (std::size_t i = 0; i < ref.names.size(); ++i) {
            const std::span<const Hash256> members = map.family(ref.families[i]);
            if (members.empty())
                continue;
            const TopoName* first = map.record(members.front());
            if (first == nullptr || first->kind != ref.kind || first->family != ref.families[i])
                continue;
            const bool wasAlone = aloneName(*first) == ref.names[i];
            std::vector<SubId> elements;
            bool unusable = false;
            for (const Hash256& member : members) {
                const TopoName* record = map.record(member);
                // Shared names cannot be told apart; a family smaller than when it was issued is incomplete.
                if (record == nullptr || record->siblingCount != members.size())
                    unusable = true;
                if (const auto s = map.find(member))
                    elements.push_back(*s);
                else
                    unusable = true;
            }
            for (const SubId& s : elements)
                unusable = unusable || !neighboursStillAround(map, shape, s, ref);
            if (unusable)
                continue;
            if (wasAlone && members.size() > 1) {
                std::sort(elements.begin(), elements.end());
                std::erase_if(elements, [&](const SubId& s) { return zeroMeasure(shape, s, ref.hint); });
                if (elements.empty())
                    continue;
                if (const std::vector<SubId> inside = piecesInsideOutline(shape, elements, ref.outline);
                    inside.size() < elements.size()) {
                    if (!piecesHoldMostOf(shape, inside, ref.territory))
                        continue; // moved, or new ground outside the outline: cannot be told (as above)
                    elements = inside;
                }
                if (std::vector<SubId> kept = piecesInsideMovedPick(map, shape, elements, ref);
                    kept.size() < elements.size()) {
                    if (kept.empty())
                        continue;               // no piece where the pick was, its feature's move unknown (as above)
                    elements = std::move(kept); // a piece past the moved pick, left out (as above)
                }
                std::string why;
                if (!piecesLieAsPicked(map, shape, elements, ref.hint) ||
                    !facePiecesStayInside(map, shape, elements, ref) ||
                    !piecesFitEvidence(map, shape, elements, ref, why))
                    continue;
                return make(ResolveState::Split, std::move(elements));
            }
            if (!wasAlone && members.size() == 1 && ref.kind != SubKind::Vertex &&
                canContain(map, shape, elements.front(), ref.hint, ref.territory) &&
                surroundedLikePick(elements.front()))
                return make(ResolveState::Merged, std::move(elements));
        }
    }
    if (!ref.splitParents.empty() && ref.kind != SubKind::Vertex) {
        // Every parent must be found, unsplit, on one and the same element.
        Walk parents;
        bool everyParent = true;
        for (const Hash256& parent : ref.splitParents) {
            Walk w;
            walk(map, parent, covered, 0, w);
            everyParent = everyParent && !w.elements.empty();
            parents.shared = parents.shared || w.shared;
            parents.split = parents.split || w.split;
            parents.uncertain = parents.uncertain || w.uncertain;
            parents.elements.insert(w.elements.begin(), w.elements.end());
        }
        if (everyParent && !parents.shared && !parents.split && !parents.uncertain && parents.elements.size() == 1) {
            const SubId c = *parents.elements.begin();
            if (c.kind == ref.kind && canContain(map, shape, c, ref.hint, ref.territory) && surroundedLikePick(c))
                return make(ResolveState::Merged, {c});
        }
    }

    // ---- stage 4: Repaired ----
    return repaired(map, shape, ref);
}

} // namespace

bool zeroMeasure(const kernel::Shape& shape, kernel::SubId element, const Fingerprint& hint)
{
    try {
        // The picked element had measure: more than a quantum of length, or of width (area over the box diagonal).
        if (static_cast<double>(std::abs(hint.qArea)) <= kZeroWidth * kLengthQuantaPerMm)
            return false;
        // The measure as the kernel gives it now; an element whose geometry cannot be read is not known to have none.
        if (element.kind == SubKind::Face) {
            const auto info = kernel::surfaceInfo(shape, element);
            const auto box = elementBox(shape, element);
            if (!info || !box)
                return false;
            const double diagonal = norm(sub(box->max, box->min));
            return std::abs(info->area) <= kZeroWidth * std::max(diagonal, kZeroWidth);
        }
        if (element.kind == SubKind::Edge) {
            const auto info = kernel::curveInfo(shape, element);
            return info && std::abs(info->length) <= kZeroWidth;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool edgeContinuesUnderOtherName(const ElementMap& map, const kernel::Shape& shape, kernel::SubId edge)
{
    try {
        if (edge.kind != SubKind::Edge || !map.impl)
            return false;
        // The face names each name of the edge was made from, and the pieces those faces were split into since.
        std::set<Hash256> own, pieces;
        for (const Hash256& name : map.namesOf(edge)) {
            const TopoName* record = map.record(name);
            if (record == nullptr || record->role != "nb.edge")
                continue;
            for (const Hash256& parent : record->parents) {
                own.insert(parent);
                for (const TopoName* child : splitChildren(map, parent))
                    pieces.insert(child->id);
            }
        }
        if (pieces.empty())
            return false;
        auto holds = [&](SubId face, const std::set<Hash256>& set) {
            for (const Hash256& n : map.namesOf(face))
                if (set.count(n) != 0)
                    return true;
            return false;
        };
        std::set<Hash256> ownOrPieces = own;
        ownOrPieces.insert(pieces.begin(), pieces.end());
        for (const SubId& vertex : kernel::adjacent(shape, edge, SubKind::Vertex))
            for (const SubId& other : kernel::adjacent(shape, vertex, SubKind::Edge)) {
                if (other == edge)
                    continue;
                const std::vector<SubId> faces = kernel::adjacent(shape, other, SubKind::Face);
                if (faces.size() != 2)
                    continue;
                // One face a piece of a face the edge was named from, the other that face's partner or a piece of it.
                const bool a = holds(faces[0], pieces) && holds(faces[1], ownOrPieces);
                const bool b = holds(faces[1], pieces) && holds(faces[0], ownOrPieces);
                if (a || b)
                    return true;
            }
        return false;
    } catch (...) {
        return true;
    }
}

bool holdsHintCentre(const kernel::Shape& shape, kernel::SubId face, const Fingerprint& hint)
{
    try {
        if (face.kind != SubKind::Face)
            return false;
        const auto info = kernel::surfaceInfo(shape, face);
        if (!info || info->type != kernel::SurfaceType::Plane)
            return false;
        const double n = norm(info->axis);
        if (!(n > 0.0))
            return false;
        const Vec3 p{q(hint.qCx), q(hint.qCy), q(hint.qCz)};
        const Vec3 axis{info->axis.x / n, info->axis.y / n, info->axis.z / n};
        if (std::abs(dot(sub(p, info->origin), axis)) > kContainTolerance)
            return false;
        const auto inside = insidePlanarFace(shape, face, p, *info);
        return inside.has_value() && *inside;
    } catch (...) {
        return false;
    }
}

bool onPickedFaces(const ElementMap& map, const kernel::Shape& shape, kernel::SubId element, const TopoRef& ref)
{
    try {
        return map.impl && neighboursStillAround(map, shape, element, ref);
    } catch (...) {
        return false;
    }
}

std::optional<Resolved> resolveThroughBuild(const ElementMap& map, const kernel::Shape& shape, const TopoRef& ref)
{
    try {
        return resolveImpl(map, shape, ref);
    } catch (...) {
        return broken("The reference could not be resolved because of an internal failure.");
    }
}

} // namespace ray::naming::detail
