// The resolver's rules on pick-time evidence (territory.h, naming.h TopoRef::territory and pickNeighbours).
#include "territory.h"

#include <algorithm>
#include <cmath>
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

} // namespace

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
        // An Exact answer says the element is all of what was picked. The picked element's own territory is points
        // that only it held at pick time; where one of them now lies on another element of the same kind on the
        // answer's carrier, part of what was picked belongs to that element, and the answer is not all of it
        // (T-081a holdout4 9:9: a slot moved past the boundary of the face piece picked beside it, and the piece on
        // the slot's far side took a strip of the picked piece). Whether that element now continues the pick (the
        // pick came apart) or the strip is now its own cannot be told from names or territory: Broken. Points that
        // lie on nothing are territory the element lost (something cut into it), which an Exact answer allows.
        if (own > 0) {
            if (own < ref.territory.points.size() && element.kind != SubKind::Vertex) {
                std::vector<Vec3> lost;
                for (const auto& q : ref.territory.points)
                    if (!probe.on(fromQuanta(q)))
                        lost.push_back(fromQuanta(q));
                const std::uint32_t n = shape.count(element.kind);
                for (std::uint32_t i = 0; i < n; ++i) {
                    const SubId other{element.kind, i};
                    if (other == element || !territories.sameCarrier(element, other))
                        continue;
                    const Probe there(shape, other);
                    for (const Vec3& p : lost)
                        if (there.on(p))
                            return brokenBecause("Part of where the picked element was is now another element on the "
                                                 "same surface; whether the picked element came apart cannot be "
                                                 "told.");
                }
            }
            return std::nullopt;
        }
        // The element holds none of its own territory: it moved (or what was picked of it is gone), and where it was
        // picked says nothing about where its parts went. If it now runs on, with nothing geometric between them, into
        // another element of the same kind (same carrier, and for an edge the same surfaces on both sides), that
        // element may hold part of what moved: the answer is not known to be all of the pick (T-081a holdout4 5:9: a
        // cut moved by its width put its wall on the plane of another cut's start face, and the edge where that wall
        // meets the bottom ran on, past a vertex, as the edge of the other cut's face). Not when a recorded neighbour
        // touched the picked element at pick time: it was already one stretch of a longer run (the two arcs of one
        // hole's rim), and the run may simply have moved with it.
        if (element.kind != SubKind::Vertex && !pickTouchedANeighbour(ref))
            for (const SubId& other : touchingElements(shape, element))
                if (continuesSeamlessly(shape, territories, element, other))
                    return brokenBecause("The picked element moved and now runs on, with nothing between them, into "
                                         "another element on the same surfaces; which part is the picked one cannot be "
                                         "told.");
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
