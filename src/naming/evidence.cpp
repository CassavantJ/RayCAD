// The resolver's rules on pick-time evidence (territory.h, naming.h TopoRef::territory and pickNeighbours).
#include "territory.h"

#include <algorithm>
#include <cmath>
#include <memory>
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
// midpoint lies at r cos(half) from the centre, on the middle's side for an arc up to a half circle.
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

    // Does edge `info` (on the same line or circle) lie inside the place?
    bool holds(const kernel::CurveInfo& info, const Box3& edgeBox) const
    {
        if (line)
            return insideBox(edgeBox, box, tol);
        const Vec3 m =
            scaled(Vec3{info.start.x + info.end.x, info.start.y + info.end.y, info.start.z + info.end.z}, 0.5);
        const auto mid = arcMiddle(centre, axis, m, info.length / radius, radius);
        if (!mid)
            return false;
        const Vec3 middlePoint{centre.x + radius * mid->x, centre.y + radius * mid->y, centre.z + radius * mid->z};
        return onArc(info.start) && onArc(info.end) && onArc(middlePoint);
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
        if (!place->holds(*info, *eBox) || namedAtPick(map, e, ref) ||
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
        // Moved, and grown while a recorded sibling's names are gone: the pieces may have joined (they were apart
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

} // namespace ray::naming::detail
