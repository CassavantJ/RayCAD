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
