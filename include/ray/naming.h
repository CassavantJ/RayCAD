// RayCAD interface contract: persistent naming
// Module: src/naming   Depends on: core, kernel
// Status: PLAN DRAFT, revision 2. Amended after spike S4 (PLAN.md section 3.3, spikes/s4-naming/REPORT.md):
// the first rules resolved 17 of 33,785 references to a wrong element; these gave 0 on the same scenarios.
// Amended after the T-081a gate (PLAN.md 0.6): pick-time evidence in TopoRef, its JSON form, input shapes in
// NamingInput, and Resolved::postableWithoutReview.
//
// Goal: a face, edge or vertex picked today still resolves after upstream edits, or fails loudly.
// A reference NEVER silently resolves to a different element.
#pragma once
#include "core.h"
#include "kernel.h"
#include <array>
#include <optional>
#include <string_view>

namespace ray::naming {

// A stable name for one topological element of one body.
// Names are derived from HOW the element was made, not from its index in the shape.
struct TopoName {
    Hash256 id;                   // hash of the fields below; this is what gets stored
    kernel::SubKind kind;
    FeatureId origin;             // feature that first created the element
    std::string role;             // e.g. "extrude.side", "extrude.endcap", "fillet.blend", "import.face"
    std::vector<Hash256> parents; // names (or sketch-entity / loop / hole-position ids hashed) this element was generated from, sorted
    // Siblings share (kind, origin, role, parents). They are told apart first by `discriminator`:
    // the sorted names of the faces around the element that are not among its parents. May be empty.
    std::vector<Hash256> discriminator;
    std::uint32_t ordinal;        // last resort, among siblings with an identical discriminator; by fingerprint sort.
                                  // 0 = not needed; 1-based when used, so ambiguous() == (ordinal != 0)
    std::uint32_t siblingCount;   // hashed into id. A name issued alone differs from one issued as 1 of n.
    Hash256 family;               // hash(kind, origin, role, parents); NOT part of id
    bool ambiguous() const;       // true if this name needed the ordinal; such a name is trusted only while the
                                  // stored fingerprint still matches (see ResolveState::Exact)
};

// Geometric fingerprint, quantised to 1e-4 mm / 1e-6 rad. Used for imported (history-free) geometry,
// to verify ambiguous names, and as a repair hint. Compare with matches(), never with ==: values rounded
// to a grid flip at rounding boundaries.
struct Fingerprint {
    kernel::SurfaceType surface;      // or curve type for edges
    std::int64_t qArea, qCx, qCy, qCz, qRadius;
    std::int64_t qAxisX, qAxisY, qAxisZ;
    std::uint16_t neighbourCount;     // a filter only; Repaired needs neighbour NAMES (TopoRef::neighbourNames)
    auto operator<=>(const Fingerprint&) const = default;   // for sorting siblings only
    bool matches(const Fingerprint&) const;                 // tolerance compare: 1e-3 mm on centroid, 1e-7 relative on area
};

// Per-body table: every current sub-shape of the body's Shape has ONE OR MORE names
// (several after same-domain unification merged elements; the merged element carries every input name).
class ElementMap {
public:
    std::optional<kernel::SubId> find(const Hash256& nameId) const;
    std::span<const Hash256> namesOf(kernel::SubId) const;             // first is the primary name
    const TopoName* record(const Hash256& nameId) const;               // null if not issued in this build
    std::span<const Hash256> family(const Hash256& familyId) const;    // member names issued in this build
    Fingerprint fingerprintOf(kernel::SubId) const;
    std::size_t size() const;                                          // number of named sub-shapes
    std::vector<std::byte> serialize() const;                       // canonical, deterministic
    static Result<ElementMap> deserialize(std::span<const std::byte>);
    struct Impl; std::shared_ptr<const Impl> impl;   // opaque, immutable, shared by copies; null = empty map
};

// Lineage of the CURRENT build: when a named element is split, merged or deleted, record it.
// Not kept across rebuilds (names are rebuilt deterministically; S4 did not need older lineage).
struct LineageEvent {
    enum class Kind { Split, Merged, Deleted } kind;
    Hash256 from;
    std::vector<Hash256> to;      // empty for Deleted
    FeatureId cause;
};

// ---- Building maps (called by the document rebuilder after each feature) ----
struct NamingInput {
    FeatureId feature;
    std::string featureRole;                           // base role string for elements this feature generates
    std::span<const ElementMap* const> inputMaps;      // one per input shape, same order as the kernel op
    const kernel::ShapeHistory* history;
    const kernel::Shape* result;
    std::span<const kernel::SweepFaceOrigin> sweepOrigins;   // optional, from extrude/revolve
    // Optional: the input shapes themselves, parallel to inputMaps (each the shape its map was made for).
    // With them propagate() checks a result element against the territory the input elements held (Territory,
    // below): an element the history reports Modified from one input element only, that also covers the territory
    // of an input element on the same surface (or curve) that the history reports Deleted, took that element in
    // (a boolean that unifies two coplanar faces without saying so), and carries that element's names as well, so a
    // reference to either resolves Merged instead of Exact. Empty: names come from the history alone, as before.
    // When given, the size must equal inputMaps' size and no entry may be null (else propagate() fails).
    std::span<const kernel::Shape* const> inputShapes;
};
struct NamingOutput { ElementMap map; std::vector<LineageEvent> lineage; };
Result<NamingOutput> propagate(const NamingInput&);

// Names for a history-free import: role "import.<kind>", derived from geometry and adjacency, never from
// sub-shape order (PLAN.md 3.3 rule 5), plus fingerprint.
ElementMap nameImported(FeatureId importFeature, const kernel::Shape&);
// Re-import or replace-geometry: match old names to the new shape by fingerprint + adjacency.
struct RematchResult { ElementMap map; std::vector<LineageEvent> lineage; std::vector<Hash256> unmatched; };
RematchResult rematchImported(const ElementMap& old, FeatureId importFeature, const kernel::Shape& replacement);

// ---- References held by features, CAM operations, measurements, PMI ----

// Where an element lay when it was picked: pick-time evidence that a resolver tests the current shape
// against, because names cannot tell a dimension change (the element grew into space nobody held) from an
// absorption (it grew over another element). Coordinates and the measure are quantised like Fingerprint (1e-4 mm,
// 1e-4 mm^2): the same on every platform except where a value sits on a rounding boundary, so compare with a
// tolerance, never with ==. Made by IResolver::makeRef from the exact geometry, without a mesh, deterministically
// (the same shape gives the same Territory).
inline constexpr std::size_t kTerritoryPoints = 5;   // at most this many points
struct Territory {
    std::array<std::int64_t, 3> qMin{}, qMax{};       // tight axis-aligned box of the element
    std::int64_t qMeasure = 0;                        // length of an edge, area of a face; 0 for a vertex
    // Points ON the element, each at least 2e-3 mm from its boundary (an edge's vertices, a face's edges): territory
    // that only this element held. At most kTerritoryPoints, spread over the element; a vertex has its own point.
    // Empty when none could be found (a sliver, an element whose geometry the kernel could not read): the territory
    // is then unknown, and nothing is concluded from it.
    std::vector<std::array<std::int64_t, 3>> points;
    bool known() const { return !points.empty(); }
};

// Another element that the picked one could later be confused with or grow over, recorded at pick time with its
// names and territory. At most kMaxPickNeighbours per reference, siblings first, then same-carrier elements
// nearest first (by the gap between their boxes, ties by SubId).
inline constexpr std::size_t kMaxPickNeighbours = 8;
struct PickNeighbour {
    enum class Relation : std::uint8_t {
        Sibling,      // another piece of the same logical element: shares a name family or a split parent with the
                      // picked element, or (edge, vertex) lies on exactly the faces the picked element lies on (a
                      // face a feature generated, such as a blend face, standing for all its faces of that role)
        SameCarrier   // a face on the same plane (same side) or cylinder, an edge on the same line or circle; adjacent
                      // or not ("coplanar neighbour")
    } relation;
    std::vector<Hash256> names;          // every name it had at pick time (sorted, never empty)
    Territory territory;
};

struct TopoRef {
    BodyId body;
    kernel::SubKind kind;
    std::vector<Hash256> names;          // every name of the picked element at pick time
    std::vector<Hash256> families;       // parallel to names
    std::vector<bool>    ambiguous;      // parallel to names
    std::vector<Hash256> splitParents;   // for each name whose role is "split": the name it was split from
    std::vector<Hash256> neighbourNames; // names of the faces around the element at pick time (needed for Repaired)
    Fingerprint hint;                    // captured at pick time
    bool repairConfirmed = false;        // set by the user in the reference review panel
    // Pick-time evidence. Optional: a reference saved without evidence has neither, and resolves as it did then
    // (the rules that need evidence are skipped). The resolver uses them so that
    //  - an Exact answer whose element now covers territory that a recorded neighbour held at pick time is Merged
    //    (it still holds territory of its own) or Broken (it does not: it is somewhere else);
    //  - an Exact answer that no longer lies where it was picked is Merged when it grew and a recorded sibling's names
    //    are gone (the pieces joined where it now is), and Broken when a recorded sibling moved differently from it
    //    (they slid instead of moving together: which is which cannot be told);
    //  - Split pieces that together do not fit in the picked element's box (translated), or one of which covers a
    //    neighbour's territory, are Broken.
    // An element that merely grew into territory nobody recorded (a dimension change) stays Exact.
    Territory territory;                 // the picked element's own
    std::vector<PickNeighbour> pickNeighbours;
};

// ---- The JSON form of a TopoRef ----
// One implementation for every place a reference is written: feature props (src/doc), the model.reference action
// (src/api), and later CAM operations. Canonical JSON (RFC 8785 member order, no whitespace), an object:
//   {"ambiguous": [bool, ...],                 parallel to names (may be absent: all false)
//    "body": "<uuid>",                         required
//    "families": ["<64 hex>", ...],            parallel to names (may be absent)
//    "hint": {"area": n, "ax": n, "ay": n, "az": n, "cx": n, "cy": n, "cz": n, "neighbours": n, "radius": n,
//             "type": n},                      Fingerprint's integers; "type" the numeric SurfaceType (CurveType for
//                                              an edge); absent: an empty fingerprint
//    "kind": "face" | "edge" | "vertex",       required
//    "names": ["<64 hex>", ...],               required, at least one
//    "neighbourNames": [...], "splitParents": [...],
//    "pickNeighbours": [{"names": [...], "relation": "sibling" | "carrier", "territory": T}, ...],   optional
//    "repairConfirmed": bool,
//    "territory": T}                           optional
// where T = {"box": [xmin, ymin, zmin, xmax, ymax, zmax], "measure": n, "points": [[x, y, z], ...]} in quanta.
// Every integer is a JSON number, or a decimal string when beyond +-2^53. toJson writes every member (empty arrays
// included) except territory and pickNeighbours, which it writes only when the territory is known / the list is not
// empty, so a reference made without evidence is written exactly as references were before the evidence existed.
std::string toJson(const TopoRef&);
// Reads the JSON form. Every member but body, kind and names is optional; unknown members are refused. Errors are
// InvalidArgument, with the problem in `message` ("must list at least one name") and the member in context[0]
// ("names", "territory.points", "pickNeighbours[2].names"), so a caller can name the field in its own terms.
Result<TopoRef> topoRefFromJson(std::string_view json);

enum class ResolveState {
    Exact,        // a name was found; if that name is ambiguous, the element's fingerprint also still matches the hint
    Split,        // name gone, but its family now has more members, or a lineage split exists: ALL pieces returned
    Merged,       // name gone because its family is whole again or the element was unified with others:
                  // the returned element contains what was picked plus more. Consumers treat it like Split.
    Repaired,     // name gone; exactly one element matches the hint AND is surrounded by exactly the faces that
                  // neighbourNames resolve to. A SUGGESTION: usable() is false until the user confirms it.
    Broken        // nothing trustworthy; the consumer must go stale / error, never guess
};
struct Resolved {
    ResolveState state;
    std::vector<kernel::SubId> elements;
    std::string reason;                  // for Broken and Repaired: why, in words the review panel can show
    // The reference's TopoRef::repairConfirmed, copied by IResolver::resolve(). Only a Repaired answer
    // looks at it.
    bool repairConfirmed = false;
    // Usable for a rebuild: Exact, Split and Merged, and Repaired when repairConfirmed; never Broken, never an answer
    // without elements. A feature rebuild may consume `elements` only when this is true. (S4: an unconfirmed repair
    // built a chamfer on the wrong edge.) Split and Merged may still change what the feature makes (all pieces, the
    // whole merged element): the consumer says so in its warnings.
    bool usable() const;
    // Postable without review: Exact, or a Repaired the user confirmed (the confirmation is the review). A
    // Split or Merged answer is usable for a rebuild but changes what an operation machines, so a CAM operation that
    // posts G-code from it must have the reference reviewed first. Implies usable().
    bool postableWithoutReview() const;
};

class IResolver {
public:
    virtual ~IResolver() = default;
    // Sets Resolved::repairConfirmed from the reference.
    virtual Resolved resolve(const TopoRef&) const = 0;
    // Records the element's names, neighbour names and hint, and its pick-time evidence (territory and
    // pickNeighbours). An element without a name gives a reference with no names (it resolves Broken).
    virtual TopoRef makeRef(BodyId, kernel::SubId) const = 0;
};

// One body of the current build: its element map and the shape the map was made for.
struct ResolverBody {
    BodyId id;
    ElementMap map;
    kernel::Shape shape;
};
// A resolver over `bodies` (the document rebuilder makes one per feature). If two bodies share an id, the first
// one listed is used. resolve() runs the stages of PLAN.md 3.3 in order: Exact (an ambiguous name also needs a
// matching fingerprint), then lineage and family rules (Split, Merged) and Repaired, then Broken with the reason
// in words. Nothing throws: an internal failure during resolve() is Broken. Null only when memory runs out.
// Thread-safe for concurrent resolve() and makeRef() calls.
std::unique_ptr<IResolver> makeResolver(std::vector<ResolverBody> bodies);

} // namespace ray::naming
