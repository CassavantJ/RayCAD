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

} // namespace

std::optional<Resolved> exactAgainstEvidence(const ElementMap& map, const kernel::Shape& shape, SubId element,
                                             const TopoRef& ref)
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
