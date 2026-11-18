// RayCAD interface contract: document model
// Module: src/doc   Depends on: core, kernel, naming, sketch, cam (data types only)
// Status: PLAN DRAFT.
//
// Model: the document is a chain of immutable snapshots (DocState). The UI thread owns the
// "current" pointer. Commands produce a new snapshot; workers read snapshots freely without locks.
// Derived data (shapes, element maps, meshes, toolpaths) lives in a content-addressed cache
// keyed by input hash, so undo/redo and branch switches re-use results instead of recomputing.
#pragma once
#include "core.h"
#include "kernel.h"
#include "naming.h"
#include "sketch.h"
#include <map>
#include <optional>
#include <variant>

namespace ray::cam { struct CamState; }   // defined in cam.h

namespace ray::doc {

// Unknown keys: every stored JSON object keeps the keys this version does not know in `extraJson`
// (a canonical JSON object, or empty for none) and writes them back unchanged (PLAN.md 4.4).

// ---------- Parameters ----------
struct Parameter {
    ParamId id;
    std::string name;            // identifier: [A-Za-z_][A-Za-z0-9_]*
    std::string expression;      // "25.4 mm", "width * 2", "0.5 in"
    enum class Kind { Length, Angle, Count, Unitless } kind;
    std::string extraJson;
};
// Expression language: + - * / ^ ( ) , unit suffixes (mm, in, deg, rad), functions
// min max abs sqrt sin cos tan floor ceil round, and parameter names. Evaluated with dmath.
// Names in `params` carry no unit, so they are not unit-checked here; evaluateParameters checks them.
Result<double> evaluate(std::string_view expression, const std::map<std::string, double>& params, Parameter::Kind);
// Evaluates every parameter (in internal units: mm, rad) with full unit checking of references between them.
// Errors (cycle, unknown name, unit mismatch, division by zero, bad or duplicate name) name the parameter.
Result<std::map<std::string, double>> evaluateParameters(const std::vector<Parameter>&);

// ---------- Features ----------
// A feature is plain data: a type name plus a canonical-JSON property bag. Built-in and scripted
// features use the same mechanism (differentiator 7). Property schema per type is in PLAN.md 3.2.
struct Feature {
    FeatureId id;
    std::string type;            // "import", "sketch", "extrude", "revolve", "fillet", "chamfer", "hole",
                                 // "delete_face", "offset_face", "resize_hole", "datum_plane", ...
    std::string name;            // user-visible, e.g. "Extrude 3"
    std::string propsJson;       // canonical JSON; may contain TopoRefs, ParamIds, expressions, blob hashes
    bool suppressed = false;
    std::string extraJson;
};

struct Body {
    BodyId id;
    std::string name;
    enum class Role { Part, Fixture, StockModel } role;   // Fixture bodies are static collision bodies in v1
    std::array<float, 4> color;
    bool visible = true;
    std::string extraJson;
};

struct DocSettings { UnitSystem units = UnitSystem::Inch; int decimals = 4; MaterialId material; std::string extraJson; };

struct DocState {
    DocSettings settings;
    std::vector<Parameter> parameters;
    std::vector<Feature> features;        // ordered history; the order IS the rebuild order
    std::vector<Body> bodies;
    std::shared_ptr<const cam::CamState> cam;
    std::string extraJson;
    Hash256 stateHash() const;            // hash of canonical serialisation; equals the stored object hash
};
using DocStatePtr = std::shared_ptr<const DocState>;

// ---------- Serialisation (PLAN.md 3.1, 4.1) ----------
// A snapshot is stored as a manifest object that lists its feature objects by hash, so editing one feature
// rewrites one small object plus the manifest. Hashes are BLAKE3 of the canonical JSON bytes.
// stateHash() is the hash of `manifest`.
struct StateObjects {
    std::string manifest;                                   // canonical JSON
    std::vector<std::pair<Hash256, std::string>> features;  // each feature object, in history order
};
StateObjects serializeState(const DocState&);
// `featureObject` returns the bytes stored under a hash. Malformed input returns FormatError and a newer
// "schema" returns FormatTooNew; never throws.
Result<DocState> deserializeState(std::string_view manifest,
                                  const std::function<Result<std::string>(const Hash256&)>& featureObject);

// ---------- Feature type plug-in point ----------
struct RebuildContext {
    const DocState& state;
    const std::map<std::string, double>& paramValues;
    const naming::IResolver& resolver;
    JobContext& job;
    RebuildContext(const DocState& s, const std::map<std::string, double>& params, const naming::IResolver& r, JobContext& j)
        : state(s), paramValues(params), resolver(r), job(j) {}
    // Fetch a body's shape and element map as of just before this feature.
    virtual Result<kernel::Shape> shapeOf(BodyId) const = 0;
    virtual const naming::ElementMap& mapOf(BodyId) const = 0;
    virtual Result<std::vector<std::byte>> blob(const Hash256&) const = 0;   // e.g. imported BREP
    virtual ~RebuildContext() = default;
};
struct BodyUpdate {
    BodyId body;
    kernel::Shape shape;
    naming::ElementMap map;
    std::vector<naming::LineageEvent> lineage;
};
struct FeatureOutput { std::vector<BodyUpdate> bodies; std::vector<std::string> warnings; };

class IFeatureType {
public:
    virtual ~IFeatureType() = default;
    virtual std::string_view typeName() const = 0;
    virtual std::string_view propsSchemaJson() const = 0;            // JSON Schema; used by UI, API and validation
    virtual std::vector<naming::TopoRef> references(const Feature&) const = 0;
    virtual Result<FeatureOutput> rebuild(const Feature&, RebuildContext&) const = 0;
};
class FeatureRegistry {
public:
    void add(std::unique_ptr<IFeatureType>);
    const IFeatureType* find(std::string_view typeName) const;
};

// ---------- Rebuild ----------
enum class FeatureStatus { Ok, Warning, Failed, Suppressed, NotBuilt };
struct FeatureReport { FeatureId id; FeatureStatus status; std::string message; double seconds; bool cacheHit; };
struct BuiltBody { kernel::Shape shape; naming::ElementMap map; };
struct BuildResult {
    std::map<BodyId, BuiltBody> bodies;         // state after the last successful feature
    std::vector<FeatureReport> reports;
    std::vector<naming::LineageEvent> lineage;
};
using BuildResultPtr = std::shared_ptr<const BuildResult>;

class IRebuilder {
public:
    virtual ~IRebuilder() = default;
    // Incremental: a feature is recomputed only if hash(feature props, resolved inputs, upstream output hashes)
    // misses the cache. A failed feature stops that body's chain; later features report NotBuilt.
    virtual Result<BuildResultPtr> rebuild(DocStatePtr, JobContext&) = 0;
};

// ---------- Commands and undo ----------
// Every user-visible change, including AI and script actions, is one Command (differentiator 4).
struct Command {
    std::string name;            // registry name, e.g. "feature.add", "cam.operation.edit"
    std::string argsJson;        // canonical JSON
    std::string label;           // for the undo menu: "Edit Pocket 2"
};
class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual std::string_view name() const = 0;
    virtual std::string_view argsSchemaJson() const = 0;
    virtual bool changesToolpathsOrGcode() const = 0;       // true => automation must get user confirmation
    virtual Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const = 0;   // pure function
};

class IDocument {
public:
    virtual ~IDocument() = default;
    virtual DocStatePtr state() const = 0;
    virtual BuildResultPtr build() const = 0;                 // latest finished rebuild (may lag state())
    virtual Result<DocStatePtr> execute(const Command&) = 0;  // UI thread; pushes undo entry; schedules rebuild
    virtual bool canUndo() const = 0;  virtual bool canRedo() const = 0;
    virtual Status undo() = 0;         virtual Status redo() = 0;      // unlimited; pointer swaps between snapshots
    virtual void beginPreview(const Command&) = 0;            // live preview: rebuilds off to the side, never enters undo
    virtual void cancelPreview() = 0;
    virtual bool dirty() const = 0;
    using Listener = std::function<void(DocStatePtr, BuildResultPtr)>;
    virtual int subscribe(Listener) = 0;                      // called on the UI thread
    virtual void unsubscribe(int) = 0;
};

// Canonical JSON: UTF-8, keys sorted, no insignificant whitespace, doubles printed with the
// shortest round-trip representation, -0 written as 0. All hashing of JSON uses this form.
// Returns an empty string if `anyJson` is not valid JSON.
std::string canonicalJson(std::string_view anyJson);

} // namespace ray::doc
