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
// Amended by T-034 (hole recognition): recognizeHoles takes the setup frame, has an overload without a resolver, and
// HoleSegment, RecognizedHole and HoleOptions grew (see that section).
// Amended for the chip-thickness exponent (lib::Material::mc): PowerModel MillingKcMc and DrillingKcMc, and
// FeedsResult::meanChipThicknessMm and specificCuttingForceNmm2.
// Amended so closed documents do not leak: ICamEngine::forget (a no-op by default).
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
// Amended by T-035 (the drilling operation, the first operation type):
//  - the thread of a tapped hole reaches generators: HoleThread moved up and grew its nominal diameter and thread
//    depth; CamSnapshot::holeThreads (doc::camSnapshot fills it from the hole features); ResolvedGeometry carries the
//    part bodies' element maps and those threads (resolveSetupGeometry), holeOptionsFor() turns them into the
//    HoleOptions of recognizeHoles, and RecognizedHole carries the thread's nominal diameter and depth;
//  - operationInputHash covers the threads of the hole features named on the setup's part bodies, so a thread edit
//    (which changes no geometry: the thread is not modelled) makes the operations on that body stale;
//  - registerBuiltinOperationTypes: the operation types src/cam ships ("drill" then; the eight of M3 now, listed at the
//    function), which the app registers.
// Amended by T-045 (the operation panel; each also in docs/interfaces/cam.h):
//  - Setup::material: the stock material the feeds are calculated for (none was stored anywhere a command can set);
//    Operation::calculated: what "Use calculated" last wrote. Neither is read by a generator or in the input hash;
//  - IOperationType::check: validate plus the warnings a generation would report, for live feedback (a default
//    that calls validate, so existing types need no change);
//  - OpState::progress; ICamEngine::subscribe (the engine says when a status it holds changed, so the UI need not
//    poll), statusAsync (status on a worker, owned by the engine), validate (check on a worker) and preview (a
//    toolpath of a snapshot that is not the document's, cached by input hash but never an operation's latest).
// Amended by T-059 (cycle time): CycleTimeOperation, CycleTimePart, CycleTimeAssumptions, CycleTimeOptions and the
// default constants; CycleTime derives from CycleTimePart (its four members unchanged) and carries the operations'
// parts, the assumptions and notes; estimateCycleTime takes tool numbers (the plan draft's form stays).
// Amended by T-051 (linking): EntryKind::Plunge, PreDrillPoint, LinkOptions' added members, LinkPass, and linkPasses
// takes LinkPass and a JobContext (the Linking section says why).
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
    // Added by T-045: the stock material (an embedded one, CamState::materials), nil for none; the feeds and speeds
    // of the setup's operations are calculated for it (computeFeeds). Not read by generators, not in the input hash.
    MaterialId material;
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

// Added by T-045: what "Use calculated" (computeFeeds) last wrote into an operation's cutting data, so the operation
// panel tells calculated values from the user's own and offers (never forces) an update when the tool or material
// changes. A record only: no generator reads it, it is not in the input hash. Feeds in mm/min.
struct CalculatedFeeds {
    ToolId tool;                                        // calculated for this tool,
    MaterialId material;                                // this material (nil: none known)
    MachineId machine;                                  // and this machine (nil: none known)
    std::string opClass;                                // the preset asked for; "" = chosen by computeFeeds
    std::optional<double> surfaceSpeedMMin, chipLoadMm; // the overrides given, if any
    double spindleRpm = 0, feedCut = 0, feedPlunge = 0, feedRamp = 0;   // the values written
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
    std::optional<CalculatedFeeds> calculated; // T-045: set by "Use calculated"; empty when the values are typed
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
// The thread of a tapped hole feature as CAM reads it (T-097b; defined here, before its first use; the hole
// recognition section says how it is picked up). Lengths in mm. Amended by T-035: nominalDiameter and depth.
struct HoleThread {
    std::string spec;              // the designation, "M6x1", "1/4-20 UNC", "M8x1-LH"
    double pitch = 0;
    double nominalDiameter = 0;    // the thread's major diameter; 0 when not known
    double depth = 0;              // full-thread depth from the top of the tapped bore; 0: the whole tapped length
};
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
    // Added by T-035: the element map of each part body (parallel to partShapes) and the threads of the document's
    // tapped hole features (CamSnapshot::holeThreads), so a generator can read the thread of a hole it recognises
    // (holeOptionsFor, in the hole recognition section). Empty maps where the caller has none.
    std::vector<naming::ElementMap> partNames;
    std::map<FeatureId, HoleThread> holeThreads;
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
    // Added by T-045: validate's error, or the warnings generate would put in Toolpath::warnings (in that order), for
    // the operation panel's live feedback. Runs on a worker (ICamEngine::validate). The default is validate() with no
    // warnings; a type whose warnings come from a plan it makes anyway (drill) returns them here.
    virtual Result<std::vector<std::string>> check(const GenerateInput& in) const
    {
        if (auto st = validate(in); !st) return tl::unexpected(st.error());
        return std::vector<std::string>{};
    }
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
// Added by T-035: adds the operation types src/cam ships (src/cam/ops): "drill" (spot, drill, peck, chipBreak, tap,
// bore and ream by its "cycle" parameter), "face", "contour2d", "pocket2d", "slot", "chamfer2d", "engrave" and
// "threadmill" (M3, T-052 to T-056), and "adaptive2d" (M4, T-061). The app calls it once at start-up (src/api).
void registerBuiltinOperationTypes(OperationRegistry&);

// ---------- Staleness / associativity ----------
enum class OpStatus { UpToDate, Stale, Generating, Failed, ReferenceBroken, Suppressed };
// progress (T-045): 0..1 while Generating once the generator reported any, else -1.
struct OpState { OpStatus status = OpStatus::Stale; std::string message; std::shared_ptr<const Toolpath> toolpath; double progress = -1; };
// T-045: what ICamEngine::validate found. `error`: validate's error, or why the operation cannot be generated at all
// (suppressed, a broken reference, an unknown type: `status` says which); empty when it would generate. `warnings`:
// those a generation would report (IOperationType::check). `status`: UpToDate when a toolpath of these exact inputs
// is cached, Stale when it is valid and not generated, else as OpState.
struct OpValidation { OpStatus status = OpStatus::Stale; std::optional<Error> error; std::vector<std::string> warnings; };

class ICamEngine {
public:
    virtual ~ICamEngine() = default;
    // ---- Added by T-045 (the operation panel and the CAM tree) ----
    // Notification: `listener(op)` is called on the UI thread (posted through the job system's dispatcher, never
    // inside an engine call) whenever something the engine holds about `op` changed: a generation started, reported
    // progress (at most 30 times a second) or ended (filed, failed or cancelled), or a preview toolpath was filed in
    // the cache. Document changes are not notified (the caller sees those itself). 0 for an empty listener. A listener
    // may call the engine. The defaults do nothing.
    using Listener = std::function<void(const OperationId&)>;
    virtual int subscribe(Listener) { return 0; }
    virtual void unsubscribe(int) {}
    // status() on a worker: the job holds what it needs (never a pointer to the engine), so it may outlive the engine
    // (it then reports what the engine held). The result arrives through then() on the UI thread. An engine without it
    // returns an empty handle.
    virtual JobHandle<std::map<OperationId, OpState>> statusAsync(doc::DocStatePtr, doc::BuildResultPtr) { return {}; }
    // IOperationType::check of operation `op` of the snapshot, on a worker (setup resolution included): live feedback
    // for an edit that is not committed (Document::previewState). Changes nothing the engine holds. Empty by default.
    virtual JobHandle<OpValidation> validate(doc::DocStatePtr, doc::BuildResultPtr, OperationId) { return {}; }
    // The toolpath of `op` in a snapshot that is not the document's own (a panel's preview): input hash, resolution and
    // generation all on a worker; found in the cache by input hash, or generated and filed there (so the committed
    // operation with the same inputs is UpToDate at once), but never made the operation's latest toolpath nor its
    // running generation, so the tree's view of the document is untouched. Cancel through the handle; each call is a
    // job of its own. Failures are not remembered. Empty by default.
    virtual JobHandle<std::shared_ptr<const Toolpath>> preview(doc::DocStatePtr, doc::BuildResultPtr, OperationId) { return {}; }
    // ---- T-032 ----
    // Computes the input hash of every operation against the given build; anything whose hash differs
    // from its stored toolpath becomes Stale. A Stale or ReferenceBroken operation can NOT be posted.
    virtual std::map<OperationId, OpState> status(doc::DocStatePtr, doc::BuildResultPtr) const = 0;
    virtual JobHandle<std::shared_ptr<const Toolpath>> generate(doc::DocStatePtr, doc::BuildResultPtr, OperationId) = 0;
    // Operations run in parallel; each is internally deterministic and MUST be re-entrant (no shared globals, no
    // clock-driven branches). Cancellation is polled by iteration count, never by wall clock.
    virtual void generateAllStale(doc::DocStatePtr, doc::BuildResultPtr) = 0;
    // Drops what the engine holds for these operations by id: the latest toolpath of each (shown while stale) and the
    // record of a running generation. Called when their document closes, after their generations were cancelled; a
    // generation that still ends later is filed in the cache by its input hash only. The cache by input hash is kept
    // (bounded: CamEngineOptions::cacheEntries), so a document opened again finds toolpaths whose inputs it still has.
    // Ids are not per document: the caller leaves out operations another open document has (a second copy of a file).
    // The default does nothing (an engine without per-operation state).
    virtual void forget(std::span<const OperationId>) {}
};

// ---------- The engine (task T-032) ----------
// The CAM view of one document snapshot and its build. src/cam may not include document.h (PLAN.md 1.1), so the
// engine reads snapshots through a function that makes this view; for documents that is doc::camSnapshot.
struct CamSnapshot {
    std::shared_ptr<const CamState> cam;        // null: the document has no CAM state
    bool built = false;                         // false: no build yet; every operation is Stale until there is one
    std::vector<naming::ResolverBody> bodies;   // the built bodies (BuildResult::bodies), sorted by id
    // Added by T-035: the thread of every tapped hole feature of the document, by feature id (the TopoName::origin
    // of the "hole.*" names on its faces). The document's thread is not modelled, so only this tells CAM about it.
    std::map<FeatureId, HoleThread> holeThreads;
};
using CamSnapshotFn = std::function<CamSnapshot(const doc::DocStatePtr&, const doc::BuildResultPtr&)>;

// The input hash of operation `op` (Toolpath::inputHash): BLAKE3 over everything its generator consumes, and nothing
// else, so it changes exactly when the toolpath may change:
//   the operation's type, params, tool stick-out, cutting data and heights; the embedded tool's and holder's geometry
//   (lib::Tool::geometryHash, lib::Holder::geometryHash) and the machine fields a generator reads (generatorMachine:
//   kind, axes, spindle speed range, rigid tapping); each geometry reference as it resolves now (state, and the content of every
//   resolved element: surface, curve or point geometry); the setup's part and fixture bodies (each body's content
//   hash, in setup order: GenerateInput carries the whole part shapes, so an edit anywhere on a part body counts;
//   with each part body, the CamSnapshot::holeThreads of the hole features named on its faces, by feature id);
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
// resolved element; a reference on a body that is not a part body is ReferenceBroken), stockSlices empty; partNames
// the part bodies' element maps and holeThreads the snapshot's (T-035).
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
// engine is gone do nothing. T-045: a running generation's status carries its progress (OpState::progress); the
// listeners (ICamEngine::subscribe) are called as that section says; statusAsync, validate and preview run wholly on
// workers of `jobs`.
std::unique_ptr<ICamEngine> makeCamEngine(IJobSystem& jobs, std::shared_ptr<const OperationRegistry> types,
                                          CamSnapshotFn snapshot, CamEngineOptions options = {});

// ---------- Linking (shared by all milling operation types) ----------
// Entry falls back in order helix -> ramp along the cut -> pre-drill point (if given); if none fits, generation
// fails with the minimum width needed (S5: upstream adaptive produced nothing for a slot 1.8 mm wider than the tool).
// Amended by T-051 (src/cam/link.cpp; each also in docs/interfaces/cam.h):
//  - EntryKind::Plunge: a straight plunge, for a centre-cutting tool only, when the entry order lists it;
//  - LinkOptions grew what linking cannot work without: the tool (diameter, centre cutting), the feeds of plunges,
//    ramps and links, the stay-down lift, the ramp's minimum run, the entry clearance, pre-drilled points and keeping
//    the given order. allowRapidRetract: retract from the cut by a rapid straight up (else feed up to the feed plane);
//  - passes are LinkPass (a list of moves does not say where the tool starts it; ordering constraints) and linkPasses
//    takes a JobContext (long work polls cancellation).
// The operation types in src/cam link through the full form, src/cam/link.h (2D regions where leads and helices may
// go, the area already cut, rotatable closed passes, per-pass leads and entries). This public form is that function
// for callers without the 2D geometry core: no room is known, so leadRadius must be 0 (InvalidArgument otherwise)
// and a helix never fits (the entry falls back to the next kind in entryOrder).
enum class EntryKind : std::uint8_t { Helix, Ramp, PreDrill, Plunge };
struct PreDrillPoint { double x = 0, y = 0, diameter = 0, bottom = 0; };   // WCS mm; the tool enters if it fits the hole
struct LinkOptions { double maxStayDownDistance = 0; bool allowRapidRetract = false; double leadRadius = 0; double rampAngleDeg = 0; double helixDiameter = 0; double helixMinDiameter = 0; std::vector<EntryKind> entryOrder;
    // Added by T-051. Lengths mm, feeds mm/min (0: the cut feed of the pass being entered); entryOrder empty: helix,
    // ramp, pre-drill.
    double toolDiameter = 0;            // > 0
    bool centreCutting = false;         // the tool may plunge straight into material
    double feedPlunge = 0, feedRamp = 0, feedLink = 0;
    double stayDownLift = 0;            // a stay-down link runs this much above its two ends (a lifted feed link)
    double minRampRun = 0;              // a zigzag ramp's run along the cut is at least this; 0: the tool diameter
    double entryClearance = 0;          // a helix or ramp starts this far above the material it enters
    std::vector<PreDrillPoint> preDrillPoints;
    bool keepOrder = false;             // cut the passes in the order given
};
// One cut pass: the tool is at (startX, startY, startZ) before moves[0]; feed moves (lines, XY arcs, helical ones).
// Passes are cut in non-decreasing sequence and, inside a cluster (>= 0), non-decreasing level (depths of a pocket).
struct LinkPass { Nm startX = 0, startY = 0, startZ = 0; std::vector<Move> moves; bool reversible = false; int sequence = 0, cluster = -1, level = 0; };
// Orders cut passes and inserts ramps, retracts and links. Never rapids below feedPlane inside stock bounds, never
// links across stock that is not cut. Heights: clearance, retract and feedPlane as absolute WCS Z in mm (top and
// bottom are not read); the stock is ResolvedGeometry::stockBounds. moves[0] is a rapid to the toolpath's start
// (Toolpath::startX/Y/Z): the clearance above where the first pass is entered. Errors: InvalidArgument for options or
// passes linking cannot use (heights out of order, the retract plane under the stock top), ToolpathFailed for a pass
// no allowed entry reaches (the message names why each did not fit), Cancelled.
Result<std::vector<Move>> linkPasses(std::span<const LinkPass> passes, const Heights&, const LinkOptions&, const ResolvedGeometry&, JobContext&);

// ---------- Hole recognition (v1: cylinders with planar/conical bottoms; full AFR is v2) ----------
// Task T-034, src/cam/holes.cpp, which states every rule and tolerance. Amended by T-034: recognition takes the setup
// frame (wcsFromModel, so positions come out in the WCS in nanometres, as the toolpath IR needs them) instead of setup
// Z alone; a second overload without a resolver serves generators, which get shapes and SubIds but no resolver
// (GenerateInput); HoleSegment and RecognizedHole carry what the drilling operation (T-035) needs; HoleOptions carries
// the thread pickup from hole features (T-097b) and a size limit.
//
// A hole is a stack of coaxial concave faces along setup Z, open at the top to an up-facing plane with nothing above
// it: cylinders (pieces of one cylinder merged, as STEP exporters split them), cones (countersinks, chamfers, drill
// points) and planar steps; it ends in a flat floor, a cone (drill point) or by breaking out (through). Segments run
// from the top down; planar steps between them are not segments.
struct HoleSegment {
    double diameter, depth; bool conical; double coneAngleDeg;   // cone: diameter at its top, included angle
    // Added by T-034:
    double bottomDiameter;    // the diameter at the segment's lower end: = diameter for a cylinder, 0 at a drill point
    Nm zTop, zBottom;         // WCS Z of the segment's ends (a drill point's zBottom is its tip); depth = zTop - zBottom
};
enum class HoleBottom : std::uint8_t { Through, Flat, Conical };
struct RecognizedHole {
    naming::TopoRef axisFace;  // the main bore's wall (its lowest SubId); empty from the overload without a resolver
    Vec3 top;                  // the WCS point (x, y, zTop) in mm
    Vec3 axis;                 // the hole axis in the model frame, pointing out of the hole (unit)
    std::vector<HoleSegment> segments;   // top down; at least one cylinder
    bool through;              // == (bottom == HoleBottom::Through)
    std::string threadSpec;    // the hole feature's thread designation ("M6x1"), empty when unknown or not tapped
    // Added by T-034:
    Nm x = 0, y = 0;           // the axis in the WCS
    Nm zTop = 0, zBottom = 0;  // the top, and the lowest point (a drill point's tip; where a through hole breaks out)
    HoleBottom bottom = HoleBottom::Through;
    std::size_t bore = 0;      // index in segments of the main bore: the lowest cylinder, the drilled diameter
    bool counterbored = false; // a cylinder above the bore is wider than it
    bool countersunk = false;  // an entry cone (above the first cylinder) wider than a chamfer (holes.cpp, kChamferMax*)
    bool chamfered = false;    // an entry cone no wider than a chamfer: an edge break, not a countersink
    double threadPitch = 0;    // mm, with threadSpec; 0 when unknown
    kernel::SubId axisSub{};   // the face axisFace names, in the shape passed
    std::vector<kernel::SubId> faces;   // every face of the hole (walls, steps, floor) in the shape passed, sorted
    // Added by T-035, with threadSpec (HoleThread): the thread's nominal diameter and full-thread depth, mm (0 unknown;
    // a depth of 0 is the whole tapped length).
    double threadNominalDiameter = 0, threadDepth = 0;
};
// Thread pickup (T-097b): the thread of the hole feature `feature`, which made the bore (TopoName::origin of a name
// of role "hole.*" on the bore's faces, read through `names`); nullopt when the feature has none. HoleThread is
// defined in the generation section.
struct HoleOptions {
    const naming::ElementMap* names = nullptr;   // the body's element map; null: no thread pickup
    std::function<std::optional<HoleThread>(const FeatureId&)> threadOf;   // empty: no thread pickup
    double maxDiameter = 0;   // > 0: holes whose bore is wider are not reported (circular pockets); 0: no limit
};
// Added by T-035: the thread pickup of part `part` of a ResolvedGeometry (its partNames entry and holeThreads), for
// a generator; no pickup when the geometry has no element map for that part. The options point into `geom`, which
// must outlive them.
HoleOptions holeOptionsFor(const ResolvedGeometry& geom, std::size_t part);
// Holes of `shape` (model frame) along Z of the frame wcsFromModel (a rigid move: the setup's ResolvedSetup), sorted
// by (x, y, zTop) and then axisSub, so the result does not depend on face order. Deterministic. Errors:
// InvalidArgument for a null shape or a frame that is not a rigid move; KernelFailure when a face or edge cannot be
// read; Cancelled. Polls cancellation per face.
Result<std::vector<RecognizedHole>> recognizeHoles(const kernel::Shape&, const naming::IResolver&, BodyId,
                                                   const Xform& wcsFromModel, JobContext&, const HoleOptions& = {});
Result<std::vector<RecognizedHole>> recognizeHoles(const kernel::Shape&, const Xform& wcsFromModel, JobContext&,
                                                   const HoleOptions& = {});
// The visiting order of points `xy` from `start`: nearest neighbour (ties to the lower index), then 2-opt and Or-opt
// improvement with a fixed pass bound, never longer than the nearest-neighbour tour. Deterministic: integer
// nanometre coordinates (toNm of each value) and integer squared distances for every comparison. holes.cpp has the
// complexity. A permutation of 0..xy.size()-1.
std::vector<std::size_t> orderHolesShortestPath(std::span<const Vec2> xy, Vec2 start);

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
// How power and torque were estimated (feeds.cpp, kc model). With the material's chip-thickness exponent mc
// (lib::Material::mc), milling and drilling take kc = kc1.1 hm^-mc at the mean chip thickness hm of the cut; without
// it kc1.1 flat, which reads low for chips thinner than 1 mm. Taps always use kc1.1 flat.
enum class PowerModel : std::uint8_t {
    Unknown,        // the material's kc1.1 is 0 (not known): power and torque are 0, their limits Unknown
    MillingKc11,    // P = ae ap vf kc1.1
    DrillingKc11,   // P = MRR kc1.1 (equal to vc f D kc / 240000 for solid drilling; an annulus when opening a hole)
    TappingKc11,    // torque = kc1.1 x thread volume per revolution / 2 pi (ISO 68-1 basic profile), power from it
    MillingKcMc,    // P = ae ap vf kc, kc = kc1.1 hm^-mc
    DrillingKcMc    // P = MRR kc, kc = kc1.1 hm^-mc
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
    double meanChipThicknessMm = 0;   // hm of the programmed feed (KcMc models; 0 otherwise)
    double specificCuttingForceNmm2 = 0;   // the kc power and torque use (kc1.1 for the Kc11 models; 0 if unknown)
    bool tapFeedExact = true;         // taps: feedMicronPerMin x 1000 == pitch in nm x rpm, no rounding
    struct Limits { FeedsLimit maxRpm, minRpm, maxFeed, power, torque; } limits;
    std::vector<std::string> notes;   // user-presentable English, in a fixed order: defaults used, unknown limits, ...
};
// Errors: InvalidArgument for a tool without a positive diameter, flutes (not taps) or pitch (taps), an engagement
// that is negative or not finite, a surface speed or chip load that is not positive, a machine whose minimum rpm is
// above its maximum, or cutting data that give less than 1 rpm or a feed outside 1 um/min..2^31-1 um/min; NotFound
// when the tool has no preset for the material and opClass and the overrides do not replace it.
Result<FeedsResult> computeFeeds(const FeedsInput&);

// ---------- Cycle time (task T-059, src/cam/cycletime.cpp) ----------
// Amended by T-059: the toolpath IR carries no tool number, so the estimate takes each operation's tool number with its
// toolpath (CycleTimeOperation) to count tool changes; CycleTime keeps its four members and adds dwell time, distances,
// the split of each operation and the assumptions made where the machine definition is silent; CycleTimeOptions holds
// what MachineDef has no field for (G0 interpolation, the corner rule).
//
// How long one setup's program runs on `machine`, from the toolpath IR alone: the setup's operations in program order,
// one call per setup (a setup is one program). An estimate: cycletime.cpp states the model, its simplifications and
// its constants. In short: every motion has a trapezoidal velocity profile (constant acceleration, no jerk limit); a
// straight motion is limited by each axis's acceleration and speed scaled by that axis's share of the direction; feeds
// run at the programmed feed held under the machine's maximum feed per axis; an arc's speed is also held so that its
// centripetal acceleration stays within its plane's axes; rapids run at the axes' rapid rates and stop at their ends;
// consecutive feed moves run through a junction (at the lower of the two speeds) where the direction turns by at most
// CycleTimeOptions::cornerStopDeg and stop there otherwise; canned cycles run as cam.h "Canned cycles" expands them,
// every peck and retract included; dwells and tool changes add their time. Deterministic (plain IEEE arithmetic in a
// fixed order and ray::dmath): the same inputs give the same bits on every platform.
// A machine value of 0 is "not known": the documented default below is used instead (slow on purpose, so the estimate
// errs long) and CycleTimeAssumptions says so, for the UI to show "estimate, machine accelerations unknown".
inline constexpr double kCycleTimeDefaultAccelMmS2 = 1000;      // per axis, when AxisDef::accelMmS2 is 0
inline constexpr double kCycleTimeDefaultRapidMmMin = 10000;    // per axis, when AxisDef::rapidMmMin is 0
inline constexpr double kCycleTimeDefaultToolChangeSec = 10;    // when MachineDef::toolChangeSec is 0
struct CycleTimeOptions {
    // G0: false moves each axis at its own rapid rate, so the axes may finish at different times (a dogleg: Fanuc's
    // default, parameter LRP = 0, and Haas); true moves along the straight line, every axis scaled to finish together.
    bool rapidInterpolated = false;
    // Two feed moves whose directions differ by more than this (degrees) stop at their junction; others run through it.
    double cornerStopDeg = 10;
};
struct CycleTimeOperation {
    const Toolpath* toolpath = nullptr;   // null: the operation adds nothing (no toolpath yet)
    int toolNumber = 0;                   // ToolRef::toolNumber; 0 or less: not known, counted as a tool change
};
// Time in seconds and path length in mm by kind of motion.
struct CycleTimePart {
    double cutSec = 0;          // feed motion: G1, G2, G3 and the feeds of cycles, whatever their role
    double rapidSec = 0;        // G0 and the rapids of cycles
    double toolChangeSec = 0;
    double totalSec = 0;        // cutSec + rapidSec + toolChangeSec + dwellSec
    double dwellSec = 0;        // Dwell moves and the dwells of cycles
    double cutMm = 0, rapidMm = 0;   // path length; a rapid counted along its straight line
    int toolChanges = 0;
};
// What the estimate assumed because the machine definition (or the toolpath) did not say.
struct CycleTimeAssumptions {
    bool accelerationUnknown = false;   // an X, Y or Z axis without acceleration: kCycleTimeDefaultAccelMmS2
    bool rapidUnknown = false;          // an X, Y or Z axis without rapid rate: kCycleTimeDefaultRapidMmMin
    bool maxFeedUnknown = false;        // an X, Y or Z axis without maximum feed: its rapid rate limits feeds
    bool toolChangeUnknown = false;     // a tool change was counted and the machine's time is not known
    bool feedMissing = false;           // a feed move without a feed: the one before it, else the maximum feed
    bool any() const { return accelerationUnknown || rapidUnknown || maxFeedUnknown || toolChangeUnknown || feedMissing; }
};
// The whole setup (the members it inherits, unchanged from the plan draft for cutSec, rapidSec, toolChangeSec and
// totalSec), each operation's part in input order (an operation's tool change, or its rapid from the previous
// operation's end when the tool stays, is its own), and the assumptions with one English note each, in the order of
// CycleTimeAssumptions' members.
struct CycleTime : CycleTimePart {
    std::vector<CycleTimePart> operations;
    CycleTimeAssumptions assumptions;
    std::vector<std::string> notes;
};
// Between operations: a tool change before the first operation and wherever the tool number changes (or is not
// known) takes the machine's tool change time and no motion (the change ends with the tool at the next toolpath's
// start); where the tool stays, the tool rapids from the previous operation's end to the next one's start.
CycleTime estimateCycleTime(std::span<const CycleTimeOperation>, const lib::MachineDef&, const CycleTimeOptions& = {});
// The plan draft's form: tool numbers not known, so every toolpath counts a tool change.
CycleTime estimateCycleTime(std::span<const Toolpath* const>, const lib::MachineDef&);

} // namespace ray::cam
