// Private to src/naming and its tests (task T-084): how rematchImported carries the names of an imported body over to
// a new revision of it (naming.h, "Re-import or replace-geometry"; PLAN.md 3.3 rule 5).
//
// THE DECISION, per kind (vertices, edges, faces), never from the order in which a shape lists its sub-shapes:
//  - UNCHANGED: an old element and a new element that are the same geometry, and no other element of either shape is.
//    With the old shape (planWithShape): a vertex at the same point (kRematchTolerance); an edge of the same curve type
//    on the same carrier (line: direction and position; circle: centre, axis, radius; any other curve: compared by its
//    points only), of the same length, between the matching vertices, every territory point of each (territory.h)
//    on the other; a face of the same surface type on the same carrier (plane: outward normal and offset; cylinder:
//    axis line, radius and material side; any other surface: centroid and area, and its mesh nodes on the other), bounded
//    by exactly the matching edges, every territory point and sample of each on the other. Matches must be one to one:
//    when an element has two candidates (two holes that coincide within the tolerance), neither is matched. An edge or
//    vertex is unchanged only if, besides, every face it lay on carries over to a face it lies on now; otherwise it is
//    the same geometry with other faces around it, and counts as changed.
//    Without the old shape (planByFingerprint): an element whose fingerprint (Fingerprint::matches) has exactly one
//    match on each side, and (faces) whose neighbours, read from the edge names (an imported edge is named from its
//    faces), match likewise; an edge or vertex whose faces match. Nothing is "changed" in this mode.
//  - CHANGED: old and new elements that are not unchanged but lie on the same carrier (plane or cylinder; line or
//    circle) and share ground: an interior sample of one (at least kRematchMargin inside it) lies on the other. These
//    pairs form groups (connected components). A group of one old element and one new one (a face that grew or shrank),
//    of several old and one new (faces that became one), or of one old and several new (a face cut in pieces) carries
//    the old names: the new element keeps its own new name AND takes the old ones, so a reference to the old element
//    is not Exact: the resolver answers Merged (one new element) or Split (the pieces, named "split" from the old
//    names) and checks that answer against the reference's pick-time evidence, as for any model edit (Broken when the
//    evidence objects). A group of several old and several new elements is ambiguous and carries nothing.
//  - REMOVED: an old element in neither: its names are gone (Deleted), and a reference to it resolves Broken (or
//    Repaired, a suggestion, when one element still fits its fingerprint and neighbours).
//  - NEW: a new element in neither: only its own name.
// A new element's own name is nameImported's for the new shape; when that equals any name of the old map (a congruent
// face in place, such as a square pocket floor turned 45 degrees, has the same fingerprint and so the same imported
// name) it is made distinct (a mark in its discriminator), so an old reference never finds a changed element by its
// own new name. A name that several old elements shared (ElementMap: such a name finds nothing) is carried only when
// every element holding it is unchanged, so a reference that could not be resolved before cannot pick one of them now.
// No rigid move of the whole part is looked for: a part that moved has moved faces.
#pragma once
#include "naming_internal.h"

#include <array>
#include <string>

namespace ray::naming::detail {

// Same point, length or radius, mm.
inline constexpr double kRematchTolerance = 1e-3;
// Axes parallel: 1 - |cos| at most this (about 4.5e-5 rad).
inline constexpr double kRematchParallel = 1e-9;
// How far inside an element (from its boundary) a sample must lie to show that two elements share ground, mm.
inline constexpr double kRematchMargin = 0.02;

// The decision for one kind. Indices are SubId indices of that kind in the old and the new shape.
struct KindPlan {
    std::vector<std::int64_t> same; // per old index: the new index of the unchanged element, or -1
    struct Group {
        std::vector<std::uint32_t> olds, news; // sorted
    };
    std::vector<Group> groups; // changed elements that carry names (1:1, n:1, 1:n); ambiguous groups are left out
};
struct RematchPlan {
    std::array<KindPlan, 3> kinds; // vertex, edge, face (kernel::SubKind order)
};

// The decision with the old shape (the shape the old map was made for). Error when a shape cannot be read.
Result<RematchPlan> planWithShape(const kernel::Shape& oldShape, const kernel::Shape& replacement);
// The decision from the two maps alone (no old shape).
Result<RematchPlan> planByFingerprint(const ElementMap& old, const ElementMap& natural);
// The names of the new shape from a plan: see the rules above. `natural` is nameImported's map of the new shape.
Result<RematchResult> assembleRematch(const ElementMap& old, const ElementMap& natural, const RematchPlan& plan,
                                      const FeatureId& feature);

// The discriminator entry that makes a new element's own name distinct from the old names.
Hash256 rematchMark();

} // namespace ray::naming::detail
