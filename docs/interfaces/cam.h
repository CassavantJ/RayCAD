// RayCAD interface contract: CAM data model, toolpath IR and operation plug-in point
// Module: src/cam   Depends on: core, kernel, naming, libraries (data types)
// Status: PLAN DRAFT.
#pragma once
#include "core.h"
#include "kernel.h"
#include "naming.h"
#include <map>
#include <optional>
#include <variant>

namespace ray::lib { struct Tool; struct Holder; struct MachineDef; struct Material; }

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
};

struct CamState {
    std::vector<Setup> setups;
    std::map<OperationId, Operation> operations;
};

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
    Nm startX, startY, startZ;
    struct Stats { double cutLengthMm, rapidLengthMm; double minZ, maxZ; double maxEngagementDeg; } stats;
    std::vector<std::string> warnings;
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
class OperationRegistry {
public:
    void add(std::unique_ptr<IOperationType>);
    const IOperationType* find(std::string_view typeName) const;
    std::vector<std::string_view> types() const;
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
    virtual void generateAllStale(doc::DocStatePtr, doc::BuildResultPtr) = 0;   // operations run in parallel; each is internally deterministic
};

// ---------- Linking (shared by all milling operation types) ----------
struct LinkOptions { double maxStayDownDistance; bool allowRapidRetract; double leadRadius; double rampAngleDeg; double helixDiameter; };
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

namespace ray::doc { struct DocState; using DocStatePtr = std::shared_ptr<const DocState>; struct BuildResult; using BuildResultPtr = std::shared_ptr<const BuildResult>; }
