// RayCAD interface contract: persistent naming
// Module: src/naming   Depends on: core, kernel
// Status: PLAN DRAFT. Final encoding is confirmed by spike S4 (see PLAN.md section 3.3).
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
    std::vector<Hash256> parents; // names (or sketch-entity ids hashed) this element was generated from, sorted
    std::uint32_t ordinal;        // tie-break among siblings with identical origin/role/parents (see PLAN 3.3)
};

// Geometric fingerprint, quantised to 1e-4 mm / 1e-6 rad. Used for imported (history-free)
// geometry and as a last-resort repair hint. Never used silently when a name still resolves.
struct Fingerprint {
    kernel::SurfaceType surface;      // or curve type for edges
    std::int64_t qArea, qCx, qCy, qCz, qRadius;
    std::int64_t qAxisX, qAxisY, qAxisZ;
    std::uint16_t neighbourCount;
    auto operator<=>(const Fingerprint&) const = default;
};

// Per-body table: every current sub-shape of the body's Shape has exactly one name.
class ElementMap {
public:
    std::optional<kernel::SubId> find(const Hash256& nameId) const;
    const TopoName& nameOf(kernel::SubId) const;
    Fingerprint fingerprintOf(kernel::SubId) const;
    std::size_t size() const;
    std::vector<std::byte> serialize() const;                       // canonical, deterministic
    static Result<ElementMap> deserialize(std::span<const std::byte>);
};

// Lineage kept across rebuilds: when a named element is split, merged or deleted, record it.
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
    Hash256 name;
    kernel::SubKind kind;
    Fingerprint hint;             // captured at pick time
};

enum class ResolveState {
    Exact,        // name found
    Split,        // name was split; all descendants returned (caller decides whether that is acceptable)
    Repaired,     // name lost, exactly one element matches hint + adjacency; flagged for user review
    Broken        // nothing trustworthy; the consumer must go stale / error, never guess
};
struct Resolved { ResolveState state; std::vector<kernel::SubId> elements; };

class IResolver {
public:
    virtual ~IResolver() = default;
    virtual Resolved resolve(const TopoRef&) const = 0;
    virtual TopoRef makeRef(BodyId, kernel::SubId) const = 0;
};

} // namespace ray::naming
