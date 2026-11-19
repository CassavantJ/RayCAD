// Private to src/naming and its tests (task T-007). Not a public interface.
//
// The storage behind ElementMap, the hashes that make a TopoName's id and family, and fingerprint helpers.
// Everything here is deterministic (PLAN.md section 8): no <cmath> trig, no unordered containers, no pointer
// values; every table is a vector sorted by a stable key.
#pragma once
#include <ray/naming.h>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ray::naming {

// The table behind ElementMap. Immutable once finished (detail::finishMap).
struct ElementMap::Impl {
    struct Element {
        kernel::SubId sub;
        Fingerprint fingerprint;
        std::vector<Hash256> names; // one or more; the first is the primary name
    };
    std::vector<Element> elements; // sorted by sub, no duplicates
    std::vector<TopoName> records; // sorted by id, no duplicates; every one is held by at least one element

    // Derived by detail::finishMap, never serialised:
    // name -> element, for names held by EXACTLY one element. A name held by several elements (they could not
    // be told apart by anything but sub-shape order) is deliberately absent, so find() never picks one of them.
    std::vector<std::pair<Hash256, kernel::SubId>> byName; // sorted by name
    // family -> member names issued in this map, sorted.
    std::vector<std::pair<Hash256, std::vector<Hash256>>> families; // sorted by family
};

namespace detail {

// TopoName::id: the hash of kind, origin, role, parents, discriminator, ordinal and siblingCount.
Hash256 nameId(const TopoName& name);
// TopoName::family: the hash of kind, origin, role and parents.
Hash256 familyId(kernel::SubKind kind, const FeatureId& origin, std::string_view role,
                 std::span<const Hash256> parents);

// A discriminator made in propagate()'s fallback mode, from the names of ALL the faces around a sibling (rule 4,
// propagate.h), carries this extra entry; one made from the faces of the feature that issued the siblings does not.
// So a discriminator of one mode never equals one of the other mode: a sibling whose faces of the current feature
// are, say, {A} and another sibling whose faces of every feature are {A} get different names (spike S4 V34 and the
// T-081a random sequence 19:1, where the two pieces of a cut wall swapped names this way).
Hash256 allFacesDiscriminatorMark();

// Does the name tell its element from its siblings only by surroundings that need not move with it: an ordinal (by
// fingerprint), or a discriminator made from all the faces around it (allFacesDiscriminatorMark)? Faces at the ends
// of an edge, or the neighbours a piece happens to touch, change with unrelated edits, and a sibling can end up with
// exactly the surroundings another sibling had (T-081a random 18:48: two lines where a moved boss pierces a
// cylinder, told apart by the faces they end on, swapped). Every edge or vertex among siblings counts as such: the
// pieces of an edge keep their end faces while the feature between them moves and they trade territory (random
// 9:41). Such a name is trusted only while the element's
// fingerprint still matches the reference's hint, like an ambiguous name (PLAN.md 3.3 rule 4). A name alone in its
// family, or told apart by faces of the feature that issued the siblings (the walls of the groove that split a
// face), is not.
bool needsHint(const TopoName& name);

// How much a piece's quantised size (Fingerprint::qArea: area or length) may exceed its hint before it counts as
// grown: a few quanta of rounding (1e-4 mm, 1e-4 mm^2 each).
inline constexpr double kGrowthSlackQuanta = 20.0;

// Checks the table (sorted, every name has a record, ids and families recomputed, kinds agree) and builds
// the derived indexes. A table that breaks a rule is an Internal error, or FormatError when `fromFile`.
Result<ElementMap> finishMap(ElementMap::Impl table, bool fromFile);

// ---------- Fingerprints ----------
// Quantisation steps (naming.h): 1e-4 mm for lengths, areas and positions, 1e-6 for unit-vector components.
inline constexpr double kLengthQuantaPerMm = 10000.0;
inline constexpr double kAxisQuanta = 1000000.0;

// round(value * quantaPerUnit), clamped to +-2^60 so differences of two quantised values cannot overflow.
// NaN gives 0.
std::int64_t quantise(double value, double quantaPerUnit) noexcept;

// Fingerprints of the three kinds. `neighbourCount` is the number of faces around the element: adjacent faces
// for a face, the faces containing an edge or a vertex.
//  - face: surface type; centroid; area; radius; axis = outward normal for planes, otherwise the surface axis
//    with a canonical sign (first non-zero quantised component positive).
//  - edge: the curve type stored in `surface` (numeric value of kernel::CurveType); position = circle or
//    ellipse centre for a closed conic, else the midpoint of start and end; qArea = length; radius; axis with
//    a canonical sign (zero for B-splines and others).
//  - vertex: surface Other; position = the point; everything else zero.
Fingerprint faceFingerprint(const kernel::SurfaceInfo& info, std::uint16_t neighbourCount) noexcept;
Fingerprint edgeFingerprint(const kernel::CurveInfo& info, std::uint16_t neighbourCount) noexcept;
Fingerprint vertexFingerprint(const Vec3& point, std::uint16_t neighbourCount) noexcept;

// The fingerprint of an element that has none (unknown SubId): surface Other, everything zero.
Fingerprint emptyFingerprint() noexcept;

// Hash of every field of a fingerprint (its exact quantised values).
Hash256 fingerprintKey(const Fingerprint& fingerprint);

// Order used for ordinals (PLAN.md 3.3 rule 4): centroid, then area, then axis, then the remaining fields.
std::strong_ordering ordinalOrder(const Fingerprint& a, const Fingerprint& b) noexcept;

} // namespace detail
} // namespace ray::naming
