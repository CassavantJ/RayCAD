// RayCAD interface contract: backplot, material-removal simulation, collision and gouge checks
// Module: src/sim   Depends on: core, kernel (TriSoup), cam, lib
// Status: PLAN DRAFT.
//
// Tier rule: the tier changes RESOLUTION and SPEED of the stock model, never the toolpath.
// Verdicts that gate posting (rapid-into-stock, holder collision, fixture collision) are computed
// analytically from the toolpath IR and exact triangulations, so they are tier-independent.
// Resolution-dependent results (gouge depth, leftover map) are reported with their tolerance.
#pragma once
#include "core.h"
#include "kernel.h"
#include "cam.h"

namespace ray::render { class IComputeBackend; }   // render.h

namespace ray::sim {

// ---------- Backplot (instant, all tiers) ----------
struct BackplotVertex { float x, y, z; std::uint8_t role; std::uint8_t kind; std::uint16_t pad; float feed; std::uint32_t moveIndex; };
struct Backplot { std::vector<BackplotVertex> lineVertices; std::vector<std::uint32_t> moveFirstVertex; };
// Arcs are flattened to `chordTol` for DISPLAY only. Budget: 1 M moves in under 150 ms on the Low reference CPU.
Backplot buildBackplot(const cam::Toolpath&, const Xform& modelFromWcs, double chordTol);

// ---------- Stock model ----------
struct StockResolution { double cellMm; };      // from tier preset: Low 0.5, Medium 0.2, High 0.05 (starting values)

class IStockModel {
public:
    virtual ~IStockModel() = default;
    virtual void reset(const kernel::TriSoup& stockSolid, const Box3& bounds, StockResolution) = 0;
    // Remove the volume swept by the cutting part of the tool along one move. Returns removed volume in mm^3.
    virtual double cut(const struct ToolShape&, const Vec3& from, const Vec3& to, bool rapid) = 0;
    virtual double cutArc(const struct ToolShape&, const Vec3& from, const Vec3& to, const Vec3& centre, bool cw, cam::ArcPlane) = 0;
    virtual bool intersects(const struct ToolShape& nonCuttingPart, const Vec3& from, const Vec3& to) const = 0;
    virtual kernel::Mesh toMesh(std::uint32_t changedSinceVersion, Box3* dirtyOut) const = 0;   // incremental display mesh
    virtual kernel::TriSoup toSolid() const = 0;                                                // carried to the next setup
    virtual std::unique_ptr<IStockModel> clone() const = 0;                                     // timeline keyframes
    virtual std::uint32_t version() const = 0;
};
// CPU tri-dexel (three orthogonal dexel grids) so stock survives re-orientation between setups.
std::unique_ptr<IStockModel> makeTriDexelCpu(IJobSystem&);
// High tier, v2: same interface on GPU compute through render::IComputeBackend. Not part of v1.
std::unique_ptr<IStockModel> makeTriDexelGpu(render::IComputeBackend&);

struct ToolShape {            // revolved profile built from lib::Tool + lib::Holder + stick-out
    std::vector<Vec2> cuttingProfile;      // (radius, height) polyline from tip up to flute length
    std::vector<Vec2> shankProfile;
    std::vector<Vec2> holderProfile;
};
ToolShape makeToolShape(const lib::Tool&, const lib::Holder*, double stickOut);

// ---------- Events ----------
enum class Severity { Info, Warning, Error };
enum class EventKind {
    RapidIntoStock,            // Error, tier-independent
    HolderHitsStock, ShankHitsStock, ToolHitsFixture, HolderHitsFixture, HolderHitsPart,   // Error, tier-independent
    Gouge,                     // tool cuts below the design surface by more than tolerance
    PlungeWithNonCentreCutting, ExcessEngagement, FeedIntoStockAtRapidHeight,
    TravelLimit,               // move exceeds machine axis travel
    ToolTooShort               // required flute length or stick-out exceeds the tool; suggests minimum safe stick-out
};
struct SimEvent {
    EventKind kind; Severity severity;
    OperationId operation; std::uint32_t moveIndex;
    Vec3 position; double amountMm;       // gouge depth, overtravel or required extra stick-out
    std::string message;
};

// ---------- Simulator ----------
struct SimProgram {                        // one setup
    const cam::Setup* setup;
    std::vector<const cam::Operation*> ops;
    std::vector<std::shared_ptr<const cam::Toolpath>> toolpaths;   // parallel to ops
    std::vector<ToolShape> tools;                                  // parallel to ops
    kernel::TriSoup partSolid, stockSolid;
    std::vector<kernel::TriSoup> fixtureSolids;
    Xform modelFromWcs;
    const lib::MachineDef* machine;
};
struct SimOptions { StockResolution resolution; double gougeTolMm = 0.05; bool stopOnError = false; };

struct TimelinePosition { std::uint32_t opIndex; std::uint32_t moveIndex; double t01; };   // t01 = fraction along the move

class ISimulator {
public:
    virtual ~ISimulator() = default;
    // Analytic safety checks only (no stock model). Fast enough to run after every toolpath generation.
    virtual Result<std::vector<SimEvent>> verifyFast(const SimProgram&, JobContext&) = 0;
    // Full material-removal run. Stores a stock keyframe every N moves so scrubbing backwards is fast.
    virtual Status run(const SimProgram&, const SimOptions&, JobContext&) = 0;
    virtual std::span<const SimEvent> events() const = 0;               // sorted by (opIndex, moveIndex)
    virtual Status seek(TimelinePosition) = 0;                          // budget: under 250 ms on Low via keyframes
    virtual const IStockModel& stock() const = 0;
    virtual Xform toolPose() const = 0;
    virtual std::uint64_t totalMoves() const = 0;
    virtual kernel::TriSoup finalStock() const = 0;                     // feeds cam::FromSetupStock
};
std::unique_ptr<ISimulator> makeSimulator(IJobSystem&, std::unique_ptr<IStockModel>);

// As-machined vs design comparison (v2): signed distance per display vertex.
struct DeviationMap { std::vector<float> perVertexMm; double maxLeftover, maxGouge; };

} // namespace ray::sim
