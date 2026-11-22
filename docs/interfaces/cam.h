// RayCAD interface contract: CAM data model, toolpath IR and operation plug-in point
// Module: src/cam   Depends on: core, kernel, naming, libraries (data types)
// Status: PLAN DRAFT. Amended by T-032 (CAM state and the staleness engine):
//  - Setup, Operation and CamState keep unknown keys (extraJson), as every stored object does (document.h, PLAN 4.4);
//  - CamState holds the embedded copies of the library items the document uses (PLAN.md 3.1 "embedded"), so
//    libraries.h is included for their types instead of being forward-declared;
//  - OperationRegistry has its storage;
//  - the engine section: CamSnapshot (src/cam cannot read doc types, PLAN.md 1.1 and 15: the engine reads snapshots
//    through a function the document module provides, doc::camSnapshot), operationInputHash, the setup-resolution
//    seam for T-033 (ResolveGeometryFn) and makeCamEngine. ICamEngine itself is unchanged.
// Amended when the wave-1 CAM modules were joined (each also in docs/interfaces/cam.h):
//  - Embedded<T> has no extraJson of its own: the library item carries its unknown keys (libraries.h extraJson), so
//    an item embedded from a library keeps them;
//  - embeddedJson / embedded*FromJson: an embedded item's JSON is the library's file format (lib::toJson), so a
//    library file and the same item in a document are the same JSON. src/doc reads and writes embedded items through
//    these, as it may use cam but not lib (PLAN.md 1.1);
//  - operationInputHash takes the tool's and holder's geometry (geometryHash) and only the machine fields a generator
//    reads; GenerateInput carries the items reduced to exactly that (generatorTool and friends);
//  - default member initialisers for the plain structs (not Move, FeedsInput/FeedsResult, ResolvedGeometry,
//    HoleSegment and RecognizedHole, which other tasks are changing).
#pragma once
#include "core.h"
#include "kernel.h"
#include "libraries.h"
#include "naming.h"
#include <functional>
#include <map>
#include <optional>
#include <variant>

namespace ray::doc { struct DocState; using DocStatePtr = std::shared_ptr<const DocState>; struct BuildResult; using BuildResultPtr = std::shared_ptr<const BuildResult>; }

namespace ray::cam {

// ---------- Setup ----------
struct BoxStock      { double xNeg = 0, xPos = 0, yNeg = 0, yPos = 0, zNeg = 0, zPos = 0; };   // offsets beyond the part bounding box, in the WCS frame
struct CylinderStock { double diameter = 0, length = 0; int axis = 2; double endOffset = 0; };   // axis 0 x, 1 y, 2 z
struct SolidStock    { BodyId body; };
struct FromSetupStock{ SetupId previous; };                            // in-process stock carried from an earlier setup
using StockDef = std::variant<BoxStock, CylinderStock, SolidStock, FromSetupStock>;

struct Wcs {
    // Orientation and origin are defined by references so they follow model edits.
    std::optional<naming::TopoRef> zFace;      // Z = outward normal of this planar face
    std::optional<naming::TopoRef> xEdge;      // X along this edge
    enum class OriginMode { StockCorner, StockTopCenter, ModelPoint } originMode = OriginMode::StockCorner;
    int stockCornerIndex = 0;                  // 0..7 when StockCorner
    std::optional<naming::TopoRef> originVertex;
    int workOffset = 1;                        // 1 => G54, 2 => G55 ...
};

struct Setup {
    SetupId id;
    std::string name;
    std::vector<BodyId> partBodies;
    std::vector<BodyId> fixtureBodies;         // collision bodies; never cut
    StockDef stock;
    Wcs wcs;
    MachineId machine;
    std::string postId;                        // post script file stem, e.g. "haas-ngc"
    int programNumber = 1001;
    std::string programComment;
    std::vector<OperationId> operations;       // ordered
    std::string extraJson;                     // unknown keys, kept and written back (document.h)
};

// ---------- Operation ----------
struct ToolRef { ToolId tool; HolderId holder; int toolNumber = 1; int lengthOffset = 1; int diameterOffset = 1; double stickOut = 0; };

struct CuttingData {
    double spindleRpm = 0; bool clockwise = true;
    double feedCut = 0, feedPlunge = 0, feedRamp = 0, feedLeadIn = 0, feedLeadOut = 0;   // mm/min
    enum class Coolant { Off, Flood, Mist, ThroughTool, Air } coolant = Coolant::Off;
};

struct Heights {               // all relative to the WCS Z of the referenced level
    double clearance = 0;      // rapid-safe plane between operations
    double retract = 0;        // rapid-safe plane inside an operation
    double feedPlane = 0;      // switch from rapid to feed
    double top = 0, bottom = 0;    // machining range; may be driven by geometry references
};

struct Operation {
    OperationId id;
    std::string type;          // "face","contour2d","pocket2d","adaptive2d","slot","chamfer2d","engrave",
                               // "threadmill","drill"  (drill covers spot/drill/peck/tap/bore/ream by cycle)
    std::string name;
    ToolRef tool;
    CuttingData cutting;
    Heights heights;
    std::vector<naming::TopoRef> geometry;     // faces, edges or hole cylinders this op machines
    std::string paramsJson;                    // type-specific, canonical JSON; schema from IOperationType
    std::string templateId;                    // shop template or rule that created it, empty if manual
    bool suppressed = false;
    std::string extraJson;                     // unknown keys, kept and written back (document.h)
};

// A library item embedded in the document (PLAN.md 3.1 "embedded", libraries.h): the copy its operations and setups
// use, so a file opens, generates and posts identically on a machine that does not have the library. It is taken
// from the library on first use (the cam.* commands carry it in their arguments) and changes only by a command.
// The item's unknown keys are its own extraJson (libraries.h), so they come along from the library.
template <class T>
struct Embedded {
    T item;
};

struct CamState {
    std::vector<Setup> setups;                 // in program order
    std::map<OperationId, Operation> operations;   // every one listed by exactly one setup
    std::map<ToolId, Embedded<lib::Tool>> tools;
    std::map<HolderId, Embedded<lib::Holder>> holders;
    std::map<MachineId, Embedded<lib::MachineDef>> machines;
    std::map<MaterialId, Embedded<lib::Material>> materials;
    std::string extraJson;                     // unknown keys of the CAM object
    bool empty() const { return setups.empty() && operations.empty() && tools.empty() && holders.empty() &&
                                machines.empty() && materials.empty() && extraJson.empty(); }
};

// The JSON of an embedded item is the library's file format of the item (lib::toJson: canonical, its unknown keys
// included), so a library file and the same item embedded in a document are the same JSON; and back (lib::toolFromJson
// and the others: the item's schema is checked; FormatError, or FormatTooNew for a newer "schema", naming the first
// problem). src/doc stores and reads embedded items through these, as it may use cam but not lib (PLAN.md 1.1).
std::string embeddedJson(const Embedded<lib::Tool>&);
std::string embeddedJson(const Embedded<lib::Holder>&);
std::string embeddedJson(const Embedded<lib::MachineDef>&);
std::string embeddedJson(const Embedded<lib::Material>&);
Result<Embedded<lib::Tool>> embeddedToolFromJson(std::string_view);
Result<Embedded<lib::Holder>> embeddedHolderFromJson(std::string_view);
Result<Embedded<lib::MachineDef>> embeddedMachineFromJson(std::string_view);
Result<Embedded<lib::Material>> embeddedMaterialFromJson(std::string_view);

// ---------- Toolpath IR ----------
// Coordinates are in the setup WCS, integer nanometres. This is the single source for backplot,
// simulation, cycle time and posting. It is hardware-tier independent and bit-identical across
// tiers and thread counts (PLAN.md section 8).
enum class MoveKind : std::uint8_t { Rapid, Linear, ArcCW, ArcCCW, Dwell, CycleDrill, CyclePeck, CycleChipBreak,
                                     CycleTap, CycleBore, CycleReam, CycleCancel };
enum class MoveRole : std::uint8_t { Cut, Plunge, Ramp, LeadIn, LeadOut, Link, Retract, Approach };
enum class ArcPlane : std::uint8_t { XY, XZ, YZ };

struct Move {
    MoveKind kind;
    MoveRole role;
    Nm x, y, z;                 // end point
    Nm i, j, k;                 // arc centre (absolute); helical when z changes
    ArcPlane plane;
    std::int32_t feedMicronPerMin;   // 0 for rapids
    // Cycles: (x,y) hole position, z = bottom; fields below
    Nm cycleRetract, cyclePeck;
    std::int32_t cycleDwellMs;
    std::int32_t threadPitchNm;      // tapping
    std::uint8_t compensation;       // 0 off, 1 left (G41), 2 right (G42) for "in control" cutter comp
};

struct Toolpath {
    OperationId operation;
    Hash256 inputHash;               // hash of everything the generator consumed
    std::vector<Move> moves;
    Nm startX = 0, startY = 0, startZ = 0;
    // maxEngagementDeg alone is a short spike (S5); p99 and air-cut length are what describe an adaptive path.
    struct Stats { double cutLengthMm = 0, rapidLengthMm = 0; double minZ = 0, maxZ = 0; double maxEngagementDeg = 0, p99EngagementDeg = 0, airCutLengthMm = 0; } stats;
    std::vector<std::string> warnings;   // includes generator failure flags: start point not found, uncleared area remains, lead failed
    std::vector<std::byte> serialize() const;        // canonical little-endian; the golden-file format
    static Result<Toolpath> deserialize(std::span<const std::byte>);
    Hash256 hash() const;
};

// ---------- Generation ----------
struct ResolvedGeometry {
    // Everything an operation needs, already resolved from references and expressed in the WCS frame.
    Xform wcsFromModel;
    std::vector<kernel::Shape> partShapes;
    std::vector<kernel::Shape> fixtureShapes;
    std::vector<kernel::SubId> selected;             // parallel to Operation::geometry after resolution
    std::vector<int> selectedShapeIndex;
    Box3 stockBounds;
    // 2D in-process stock for rest-aware 2.5D ops: boundary loops of remaining material per Z level, if known.
    std::optional<std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>> stockSlices;
};
// The engine hands a generator the embedded tool, holder and machine reduced to what the input hash covers
// (generatorTool, generatorHolder, generatorMachine below): ids, names, vendor text, presets, comments and the
// machine's post and controller fields are empty or zero there, so a generator cannot depend on what does not make
// its operation stale. Cutting data is the operation's own (Operation::cutting).
struct GenerateInput {
    const Operation& op;
    const lib::Tool& tool;
    const lib::Holder* holder;
    const lib::MachineDef& machine;
    const ResolvedGeometry& geom;
};

class IOperationType {
public:
    virtual ~IOperationType() = default;
    virtual std::string_view typeName() const = 0;
    virtual std::string_view paramsSchemaJson() const = 0;      // JSON Schema with defaults; drives the UI form
    virtual Status validate(const GenerateInput&) const = 0;    // cheap; runs on every edit
    // Pure function of GenerateInput. Runs on a worker. Must not read tier, thread count, clock or locale.
    virtual Result<Toolpath> generate(const GenerateInput&, JobContext&) const = 0;
};
// Filled once at start-up, then shared read-only (the engine reads it from workers without a lock).
class OperationRegistry {
public:
    // A null type, an empty name or a name that is already registered is ignored and logged: the first wins.
    void add(std::unique_ptr<IOperationType>);
    const IOperationType* find(std::string_view typeName) const;   // null if unknown
    std::vector<std::string_view> types() const;                   // sorted
private:
    std::map<std::string, std::unique_ptr<IOperationType>, std::less<>> types_;
};

// ---------- Staleness / associativity ----------
enum class OpStatus { UpToDate, Stale, Generating, Failed, ReferenceBroken, Suppressed };
struct OpState { OpStatus status = OpStatus::Stale; std::string message; std::shared_ptr<const Toolpath> toolpath; };

class ICamEngine {
public:
    virtual ~ICamEngine() = default;
    // Computes the input hash of every operation against the given build; anything whose hash differs
    // from its stored toolpath becomes Stale. A Stale or ReferenceBroken operation can NOT be posted.
    virtual std::map<OperationId, OpState> status(doc::DocStatePtr, doc::BuildResultPtr) const = 0;
    virtual JobHandle<std::shared_ptr<const Toolpath>> generate(doc::DocStatePtr, doc::BuildResultPtr, OperationId) = 0;
    // Operations run in parallel; each is internally deterministic and MUST be re-entrant (no shared globals, no
    // clock-driven branches). Cancellation is polled by iteration count, never by wall clock.
    virtual void generateAllStale(doc::DocStatePtr, doc::BuildResultPtr) = 0;
};

// ---------- The engine (task T-032) ----------
// The CAM view of one document snapshot and its build. src/cam may not include document.h (PLAN.md 1.1), so the
// engine reads snapshots through a function that makes this view; for documents that is doc::camSnapshot.
struct CamSnapshot {
    std::shared_ptr<const CamState> cam;        // null: the document has no CAM state
    bool built = false;                         // false: no build yet; every operation is Stale until there is one
    std::vector<naming::ResolverBody> bodies;   // the built bodies (BuildResult::bodies), sorted by id
};
using CamSnapshotFn = std::function<CamSnapshot(const doc::DocStatePtr&, const doc::BuildResultPtr&)>;

// The input hash of operation `op` (Toolpath::inputHash): BLAKE3 over everything its generator consumes, and nothing
// else, so it changes exactly when the toolpath may change:
//   the operation's type, params, tool stick-out, cutting data and heights; the embedded tool's and holder's geometry
//   (lib::Tool::geometryHash, lib::Holder::geometryHash) and the machine fields a generator reads (generatorMachine:
//   kind, axes, spindle speed range, rigid tapping); each geometry reference as it resolves now (state, and the content of every
//   resolved element: surface, curve or point geometry); the setup's part and fixture bodies (each body's content
//   hash, in setup order: GenerateInput carries the whole part shapes, so an edit anywhere on a part body counts);
//   the stock (its definition; a solid stock's body content; a previous setup's stock and operation hashes for
//   FromSetupStock) and the WCS (mode, corner, and its references as they resolve).
// Not in it: names, the template id, tool number and offset registers, program number and comment, post, work
// offset, operation order, bodies the setup does not use, the document's other features, the library items' ids,
// names, vendor text, comments, presets (cutting data is the operation's), thread designation, body material and
// unknown keys, the machine's power, torque, tool changer, controller, options and post properties, and anything of
// the hardware tier (display tessellation is not part of a shape's content). Generators must not read Operation::name
// or templateId.
// Errors: ReferenceBroken when a geometry or WCS reference does not resolve usably (Broken; Repaired and not
// confirmed; Split or Merged, which change what is machined and need review: naming.h postableWithoutReview), or a
// body the operation needs is not built; NotFound when the operation, its setup or an embedded item it names is
// missing; InvalidArgument for a FromSetupStock chain that does not lead to an earlier setup.
Result<Hash256> operationInputHash(const CamSnapshot&, const naming::IResolver&, const OperationId& op);
// The items as GenerateInput carries them: copies with only what the input hash covers. A tool keeps its type,
// flutes, diameters, lengths, corner radius, tip and thread pitch; a holder its gauge length and segments; a machine
// its kind, axes, minimum and maximum rpm and rigid tapping. Everything else is the default (an empty id and name).
lib::Tool generatorTool(const lib::Tool&);
lib::Holder generatorHolder(const lib::Holder&);
lib::MachineDef generatorMachine(const lib::MachineDef&);

// Setup resolution: the ResolvedGeometry an operation is generated on (task T-033 implements it). The engine calls
// it on a worker, after the input hash is known and before IOperationType::validate and generate. It must be a pure
// function of the request (the hash above covers what it may read).
struct GeometryRequest {
    const CamSnapshot& snapshot;
    const Setup& setup;
    const Operation& op;
    const naming::IResolver& resolver;
};
using ResolveGeometryFn = std::function<Result<ResolvedGeometry>(const GeometryRequest&, JobContext&)>;
// The stand-in until T-033: wcsFromModel is the identity, partShapes and fixtureShapes are the setup's bodies in
// order, `selected` / `selectedShapeIndex` are the resolved geometry references (one entry per resolved element),
// stockBounds is the box around the part shapes grown by a BoxStock's offsets (the plain part box otherwise).
Result<ResolvedGeometry> resolveGeometryBasic(const GeometryRequest&, JobContext&);

struct CamEngineOptions {
    ResolveGeometryFn resolveGeometry;   // empty: resolveGeometryBasic
    std::size_t cacheEntries = 256;      // toolpaths kept by input hash; the least recently used go first
};
// The engine. status() may be called from any thread. generate() and generateAllStale() return at once: the hash is
// computed on the calling thread, the toolpath on a worker of `jobs`, and the result is applied on the UI thread
// (the job's continuation, through the job system's dispatcher), keyed by the input hash it was generated for: a
// toolpath whose operation changed while it ran is kept for that older hash and the operation stays Stale. A
// generate() for an operation already generating with the same inputs returns that job; with other inputs it
// cancels it. Failures are remembered by input hash (Failed until the inputs change); a cancelled run leaves the
// operation Stale. `jobs`, and the dispatcher it posts to, must outlive the engine; continuations that run after the
// engine is gone do nothing.
std::unique_ptr<ICamEngine> makeCamEngine(IJobSystem& jobs, std::shared_ptr<const OperationRegistry> types,
                                          CamSnapshotFn snapshot, CamEngineOptions options = {});

// ---------- Linking (shared by all milling operation types) ----------
// Entry falls back in order helix -> ramp along the cut -> pre-drill point (if given); if none fits, generation
// fails with the minimum width needed (S5: upstream adaptive produced nothing for a slot 1.8 mm wider than the tool).
enum class EntryKind : std::uint8_t { Helix, Ramp, PreDrill };
struct LinkOptions { double maxStayDownDistance = 0; bool allowRapidRetract = false; double leadRadius = 0; double rampAngleDeg = 0; double helixDiameter = 0; double helixMinDiameter = 0; std::vector<EntryKind> entryOrder; };
// Orders cut passes and inserts leads, ramps, retracts and links. Never rapids below feedPlane inside stock bounds.
Result<std::vector<Move>> linkPasses(std::span<const std::vector<Move>> passes, const Heights&, const LinkOptions&, const ResolvedGeometry&);

// ---------- Hole recognition (v1: cylinders with planar/conical bottoms; full AFR is v2) ----------
struct HoleSegment { double diameter, depth; bool conical; double coneAngleDeg; };
struct RecognizedHole { naming::TopoRef axisFace; Vec3 top, axis; std::vector<HoleSegment> segments; bool through; std::string threadSpec; };
Result<std::vector<RecognizedHole>> recognizeHoles(const kernel::Shape&, const naming::IResolver&, BodyId, Vec3 setupZ, JobContext&);
std::vector<std::size_t> orderHolesShortestPath(std::span<const Vec2> xy, Vec2 start);   // deterministic nearest-neighbour + 2-opt

// ---------- Feeds and speeds ----------
struct FeedsInput { const lib::Tool& tool; const lib::Material& material; const lib::MachineDef& machine;
                    double radialEngagementMm, axialDepthMm; std::string_view opType; };
struct FeedsResult { double rpm, feedMmMin, chipLoadMm, effectiveChipLoadMm, mrrCm3Min, powerKw, torqueNm;
                     bool limitedByRpm, limitedByPower, limitedByFeed; };
FeedsResult computeFeeds(const FeedsInput&);   // includes radial chip thinning

// ---------- Cycle time ----------
struct CycleTime { double cutSec = 0, rapidSec = 0, toolChangeSec = 0, totalSec = 0; };
CycleTime estimateCycleTime(std::span<const Toolpath* const>, const lib::MachineDef&);   // v1: trapezoidal accel per axis

} // namespace ray::cam
