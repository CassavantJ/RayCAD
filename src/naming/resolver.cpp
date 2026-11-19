// The resolver (task T-007): Exact and Broken. See resolver.h for the stages; Split, Merged and Repaired (task
// T-081a) run between the Exact stage and the final Broken, in propagate_resolve.cpp.
#include "resolver.h"

#include "naming_internal.h"
#include "propagate.h"
#include "territory.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace ray::naming {

bool Resolved::usable() const
{
    if (elements.empty())
        return false;
    // Repaired is a suggestion until the user confirms it (TopoRef::repairConfirmed, copied by resolve()).
    return state == ResolveState::Exact || state == ResolveState::Split || state == ResolveState::Merged ||
           (state == ResolveState::Repaired && repairConfirmed);
}

bool Resolved::postableWithoutReview() const
{
    if (elements.empty())
        return false;
    return state == ResolveState::Exact || (state == ResolveState::Repaired && repairConfirmed);
}

namespace {

// What the names of a reference map to in the current element map.
struct Lookup {
    std::vector<kernel::SubId> found;    // the element of every name that was found, sorted, unique
    std::vector<std::size_t> foundNames; // positions in ref.names of the names that were found
    std::size_t missing = 0;             // names not issued in this build
    std::size_t shared = 0;              // names issued, but held by several elements that cannot be told apart
};

Resolved broken(std::string reason)
{
    return Resolved{ResolveState::Broken, {}, std::move(reason)};
}

class MapResolver final : public IResolver {
public:
    explicit MapResolver(std::vector<ResolverBody> bodies) : bodies_(std::move(bodies))
    {
        // Sort by id; keep the first listed body of each id.
        std::stable_sort(bodies_.begin(), bodies_.end(),
                         [](const ResolverBody& a, const ResolverBody& b) { return a.id < b.id; });
        bodies_.erase(std::unique(bodies_.begin(), bodies_.end(),
                                  [](const ResolverBody& a, const ResolverBody& b) { return a.id == b.id; }),
                      bodies_.end());
        for (const ResolverBody& body : bodies_)
            territories_.push_back(std::make_unique<detail::TerritoryCache>(body.shape));
    }

    Resolved resolve(const TopoRef& ref) const override
    {
        Resolved out;
        try {
            out = resolveStages(ref);
        } catch (...) {
            out = broken("The reference could not be resolved because of an internal failure.");
        }
        out.repairConfirmed = ref.repairConfirmed;
        return out;
    }

    TopoRef makeRef(BodyId body, kernel::SubId sub) const override
    {
        TopoRef ref;
        ref.body = body;
        ref.kind = sub.kind;
        ref.hint = detail::emptyFingerprint();
        try {
            fillRef(ref, sub);
        } catch (...) {
            // An empty reference resolves Broken, never to some element.
            TopoRef empty;
            empty.body = body;
            empty.kind = sub.kind;
            empty.hint = detail::emptyFingerprint();
            return empty;
        }
        return ref;
    }

private:
    const ResolverBody* findBody(const BodyId& id) const
    {
        const auto it = std::lower_bound(bodies_.begin(), bodies_.end(), id,
                                         [](const ResolverBody& b, const BodyId& i) { return b.id < i; });
        return it != bodies_.end() && it->id == id ? &*it : nullptr;
    }

    const detail::TerritoryCache& territoriesOf(const ResolverBody* body) const
    {
        return *territories_[static_cast<std::size_t>(body - bodies_.data())];
    }

    static bool wellFormed(const TopoRef& ref)
    {
        const std::size_t n = ref.names.size();
        return n > 0 && (ref.families.empty() || ref.families.size() == n) &&
               (ref.ambiguous.empty() || ref.ambiguous.size() == n);
    }

    static Lookup lookUp(const ElementMap& map, const TopoRef& ref)
    {
        Lookup out;
        for (std::size_t i = 0; i < ref.names.size(); ++i) {
            if (const auto sub = map.find(ref.names[i])) {
                out.found.push_back(*sub);
                out.foundNames.push_back(i);
            } else if (map.record(ref.names[i]) != nullptr) {
                ++out.shared;
            } else {
                ++out.missing;
            }
        }
        std::sort(out.found.begin(), out.found.end());
        out.found.erase(std::unique(out.found.begin(), out.found.end()), out.found.end());
        return out;
    }

    // Stage 1. A value is final (Exact, or Broken because an ambiguous name no longer matches its hint);
    // nullopt passes the reference on to the next stage.
    static std::optional<Resolved> exact(const ResolverBody& body, const TopoRef& ref, const Lookup& lookup)
    {
        const ElementMap& map = body.map;
        if (lookup.found.size() != 1 || lookup.missing != 0 || lookup.shared != 0)
            return std::nullopt;
        const kernel::SubId element = lookup.found.front();
        if (element.kind != ref.kind)
            return std::nullopt;
        // An element that also holds a name the reference does not have absorbed another element (T-081a): Merged.
        for (const Hash256& name : map.namesOf(element))
            if (std::find(ref.names.begin(), ref.names.end(), name) == ref.names.end())
                return std::nullopt;
        bool ambiguous = false;
        bool sibling = false;
        for (const std::size_t i : lookup.foundNames) {
            const TopoName* record = map.record(ref.names[i]);
            ambiguous = ambiguous || (record != nullptr && detail::needsHint(*record)) ||
                        (i < ref.ambiguous.size() && ref.ambiguous[i]);
            sibling = sibling || (record != nullptr && record->siblingCount > 1);
        }
        const Fingerprint now = map.fingerprintOf(element);
        // An edge or face left as a stub of under 1e-3 mm (or mm^2) and a thousandth of its picked size is what
        // remains where a cut took the element away, not the element (T-081a random 5:43: a 9.5 mm edge kept its name
        // on a stub of a micron at its end).
        if (element.kind != kernel::SubKind::Vertex && now.qArea < 10 && now.qArea * 1000 < ref.hint.qArea)
            return broken("Only a vanishingly small stub of the picked element is left.");
        if (ambiguous && !now.matches(ref.hint))
            return broken("The picked element could only be told apart from similar elements by its surroundings or "
                          "its geometry, and its geometry has changed.");
        // A piece told apart from its siblings by the faces of the feature that made them (T-081a): it may shrink
        // (something cut into it), but a piece that grew may have taken in what was a sibling's when it was picked
        // (random sequence 9:7: the separating cut no longer ran between two pieces of an edge, and one of them took
        // in most of the other). Whether the extra is new territory or a sibling's cannot be told from the names.
        if (sibling && element.kind != kernel::SubKind::Vertex && !now.matches(ref.hint) &&
            static_cast<double>(now.qArea) >
                static_cast<double>(ref.hint.qArea) * (1.0 + 1e-6) + detail::kGrowthSlackQuanta)
            return broken("The picked element was one of several pieces and has grown since; it may now include what "
                          "was a separate piece when it was picked.");
        // A piece that shrank must still lie where it was picked: its picked centroid inside its outline (something cut
        // into it, as in spike S4 V34). One that moved away is the sibling the faces now fit (random 16:14: the
        // pieces of a boss wall split by a slot, after the boss moved twice by its width), or a piece whose old centre
        // went to a sibling. Only planar faces are checked; any other piece whose geometry changed is Broken.
        if (sibling && element.kind == kernel::SubKind::Face && !now.matches(ref.hint) &&
            !detail::holdsHintCentre(body.shape, element, ref.hint))
            return broken("The picked element was one of several pieces and no longer lies where it was picked.");
        // An edge or vertex whose faces are no longer the ones it was picked on is another element (T-081a).
        if (!detail::onPickedFaces(map, body.shape, element, ref))
            return broken("The picked element no longer lies on the faces it was picked on.");
        if (element.kind == kernel::SubKind::Edge && !now.matches(ref.hint) &&
            detail::edgeContinuesUnderOtherName(map, body.shape, element))
            return broken("One of the picked edge's faces was split, and the edge runs on under another name; which "
                          "stretches are the picked edge cannot be told.");
        // A face that grew may have taken in a face of another feature on the same surface whose name does not
        // survive the edit (T-081a random 6:29); the names cannot tell that from a dimension change, which must stay
        // Exact (T-096). The pick-time evidence can: territory that a recorded neighbour held (territory.h).
        if (auto evidence = detail::exactAgainstEvidence(map, body.shape, element, ref))
            return evidence;
        return Resolved{ResolveState::Exact, {element}, {}};
    }

    static std::string brokenReason(const Lookup& lookup, const TopoRef& ref)
    {
        if (lookup.found.size() > 1)
            return "The picked element now corresponds to several elements.";
        if (lookup.found.size() == 1 && lookup.found.front().kind != ref.kind)
            return "The picked element's name now belongs to an element of another kind.";
        if (lookup.shared > 0)
            return "The picked element can no longer be told apart from an identical element.";
        if (!lookup.found.empty())
            return "Part of the picked element no longer exists.";
        return "The picked element no longer exists.";
    }

    Resolved resolveStages(const TopoRef& ref) const
    {
        const ResolverBody* body = findBody(ref.body);
        if (body == nullptr)
            return broken("The body of the picked element no longer exists.");
        if (!wellFormed(ref))
            return broken("The reference is damaged: it holds no usable name.");
        const Lookup lookup = lookUp(body->map, ref);
        if (auto result = exact(*body, ref, lookup))
            return std::move(*result);
        // T-081a, in this order: lineage (Split, Merged), the family rule (Split, Merged), Repaired.
        if (auto result = detail::resolveThroughBuild(body->map, body->shape, ref))
            return std::move(*result);
        return broken(brokenReason(lookup, ref));
    }

    void fillRef(TopoRef& ref, kernel::SubId sub) const
    {
        const ResolverBody* body = findBody(ref.body);
        if (body == nullptr)
            return;
        const ElementMap& map = body->map;
        for (const Hash256& name : map.namesOf(sub)) {
            const TopoName* record = map.record(name);
            ref.names.push_back(name);
            ref.families.push_back(record != nullptr ? record->family : Hash256{});
            ref.ambiguous.push_back(record != nullptr && record->ambiguous());
            // A piece of an element that held several names has several parents (T-081a): all of them.
            if (record != nullptr && record->role == "split")
                ref.splitParents.insert(ref.splitParents.end(), record->parents.begin(), record->parents.end());
        }
        if (ref.names.empty())
            return;
        // The faces around the element: adjacent faces of a face, the faces containing an edge or a vertex.
        // Every name of each of them (T-081a): a merged face's first name is arbitrary.
        for (const kernel::SubId face : kernel::adjacent(body->shape, sub, kernel::SubKind::Face)) {
            const std::span<const Hash256> names = map.namesOf(face);
            ref.neighbourNames.insert(ref.neighbourNames.end(), names.begin(), names.end());
        }
        std::sort(ref.neighbourNames.begin(), ref.neighbourNames.end());
        ref.neighbourNames.erase(std::unique(ref.neighbourNames.begin(), ref.neighbourNames.end()),
                                 ref.neighbourNames.end());
        ref.hint = map.fingerprintOf(sub);
        // Pick-time evidence: where the element lies, and the elements it could later be confused with or grow over.
        const detail::TerritoryCache& territories = territoriesOf(body);
        ref.territory = territories.of(sub);
        ref.pickNeighbours = detail::pickNeighboursOf(map, body->shape, sub, territories);
    }

    std::vector<ResolverBody> bodies_;                                 // sorted by id, unique
    std::vector<std::unique_ptr<detail::TerritoryCache>> territories_; // parallel to bodies_
};

} // namespace

std::unique_ptr<IResolver> makeResolver(std::vector<ResolverBody> bodies)
{
    try {
        return std::make_unique<MapResolver>(std::move(bodies));
    } catch (...) {
        return nullptr; // out of memory
    }
}

} // namespace ray::naming
