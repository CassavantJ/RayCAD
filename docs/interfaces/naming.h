// RayCAD interface contract: persistent naming
// Module: src/naming   Depends on: core, kernel
// Status: PLAN DRAFT, revision 2. Amended after spike S4 (PLAN.md section 3.3, spikes/s4-naming/REPORT.md):
// the first rules resolved 17 of 33,785 references to a wrong element; these gave 0 on the same scenarios.
// Amended after the T-081a gate (PLAN.md 0.6): pick-time evidence in TopoRef, its JSON form, input shapes in
// NamingInput, and Resolved::postableWithoutReview.
// Amended by T-084 (replace imported geometry): a second rematchImported that also takes the shape the old map was
// made for, so that "unchanged" is decided by geometry, and the rules of both written down. Additive.
// Amended by T-081b (naming through direct edits): TopoRef::outline, an optional planar face's pick-time outline, and
// its JSON member; additive (a reference without it resolves as before). No format version was bumped (pre-release,
// as for T-081a's evidence): an older build, which refuses unknown members, cannot read a reference that has one.
// Amended by T-081b round 4: feature placements (FeaturePlacement) in NamingInput, ElementMap (element-map format
// version 2, written only when a map holds one) and TopoRef (its JSON member "placements"), so that a face whose feature
// was moved is followed by the feature's own move. Additive in the same way.
// Amended by T-081b round 8: TopoRef::samples (PickSamples, JSON member "samples", versioned), the evidence of the
// resolver's one post-condition on every answer. Additive in the same way; a reference without it still loads.
//
// Goal: a face, edge or vertex picked today still resolves after upstream edits, or fails loudly.
// A reference NEVER silently resolves to a different element.
#pragma once
#include "core.h"
#include "kernel.h"
#include <array>
#include <optional>
#include <span>
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

// Where a feature placed what it made (T-081b round 4): the rigid transform from the feature's own frame (a sketch's
// plane: x along its x direction, z along its normal, the origin at its origin) to the model, row-major 3x4 like Xform,
// quantised: the rotation's nine entries to 1e-9, the translation (q[3], q[7], q[11]) to 1e-4 mm like Territory. Made
// with placementOf(); compare two with samePlacement(), never with ==.
struct FeaturePlacement {
    FeatureId feature;
    std::array<std::int64_t, 12> q{};
};
FeaturePlacement placementOf(FeatureId feature, const Xform& frameToModel);
// Do two placements put the feature in the same place: rotations within 1e-6 per entry, translations within 1e-3 mm?
bool samePlacement(const FeaturePlacement& a, const FeaturePlacement& b);
// A sketched feature's placement (NamingInput::placement; T-081b round 7): its sketch plane (x along the plane's x
// direction, z along its normal), its origin moved to the centre of the box of its profile in the plane, so that a
// profile moved or resized within its sketch shows as another placement, as a moved sketch does. `loops`: the curves'
// points as kernel::ProfileCurve gives them (a circle: its centre and radius); `points`: reference points in the plane
// (a hole feature's positions). The document and the naming oracle's harness both call these.
Xform sweepPlacement(const kernel::Plane& plane, std::span<const kernel::ProfileLoop> loops);
Xform sweepPlacement(const kernel::Plane& plane, std::span<const Vec2> points);

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
    // The placement of `feature` in this build (NamingInput::placement, carried from map to map); null when the feature
    // gave none or is not upstream of this map.
    const FeaturePlacement* placement(FeatureId feature) const;
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
    // Optional (T-081b round 4): where this feature placed what it makes (FeaturePlacement: a sketch's plane, a hole's
    // frame). The result map holds it, and every placement its input maps held for other features; a reference records
    // the placements of the features that made the picked element, so that when such a feature is moved (another
    // placement) the resolver follows the pick by the feature's own move instead of guessing it from the geometry.
    // A feature whose size or depth changed but not its placement, and a direct edit (no placement), are guessed.
    std::optional<Xform> placement;
};
struct NamingOutput { ElementMap map; std::vector<LineageEvent> lineage; };
Result<NamingOutput> propagate(const NamingInput&);

// Names for a history-free import: role "import.<kind>", derived from geometry and adjacency, never from
// sub-shape order (PLAN.md 3.3 rule 5), plus fingerprint.
ElementMap nameImported(FeatureId importFeature, const kernel::Shape&);
// Re-import or replace-geometry: match old names to the new shape by fingerprint + adjacency.
// The map names every element of `replacement`. An element that is UNCHANGED (the same geometry as exactly one old
// element, and no other element of either shape is) holds that element's names, so a reference to it resolves Exact.
// An element that CHANGED (it lies on the carrier of old elements and shares ground with them: a face that grew, shrank,
// became one with others or was cut in pieces) holds a new name of its own and the old names (or, for pieces, "split"
// names made from them), so a reference resolves Merged or Split, checked against its pick-time evidence like after any
// model edit, never Exact. Anything else is NEW (a new name only) or REMOVED (its names are gone: Broken). A new name
// never equals an old one. Nothing is guessed: two candidates that cannot be told apart are both unmatched.
// lineage: Merged (old name -> every name of the element now holding it), Split (old name -> the pieces' names),
// Deleted (old name held by nothing now). unmatched: every old name that is not on an unchanged element, sorted.
// This form has no old geometry: an element is unchanged when its fingerprint and its neighbours' match one to one,
// and nothing is reported as changed. src/naming/rematch.h has the rules in full.
struct RematchResult { ElementMap map; std::vector<LineageEvent> lineage; std::vector<Hash256> unmatched; };
RematchResult rematchImported(const ElementMap& old, FeatureId importFeature, const kernel::Shape& replacement);
// The same with `oldShape`, the shape `old` was made for (T-084): unchanged is then decided by geometry (carrier,
// boundary and points on each other within 1e-3 mm), and changed elements are found. No rigid move of the whole part is
// looked for: a part that moved has moved faces. The form to use when the old shape is at hand.
RematchResult rematchImported(const ElementMap& old, const kernel::Shape& oldShape, FeatureId importFeature,
                              const kernel::Shape& replacement);

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

// The outline of a picked PLANAR face at pick time (T-081b round 2): its boundary as chords on its plane, unordered
// (a point of the plane is inside when a ray from it crosses an odd number of chords). A line edge is one chord; an
// arc is cut into equal chords, at least 4, sharing what is left of kMaxOutlineChords once every line has its one.
// Quantised like Territory; each chord carries how far it may lie from the boundary it stands for (0 for a line, the
// sagitta for an arc's chord), so a point near a line is judged exactly and near an arc within that. Empty when
// the face is not planar, has an edge that is neither a line nor an arc, or more edges than kMaxOutlineChords / 4
// arcs allow: such a pick has no outline evidence and resolves as references made before the outline existed. About
// 200 bytes of JSON for a rectangle, 1 to 3 kB for a face with holes; never more than kMaxOutlineChords chords.
inline constexpr std::size_t kMaxOutlineChords = 96;
struct PickOutline {
    std::vector<std::array<std::int64_t, 7>> chords; // (x0, y0, z0, x1, y1, z1, slack) in quanta
    bool known() const { return !chords.empty(); }
};

// Pick-time samples (T-081b round 8, version 1): where the picked element lay, densely enough that the resolver can
// check every answer against the pick as its features moved it (the post-condition, src/naming/postcondition.cpp: an
// answer must hold the pick, hold nothing another pick-time element held, and be all of it). Quantised like Territory
// (1e-4 mm; directions to 1e-6 like Fingerprint's axis). A reference made before samples existed has version 0 and
// none; it resolves as one whose features' moves are not known.
inline constexpr std::uint32_t kPickSamplesVersion = 1;
inline constexpr std::size_t kMaxPickSamples = 24;  // points of a face; an edge has its two ends and up to 11 more
inline constexpr std::size_t kMaxSampleCarriers = 8; // the surfaces the element lies on
// A surface the picked element lay on: a face's own, an edge's or a vertex's faces'.
struct SampleCarrier {
    enum class Type : std::uint8_t { Plane, Cylinder, Other } type = Type::Other;
    std::array<std::int64_t, 3> qOrigin{}; // plane: a point of it; cylinder: a point of its axis (1e-4 mm)
    std::array<std::int64_t, 3> qAxis{};   // plane: its outward normal; cylinder: its axis (1e-6)
    std::int64_t qRadius = 0;              // cylinder: its radius (1e-4 mm), negative when the face looks at the axis
    // Index into PickSamples::placements of the feature whose placement carries this surface (a sweep's side or cap, a
    // hole's wall or floor): the surface moves when that feature is placed elsewhere. -1: no such feature (an imported
    // face, a direct edit's): the surface is taken to stay where it was. -2 - i: a piece split off by the feature
    // placements[i], which may or may not carry it (its own face split as it was made, or an imported face it cut).
    std::int32_t owner = -1;
};
struct PickSamples {
    std::uint32_t version = 0; // kPickSamplesVersion; 0: none recorded
    //  face: up to kMaxPickSamples points on it, each at least 2e-3 mm inside its boundary, spread farthest-first (the
    //        first the deepest, the later ones out to its boundary); empty for a surface other than a plane bounded by
    //        lines and arcs or a cylinder bounded by lines along it and circles about it (the territory points stand in);
    //  edge: its start, its end, then points along it at fixed fractions of its length;
    //  vertex: its point.
    std::vector<std::array<std::int64_t, 3>> points;
    // An edge's unit directions from its start into it (0..2) and from its end into it (3..5), 1e-6; zero otherwise.
    std::array<std::int64_t, 6> ends{};
    // How far a point of the element may lie from its nearest sample (a face: within the margin; an edge: along it).
    std::int64_t qSpacing = 0;
    std::vector<SampleCarrier> carriers;
    // How many of `carriers`, the first, are the element's own surfaces; the rest are the other faces at an edge's end
    // vertices (what bounds it: a crossing whose own surfaces did not move moves only with them), those at its first
    // vertex (`firstEndCarriers` of them) before those at its other.
    std::uint32_t ownCarriers = 0, firstEndCarriers = 0;
    // Parallel to TopoRef::pickNeighbours: the index into `placements` of the feature that carries each one, or -1.
    std::vector<std::int32_t> neighbourOwners;
    std::vector<FeaturePlacement> placements; // the owners' pick-time placements, sorted by feature
    bool known() const { return version != 0 && !points.empty(); }
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
    //    neighbour's territory, are Broken; and a Split face piece that lies wholly outside the picked face's
    //    outline on its plane is not a piece (ground the picked face never had, past a bound that moved).
    // An element that merely grew into territory nobody recorded (a dimension change) stays Exact. Since T-081b round 8
    // every answer also passes one post-condition last, against `samples` (PickSamples): it holds the pick as its
    // features moved it, all of it, and nothing another pick-time element held.
    Territory territory;                 // the picked element's own
    std::vector<PickNeighbour> pickNeighbours;
    PickOutline outline;                 // a planar face's (T-081b round 2)
    // The pick-time placements (T-081b round 4) of the features whose names the picked element held (the origins of
    // its names) that had one, sorted by feature. A face that holds none of its territory where it was picked, whose
    // feature has another placement now, is the pick moved with its feature when the territory carried by the feature's
    // move lands on it (at least half of it), or when it lies wholly inside the pick so carried; else Broken.
    std::vector<FeaturePlacement> placements;
    // Pick-time samples (T-081b round 8): what every answer is checked against last (PickSamples).
    PickSamples samples;
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
//    "outline": {"chords": [[x0, y0, z0, x1, y1, z1, slack], ...]},   optional, in quanta
//    "pickNeighbours": [{"names": [...], "relation": "sibling" | "carrier", "territory": T}, ...],   optional
//    "placements": [{"feature": "<uuid>", "q": [12 integers]}, ...],  optional, FeaturePlacement, sorted by feature
//    "repairConfirmed": bool,
//    "samples": S,                             optional (T-081b round 8), PickSamples
//    "territory": T}                           optional
// where T = {"box": [xmin, ymin, zmin, xmax, ymax, zmax], "measure": n, "points": [[x, y, z], ...]} in quanta, and
// S = {"carriers": [[type, ox, oy, oz, ax, ay, az, radius, owner], ...], "ends": [6 integers], "firstEnd": n,
//      "neighbourOwners": [n, ...], "own": n, "placements": [as above], "points": [[x, y, z], ...], "spacing": n,
//      "version": 1}
// (type 0 plane, 1 cylinder, 2 other; a version this build does not know is refused).
// Every integer is a JSON number, or a decimal string when beyond +-2^53. toJson writes every member (empty arrays
// included) except territory, pickNeighbours, outline, placements and samples, which it writes only when the territory,
// the outline or the samples are known / the list is not empty, so a reference made without evidence is written
// exactly as references were before the evidence existed.
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
