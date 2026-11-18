// Private to src/naming and its tests (task T-007): how nameImported names a history-free body.
//
// The rule (PLAN.md 3.3 rules 3, 4 and 5, read for a body without history):
//  - Face: (import feature, "import.face", parents = [hash of its quantised fingerprint]). A history-free face
//    was made from nothing but its geometry, so its fingerprint stands where the parents of a generated face
//    stand. The family of a face is therefore "faces with exactly this fingerprint".
//  - Edge: (import feature, "import.edge", parents = sorted names of the faces that contain it); vertex:
//    (import feature, "import.vertex", parents = sorted names of its faces) (rule 3). An edge or vertex
//    on no face falls back to the hash of its own fingerprint.
//  - Siblings (same family) are told apart by a discriminator (rule 4): for a face, the labels of its adjacent
//    faces (a neighbour's name if it is alone in its family, else its family id; sorted, repeats kept); for an
//    edge, the names of the faces at its end vertices that are not among its parents; for a vertex, the names of
//    the faces at the far ends of its edges that are not among its parents.
//  - Only siblings with identical discriminators get an ordinal, 1-based, by fingerprint (centroid, area,
//    axis, ...), then for edges by start and end point. Such a name is ambiguous(). siblingCount is the size
//    of the family and is hashed into the name.
//  - Elements that tie on everything (family, discriminator, fingerprint, end points) get the SAME ordinal and
//    so the same name; ElementMap::find() returns nothing for a name held by several elements. Nothing in a
//    name depends on the order in which the kernel lists sub-shapes.
//  - Solids are not named.
#pragma once
#include "naming_internal.h"

#include <functional>

namespace ray::naming::detail {

struct FaceFacts {
    std::uint32_t index{};
    Fingerprint fingerprint{};
    std::vector<std::uint32_t> neighbours; // adjacent faces (sharing an edge)
};
struct EdgeFacts {
    std::uint32_t index{};
    Fingerprint fingerprint{};
    std::array<std::int64_t, 6> ends{}; // quantised start and end point, the last tie-break for ordinals
    std::vector<std::uint32_t> faces;   // faces containing the edge
    std::vector<std::uint32_t> vertices;
};
struct VertexFacts {
    std::uint32_t index{};
    Fingerprint fingerprint{};
    std::vector<std::uint32_t> faces; // faces containing the vertex
    std::vector<std::uint32_t> edges;
};
// Everything nameImported needs to know about a body. The vectors are in VISITING order (see VisitOrder);
// every SubId index of each kind appears exactly once.
struct BodyFacts {
    std::uint32_t faceCount{}, edgeCount{}, vertexCount{};
    std::vector<FaceFacts> faces;
    std::vector<EdgeFacts> edges;
    std::vector<VertexFacts> vertices;
};

// The order in which the SubId indices of one kind are visited: a permutation of 0..count-1. Production code
// visits in canonical order (an empty function). Tests pass permutations to prove the names do not depend on
// it.
using VisitOrder = std::function<std::vector<std::uint32_t>(kernel::SubKind kind, std::uint32_t count)>;

// Asks the kernel about every face, edge and vertex. KernelFailure if a query fails; InvalidArgument if
// `order` does not return a permutation.
Result<BodyFacts> gatherFacts(const kernel::Shape& shape, const VisitOrder& order);

// The names, from facts alone (no kernel calls). InvalidArgument if the facts are inconsistent.
Result<ElementMap> assignNames(const FeatureId& importFeature, const BodyFacts& facts);

// nameImported with a visiting order. Returns an empty map (and logs why) if anything fails.
ElementMap nameImported(const FeatureId& importFeature, const kernel::Shape& shape, const VisitOrder& order);

} // namespace ray::naming::detail
