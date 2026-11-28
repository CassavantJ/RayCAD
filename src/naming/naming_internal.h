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
#include <optional>
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
    std::vector<Element> elements;            // sorted by sub, no duplicates
    std::vector<TopoName> records;            // sorted by id, no duplicates; every one is held by at least one element
    std::vector<FeaturePlacement> placements; // sorted by feature, no duplicates (T-081b round 4)

    // Kept by propagate() in memory for pick-time evidence (T-081b round 9), never serialised (naming.h
    // ElementMap::serialize):
    // The records of names no element holds any more that held split pieces came from (a split record's parents, and
    // theirs, as far as the input maps knew them), sorted by id, no duplicates: what a piece lies on (whose feature
    // made the face it was split from) stays known after the face it was split from is gone as a whole.
    std::vector<TopoName> ancestors;
    // The rigid moves of the whole body (NamingInput::bodyPlacement), oldest first, each with the features whose
    // elements it carried (every origin of a record or ancestor of its input maps), sorted.
    struct BodyMove {
        FeaturePlacement placement;
        std::vector<FeatureId> carried;
    };
    std::vector<BodyMove> bodyMoves;
    // The rigid moves of single faces by direct edits (NamingInput::faceMoves, T-081b round 10), oldest first: each the
    // move (a FeaturePlacement's quanta, `placement.feature` the edit) and every name the face had in the edit's input,
    // sorted. A later body move conjugates the earlier face moves by itself (propagate), so that composing face moves
    // after body moves and placements gives where the face is.
    struct FaceMoveRecord {
        FeaturePlacement placement;
        std::vector<Hash256> names;
    };
    std::vector<FaceMoveRecord> faceMoves;
    // One-to-one history (T-081b round 11), kept in memory like the above, never serialised (a map read back has none:
    // nothing is identity by history on it):
    // Each face's RUN of one-to-one history: the feature whose operation last made the face otherwise than one to one
    // (issued its names: a sweep's face, an imported face, a blend's, a piece; or merged faces into it, or generated
    // something into it), every operation after it having carried the face one to one (the kernel's history: Modified
    // from that one input face to that one result face, from no other input element, nothing Generated into it;
    // propagate). Sorted by face, faces only; a face not listed has no known run (a map read back, a rematch).
    std::vector<std::pair<kernel::SubId, FeatureId>> runs;
    // The names of blend faces each the only face its blend generated from its one input edge, that edge given to the
    // blend by a reference that resolved Exact in this build (NamingInput::exactInputs); kept by later maps while held;
    // sorted, unique.
    std::vector<Hash256> wholeBlends;

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

// Below this extent, mm, an element has none: an edge this long or shorter, a face no wider (its area over its box
// diagonal) is of zero measure within the precision names are recorded at (the fingerprint's quantum, 1e-4 mm). The
// resolver never answers with such an element for a pick that had measure (T-081a holdout5 1:7: a cut whose start
// plane lay 7.6e-7 mm inside the part's back face left a slab of material that thin, and the slab's end, a face 30 mm
// long and under 1e-6 mm wide, came out as a second piece of the face the cut trimmed): a downstream feature cannot
// use it. A real sliver (holdout3 17:8: a cylinder piece 0.02 mm long and 0.007 mm wide) is not of zero measure.
inline constexpr double kZeroWidth = 1e-4;

// Checks the table (sorted, every name has a record, ids and families recomputed, kinds agree) and builds
// the derived indexes. A table that breaks a rule is an Internal error, or FormatError when `fromFile`.
Result<ElementMap> finishMap(ElementMap::Impl table, bool fromFile);

// The record of `name`: the map's own (a held name), else an ancestor's (ElementMap::Impl::ancestors); null when the
// map knows neither.
const TopoName* recordOrAncestor(const ElementMap& map, const Hash256& name);

// The body moves (ElementMap::Impl::bodyMoves) that carried elements of `feature`, oldest first.
std::vector<const FeaturePlacement*> bodyMovesOf(const ElementMap& map, const FeatureId& feature);

// The face moves (ElementMap::Impl::faceMoves) whose face held one of `names`, oldest first.
std::vector<const FeaturePlacement*> faceMovesOf(const ElementMap& map, std::span<const Hash256> names);

// ElementMap::Impl::runs (T-081b round 11): the feature that started face `face`'s run of one-to-one history, nullopt
// when not known.
std::optional<FeatureId> runOf(const ElementMap& map, kernel::SubId face);
// ElementMap::Impl::wholeBlends: is `name` among them?
bool wholeBlend(const ElementMap& map, const Hash256& name);

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
