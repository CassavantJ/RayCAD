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
// Amended by the canned-cycle settlement (after T-037 and T-039, which had each written down assumptions): the cycle
// part of Move is decided (cycleTop, cycleClearance, cycleReturn, cyclePeckClearance, cycleLeftHand; what each cycle
// kind does; cycle runs), Move is laid out as the 112-byte RCTP record (version 2), and kDefaultPeckClearanceNm /
// peckClearanceNm are the one place the expansion fallback lives for src/sim and src/post.
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
struct BoxStock      { double xNeg, xPos, yNeg, yPos, zNeg, zPos; };   // offsets beyond the part bounding box, in the WCS frame
struct CylinderStock { double diameter, length; int axis; double endOffset; };
struct SolidStock    { BodyId body; };
struct FromSetupStock{ SetupId previous; };                            // in-process stock carried from an earlier setup
using StockDef = std::variant<BoxStock, CylinderStock, SolidStock, FromSetupStock>;

struct Wcs {
    // Orientation and origin are defined by references so they follow model edits.
    std::optional<naming::TopoRef> zFace;      // Z = outward normal of this planar face
    std::optional<naming::TopoRef> xEdge;      // X along this edge
    enum class OriginMode { StockCorner, StockTopCenter, ModelPoint } originMode;
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
struct ToolRef { ToolId tool; HolderId holder; int toolNumber; int lengthOffset; int diameterOffset; double stickOut; };

struct CuttingData {
    double spindleRpm; bool clockwise = true;
    double feedCut, feedPlunge, feedRamp, feedLeadIn, feedLeadOut;   // mm/min
    enum class Coolant { Off, Flood, Mist, ThroughTool, Air } coolant;
};

struct Heights {           // all relative to the WCS Z of the referenced level
    double clearance;      // rapid-safe plane between operations
    double retract;        // rapid-safe plane inside an operation
    double feedPlane;      // switch from rapid to feed
    double top, bottom;    // machining range; may be driven by geometry references
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
template <class T>
struct Embedded {
    T item;
    std::string extraJson;                     // unknown keys of the item's JSON, kept and written back
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

// ---------- Toolpath IR ----------
// Coordinates are in the setup WCS, integer nanometres. This is the single source for backplot,
// simulation, cycle time and posting. It is hardware-tier independent and bit-identical across
// tiers and thread counts (PLAN.md section 8).
enum class MoveKind : std::uint8_t { Rapid, Linear, ArcCW, ArcCCW, Dwell, CycleDrill, CyclePeck, CycleChipBreak,
                                     CycleTap, CycleBore, CycleReam, CycleCancel };
enum class MoveRole : std::uint8_t { Cut, Plunge, Ramp, LeadIn, LeadOut, Link, Retract, Approach };
enum class ArcPlane : std::uint8_t { XY, XZ, YZ };
enum class CycleReturn : std::uint8_t { RPlane, Initial };   // where a cycle leaves the tool: G99, G98

// ---------- Canned cycles ----------
// One cycle move is one hole, run as a Fanuc/Haas control runs its canned cycle. Every height is an absolute WCS Z in
// nanometres, like x, y and z, and they are ordered  z < cycleTop <= cycleRetract <= cycleClearance:
//   x, y                 the hole
//   z                    the hole bottom
//   cycleTop             the hole top (the post API's CycleMove.top)
//   cycleRetract         the R plane: rapid down to it, feed from it
//   cycleClearance       the initial level (G98 returns here)
//   cycleReturn          the level the tool goes to after this hole: RPlane = cycleRetract (G99) or Initial =
//                        cycleClearance (G98). Chosen per move by the generator from what sits between this hole and
//                        the next: the tool travels to the next hole at that level
//   cyclePeck            the peck depth Q, a positive increment, the first peck measured from the R plane (the
//                        Fanuc/Haas G83 and G73 meaning of Q); 0 = no pecking: the hole is drilled in one feed
//   cyclePeckClearance   c, the G83 re-entry clearance and the G73 chip-break back-off; 0 = the control's own parameter,
//                        and a post that emits the native cycle writes nothing for it. Only an expansion into plain
//                        moves (the post host for a control without the cycle, the backplot, simulation) needs a
//                        number: peckClearanceNm() below, the move's value, else the machine's, else 0.5 mm
//   cycleDwellMs         dwell at the bottom, >= 0. CycleDrill (G82) and CycleBore / CycleReam (G89) take it on every
//                        control; CyclePeck, CycleChipBreak and CycleTap only where the control's G83, G73 or G84 has a
//                        P word (Fanuc's G84 does, its G83 and G73 do not). A post for a control without it warns or
//                        fails; it never drops a dwell silently. An expansion dwells at the bottom for every kind
//   threadPitchNm        CycleTap: the thread pitch (> 0); 0 for the other kinds
//   cycleLeftHand        CycleTap: 1 = left-hand thread (G74 instead of G84); 0 otherwise
//   feedMicronPerMin     the feed into the hole (and out of it for tap, bore and ream), per minute. For CycleTap it is
//                        pitch times rpm: feedMicronPerMin = threadPitchNm * rpm / 1000 with the operation's whole-number
//                        spindle speed, exact whenever that is a whole number of um/min (always for a pitch that is a
//                        whole number of micrometres: every metric pitch, some inch ones), otherwise rounded half to even.
//                        Rigid versus floating tapping is the machine's and the post's business
//                        (MachineDef::rigidTapping, capabilities.rigidTap), not the IR's
// The motion of one cycle move, from the tool's position P (the end of the previous move, or the toolpath start):
//   1. rapid in X and Y at P's Z to the hole;
//   2. if P is above cycleClearance, rapid down to it (this happens only at the start of a run, see below);
//   3. rapid to the R plane;
//   4. by kind, with Q = cyclePeck and c the peck clearance:
//      CycleDrill (G81; G82 with a dwell; spot drilling too)   feed to z, dwell, rapid out to the return level
//      CyclePeck (G83)       feed to R - Q; then, until z: rapid to R, rapid to c above the last depth, feed Q deeper
//                            than the last depth (the last peck stops at z); dwell, rapid out to the return level
//      CycleChipBreak (G73)  feed to R - Q; then, until z: rapid up by c, feed Q deeper than the last depth (the last
//                            peck stops at z); dwell, rapid out to the return level
//      CycleTap (G84; G74 when left-hand)   feed to z, dwell, spindle reversal, feed out to R at the same feed; with
//                            Initial, then rapid to cycleClearance
//      CycleBore, CycleReam (G85; G89 with a dwell)   feed to z, dwell, feed out to R at the same feed; with Initial,
//                            then rapid to cycleClearance
//      A re-entry or back-off never goes above R. Spindle-stop rapid-out boring (G86), fine boring with an oriented
//      stop and shift (G76) and back boring (G87) are future move kinds; they are not part of this format.
// After the move the tool is at (x, y, return level), and the next move starts there. CycleCancel (G80) ends a run
// and does not move the tool; nor does Dwell.
// Runs: consecutive cycle moves with the same kind, cycleLeftHand and cycleClearance form a run; any other move
// (CycleCancel and Dwell included), or a change of one of those three, ends it. RULE: at the first move of a run the
// tool must be at or above that move's cycleClearance (P.z >= cycleClearance), either already over the hole or
// approaching it by step 1, a rapid at P's Z that the generator must have made safe. So a native cycle always starts
// at its initial level: when P is above cycleClearance, the post host emits steps 1 and 2 as rapids before the cycle
// block. Later moves of a run start at the previous move's return level, which is at most the clearance. The toolpath
// invariant checker (tests/unit/cam/invariants) enforces this and the level order; the post host refuses a toolpath
// that breaks either.
inline constexpr Nm kDefaultPeckClearanceNm = 500'000;   // 0.5 mm: the expansion fallback when nothing else is known

// ---------- Move: the RCTP record ----------
// Fixed size, 112 bytes, no implicit padding. The member order is the byte layout of the record in Toolpath::serialize
// (every field little-endian, the pads written as 0):
//    0 kind u8          1 role u8           2 plane u8           3 compensation u8
//    4 cycleReturn u8   5 cycleLeftHand u8  6 pad0 u16
//    8 feedMicronPerMin i32                 12 cycleDwellMs i32
//   16 threadPitchNm i32                    20 pad1 u32
//   24 x   32 y   40 z   48 i   56 j   64 k                                  (i64 each)
//   72 cycleRetract   80 cycleTop   88 cycleClearance   96 cyclePeck   104 cyclePeckClearance   (i64 each)
// Moves are built value-initialised (Move m{}) and set by name; the order is not an aggregate-initialisation order.
struct Move {
    MoveKind kind;
    MoveRole role;
    ArcPlane plane;
    std::uint8_t compensation;       // 0 off, 1 left (G41), 2 right (G42) for "in control" cutter comp
    CycleReturn cycleReturn;         // cycles: see above
    std::uint8_t cycleLeftHand;      // CycleTap: 1 left-hand (G74)
    std::uint16_t pad0;              // 0
    std::int32_t feedMicronPerMin;   // 0 for rapids
    std::int32_t cycleDwellMs;       // cycles, and the Dwell move's duration
    std::int32_t threadPitchNm;      // CycleTap
    std::uint32_t pad1;              // 0
    Nm x, y, z;                      // end point; for a cycle the hole (x, y) and its bottom z
    Nm i, j, k;                      // arc centre (absolute); helical when z changes
    Nm cycleRetract, cycleTop, cycleClearance, cyclePeck, cyclePeckClearance;   // cycles: see above
};
static_assert(sizeof(Move) == 112, "Move is the 112-byte RCTP record; see the layout above");
inline constexpr std::uint32_t kToolpathFormatVersion = 2;   // RCTP version (1: the plan draft's record, never written)

// The peck clearance an expansion into plain moves uses for `m`: its own value, else the machine definition's
// (lib::MachineDef::peckClearance, mm), else kDefaultPeckClearanceNm. `machine` may be null.
inline Nm peckClearanceNm(const Move& m, const lib::MachineDef* machine)
{
    if (m.cyclePeckClearance > 0) {
        return m.cyclePeckClearance;
    }
    if (machine != nullptr && machine->peckClearance > 0 && machine->peckClearance <= 1000.0) {
        return toNm(machine->peckClearance);
    }
    return kDefaultPeckClearanceNm;
}

struct Toolpath {
    OperationId operation;
    Hash256 inputHash;               // hash of everything the generator consumed
    std::vector<Move> moves;
    Nm startX, startY, startZ;
    // maxEngagementDeg alone is a short spike (S5); p99 and air-cut length are what describe an adaptive path.
    struct Stats { double cutLengthMm, rapidLengthMm; double minZ, maxZ; double maxEngagementDeg, p99EngagementDeg, airCutLengthMm; } stats;
    std::vector<std::string> warnings;   // includes generator failure flags: start point not found, uncleared area remains, lead failed
    // The RCTP format, canonical little-endian, the golden-file format (PLAN.md 3.6): "RCTP", u32 version
    // (kToolpathFormatVersion), operation id (16 bytes), inputHash (32 bytes), u64 move count, startX, startY, startZ
    // (i64), then the 112-byte Move records. 88 + 112 n bytes; stats and warnings are not stored. deserialize refuses
    // a wrong magic, size or pad, a value outside its enum (compensation above 2, cycleLeftHand above 1), version 1
    // (FormatError) and a later version (FormatTooNew); its stats are zero and it has no warnings. hash() is BLAKE3 of
    // serialize().
    std::vector<std::byte> serialize() const;
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
struct OpState { OpStatus status; std::string message; std::shared_ptr<const Toolpath> toolpath; };

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
//   the operation's type, params, tool stick-out, cutting data and heights; the embedded tool, holder and machine
//   (their content, not their ids); each geometry reference as it resolves now (state, and the content of every
//   resolved element: surface, curve or point geometry); the setup's part and fixture bodies (each body's content
//   hash, in setup order: GenerateInput carries the whole part shapes, so an edit anywhere on a part body counts);
//   the stock (its definition; a solid stock's body content; a previous setup's stock and operation hashes for
//   FromSetupStock) and the WCS (mode, corner, and its references as they resolve).
// Not in it: names, the template id, tool number and offset registers, program number and comment, post, work
// offset, operation order, bodies the setup does not use, the document's other features, and anything of the
// hardware tier (display tessellation is not part of a shape's content). Generators must not read Operation::name or templateId.
// Errors: ReferenceBroken when a geometry or WCS reference does not resolve usably (Broken; Repaired and not
// confirmed; Split or Merged, which change what is machined and need review: naming.h postableWithoutReview), or a
// body the operation needs is not built; NotFound when the operation, its setup or an embedded item it names is
// missing; InvalidArgument for a FromSetupStock chain that does not lead to an earlier setup.
Result<Hash256> operationInputHash(const CamSnapshot&, const naming::IResolver&, const OperationId& op);

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
struct LinkOptions { double maxStayDownDistance; bool allowRapidRetract; double leadRadius; double rampAngleDeg; double helixDiameter; double helixMinDiameter; std::vector<EntryKind> entryOrder; };
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
struct CycleTime { double cutSec, rapidSec, toolChangeSec, totalSec; };
CycleTime estimateCycleTime(std::span<const Toolpath* const>, const lib::MachineDef&);   // v1: trapezoidal accel per axis

} // namespace ray::cam
