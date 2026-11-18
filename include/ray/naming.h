// RayCAD interface contract: persistent naming
// Module: src/naming   Depends on: core, kernel
// Status: PLAN DRAFT, revision 2. Amended after spike S4 (PLAN.md section 3.3, spikes/s4-naming/REPORT.md):
// the first rules resolved 17 of 33,785 references to a wrong element; these gave 0 on the same scenarios.
//
// Goal: a face, edge or vertex picked today still resolves after upstream edits, or fails loudly.
// A reference NEVER silently resolves to a different element.
#pragma once
#include "core.h"
#include "kernel.h"
#include <optional>

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
    std::uint32_t ordinal;        // last resort, among siblings with an identical discriminator; by fingerprint sort
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
    std::size_t size() const;
    std::vector<std::byte> serialize() const;                       // canonical, deterministic
    static Result<ElementMap> deserialize(std::span<const std::byte>);
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
};
struct NamingOutput { ElementMap map; std::vector<LineageEvent> lineage; };
Result<NamingOutput> propagate(const NamingInput&);

// Names for a history-free import: role "import.<kind>", ordinal from canonical order, plus fingerprint.
ElementMap nameImported(FeatureId importFeature, const kernel::Shape&);
// Re-import or replace-geometry: match old names to the new shape by fingerprint + adjacency.
struct RematchResult { ElementMap map; std::vector<LineageEvent> lineage; std::vector<Hash256> unmatched; };
RematchResult rematchImported(const ElementMap& old, FeatureId importFeature, const kernel::Shape& replacement);

// ---- References held by features, CAM operations, measurements, PMI ----
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
};

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
    // False for Broken, and for Repaired unless TopoRef::repairConfirmed. A feature rebuild or a posted CAM
    // operation may consume `elements` only when this is true. (S4: an unconfirmed repair built a chamfer on
    // the wrong edge.)
    bool usable() const;
};

class IResolver {
public:
    virtual ~IResolver() = default;
    virtual Resolved resolve(const TopoRef&) const = 0;
    virtual TopoRef makeRef(BodyId, kernel::SubId) const = 0;
};

} // namespace ray::naming
