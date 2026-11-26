// The resolver's rules on pick-time evidence (territory.h, naming.h TopoRef::territory and pickNeighbours).
#include "territory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace ray::naming::detail {

namespace {

using kernel::SubId;
using kernel::SubKind;

Resolved brokenBecause(std::string reason)
{
    return Resolved{ResolveState::Broken, {}, std::move(reason)};
}

double extentTolerance(const Box3& box)
{
    const double size = std::max({box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z, 0.0});
    return 2e-3 + 5e-4 * size; // an arc's box is made from chords; quanta round both boxes
}

Vec3 minus(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double length(const Vec3& a)
{
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

// A placement as a rigid transform, frame to model (naming.h FeaturePlacement).
struct Rigid {
    std::array<double, 9> r{};
    Vec3 t{};
};
Rigid rigidOf(const FeaturePlacement& p)
{
    Rigid out;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            out.r[i * 3 + j] = static_cast<double>(p.q[i * 4 + j]) / 1e9;
    out.t = {static_cast<double>(p.q[3]) / kLengthQuantaPerMm, static_cast<double>(p.q[7]) / kLengthQuantaPerMm,
             static_cast<double>(p.q[11]) / kLengthQuantaPerMm};
    return out;
}
// The model point x of a feature placed at `from`, where the same point of the feature is when it is placed at `to`.
Vec3 carry(const Rigid& from, const Rigid& to, const Vec3& x)
{
    const Vec3 d = minus(x, from.t);
    const std::array<double, 3> local{from.r[0] * d.x + from.r[3] * d.y + from.r[6] * d.z,
                                      from.r[1] * d.x + from.r[4] * d.y + from.r[7] * d.z,
                                      from.r[2] * d.x + from.r[5] * d.y + from.r[8] * d.z};
    return {to.r[0] * local[0] + to.r[1] * local[1] + to.r[2] * local[2] + to.t.x,
            to.r[3] * local[0] + to.r[4] * local[1] + to.r[5] * local[2] + to.t.y,
            to.r[6] * local[0] + to.r[7] * local[1] + to.r[8] * local[2] + to.t.z};
}

// What the reference's recorded placements (TopoRef::placements) say about the features that made the pick: none
// recorded or none in the map now (None); all where they were (Still); one move, every moved feature moved alike
// (Moved, from -> to); moves that differ (Several).
struct KnownMove {
    enum class Kind { None, Still, Moved, Several } kind = Kind::None;
    Rigid from, to;
};
KnownMove knownMove(const ElementMap& map, const TopoRef& ref)
{
    KnownMove out;
    std::vector<std::pair<Rigid, Rigid>> moved;
    for (const FeaturePlacement& then : ref.placements) {
        const FeaturePlacement* now = map.placement(then.feature);
        if (now == nullptr)
            continue;
        if (samePlacement(then, *now)) {
            if (out.kind == KnownMove::Kind::None)
                out.kind = KnownMove::Kind::Still;
            continue;
        }
        moved.emplace_back(rigidOf(then), rigidOf(*now));
    }
    if (moved.empty())
        return out;
    // Moved alike: the same images of the pick's box corners (within 1e-3 mm).
    const Box3 box = boxOf(ref.territory);
    for (std::size_t i = 1; i < moved.size(); ++i)
        for (const Vec3& c :
             {box.min, box.max, Vec3{box.min.x, box.max.y, box.min.z}, Vec3{box.max.x, box.min.y, box.max.z}})
            if (length(minus(carry(moved[0].first, moved[0].second, c), carry(moved[i].first, moved[i].second, c))) >
                1e-3) {
                out.kind = KnownMove::Kind::Several;
                return out;
            }
    out.kind = KnownMove::Kind::Moved;
    out.from = moved[0].first;
    out.to = moved[0].second;
    return out;
}

// `p` moved onto the surface `info` describes: a plane along its normal, a cylinder straight out from its axis.
// nullopt when it cannot be (on a cylinder's axis, or another surface).
std::optional<Vec3> ontoSurface(const kernel::SurfaceInfo& info, const Vec3& p)
{
    const Vec3 o = info.origin;
    const Vec3 a = info.axis;
    const Vec3 d = minus(p, o);
    const double h = d.x * a.x + d.y * a.y + d.z * a.z;
    if (info.type == kernel::SurfaceType::Plane)
        return Vec3{p.x - a.x * h, p.y - a.y * h, p.z - a.z * h};
    if (info.type != kernel::SurfaceType::Cylinder)
        return std::nullopt;
    const Vec3 radial{d.x - a.x * h, d.y - a.y * h, d.z - a.z * h};
    const double r = length(radial);
    if (r < 1e-9)
        return std::nullopt;
    const double k = info.radius / r;
    return Vec3{o.x + a.x * h + radial.x * k, o.y + a.y * h + radial.y * k, o.z + a.z * h + radial.z * k};
}

// Do two edge fingerprints lie on one line (lines: parallel within 1e-6, each midpoint within 1e-2 mm of the other's
// line) or one circle (circles: the same centre within 1e-3 mm, axis within 1e-6, radius within 1e-3 mm)? (1e-2 mm:
// a feature moved along its own wall moves that wall by the rounding of its frame, a few micrometres; the oracle's
// structural check counts such a wall as where it was.)
bool sameCurve(const Fingerprint& a, const Fingerprint& b)
{
    if (a.surface != b.surface)
        return false;
    const auto axis = [](const Fingerprint& f) {
        return Vec3{static_cast<double>(f.qAxisX) / kAxisQuanta, static_cast<double>(f.qAxisY) / kAxisQuanta,
                    static_cast<double>(f.qAxisZ) / kAxisQuanta};
    };
    const auto centre = [](const Fingerprint& f) {
        return Vec3{static_cast<double>(f.qCx) / kLengthQuantaPerMm, static_cast<double>(f.qCy) / kLengthQuantaPerMm,
                    static_cast<double>(f.qCz) / kLengthQuantaPerMm};
    };
    const Vec3 da = axis(a), db = axis(b);
    const double la = length(da), lb = length(db);
    if (la < 0.5 || lb < 0.5)
        return false;
    const Vec3 ua{da.x / la, da.y / la, da.z / la}, ub{db.x / lb, db.y / lb, db.z / lb};
    const Vec3 c{ua.y * ub.z - ua.z * ub.y, ua.z * ub.x - ua.x * ub.z, ua.x * ub.y - ua.y * ub.x};
    if (length(c) > 2e-6)
        return false;
    const Vec3 d = minus(centre(b), centre(a));
    if (a.surface == static_cast<kernel::SurfaceType>(kernel::CurveType::Line)) {
        const double along = d.x * ua.x + d.y * ua.y + d.z * ua.z;
        return length(Vec3{d.x - ua.x * along, d.y - ua.y * along, d.z - ua.z * along}) <= 1e-2;
    }
    if (a.surface == static_cast<kernel::SurfaceType>(kernel::CurveType::Circle))
        return length(d) <= 1e-3 &&
               std::abs(static_cast<double>(a.qRadius - b.qRadius)) / kLengthQuantaPerMm <= 1e-3;
    return false;
}

// Does every point of `territory` lie (within 1e-2 mm, as sameCurve) on the surface of every face of `edge` (a vertex:
// of each of its
// faces, two or more): unbounded for planes, cylinders and spheres, the face as it is for any other surface? Then the
// picked element's place is still where those surfaces meet.
bool onSurfacesOf(const kernel::Shape& shape, SubId edge, const Territory& territory)
{
    if (!territory.known())
        return false;
    const std::vector<SubId> faces = kernel::adjacent(shape, edge, SubKind::Face);
    if (edge.kind == SubKind::Edge ? faces.size() != 2 : faces.size() < 2)
        return false;
    for (const SubId& f : faces) {
        const auto info = kernel::surfaceInfo(shape, f);
        if (!info)
            return false;
        for (const auto& q : territory.points) {
            const Vec3 p = fromQuanta(q);
            const Vec3 d = minus(p, info->origin);
            const Vec3 a = info->axis;
            const double h = d.x * a.x + d.y * a.y + d.z * a.z;
            double off = 1e300;
            if (info->type == kernel::SurfaceType::Plane)
                off = std::abs(h);
            else if (info->type == kernel::SurfaceType::Cylinder)
                off = std::abs(length(Vec3{d.x - a.x * h, d.y - a.y * h, d.z - a.z * h}) - info->radius);
            else if (info->type == kernel::SurfaceType::Sphere)
                off = std::abs(length(d) - info->radius);
            else if (info->type == kernel::SurfaceType::Cone) {
                // Radius `radius` at the origin, opening by `semiAngle` along the axis.
                const double rho = length(Vec3{d.x - a.x * h, d.y - a.y * h, d.z - a.z * h});
                off = std::abs(rho - (info->radius + h * dmath::tan(info->semiAngle))) * dmath::cos(info->semiAngle);
            } else if (info->type == kernel::SurfaceType::Torus) {
                // Major radius `radius` about the axis; the minor one measured at a vertex of the face.
                const auto tube = [&](const Vec3& x) {
                    const Vec3 dx = minus(x, info->origin);
                    const double hx = dx.x * a.x + dx.y * a.y + dx.z * a.z;
                    const double rho = length(Vec3{dx.x - a.x * hx, dx.y - a.y * hx, dx.z - a.z * hx});
                    return std::sqrt((rho - info->radius) * (rho - info->radius) + hx * hx);
                };
                const std::vector<SubId> corners = kernel::adjacent(shape, f, SubKind::Vertex);
                if (!corners.empty())
                    if (const auto v = kernel::vertexPoint(shape, corners.front()))
                        off = std::abs(tube(p) - tube(*v));
                if (off > 1e-3 && Probe(shape, f).on(p))
                    off = 0;
            } else if (Probe(shape, f).on(p)) // another surface: the face itself, where it is now, holds the point
                off = 0;
            // (Planes within 1e-2 mm: a feature moved along its own wall moves the wall by the rounding of its frame,
            // a few micrometres; curved surfaces within 1e-3 mm: a blend made a little larger is another surface.)
            if (!(off <= (info->type == kernel::SurfaceType::Plane ? 1e-2 : 1e-3)))
                return false;
        }
    }
    return true;
}

// The heights along axis `a` (through `o`) between which a cylinder of radius `r` lies whose box is `box`: the box's
// extent along the axis, less what the rims' circles add to it.
std::pair<double, double> axialRange(const Box3& box, const Vec3& o, const Vec3& a, double r)
{
    double lo = 1e300, hi = -1e300;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p{(c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y,
                     (c & 4) ? box.max.z : box.min.z};
        const double h = (p.x - o.x) * a.x + (p.y - o.y) * a.y + (p.z - o.z) * a.z;
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    double inset = 0;
    for (const double ai : {a.x, a.y, a.z})
        inset += std::abs(ai) * r * std::sqrt(std::max(0.0, 1.0 - ai * ai));
    return {lo + inset, hi - inset};
}

// The one element all of `names` lead to now, if there is one.
std::optional<SubId> oneElement(const ElementMap& map, const std::vector<Hash256>& names)
{
    std::optional<SubId> out;
    for (const Hash256& name : names) {
        const auto s = map.find(name);
        if (!s)
            return std::nullopt;
        if (out && *out != *s)
            return std::nullopt;
        out = s;
    }
    return out;
}

// The gap between two boxes, mm (0 when they touch or overlap).
double gapBetween(const Box3& a, const Box3& b)
{
    const double gx = std::max({0.0, b.min.x - a.max.x, a.min.x - b.max.x});
    const double gy = std::max({0.0, b.min.y - a.max.y, a.min.y - b.max.y});
    const double gz = std::max({0.0, b.min.z - a.max.z, a.min.z - b.max.z});
    return std::sqrt(gx * gx + gy * gy + gz * gz);
}

// How far apart two boxes are along the axis on which they are furthest apart, mm: positive, the gap; negative, the
// least overlap along any axis an extent shows (an axis on which both are flat, within 1e-6 mm, counts only as a gap:
// two parallel lines a distance apart).
double gapOrOverlap(const Box3& a, const Box3& b)
{
    double out = -1e300;
    const std::array<std::array<double, 4>, 3> axes{{{a.min.x, a.max.x, b.min.x, b.max.x},
                                                      {a.min.y, a.max.y, b.min.y, b.max.y},
                                                      {a.min.z, a.max.z, b.min.z, b.max.z}}};
    bool any = false;
    for (const auto& v : axes) {
        if (v[1] - v[0] <= 1e-6 && v[3] - v[2] <= 1e-6) {
            if (std::abs(v[0] - v[2]) > 1e-6)
                return std::abs(v[0] - v[2]); // flat on both, apart: a gap
            continue;
        }
        any = true;
        out = std::max(out, std::max(v[2] - v[1], v[0] - v[3]));
    }
    return any ? out : gapBetween(a, b);
}

// Does no point of `t` lie in `box` (within the extent tolerance)? (An arc that is the rest of a circle has a box
// around the picked arc's, and none of its points near it.) False when `t` has no points.
bool noPointInside(const Territory& t, const Box3& box)
{
    if (!t.known())
        return false;
    const double tol = extentTolerance(box);
    for (const auto& q : t.points) {
        const Vec3 p = fromQuanta(q);
        if (p.x >= box.min.x - tol && p.x <= box.max.x + tol && p.y >= box.min.y - tol && p.y <= box.max.y + tol &&
            p.z >= box.min.z - tol && p.z <= box.max.z + tol)
            return false;
    }
    return true;
}

// Did a recorded neighbour touch the picked element when it was picked (their boxes within the extent tolerance)?
// A neighbour whose box is not known counts as touching. Then the picked element was already one stretch of a longer
// run of elements, and whatever continues it now may be what continued it then.
bool pickTouchedANeighbour(const TopoRef& ref)
{
    const Box3 own = boxOf(ref.territory);
    const double tol = extentTolerance(own);
    for (const PickNeighbour& n : ref.pickNeighbours) {
        const Territory& t = n.territory;
        if (t.qMeasure == 0 && t.qMin == t.qMax && !t.known())
            return true;
        if (gapBetween(own, boxOf(t)) <= tol)
            return true;
    }
    return false;
}

// Elements of `element`'s kind that share a boundary element with it: a vertex (edges), an edge (faces).
std::vector<SubId> touchingElements(const kernel::Shape& shape, SubId element)
{
    const SubKind via = element.kind == SubKind::Edge ? SubKind::Vertex : SubKind::Edge;
    std::vector<SubId> out;
    for (const SubId& b : kernel::adjacent(shape, element, via))
        for (const SubId& o : kernel::adjacent(shape, b, element.kind))
            if (o != element)
                out.push_back(o);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Does `other` continue `element` with nothing geometric between them: the same carrier (plane on the same side,
// cylinder, line, circle), and for an edge, every face of `other` one of `element`'s faces or on the carrier of one?
// Then the two are one stretch of one meeting of the same surfaces, cut in two only by a vertex (or a face by an edge)
// that no surface of either explains.
bool continuesSeamlessly(const kernel::Shape& shape, const TerritoryCache& territories, SubId element, SubId other)
{
    if (!territories.sameCarrier(element, other))
        return false;
    if (element.kind != SubKind::Edge)
        return true;
    const std::vector<SubId> own = kernel::adjacent(shape, element, SubKind::Face);
    const std::vector<SubId> theirs = kernel::adjacent(shape, other, SubKind::Face);
    if (own.empty() || theirs.empty())
        return false;
    for (const SubId& f : theirs) {
        bool matched = false;
        for (const SubId& g : own)
            matched = matched || f == g || territories.sameCarrier(f, g);
        if (!matched)
            return false;
    }
    return true;
}

// Does box `e` reach into box `place` (both mm): no farther than `tol` from it along any axis, and, along every axis
// on which `e` itself has extent, overlapping it by more than `tol`? An element that only touches the place at its
// boundary (an edge ending where the place ends) does not reach into it.
bool reachesInto(const Box3& e, const Box3& place, double tol)
{
    const double lo[3] = {e.min.x, e.min.y, e.min.z}, hi[3] = {e.max.x, e.max.y, e.max.z};
    const double plo[3] = {place.min.x, place.min.y, place.min.z}, phi[3] = {place.max.x, place.max.y, place.max.z};
    bool extent = false;
    for (int i = 0; i < 3; ++i) {
        const double overlap = std::min(hi[i], phi[i]) - std::max(lo[i], plo[i]);
        if (overlap < -tol)
            return false;
        if (hi[i] - lo[i] > 2.0 * tol) {
            extent = true;
            if (overlap <= tol)
                return false;
        }
    }
    return extent;
}

bool insideBox(const Box3& e, const Box3& place, double tol)
{
    return e.min.x >= place.min.x - tol && e.min.y >= place.min.y - tol && e.min.z >= place.min.z - tol &&
           e.max.x <= place.max.x + tol && e.max.y <= place.max.y + tol && e.max.z <= place.max.z + tol;
}

// Was `e` (by one of its names) the picked element or a recorded neighbour when the reference was made?
bool namedAtPick(const ElementMap& map, SubId e, const TopoRef& ref)
{
    for (const Hash256& name : map.namesOf(e)) {
        if (std::find(ref.names.begin(), ref.names.end(), name) != ref.names.end())
            return true;
        for (const PickNeighbour& n : ref.pickNeighbours)
            if (std::find(n.names.begin(), n.names.end(), name) != n.names.end())
                return true;
    }
    return false;
}

// Does `e` cover a point of a recorded neighbour's territory? Then it is (or holds) an element that lay apart from the
// picked one at pick time: a face's box is a coarse stand-in for its place, and a coplanar face beside the pick (inside
// the box of an L-shaped or ring-shaped pick) was there, apart, when it was picked.
bool holdsANeighbour(const kernel::Shape& shape, SubId e, const TopoRef& ref)
{
    const Probe probe(shape, e);
    for (const PickNeighbour& n : ref.pickNeighbours)
        if (n.territory.known() && probe.count(n.territory) > 0)
            return true;
    return false;
}

constexpr double kPi = 3.14159265358979323846;

double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 scaled(const Vec3& a, double k)
{
    return {a.x * k, a.y * k, a.z * k};
}

// The direction from a circle's centre `c` (axis `n`, unit) to the middle of an arc of it whose chord midpoint is `m`
// and whose length over its radius is `angle`: towards m for an arc up to a half circle, away from it beyond. No trig.
// nullopt when the chord midpoint is (nearly) the centre: a half circle, whose middle the chord does not tell.
std::optional<Vec3> arcMiddle(const Vec3& c, const Vec3& n, const Vec3& m, double angle, double radius)
{
    Vec3 u = minus(m, c);
    u = minus(u, scaled(n, dot(u, n)));
    const double d = length(u);
    if (!(d > 1e-3 * radius) || !(radius > 0.0))
        return std::nullopt;
    return scaled(u, (angle <= kPi ? 1.0 : -1.0) / d);
}

// Where a line edge or a circular arc was picked, to tell whether another edge on the same line or circle lies inside
// it. A line segment is exactly its box's stretch of its line. An arc is told by its middle direction and the cosine
// of its half angle, from the hint (chord midpoint, length, radius) and the circle now (centre, axis): the chord
// midpoint lies at r cos(half) from the centre, on the middle's side for an arc up to a half circle. A whole circle
// holds every point of it.
struct PickedPlace {
    bool line = false;
    Box3 box{};
    Vec3 centre{}, axis{}, middle{};
    double radius = 0, cosHalf = 1, tol = 0;

    bool onArc(const Vec3& p) const
    {
        Vec3 u = minus(p, centre);
        u = minus(u, scaled(axis, dot(u, axis)));
        const double d = length(u);
        return d > 0.0 && dot(u, middle) / d >= cosHalf - tol / radius;
    }

    // Does edge `info` (on the same line or circle; its box and territory given) lie inside the place? An arc lies
    // inside the picked arc when its ends and its territory points do (the territory's first point is its middle), or,
    // without a territory, its ends and the middle its chord tells.
    bool holds(const kernel::CurveInfo& info, const Box3& edgeBox, const Territory& territory) const
    {
        if (line)
            return insideBox(edgeBox, box, tol);
        if (!onArc(info.start) || !onArc(info.end))
            return false;
        if (territory.known()) {
            for (const auto& q : territory.points)
                if (!onArc(fromQuanta(q)))
                    return false;
            return true;
        }
        const Vec3 m =
            scaled(Vec3{info.start.x + info.end.x, info.start.y + info.end.y, info.start.z + info.end.z}, 0.5);
        const auto mid = arcMiddle(centre, axis, m, info.length / radius, radius);
        if (!mid)
            return false;
        return onArc(Vec3{centre.x + radius * mid->x, centre.y + radius * mid->y, centre.z + radius * mid->z});
    }
};

std::optional<PickedPlace> pickedPlace(const kernel::Shape& shape, SubId answer, const Box3& box, double tol,
                                       const TopoRef& ref)
{
    const Fingerprint& hint = ref.hint;
    const auto info = kernel::curveInfo(shape, answer);
    if (!info)
        return std::nullopt;
    PickedPlace out;
    out.box = box;
    out.tol = tol;
    if (info->type == kernel::CurveType::Line) {
        out.line = true;
        return out;
    }
    const double r = static_cast<double>(hint.qRadius) / kLengthQuantaPerMm;
    const double n = length(info->axis);
    if (info->type != kernel::CurveType::Circle || info->closed || !(n > 0.0) || !(r > 0.0) ||
        std::abs(r - info->radius) > tol)
        return std::nullopt;
    out.centre = info->center;
    out.axis = scaled(info->axis, 1.0 / n);
    out.radius = info->radius;
    const Vec3 m{static_cast<double>(hint.qCx) / kLengthQuantaPerMm, static_cast<double>(hint.qCy) / kLengthQuantaPerMm,
                 static_cast<double>(hint.qCz) / kLengthQuantaPerMm};
    const double angle = static_cast<double>(hint.qArea) / kLengthQuantaPerMm / r;
    // A whole circle was picked (a hole's rim): every point of it is the pick's.
    if (angle >= 2.0 * kPi * (1.0 - 1e-6)) {
        const Vec3 x{1, 0, 0}, y{0, 1, 0};
        const Vec3 any = std::abs(out.axis.x) < 0.9 ? x : y;
        out.middle = minus(any, scaled(out.axis, dot(any, out.axis)));
        out.middle = scaled(out.middle, 1.0 / length(out.middle));
        out.cosHalf = -2.0;
        return out;
    }
    auto middle = arcMiddle(out.centre, out.axis, m, angle, r);
    // A half circle: its chord midpoint is the centre. The picked edge's territory points lie at 0.5, 0.3, 0.7, 0.15
    // and 0.85 of its length, symmetric about its middle (territory.h): with all of them, their mean direction from
    // the centre is the middle.
    if (!middle && ref.territory.points.size() == kTerritoryPoints) {
        Vec3 sum{};
        for (const auto& q : ref.territory.points) {
            Vec3 u = minus(fromQuanta(q), out.centre);
            u = minus(u, scaled(out.axis, dot(u, out.axis)));
            const double d = length(u);
            if (d > 0.0)
                sum = Vec3{sum.x + u.x / d, sum.y + u.y / d, sum.z + u.z / d};
        }
        const double d = length(sum);
        if (d > 1e-6)
            middle = scaled(sum, 1.0 / d);
    }
    if (!middle)
        return std::nullopt;
    out.middle = *middle;
    Vec3 u = minus(m, out.centre);
    u = minus(u, scaled(out.axis, dot(u, out.axis)));
    out.cosHalf = (angle <= kPi ? 1.0 : -1.0) * std::min(1.0, length(u) / r);
    return out;
}

// The fuller answer of the completeness check: `answer` and `holders` (elements that hold part of what was picked),
// when that set can be shown to be all of the pick. Only for edges on a line or a circle whose picked place is known
// (PickedPlace: a line segment is exactly its box's stretch of its line; an arc is told by its middle and half angle).
// The set must contain every edge on the answer's line or circle that reaches into the picked box; each edge added
// must lie inside the picked segment or arc, carry no name the pick or a recorded neighbour had (it did not exist apart
// at pick time), and lie between the answer's surfaces (continuesSeamlessly); together they must fit the evidence as
// Split pieces do (piecesFitEvidence). Then the pick came apart into these and nothing else of it is left: a stretch of
// the picked segment or arc on none of them lies on no edge of that line or circle (cut away). nullopt when any of
// this fails.
std::optional<std::vector<SubId>> fullerAnswer(const ElementMap& map, const kernel::Shape& shape,
                                               const TerritoryCache& territories, const std::vector<SubId>& answer,
                                               const std::set<SubId>& holders, const Box3& box, double tol,
                                               const TopoRef& ref)
{
    if (ref.kind != SubKind::Edge)
        return std::nullopt;
    const auto place = pickedPlace(shape, answer.front(), box, tol, ref);
    if (!place)
        return std::nullopt;
    for (const SubId& a : answer)
        if (!territories.sameCarrier(answer.front(), a))
            return std::nullopt;
    std::set<SubId> out(answer.begin(), answer.end());
    const std::uint32_t n = shape.count(SubKind::Edge);
    for (std::uint32_t i = 0; i < n; ++i) {
        const SubId e{SubKind::Edge, i};
        if (out.count(e) != 0 || !territories.sameCarrier(answer.front(), e))
            continue;
        const auto eBox = elementBox(shape, e);
        const auto info = kernel::curveInfo(shape, e);
        if (!eBox || !info)
            return std::nullopt;
        if (!reachesInto(*eBox, box, tol) && holders.count(e) == 0)
            continue;
        if (!place->holds(*info, *eBox, territories.of(e)) || namedAtPick(map, e, ref) ||
            !continuesSeamlessly(shape, territories, answer.front(), e))
            return std::nullopt;
        out.insert(e);
    }
    std::vector<SubId> pieces(out.begin(), out.end());
    if (pieces.size() == answer.size())
        return std::nullopt;
    std::string why;
    if (!piecesFitEvidence(map, shape, pieces, ref, why))
        return std::nullopt;
    return pieces;
}

} // namespace

std::optional<Resolved> completeAgainstEvidence(const ElementMap& map, const kernel::Shape& shape,
                                                const TerritoryCache& territories, const std::vector<SubId>& answer,
                                                const TopoRef& ref)
{
    try {
        if (!ref.territory.known() || answer.empty() || ref.kind == SubKind::Vertex)
            return std::nullopt;
        for (const SubId& a : answer)
            if (a.kind != ref.kind)
                return std::nullopt;
        std::vector<std::unique_ptr<Probe>> probes;
        for (const SubId& a : answer)
            probes.push_back(std::make_unique<Probe>(shape, a));
        std::vector<Vec3> lost;
        for (const auto& q : ref.territory.points) {
            const Vec3 p = fromQuanta(q);
            bool on = false;
            for (const auto& probe : probes)
                on = on || probe->on(p);
            if (!on)
                lost.push_back(p);
        }
        // Where the pick was is known while the answer still holds some of its own territory: the picked element's
        // box at pick time. Otherwise the answer moved (or what was picked of it is gone), and its place now is not
        // known.
        const bool placeKnown = lost.size() < ref.territory.points.size();
        const Box3 place = boxOf(ref.territory);
        const double tol = extentTolerance(place);
        auto inAnswer = [&](const SubId& e) {
            return std::find(answer.begin(), answer.end(), e) != answer.end();
        };
        // On the answer's carrier, and for an edge between the answer's surfaces (continuesSeamlessly): a stretch of
        // the picked line that now lies between other surfaces is where the pick was, but not what was picked.
        auto onAnswerCarrier = [&](const SubId& e) {
            for (const SubId& a : answer)
                if (continuesSeamlessly(shape, territories, a, e))
                    return true;
            return false;
        };
        // Elements outside the answer that hold part of what was picked, found two ways:
        std::set<SubId> byTerritory, byContinuation;
        //  - a point of the picked element's own territory that the answer lost lies on another element on the
        //    answer's carrier, an edge also between the answer's surfaces (points on nothing are territory cut away,
        //    which an answer may lose);
        if (placeKnown && !lost.empty()) {
            const std::uint32_t n = shape.count(ref.kind);
            for (std::uint32_t i = 0; i < n; ++i) {
                const SubId other{ref.kind, i};
                if (inAnswer(other) || !onAnswerCarrier(other))
                    continue;
                const Probe there(shape, other);
                for (const Vec3& p : lost)
                    if (there.on(p)) {
                        byTerritory.insert(other);
                        break;
                    }
            }
        }
        //  - the answer runs on, with nothing geometric between them, into another element of its kind (it shares a
        //    vertex, or an edge, with it; same carrier; an edge also between the same surfaces: continuesSeamlessly),
        //    and that element reaches into where the pick was, and was not apart from the pick when it was picked (it
        //    carries no name of a recorded neighbour and covers none of their territory: a boundary between the pick
        //    and a neighbour that moved a little is not the pick coming apart); or, the place not being known, into it
        //    at all, unless a recorded neighbour touched the picked element at pick time (it was already one stretch of
        //    a longer run, such as the two arcs of one hole's rim, and the run may simply have moved with it).
        const bool moved = !placeKnown;
        if (!moved || !pickTouchedANeighbour(ref))
            for (const SubId& a : answer)
                for (const SubId& other : touchingElements(shape, a)) {
                    if (inAnswer(other) || !continuesSeamlessly(shape, territories, a, other))
                        continue;
                    if (!moved) {
                        const auto box = elementBox(shape, other);
                        if (!box || !reachesInto(*box, place, tol) || namedAtPick(map, other, ref) ||
                            holdsANeighbour(shape, other, ref))
                            continue;
                    }
                    byContinuation.insert(other);
                }
        if (byTerritory.empty() && byContinuation.empty())
            return std::nullopt;
        if (placeKnown) {
            std::set<SubId> holders = byTerritory;
            holders.insert(byContinuation.begin(), byContinuation.end());
            if (auto pieces = fullerAnswer(map, shape, territories, answer, holders, place, tol, ref))
                return Resolved{ResolveState::Split, std::move(*pieces), {}};
        }
        if (!byTerritory.empty())
            return brokenBecause("Part of where the picked element was is now another element on the same surface; "
                                 "whether the picked element came apart cannot be told.");
        if (moved)
            return brokenBecause("The picked element moved and now runs on, with nothing between them, into another "
                                 "element on the same surfaces; which part is the picked one cannot be told.");
        return brokenBecause(
            "The picked element now runs on, with nothing between them, into another element on the "
            "same surfaces that lies where the picked element was; which part is the picked one cannot "
            "be told.");
    } catch (...) {
        return brokenBecause("The reference could not be checked because of an internal failure.");
    }
}

std::optional<Resolved> exactAgainstEvidence(const ElementMap& map, const kernel::Shape& shape,
                                             const TerritoryCache& territories, SubId element, const TopoRef& ref)
{
    try {
        if (!ref.territory.known() || map.fingerprintOf(element).matches(ref.hint))
            return std::nullopt;
        const Probe probe(shape, element);
        const std::size_t own = probe.count(ref.territory);
        bool tookOther = false;
        for (const PickNeighbour& n : ref.pickNeighbours)
            tookOther = tookOther || (n.territory.known() && probe.count(n.territory) > 0);
        if (tookOther) {
            if (own > 0 && element.kind != SubKind::Vertex)
                return Resolved{ResolveState::Merged, {element}, {}};
            return brokenBecause(own > 0 ? "The picked vertex now lies where another vertex was when it was picked."
                                         : "The picked element now lies over another element's place, and no longer "
                                           "where it was picked.");
        }
        // An Exact answer says the element is all of what was picked: the completeness check (T-081a holdout4 9:9
        // and 5:9, holdout5 1:2).
        if (auto incomplete = completeAgainstEvidence(map, shape, territories, {element}, ref))
            return incomplete;
        if (own > 0)
            return std::nullopt;
        // A face that holds none of its own territory while another face on its carrier (the same plane on the same
        // side, the same cylinder) holds some of it: the picked place is still there, on the picked face's own surface,
        // under another face; the face the names lead to is another patch of that surface, not the pick moved (T-081b
        // holdout2 7:44 face 27: a window of a boss's start cap at the bottom of a hole; the boss moved by its width,
        // the window's ground went to the hole's floor, and the start cap's name led to a patch of the same plane
        // sticking out below the part). Which of them continues the pick cannot be told from the names: Broken.
        if (element.kind == SubKind::Face)
            for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
                const SubId other{SubKind::Face, i};
                if (other == element || !territories.sameCarrier(element, other))
                    continue;
                if (Probe(shape, other).count(ref.territory) > 0)
                    return brokenBecause("The picked face's place is now part of another face on the same surface, and "
                                         "the face its name leads to lies elsewhere on that surface.");
            }
        // An edge or face that holds none of its own territory and is now less than a twentieth of its picked size is a
        // remnant where the rest of it went, not the element (T-081b round 3, holdout2 9:3 edge 33: a 2.6 mm arc of a
        // bore's rim, its name left on a 9 um stub of it).
        if (element.kind != SubKind::Vertex && ref.hint.qArea > 0 &&
            static_cast<double>(map.fingerprintOf(element).qArea) < 0.05 * static_cast<double>(ref.hint.qArea))
            return brokenBecause("Only a small remnant of the picked element is left, and not where it was picked.");
        // An edge or vertex that holds none of its territory, in place (within 1e-2 mm) or carried by its feature's
        // move
        // (TopoRef::placements), while it lies on the very line (or circle) the picked edge lay on, or between faces on
        // whose surfaces every picked territory point still lies (the curve or point where those surfaces meet), is
        // another stretch of that curve (another of those meeting points): the curve stayed, what bounded the pick or
        // the pick's own feature moved along it, and what was picked of it is gone (T-081b round 4: the reviewer's
        // holdout1 4:33 edge 9 and holdout2 1:3 edge 42, a cut moved along its own wall's line, the wall's other end
        // now
        // where the picked end was; holdout1 7:33 edges 10 and 14, a boss moved along its own side's plane; holdout2
        // 6:8
        // edge 7, an imported arc, the cut that ended it moved, the rest of the circle now; holdout1 6:26 vertex 11, a
        // corner of a boss moved along its side, another corner of that side now). An edge that holds some of the pick
        // carried by its feature's move moved with the feature, clipped otherwise.
        if (element.kind != SubKind::Face) {
            const KnownMove move = knownMove(map, ref);
            bool holds = false;
            for (const auto& q : ref.territory.points) {
                holds = holds || probe.on(fromQuanta(q), 1e-2);
                if (move.kind == KnownMove::Kind::Moved)
                    holds = holds || probe.on(carry(move.from, move.to, fromQuanta(q)), 1e-2);
            }
            const auto box = element.kind == SubKind::Edge ? elementBox(shape, element) : std::nullopt;
            const bool apart = element.kind == SubKind::Vertex ||
                               (box && (gapOrOverlap(*box, boxOf(ref.territory)) >= -1e-3 ||
                                        noPointInside(territories.of(element), boxOf(ref.territory))));
            const bool sameCarrier =
                (element.kind == SubKind::Edge && sameCurve(map.fingerprintOf(element), ref.hint)) ||
                onSurfacesOf(shape, element, ref.territory);
            if (!holds && apart && sameCarrier)
                return brokenBecause(element.kind == SubKind::Edge
                                         ? "The picked edge's feature slid along the line the edge lies on, and the "
                                           "edge its name leads to is another stretch of it that holds none of the "
                                           "picked one."
                                         : "The picked vertex's feature slid along the faces the vertex lies on, and "
                                           "the vertex its name leads to is another of their meeting points.");
        }
        // A face that holds none of its own territory moved (or what was picked of it is gone).
        //  - A feature of the pick was moved (TopoRef::placements, another placement now; T-081b round 4): the face is
        //    the pick moved with it when the picked territory carried by the feature's own move (then onto the face's
        //    surface as it is now, for a feature that also changed size or depth) lands on it, at least half of it; or
        //    when the face lies wholly inside the pick so carried (clipped smaller where it is now: every territory
        // point
        //    of the face carried back lies inside the picked outline, or the picked box for a face without an outline).
        //  - Otherwise (that failed: a face the part clips, such as a cut's wall through the part, stays where the part
        //    is while its feature moves along it, holdout1 8:23; or the pick's features are where they were and one
        //    changed size or depth, a direct edit moved the face, or nothing was recorded) the move is guessed. A face
        //    that moved with its feature, or that a push/pull moved, still lies
        //    over its picked place seen along its surface: the picked territory carried onto the face's surface as it
        // is
        //    now (a plane: along its normal; a cylinder: straight out from its axis) lands on it, at least half of it,
        //    and so does the territory shifted as the face's box centre moved. Another patch of the same feature's
        //    surface, exposed elsewhere, does not; which it is cannot be told from the names (holdout1 7:33 face 18, a
        //    boss's side whose visible patch slid along it as a join moved over it; tuned 18:49 face 13). Another
        // surface
        //    (a sphere, a torus, a B-spline): moved and resized by more than a fifth. A face whose surface changed size
        // (a
        //    bore resized: another radius) is the same face at another size; there the bound is twice or half its area.
        if (element.kind == SubKind::Face && ref.hint.qArea > 0) {
            const Fingerprint print = map.fingerprintOf(element);
            const double now = static_cast<double>(print.qArea);
            const double then = static_cast<double>(ref.hint.qArea);
            const bool sameShape =
                print.surface == ref.hint.surface && std::abs(print.qRadius - ref.hint.qRadius) <= 10; // 1 um
            const auto info = kernel::surfaceInfo(shape, element);
            const KnownMove move = knownMove(map, ref);
            const std::size_t n = ref.territory.points.size();
            bool changed = false;
            bool carried = false;
            if (move.kind == KnownMove::Kind::Moved) {
                std::size_t landed = 0;
                for (const auto& q : ref.territory.points) {
                    const Vec3 there = carry(move.from, move.to, fromQuanta(q));
                    bool on = probe.on(there);
                    if (!on && info)
                        if (const auto onto = ontoSurface(*info, there))
                            on = probe.on(*onto);
                    landed += on ? 1 : 0;
                }
                bool inside = false;
                const Territory& held = territories.of(element);
                if (2 * landed < n && held.known()) {
                    const Box3 was = boxOf(ref.territory);
                    const double tol = extentTolerance(was);
                    inside = true;
                    for (const auto& q : held.points) {
                        const Vec3 back = carry(move.to, move.from, fromQuanta(q));
                        inside = inside && (ref.outline.known()
                                                ? insidePickOutline(ref.outline, back)
                                                : back.x >= was.min.x - tol && back.x <= was.max.x + tol &&
                                                      back.y >= was.min.y - tol && back.y <= was.max.y + tol &&
                                                      back.z >= was.min.z - tol && back.z <= was.max.z + tol);
                    }
                }
                carried = 2 * landed >= n || inside;
            }
            if (carried) {
                changed = false;
            } else if (!sameShape) {
                changed = now > 2.0 * then + kGrowthSlackQuanta || 2.0 * now + kGrowthSlackQuanta < then;
                // A bore at another radius that now runs along its axis past either end it had at pick time took in
                // the ground along its axis that other faces held there (a step, a cone, an upper bore; a resize does
                // not change how deep a bore is): the same bore, and more, however much larger (T-081b round 4, the
                // lead's decision on review holdout 18:33 face 2: never Exact).
                if (info && info->type == kernel::SurfaceType::Cylinder &&
                    ref.hint.surface == kernel::SurfaceType::Cylinder)
                    if (const auto nowBox = elementBox(shape, element)) {
                        const double wasRadius = static_cast<double>(ref.hint.qRadius) / kLengthQuantaPerMm;
                        const auto [wasLo, wasHi] =
                            axialRange(boxOf(ref.territory), info->origin, info->axis, wasRadius);
                        const auto [nowLo, nowHi] = axialRange(*nowBox, info->origin, info->axis, info->radius);
                        const double tol = 1e-2 + 1e-2 * std::max(wasRadius, info->radius);
                        if (nowLo < wasLo - tol || nowHi > wasHi + tol)
                            return Resolved{ResolveState::Merged, {element}, {}};
                    }
            } else if (liesOnOutline(shape, element, ref.outline, territories)) {
                // Still on its own pick-time ground (inside its outline, on its plane): what was picked of it shrank
                // where it was (a cut took the part its territory points were on), and it did not move.
                changed = false;
            } else if (std::abs(now - then) <= 0.01 * then + kGrowthSlackQuanta) {
                // Its area kept within a hundredth: the face moved as a whole, turned or not (a body moved onto a mate,
                // tests/unit/doc transform_body; a cut's wall through the part moved along itself, holdout1 8:23).
                changed = false;
            } else if (info &&
                       (info->type == kernel::SurfaceType::Plane || info->type == kernel::SurfaceType::Cylinder)) {
                // And carried by the face's own shift (box centre to box centre): a face that moved as a whole holds
                // its picked territory moved with it (holdout1 8:23 face 1 holds both; a cut's arc wall whose visible
                // part changed as the cut moved, sorted 15:49 face 14, holds the first only).
                const auto nowBox = elementBox(shape, element);
                const Vec3 shift = nowBox ? minus(centreOf(*nowBox), centreOf(boxOf(ref.territory))) : Vec3{0, 0, 0};
                std::size_t landed = 0, shifted = 0;
                for (const auto& q : ref.territory.points) {
                    const Vec3 p = fromQuanta(q);
                    shifted += probe.on(Vec3{p.x + shift.x, p.y + shift.y, p.z + shift.z}) ? 1 : 0;
                    if (const auto onto = ontoSurface(*info, p))
                        landed += probe.on(*onto) ? 1 : 0;
                }
                changed = 2 * landed < n || 2 * shifted < n || !nowBox;
                // Where a feature of the pick was moved (and its move did not carry the pick onto the face, above), or
                // may
                // have been (a reference made before placements were recorded, to a face of a feature that has one
                // now):
                // a plane that moved along itself (its shift has a part in its plane, as a feature moved sideways does)
                // must be a rigid copy of itself: the same area and the same sorted extents, each within a hundredth. A
                // face the part clips differently where it is now is another patch of the feature's surface (T-081a
                // random 3:23 face 14: a cut's wall where it met the part's corner, the cut moved, its name on the
                // wall's
                // patch at the corner now). Where the pick's features are known to be where they were, the face was
                // moved by a resize, a depth or a direct edit, which may change its shape: no such demand (T-081b round
                // 4: 19 of 20 sampled faces this demand broke were the pick).
                bool placedNow = false;
                if (ref.placements.empty())
                    for (const Hash256& name : map.namesOf(element))
                        if (const TopoName* record = map.record(name))
                            placedNow = placedNow || map.placement(record->origin) != nullptr;
                const bool demandRigid =
                    move.kind == KnownMove::Kind::Moved || move.kind == KnownMove::Kind::Several || placedNow;
                if (!changed && demandRigid && nowBox && info->type == kernel::SurfaceType::Plane) {
                    const Vec3 a = info->axis;
                    const double along = shift.x * a.x + shift.y * a.y + shift.z * a.z;
                    const double tangential =
                        length(Vec3{shift.x - a.x * along, shift.y - a.y * along, shift.z - a.z * along});
                    if (tangential > 1e-2) {
                        const Box3 was = boxOf(ref.territory);
                        std::array<double, 3> da{was.max.x - was.min.x, was.max.y - was.min.y, was.max.z - was.min.z};
                        std::array<double, 3> db{nowBox->max.x - nowBox->min.x, nowBox->max.y - nowBox->min.y,
                                                 nowBox->max.z - nowBox->min.z};
                        std::sort(da.begin(), da.end());
                        std::sort(db.begin(), db.end());
                        changed = std::abs(now - then) > 0.01 * then + kGrowthSlackQuanta;
                        for (std::size_t i = 0; i < 3; ++i)
                            changed = changed || std::abs(da[i] - db[i]) > 0.01 * std::max(da[i], db[i]) + 2e-3;
                    }
                }
            } else {
                changed = std::abs(now - then) > 0.2 * then + kGrowthSlackQuanta;
            }
            if (changed)
                return brokenBecause("The picked face is no longer where it was picked, nor over that place along its "
                                     "surface; whether the face its name leads to is the one picked cannot be told.");
        } // Moved, and grown while a recorded sibling's names are gone: the pieces may have joined (they were apart
        // where the element was picked, and are one element where it is now). The names say the element continues the
        // pick; whether it also holds the siblings cannot be told, so it is not Exact (T-081a random 17:44).
        const Fingerprint print = map.fingerprintOf(element);
        if (element.kind != SubKind::Vertex &&
            static_cast<double>(print.qArea) > static_cast<double>(ref.hint.qArea) * (1.0 + 1e-6) + kGrowthSlackQuanta)
            for (const PickNeighbour& n : ref.pickNeighbours) {
                if (n.relation != PickNeighbour::Relation::Sibling)
                    continue;
                bool anyFound = false;
                for (const Hash256& name : n.names)
                    anyFound = anyFound || map.record(name) != nullptr;
                if (!anyFound)
                    return Resolved{ResolveState::Merged, {element}, {}};
            }
        // Moved (or what was picked of it is gone): its siblings must have moved with it.
        const auto now = elementBox(shape, element);
        if (!now)
            return std::nullopt;
        const Vec3 shift = minus(centreOf(*now), centreOf(boxOf(ref.territory)));
        for (const PickNeighbour& n : ref.pickNeighbours) {
            if (n.relation != PickNeighbour::Relation::Sibling || n.names.empty())
                continue;
            const auto other = oneElement(map, n.names);
            if (!other || *other == element || other->kind != element.kind)
                continue;
            // Siblings of the picked element's own name family are told apart by the names' discriminators (and their
            // fingerprints, needsHint); this rule is for pieces named apart, in different families (Linux 17:0).
            bool sameFamily = false;
            for (const Hash256& name : n.names)
                if (const TopoName* record = map.record(name))
                    sameFamily = sameFamily || std::find(ref.families.begin(), ref.families.end(), record->family) !=
                                                   ref.families.end();
            if (sameFamily)
                continue;
            const auto otherNow = elementBox(shape, *other);
            if (!otherNow)
                continue;
            const Vec3 otherShift = minus(centreOf(*otherNow), centreOf(boxOf(n.territory)));
            const double tol = 2e-3 + 1e-6 * std::max(length(shift), length(otherShift));
            // A sibling that stayed where it was is not evidence of a swap (an edit moved one side only).
            if (length(otherShift) <= tol)
                continue;
            if (length(minus(shift, otherShift)) > tol)
                return brokenBecause("The picked element was one of several pieces, and the pieces no longer lie as "
                                     "they did when it was picked; which piece is which cannot be told.");
        }
        return std::nullopt;
    } catch (...) {
        return brokenBecause("The reference could not be checked because of an internal failure.");
    }
}

bool piecesFitEvidence(const ElementMap& map, const kernel::Shape& shape, const std::vector<SubId>& pieces,
                       const TopoRef& ref, std::string& why)
{
    (void)map;
    try {
        if (!ref.territory.known() || pieces.empty())
            return true;
        const Box3 picked = boxOf(ref.territory);
        Box3 all{};
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto b = elementBox(shape, pieces[i]);
            if (!b) {
                why = "The picked element came apart, and a piece cannot be read.";
                return false;
            }
            if (i == 0) {
                all = *b;
            } else {
                all.min = {std::min(all.min.x, b->min.x), std::min(all.min.y, b->min.y), std::min(all.min.z, b->min.z)};
                all.max = {std::max(all.max.x, b->max.x), std::max(all.max.y, b->max.y), std::max(all.max.z, b->max.z)};
            }
        }
        const double tol = extentTolerance(picked);
        if (all.max.x - all.min.x > picked.max.x - picked.min.x + tol ||
            all.max.y - all.min.y > picked.max.y - picked.min.y + tol ||
            all.max.z - all.min.z > picked.max.z - picked.min.z + tol) {
            why = "The picked element came apart, and its pieces together reach further than it did when it was "
                  "picked.";
            return false;
        }
        for (const SubId& piece : pieces) {
            const Probe probe(shape, piece);
            for (const PickNeighbour& n : ref.pickNeighbours)
                if (n.territory.known() && probe.count(n.territory) > 0) {
                    why = "The picked element came apart, and a piece lies over another element's place.";
                    return false;
                }
        }
        return true;
    } catch (...) {
        why = "The reference could not be checked because of an internal failure.";
        return false;
    }
}

std::vector<SubId> piecesInsideMovedPick(const ElementMap& map, const kernel::Shape& shape,
                                         const std::vector<SubId>& pieces, const TopoRef& ref)
{
    try {
        // Edges only: a face piece outside the carried pick may be new ground the moved face now reaches (a face the
        // part
        // clips otherwise where it is now), which is the pick's (holdout1-tuned 11:36 face 19).
        if (pieces.size() < 2 || !ref.territory.known() || pieces.front().kind != SubKind::Edge)
            return pieces;
        const KnownMove move = knownMove(map, ref);
        if (move.kind != KnownMove::Kind::Moved)
            return pieces;
        const Box3 was = boxOf(ref.territory);
        const double tol = extentTolerance(was);
        const bool outline = pieces.front().kind == SubKind::Face && ref.outline.known();
        const TerritoryCache territories(shape);
        std::vector<SubId> kept;
        for (const SubId& piece : pieces) {
            const Territory& t = territories.of(piece);
            bool outside = t.known();
            for (const auto& q : t.points) {
                const Vec3 back = carry(move.to, move.from, fromQuanta(q));
                const bool in = outline ? insidePickOutline(ref.outline, back)
                                        : back.x >= was.min.x - tol && back.x <= was.max.x + tol &&
                                              back.y >= was.min.y - tol && back.y <= was.max.y + tol &&
                                              back.z >= was.min.z - tol && back.z <= was.max.z + tol;
                outside = outside && !in;
            }
            if (!outside)
                kept.push_back(piece);
        }
        return kept.empty() ? pieces : kept;
    } catch (...) {
        return pieces;
    }
}

} // namespace ray::naming::detail
