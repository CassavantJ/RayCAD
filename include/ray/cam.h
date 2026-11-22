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
// Amended by T-033 (setup resolution): resolveSetup and ResolvedSetup (the frame, both directions of it, and the stock
// solid, without an operation: for the setup preview and the simulator), resolveSetupGeometry in place of the
// stand-in resolveGeometryBasic, and ResolvedGeometry's comment says which frame each member is in.
// Amended by the canned-cycle settlement (after T-037 and T-039, which had each written down assumptions): the cycle
// part of Move is decided (cycleTop, cycleClearance, cycleReturn, cyclePeckClearance, cycleLeftHand; what each cycle
// kind does; cycle runs), Move is laid out as the 112-byte RCTP record (version 2), and kDefaultPeckClearanceNm /
// peckClearanceNm are the one place the expansion fallback lives for src/sim and src/post.
// Amended by T-048 (feeds and speeds): computeFeeds returns Result, FeedsInput and FeedsResult grew (see that section).
// Amended by T-044 (CAM workspace, setups): Wcs::flipX and Wcs::flipZ, since the X edge's curve direction is not the
// user's choice; resolveSetup and the input hash read them (a setup with neither keeps its hash).
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
    // Added by T-044: X follows the X edge's curve direction (or the default rule), which the user cannot choose, so
    // the setup can turn it round. flipX: X (and with it Y) reversed; flipZ: Z reversed, into the material (X stays as
    // its rule gives it for that Z, Y = Z x X). Both false in documents written before them.
    bool flipX = false;
    bool flipZ = false;
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
    Nm startX = 0, startY = 0, startZ = 0;
    // maxEngagementDeg alone is a short spike (S5); p99 and air-cut length are what describe an adaptive path.
    struct Stats { double cutLengthMm = 0, rapidLengthMm = 0; double minZ = 0, maxZ = 0; double maxEngagementDeg = 0, p99EngagementDeg = 0, airCutLengthMm = 0; } stats;
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
    // Everything an operation needs, already resolved from references (resolveSetupGeometry, below). The shapes and
    // `selected` stay in the model frame (a SubId belongs to the shape it came from, and kernel::sliceAt, silhouette
    // and recognizeHoles take the frame or setup Z as an argument); wcsFromModel maps them into the WCS, the frame of
    // the toolpath IR; stockBounds is in the WCS.
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
//   FromSetupStock) and the WCS (mode, corner, the flips when either is set, and its references as they resolve).
// Not in it: names, the template id, tool number and offset registers, program number and comment, post, work
// offset, operation order, bodies the setup does not use, the document's other features, the library items' ids,
// names, vendor text, comments, presets (cutting data is the operation's), thread designation, body material and
// unknown keys, the machine's power, torque, tool changer, controller, options, post properties and peck clearance
// (lib::MachineDef::peckClearance: it changes how a cycle is expanded into plain moves, which the post host and the
// backplot work out from the current machine when they run, not the toolpath, whose moves carry their own
// cyclePeckClearance), and anything of the hardware tier (display tessellation is not part of a shape's content). Generators must not read Operation::name
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

// ---------- Setup resolution (task T-033) ----------
// A setup's WCS frame and stock, and the ResolvedGeometry an operation is generated on. Pure functions of their
// inputs; the input hash above covers everything they read (part, fixture and stock bodies, the stock definition,
// the origin mode and corner, the flips, the WCS references as they resolve; Wcs::workOffset is G-code only and not
// read).
//
// Frame: right-handed and orthonormal; wcsFromModel maps a model point p to R p - o (rows of R: the WCS axes in model
// coordinates, o: the origin in that orientation).
//   Z  the outward (out of the material) normal of Wcs::zFace, which must be planar; without one, model +Z.
//   X  Wcs::xEdge, which must be straight, in its curve direction (kernel::CurveInfo start to end), projected into the
//      plane normal to Z; an edge within 0.06 degrees of Z is refused. Without one: model +X projected when Z is
//      within 45 degrees of model Z (|Z.z| >= cos 45), else the X that makes Y model +Z projected (seen from outside
//      a side face, model up is WCS Y).
//   Y  Z x X.
//   Flips (T-044): with Wcs::flipZ, Z is reversed before X is chosen (so X follows the rule above for the reversed Z);
//   with Wcs::flipX, X is reversed after it is chosen; Y is Z x X either way, so the frame stays right-handed. Both
//   are exact negations: they change no bit but the sign.
//   Each reference must resolve as the input hash requires (Exact, or a repair the user confirmed) to one element of
//   its kind: zFace a face, xEdge an edge, originVertex a vertex.
// Stock (stockBounds), axis-aligned in the WCS orientation, never in the model axes:
//   BoxStock       the box around the part bodies in the WCS orientation, grown by the offsets.
//   CylinderStock  axis along WCS X, Y or Z (axis 0, 1, 2), centred on the part box across the axis; its + end lies
//                  endOffset beyond the part box along the axis and it runs `length` back from there.
//   SolidStock     the body as it lies in the WCS; stockBounds is its box in the WCS orientation.
//   FromSetupStock the simulated stock of an earlier setup, which does not exist yet: InvalidArgument saying so.
// Origin, on the stock box in the WCS orientation:
//   StockCorner (Wcs::stockCornerIndex 0..7), seen from +Z (looking down, X to the right, Y up):
//        top (Z max)        bottom (Z min)
//        3 ------ 2         7 ------ 6        corner k: X max for k = 1, 2, 5, 6; Y max for k = 2, 3, 6, 7;
//        |        |         |        |                  Z max for k = 0..3 (4..7 lie under 0..3).
//        0 ------ 1         4 ------ 5        The default, 0, is the top front left corner.
//   StockTopCenter  the centre of the stock's top face.   ModelPoint  the vertex Wcs::originVertex.
// Rounding (bit-identical on every platform): face normals and edge directions read from the model are rounded per
// component to a multiple of 2^-40 (ties to even), and an axis within 1e-9 of a model axis becomes that axis exactly;
// the rest is IEEE arithmetic with dmath::sqrt. Bounds, offsets and the origin are rounded to integer nanometres
// (toNm) in the WCS orientation, so partBounds and stockBounds are whole nanometres in the WCS.
struct ResolvedSetup {
    Xform wcsFromModel;          // model -> WCS
    Xform modelFromWcs;          // WCS -> model (sim::buildBackplot, the triad and the stock on screen)
    Box3 partBounds;             // box of the part bodies, in the WCS; all zero when the setup has none
    Box3 stockBounds;            // in the WCS
    kernel::Shape stockSolid;    // the stock solid in the WCS; null unless SetupResolveOptions::stockSolid
};
struct SetupResolveOptions { bool stockSolid = false; };
// Cheap enough for a live preview: an axis-aligned frame takes the bounds from the model boxes, a tilted one measures
// each body in it with kernel::boundsIn (T-044: no copy of the body is made). Errors: ReferenceBroken for a part, fixture or stock body that is
// not built or a reference that does not resolve usably; InvalidArgument for a WCS or stock the setup cannot have
// (above; also box or cylinder stock without part bodies); KernelFailure when a body cannot be measured or the stock
// solid made; Cancelled.
Result<ResolvedSetup> resolveSetup(const CamSnapshot&, const Setup&, const naming::IResolver&, JobContext&,
                                   SetupResolveOptions = {});

// The engine calls a ResolveGeometryFn on a worker, after the input hash is known and before IOperationType::validate
// and generate. It must be a pure function of the request (the hash above covers what it may read).
struct GeometryRequest {
    const CamSnapshot& snapshot;
    const Setup& setup;
    const Operation& op;
    const naming::IResolver& resolver;
};
using ResolveGeometryFn = std::function<Result<ResolvedGeometry>(const GeometryRequest&, JobContext&)>;
// The engine's default: resolveSetup, then partShapes and fixtureShapes are the setup's bodies in order (model
// frame), `selected` / `selectedShapeIndex` the operation's geometry references as they resolve (one entry per
// resolved element; a reference on a body that is not a part body is ReferenceBroken), stockSlices empty.
Result<ResolvedGeometry> resolveSetupGeometry(const GeometryRequest&, JobContext&);

struct CamEngineOptions {
    ResolveGeometryFn resolveGeometry;   // empty: resolveSetupGeometry
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

// ---------- Feeds and speeds (task T-048, src/cam/feeds.cpp) ----------
// Amended by T-048: computeFeeds returns Result (errors are values); FeedsInput can name the preset and replace its
// surface speed or chip load; FeedsResult says everything the feeds panel shows (what was asked for, every limit and
// what it did, the formulas' intermediate values, notes), so nothing is recomputed outside this function.
//
// Spindle speed and feed from the tool's cutting data for the material (a lib::ToolPreset: surface speed and chip
// load per flute), corrected for radial chip thinning and for the diameter in contact of ball, corner-radius,
// chamfer, engraving and spot tools at shallow depth, then held inside the machine's limits. A pure, deterministic
// function: it computes and never writes into an operation. "Use calculated" copies rpm and feed into
// Operation::cutting by a command; the panel compares them with what the operation holds. feeds.cpp states each
// formula with its public source, each assumption as a named constant, and the order in which limits apply.
//
// Units: mm, m/min (surface speed), mm/min, rpm, cm3/min, kW, N m. A machine value of 0 is "not known": that limit is
// skipped and reported Unknown (with a note), never read as a limit of zero.
// Modes, from the tool type: Tapping (taps): feed = pitch x rpm exactly. Drilling (drills, spot drills, reamers,
// boring bars, and any non-tap tool when opType is "drill"): feed = rpm x chip load x flutes, no chip thinning.
// Milling (every other tool): feed = rpm x chip load x flutes x chip thinning factor.
struct FeedsInput {
    const lib::Tool& tool; const lib::Material& material; const lib::MachineDef& machine;
    // Milling: width (ae) and depth (ap) of cut, mm; 0 = the preset's maxRadialFraction / maxAxialFraction of the
    // diameter. Chamfer, engraving and spot tools: ap is the depth from the tool tip to the top of the cut. Drilling:
    // ae is the radial stock per side when opening an existing hole (reaming, boring); 0 or at least the radius means
    // solid drilling; ap is not used.
    double radialEngagementMm, axialDepthMm;
    std::string_view opType;                       // Operation::type
    std::string_view opClass = {};                 // the preset's opClass; empty: chosen from the tool type and opType
    std::optional<double> surfaceSpeedMMin = {};   // replaces the preset's surface speed, m/min
    std::optional<double> chipLoadMm = {};         // replaces the preset's chip load per flute, mm (not for taps)
};

enum class FeedsMode : std::uint8_t { Milling, Drilling, Tapping };
// How power and torque were estimated. lib::Material has no chip-thickness exponent mc, so every model uses kc1.1
// flat, which reads low for chips thinner than 1 mm (feeds.cpp, kc model).
enum class PowerModel : std::uint8_t {
    Unknown,        // the material's kc1.1 is 0 (not known): power and torque are 0, their limits Unknown
    MillingKc11,    // P = ae ap vf kc1.1
    DrillingKc11,   // P = MRR kc1.1 (equal to vc f D kc / 240000 for solid drilling; an annulus when opening a hole)
    TappingKc11     // torque = kc1.1 x thread volume per revolution / 2 pi (ISO 68-1 basic profile), power from it
};
// One machine limit: the machine's value (0 = not known), what the cutting data asked for before this limit, and
// what happened. Exceeded: the limit could not be met by changing rpm or feed (a tap's torque, a tap feed held up by
// the minimum rpm); the result is still the best available and says so in a note.
enum class LimitState : std::uint8_t { Unknown, NotReached, Applied, Exceeded };
struct FeedsLimit { LimitState state = LimitState::Unknown; double machine = 0; double requested = 0; };

struct FeedsResult {
    // What to program: rpm is a whole number, feedMmMin is exactly feedMicronPerMin / 1000 (the toolpath feed).
    double rpm = 0, feedMmMin = 0;
    double chipLoadMm = 0;            // programmed feed per flute, feed / (rpm x flutes); 0 for taps
    double effectiveChipLoadMm = 0;   // largest chip thickness that feed cuts: chipLoadMm / chipThinningFactor
    double mrrCm3Min = 0;             // material removal rate (milling: ae ap vf, an upper bound for round tools)
    double powerKw = 0;               // at the motor (tool wear factor and spindle efficiency included); 0 if unknown
    double torqueNm = 0;              // at the spindle (tool wear factor included); 0 if unknown
    bool limitedByRpm = false, limitedByPower = false, limitedByFeed = false;   // that limit lowered rpm or feed
    // Added by T-048.
    bool limitedByTorque = false, limitedByMinRpm = false;   // limitedByMinRpm: rpm raised to the machine minimum
    std::int32_t feedMicronPerMin = 0;
    FeedsMode mode = FeedsMode::Milling;
    std::string opClass;              // the preset found; empty when there is none (the overrides gave the values)
    double surfaceSpeedMMin = 0;      // asked for (preset or override)
    double targetChipLoadMm = 0;      // asked for, per flute (preset or override); 0 for taps
    double effectiveDiameterMm = 0;   // diameter in contact, which sets the rpm
    double radialEngagementMm = 0, axialDepthMm = 0;   // as used (after the preset defaults for 0)
    double radialThinningFactor = 1, leadThinningFactor = 1;   // fz / hex of each effect
    double chipThinningFactor = 1;    // their product, at most feeds.cpp kMaxChipThinningFactor
    bool thinningCapped = false;      // the product was larger and was capped
    double actualSurfaceSpeedMMin = 0;   // at effectiveDiameterMm and the programmed rpm
    double feedPerRevMm = 0;          // feed / rpm (taps: the pitch)
    double cuttingPowerKw = 0;        // at the cutting edges (tool wear factor in), before spindle efficiency
    PowerModel powerModel = PowerModel::Unknown;
    bool tapFeedExact = true;         // taps: feedMicronPerMin x 1000 == pitch in nm x rpm, no rounding
    struct Limits { FeedsLimit maxRpm, minRpm, maxFeed, power, torque; } limits;
    std::vector<std::string> notes;   // user-presentable English, in a fixed order: defaults used, unknown limits, ...
};
// Errors: InvalidArgument for a tool without a positive diameter, flutes (not taps) or pitch (taps), an engagement
// that is negative or not finite, a surface speed or chip load that is not positive, a machine whose minimum rpm is
// above its maximum, or cutting data that give less than 1 rpm or a feed outside 1 um/min..2^31-1 um/min; NotFound
// when the tool has no preset for the material and opClass and the overrides do not replace it.
Result<FeedsResult> computeFeeds(const FeedsInput&);

// ---------- Cycle time ----------
struct CycleTime { double cutSec = 0, rapidSec = 0, toolChangeSec = 0, totalSec = 0; };
CycleTime estimateCycleTime(std::span<const Toolpath* const>, const lib::MachineDef&);   // v1: trapezoidal accel per axis

} // namespace ray::cam
