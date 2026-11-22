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
// Layout (settled in T-037; src/sim/backplot.cpp says what each move kind becomes). `lineVertices` is ONE continuous
// line strip in the model frame (modelFromWcs applied), in mm. Vertex 0 is the toolpath start and belongs to no move
// (moveIndex 0xFFFFFFFF). Every later vertex ENDS the segment from the vertex before it and carries that segment's
// attributes: `role` and `kind` are cam::MoveRole and cam::MoveKind values, `feed` is in mm/min (0 for rapid motion),
// `moveIndex` is the IR move, `pad` is 0. So segment s runs from vertex s to s+1 with the attributes of vertex s+1:
// draw segments as instances reading the buffer twice, one vertex apart (a line strip with flat attributes would take
// them from the provoking vertex, which differs between graphics APIs: the first on Direct3D, the last on OpenGL).
// `kind` is the MOTION: Rapid (draw dashed), Linear, ArcCW or ArcCCW (chords). Canned cycles are expanded into the
// rapids and feeds the control makes, as Rapid and Linear vertices of the cycle's move. A move without motion
// (Dwell, CycleCancel, a zero-length move) has one zero-length vertex with its own kind.
// `moveFirstVertex` has moves.size() + 1 entries, the last one lineVertices.size(): the vertices of move m are
// [moveFirstVertex[m], moveFirstVertex[m + 1]), never empty, all with moveIndex m. Moves a to b (inclusive, as in
// setBackplotRange) are therefore the segments [moveFirstVertex[a] - 1, moveFirstVertex[b + 1] - 1), found without a
// search. ounds (added in T-037) is the box of every vertex as stored, so the renderer needs no pass of its own.
struct BackplotVertex { float x, y, z; std::uint8_t role; std::uint8_t kind; std::uint16_t pad; float feed; std::uint32_t moveIndex; };
struct Backplot { std::vector<BackplotVertex> lineVertices; std::vector<std::uint32_t> moveFirstVertex; Box3 bounds; };
// Arcs are flattened to `chordTol` (mm, clamped to at least 1e-4) for DISPLAY only: no chord is farther than chordTol
// from its arc (before float rounding of the vertices). Deterministic: identical bytes on every platform.
// Budget: 1 M moves in under 150 ms on the Low reference CPU.
Backplot buildBackplot(const cam::Toolpath&, const Xform& modelFromWcs, double chordTol);

// ---------- Stock model ----------
// From the tier preset: Low 0.2, Medium 0.1, High 0.05 (PLAN.md 6.2). An UPPER BOUND: the model snaps the cell
// per axis to extent / ceil(extent / cellMm) so the stock is spanned exactly (without it S6 saw a +2% volume error).
struct StockResolution { double cellMm; };

class IStockModel {
public:
    virtual ~IStockModel() = default;
    virtual void reset(const kernel::TriSoup& stockSolid, const Box3& bounds, StockResolution) = 0;
    // Remove the volume swept by the cutting part of the tool along one move. Returns removed volume in mm^3.
    virtual double cut(const struct ToolShape&, const Vec3& from, const Vec3& to, bool rapid) = 0;
    virtual double cutArc(const struct ToolShape&, const Vec3& from, const Vec3& to, const Vec3& centre, bool cw, cam::ArcPlane) = 0;
    // Batch of linear moves with one tool, applied in order. This is the fast path: one cut() per move with
    // threading inside was 11x slower in spike S6. The result must not depend on thread count or batch size.
    struct CutMove { Vec3 from, to; bool rapid; };
    virtual void cutBatch(const struct ToolShape&, std::span<const CutMove>, std::span<double> removedMm3) = 0;
    // RESOLUTION-DEPENDENT: material thinner than one cell between rays can be missed. Not a safety verdict on
    // its own; see EventKind::RapidIntoCutStock.
    virtual bool intersects(const struct ToolShape& nonCuttingPart, const Vec3& from, const Vec3& to) const = 0;
    // Incremental display mesh. maxTriangles caps the output (a full 0.05 mm heightfield is 12.4 M triangles).
    virtual kernel::Mesh toMesh(std::uint32_t changedSinceVersion, Box3* dirtyOut, std::uint32_t maxTriangles) const = 0;
    virtual kernel::TriSoup toSolid() const = 0;                                                // carried to the next setup
    virtual std::unique_ptr<IStockModel> clone() const = 0;                                     // full deep copy (214 MB, 57 ms at 0.05 mm)
    // Timeline keyframes: cheaper than clone() (copy-on-write strips or deltas); restore() returns to it.
    struct Snapshot { virtual ~Snapshot() = default; };
    virtual std::shared_ptr<const Snapshot> snapshot() const = 0;
    virtual void restore(const Snapshot&) = 0;
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
    RapidIntoStock,            // Error, tier-independent: analytic, against the INITIAL stock solid
    RapidIntoCutStock,         // Error: against in-process stock, at a fixed resolution that does not come from the tier
                               // preset; the message states that resolution
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
    virtual Status seek(TimelinePosition) = 0;                          // budget: PLAN.md section 9, via IStockModel::snapshot keyframes
    virtual const IStockModel& stock() const = 0;
    virtual Xform toolPose() const = 0;
    virtual std::uint64_t totalMoves() const = 0;
    virtual kernel::TriSoup finalStock() const = 0;                     // feeds cam::FromSetupStock
};
std::unique_ptr<ISimulator> makeSimulator(IJobSystem&, std::unique_ptr<IStockModel>);

// As-machined vs design comparison (v2): signed distance per display vertex.
struct DeviationMap { std::vector<float> perVertexMm; double maxLeftover, maxGouge; };

} // namespace ray::sim
