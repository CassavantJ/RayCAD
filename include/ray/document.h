// RayCAD interface contract: document model
// Module: src/doc   Depends on: core, kernel, naming, sketch, cam (data types only)
// Status: PLAN DRAFT. Amended by T-032: CAM state is stored (StateObjects), the CAM commands are built in, and
// camSnapshot gives the CAM engine its view of a snapshot.
// Amended by T-084 (replace imported geometry): an import body may name its element map ("names"), and the command
// feature.import.replace is built in. Comments only; no declaration changed.
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

namespace ray::cam { struct CamState; struct CamSnapshot; }   // defined in cam.h

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
    // The rollback bar: the last feature the rebuild builds. Later features stay in the history but are not built
    // (reported NotBuilt, "rolled back"); a feature added without an index goes in right after it and the bar moves
    // past it. nullopt (always, when it would name the last feature) means the whole history. Saved and undone like
    // any other change ("feature.rollback"); stored in the manifest as "rollback" only when set.
    std::optional<FeatureId> rollback;
    std::string extraJson;
    Hash256 stateHash() const;            // hash of canonical serialisation; equals the stored object hash
};
using DocStatePtr = std::shared_ptr<const DocState>;

// ---------- Serialisation (PLAN.md 3.1, 4.1) ----------
// A snapshot is stored as a manifest object that lists its feature objects by hash, so editing one feature
// rewrites one small object plus the manifest. Hashes are BLAKE3 of the canonical JSON bytes.
// stateHash() is the hash of `manifest`.
// CAM (task T-032): a state whose `cam` is not empty also has a CAM object (setups and operations) and one object per
// embedded library item, named by hash in the manifest's "cam" and "embedded" keys (PLAN.md 3.1); a state without CAM
// state is written exactly as before. A document from before CAM opens with a null `cam`.
struct StateObjects {
    std::string manifest;                                   // canonical JSON
    // Every other object of the snapshot: each feature object in history order, then the CAM object and the embedded
    // library items (tools, holders, machines, materials, each by id), when there is CAM state.
    std::vector<std::pair<Hash256, std::string>> features;
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
// Filled once at start-up, then shared read-only (the rebuilder reads it from workers without a lock).
class FeatureRegistry {
public:
    // A null type, an empty name or a name that is already registered is ignored and logged: the first wins.
    void add(std::unique_ptr<IFeatureType>);
    const IFeatureType* find(std::string_view typeName) const;   // null if unknown
    std::vector<std::string> typeNames() const;                  // sorted
private:
    std::map<std::string, std::unique_ptr<IFeatureType>, std::less<>> types_;
};
// Registers the built-in feature types: "import" (PLAN.md 3.2; see "The import feature" below).
// InvalidArgument if one of their names is registered already.
Status registerBuiltinFeatureTypes(FeatureRegistry&);
// A registry holding just the built-in feature types; null only when memory runs out.
std::shared_ptr<const FeatureRegistry> builtinFeatureRegistry();

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

// ---------- Content-addressed objects the rebuild reads (PLAN.md 3.5, 4.1) ----------
// Imported geometry is source data: an import feature names the BREP object (kernel::serialize bytes) of each of
// its bodies by hash in its props, and RebuildContext::blob reads it through this interface. The store satisfies
// it with its object table (doc never links store); a document not saved yet uses an ObjectPool.
class IObjectSource {
public:
    virtual ~IObjectSource() = default;
    // The bytes stored under `hash` (BLAKE3-256 of the bytes): NotFound when absent. Thread-safe.
    virtual Result<std::vector<std::byte>> object(const Hash256& hash) const = 0;
};
// A thread-safe in-memory object set: objects of a document that is not saved yet, or waiting for the next save.
class ObjectPool : public IObjectSource {
public:
    virtual Result<Hash256> put(std::span<const std::byte> bytes) = 0;   // idempotent; returns BLAKE3 of the bytes
    virtual bool has(const Hash256&) const = 0;
    virtual std::vector<Hash256> hashes() const = 0;                     // sorted
};
std::shared_ptr<ObjectPool> makeObjectPool();
// Looks each object up in `sources` in order (e.g. new objects in a pool, then the saved file). Null entries are
// skipped. NotFound if no source has it; another error from a source is returned as it is.
std::shared_ptr<IObjectSource> makeObjectChain(std::vector<std::shared_ptr<const IObjectSource>> sources);

// ---------- The rebuilder ----------
// Features run in history order, one at a time, inside the calling job; a feature may fan out over `jobs`
// (the import feature reads its bodies in parallel through IJobSystem::parallelChunks), so the output never
// depends on the worker count. Each feature's result is cached in memory under the hash of (type, id, props,
// parameter values) together with the revisions of the bodies it read through RebuildContext (shapeOf, mapOf,
// resolver), so an unchanged feature reports cacheHit and editing feature N recomputes N and only the later
// features that read a body N wrote. Failures and cancelled work are not cached.
// Failure: a feature that fails reports Failed with the reason. The bodies it affects (those it read, and those it
// read or wrote when it last built; all bodies if it never built) keep the output of the last feature that built
// them, and later features that read such a body (or, after a failure, a body that does not exist) report NotBuilt
// until a later feature writes the body afresh. A BodyUpdate with a null shape deletes the body.
// rebuild() returns Cancelled when the token is cancelled (checked between features and inside them), and
// InvalidArgument for a null state. It never throws, and may be called from several jobs at once.
// `jobs` must outlive the rebuilder; `objects` may be null (no stored objects).
struct RebuilderOptions {
    std::size_t cacheEntries = 4096;   // feature results kept; the least recently used go first
};
std::shared_ptr<IRebuilder> makeRebuilder(IJobSystem& jobs, std::shared_ptr<const FeatureRegistry> types,
                                          std::shared_ptr<const IObjectSource> objects = {},
                                          RebuilderOptions options = {});

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
    // The same, run as part of a job (Document::prepare, on a worker): a handler whose work can take long (the sketch
    // commands, which solve) checks ctx.cancel and returns Error{Cancelled}. Same result as apply() otherwise.
    // By default apply().
    virtual Result<DocStatePtr> applyWithJob(DocStatePtr before, std::string_view argsJson, JobContext& ctx) const {
        (void)ctx;
        return apply(std::move(before), argsJson);
    }
};

// A command applied off the UI thread (Document::prepare), waiting to become the document's state (Document::commit).
struct PreparedCommand {
    Command command;      // arguments in canonical form
    DocStatePtr before;   // the snapshot it was applied to
    DocStatePtr after;    // the result
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

// ---------- Command handler registry ----------
// Filled once at start-up, then shared read-only.
class CommandRegistry {
public:
    CommandRegistry();
    ~CommandRegistry();
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;
    // InvalidArgument if the handler is null, its name is empty or the name is already registered.
    Status add(std::unique_ptr<ICommandHandler> handler);
    const ICommandHandler* find(std::string_view name) const;   // null if unknown
    std::vector<std::string> names() const;                     // sorted
private:
    std::map<std::string, std::unique_ptr<ICommandHandler>, std::less<>> handlers_;
};
// Registers feature.add, feature.edit, feature.remove, feature.reorder, feature.suppress, feature.rollback,
// param.set, body.set, document.import and feature.import.replace (T-084), and the CAM commands cam.setup.*, cam.operation.* and cam.library.embed
// (task T-032). Their argument objects are described by argsSchemaJson().
Status registerBuiltinCommands(CommandRegistry&);
// A registry holding just the built-in commands; null only when memory runs out.
std::shared_ptr<const CommandRegistry> builtinCommandRegistry();

// ---------- JSON form of commands (recovery journal, scripts) ----------
// A command is the canonical JSON object  {"args":{...},"label":"...","name":"feature.add","schema":1}
// with the arguments embedded as a JSON object (not as a string). A command log is a JSON array of these, in
// execution order. Reading rejects missing or mistyped keys, unknown keys and a newer "schema" (FormatTooNew).
// commandToJson fails with InvalidArgument if argsJson is not a JSON object.
Result<std::string> commandToJson(const Command&);
Result<Command> commandFromJson(std::string_view json);   // argsJson comes back canonical
Result<std::string> commandLogToJson(const std::vector<Command>&);
Result<std::vector<Command>> commandLogFromJson(std::string_view json);

// ---------- The document ----------
// Undo: every executed command pushes one entry holding the command (with canonical arguments) and the snapshots
// before and after it; undo and redo are pointer swaps. History is unlimited; a new command clears redo. A
// command that fails changes nothing. Commands are pure functions of (snapshot, canonical arguments).
// Threads: execute, undo, redo, the preview, rebuild and subscribe calls are made on the UI thread (PLAN.md 7);
// the const queries may be called from any thread. Listeners never run inside those calls: they are posted
// through the IUiDispatcher. Notifications that run after the document is destroyed do nothing.
// Rebuild (a document made with a rebuilder): creating the document and every change of state() (execute, undo,
// redo) schedules a rebuild of the new state() as a job and cancels the rebuild of the previous one. When a
// rebuild of the snapshot that is still state() finishes, build() and builtState() switch to it and listeners are
// called again with (state(), build()). So every listener is called once per change (with the build of that
// moment, which may lag) and once per published build. Without a rebuilder build() stays null.
// Previews: beginPreview(command) applies the command to the current snapshot off to the side and rebuilds that
// snapshot as a job; it never changes state(), the stacks, dirty(), build() or the document listeners. A second
// beginPreview replaces the first; cancelPreview, execute, undo and redo end it (and cancel its rebuild).
// The command is applied on the calling (UI) thread, so it must be one that never solves a sketch: the feature
// tools' feature.<type>.add / .edit of the non-sketch types and feature.reorder are such commands; a sketch command
// is not (a debug build asserts). A preview that needs a solve applies its command with prepare() on a worker.
class Document : public IDocument {
public:
    // The preview snapshot, or null when no preview is active or the previewed command failed.
    virtual DocStatePtr previewState() const = 0;
    // Why the active preview's command failed; nullopt when there is no preview or it succeeded.
    virtual std::optional<Error> previewError() const = 0;
    // The finished rebuild of previewState(); null until it finishes, or when the preview rebuild failed.
    virtual BuildResultPtr previewBuild() const = 0;
    // Called on the UI thread when the rebuild of the active preview finishes: (previewState(), previewBuild()).
    using PreviewListener = std::function<void(DocStatePtr, BuildResultPtr)>;
    virtual int subscribePreview(PreviewListener) = 0;   // 0 if the listener is empty
    virtual void unsubscribePreview(int) = 0;

    // The snapshot build() was made from; null while build() is null.
    virtual DocStatePtr builtState() const = 0;
    // True from a change of state() until its rebuild finished, failed or was cancelled.
    virtual bool rebuildPending() const = 0;
    // Why the last rebuild of state() did not finish (Cancelled after cancelRebuild, or an internal failure);
    // nullopt when it finished or is still running. Failed features are reported in build(), not here.
    virtual std::optional<Error> buildError() const = 0;
    virtual void cancelRebuild() = 0;     // Esc: cancels the rebuild of state(); build() stays as it was
    virtual void scheduleRebuild() = 0;   // rebuilds state() again, e.g. after a missing source file came back

    virtual std::size_t undoCount() const = 0;
    virtual std::size_t redoCount() const = 0;
    // The command the next undo() or redo() would revert or repeat (its label is for the undo menu).
    virtual std::optional<Command> undoCommand() const = 0;
    virtual std::optional<Command> redoCommand() const = 0;

    // dirty() is true while state() is not the snapshot that was current at the last markClean() (or at creation).
    // The store calls this after a save.
    virtual void markClean() = 0;

    // Commands whose work can take long (a sketch solve, PLAN.md 7) run in two steps so the UI thread never waits:
    // prepare() on a worker applies the command to state() as it is at the call (ICommandHandler::applyWithJob, with
    // `ctx` for cancellation) and changes nothing; commit() on the UI thread then makes the result the new state as
    // execute() would (one undo entry with the canonical arguments, the preview ended, a rebuild scheduled, the
    // listeners notified) if state() is still prepared.before. If it is not (another change came in between), commit()
    // changes nothing and fails with InvalidArgument; the caller prepares again or executes. Commands are pure
    // functions of (snapshot, arguments), so a committed prepared command is exactly what execute() would have made.
    virtual Result<PreparedCommand> prepare(const Command&, JobContext& ctx) const = 0;   // any thread
    virtual Result<DocStatePtr> commit(PreparedCommand) = 0;                                // UI thread
};

// `dispatcher` must outlive the document and queue posted functions, never run them inside post() (core.h).
// A null `registry` means builtinCommandRegistry(); a null `initial` means an empty DocState. Null only when
// memory runs out. This form has no rebuilder: build() stays null.
std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, std::shared_ptr<const CommandRegistry> registry = {},
                                       DocStatePtr initial = {});
// A document that rebuilds through `rebuilder` on `jobs` (both must outlive it; a null rebuilder means none).
std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, IJobSystem& jobs, std::shared_ptr<IRebuilder> rebuilder,
                                       std::shared_ptr<const CommandRegistry> registry = {}, DocStatePtr initial = {});

// ---------- The import feature (type "import", PLAN.md 3.2, 3.5) ----------
// Props (canonical JSON; unknown keys are ignored by the rebuild):
//   {"sourceFile": "<UTF-8 path as given>", "format": "step" | "iges", "fileHash": "<BLAKE3 of the file, 64 hex>",
//    "options": {"heal": "none" | "reader" | "full", "sew": true, "sewTolerance": 0.001},   (optional; defaults)
//    "role": "part" | "fixture",                                                          (optional; "part")
//    "sourceUnits": "inch" | "metric",                                                    (optional; informational)
//    "bodies": [{"id": "<uuid>", "name": "...", "color": [r, g, b, a], "blob": "<BREP object hash>",
//                "names": "<element map object hash>"}],                                    (optional; "names" too)
//    "warnings": ["..."]}                                                                 (optional)
// With "bodies" the stored BREP objects are the source of truth: the rebuild reads them through
// RebuildContext::blob and never needs the file. If an object is missing, it re-imports the file (its hash must
// equal fileHash and it must give the same number of bodies) and warns where the geometry differs from the
// stored copy. Without "bodies" it imports the file on every cache miss, with body ids importedBodyId(feature, i).
// Every body is named with naming::nameImported(feature id, shape), unless it names an element map object ("names":
// naming::ElementMap::serialize bytes, written by feature.import.replace with the names naming::rematchImported carried
// over from the revision it replaced): that map is used when it can be read and names exactly the body's elements,
// else nameImported with a warning. The stored warnings are reported again. referencedObjects lists "names" objects too.
// Replacing the file with a new revision (T-084): "feature.import.replace" {"id": "<import feature>", "props": {...}}
// puts new props (same bodies and role; new file, hash, units, warnings, blobs and names) on the import feature as one
// undo step. Its objects and props are prepared on a worker from the new file, as an import's are (src/doc/commands/
// replace_import.h; the api action file.replaceImport does both steps).
//
// How an import enters a document (UI thread, after a worker ran prepareImport):
//   1. prepareImport(request, ctx) on a worker: hashes and imports the file, serialises every body;
//   2. put PreparedImport::objects into the document's object pool (and into the store when saving);
//   3. IDocument::execute(PreparedImport::command): a "document.import" command that adds the feature and a Body
//      (name, colour and role from the file) for each imported body, as one undo step.
// "document.import" args: {"id": "<feature uuid>", "name": "...", "props": {import props}, "index"?: n}.
enum class ImportFormat { Step, Iges };
struct ImportRequest {
    FeatureId feature;                    // id of the new feature, made by the caller so a replay is identical
    std::string name;                     // feature name; empty means "Import <file name>"
    std::string path;                     // UTF-8
    std::optional<ImportFormat> format;   // nullopt: from the extension (.step .stp .p21 / .iges .igs, any case)
    kernel::ImportOptions options;
    Body::Role role = Body::Role::Part;   // Part or Fixture
    std::optional<std::size_t> index;     // position in the feature list; nullopt appends
};
struct PreparedImport {
    Command command;                                                 // "document.import"
    std::vector<std::pair<Hash256, std::vector<std::byte>>> objects; // the BREP object of every body, by hash
};
// Worker only (seconds for a large file). Errors: NotFound / IoError for the file, FormatError and
// KernelFailure from the kernel, InvalidArgument for a bad request, Cancelled.
Result<PreparedImport> prepareImport(const ImportRequest&, JobContext&);
// The id of body `index` of an import feature whose props list no bodies (a version 4 UUID derived by hashing).
BodyId importedBodyId(const FeatureId& importFeature, std::size_t index);
// BLAKE3 of a file's bytes, as stored in "fileHash". Checks cancellation every MiB.
Result<Hash256> hashFile(const std::string& utf8Path, JobContext&);
// Every object the state's features name (the BREP objects of import features), sorted and unique: what a save
// must write as source objects, and what compaction must keep.
std::vector<Hash256> referencedObjects(const DocState&);

// ---------- CAM (task T-032) ----------
// The CAM engine's view of a snapshot and its build (cam::CamSnapshot): DocState::cam and BuildResult::bodies. Pass it
// to cam::makeCamEngine; src/cam cannot read doc types itself (PLAN.md 1.1 and 15). A null build gives built = false.
// Amended by T-035: holeThreads, the thread of every tapped hole feature (features/hole_feature.h holeThreadOf, its
// expressions over the document's parameters), when the document has CAM state.
cam::CamSnapshot camSnapshot(const DocStatePtr&, const BuildResultPtr&);

// Canonical JSON: UTF-8, keys sorted, no insignificant whitespace, doubles printed with the
// shortest round-trip representation, -0 written as 0. All hashing of JSON uses this form.
// Returns an empty string if `anyJson` is not valid JSON.
std::string canonicalJson(std::string_view anyJson);

} // namespace ray::doc
