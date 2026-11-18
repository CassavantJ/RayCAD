// Resolver stages 2 to 4 (task T-081a): following names through the current build. See propagate.h.
#include "propagate.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
    return record != nullptr && record->ambiguous();
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

// Can element `c` contain the element the reference was picked on? Used before a family or split-parent rule
// answers Merged: the rule says the pieces re-joined, but the same names also appear when a piece vanished and
// the rest of the element is left whole. This keeps Merged to elements of the same type on the same carrier, at
// least as large, with the picked element's position on them. Only planes, cylinders, lines and full circles
// are checked; anything else is not given Merged.
bool canContain(const ElementMap& map, const kernel::Shape& shape, SubId c, const Fingerprint& hint)
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
        if (info->type == kernel::SurfaceType::Plane)
            return std::abs(dot(d, axis)) <= kContainTolerance;
        if (info->type == kernel::SurfaceType::Cylinder) {
            const double along = dot(d, axis);
            const Vec3 radial{d.x - along * axis.x, d.y - along * axis.y, d.z - along * axis.z};
            return norm(radial) <= info->radius + kContainTolerance;
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
        for (const SubId& s : elements)
            if (!neighboursStillAround(map, shape, s, ref))
                return broken("The picked element no longer lies on the faces it was picked on.");
        if (elements.size() == 1 && !all.split) {
            for (const Hash256& name : map.namesOf(elements.front()))
                if (refNames.count(name) == 0) {
                    if (!surroundedLikePick(elements.front()))
                        return broken("The picked edge was joined with another one that does not lie on the same "
                                      "faces.");
                    return make(ResolveState::Merged, std::move(elements));
                }
            if (namesFound < ref.names.size())
                return make(ResolveState::Split, std::move(elements));
            return std::nullopt; // the Exact stage's business
        }
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
            if (wasAlone && members.size() > 1)
                return make(ResolveState::Split, std::move(elements));
            if (!wasAlone && members.size() == 1 && ref.kind != SubKind::Vertex &&
                canContain(map, shape, elements.front(), ref.hint) && surroundedLikePick(elements.front()))
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
            if (c.kind == ref.kind && canContain(map, shape, c, ref.hint) && surroundedLikePick(c))
                return make(ResolveState::Merged, {c});
        }
    }

    // ---- stage 4: Repaired ----
    return repaired(map, shape, ref);
}

} // namespace

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
