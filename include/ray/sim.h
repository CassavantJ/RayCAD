// RayCAD interface contract: backplot, material-removal simulation, collision and gouge checks
// Module: src/sim   Depends on: core, kernel (TriSoup), cam, lib
// Status: PLAN DRAFT.
//
// Tier rule: the tier changes RESOLUTION and SPEED of the stock model, never the toolpath.
// Verdicts that gate posting (rapid-into-stock, holder collision, fixture collision) are computed
// analytically from the toolpath IR and exact triangulations, so they are tier-independent.
// Resolution-dependent results (gouge depth, leftover map) are reported with their tolerance.
//
// Changed in T-070 (CPU tri-dexel stock and tool shapes; each also in docs/interfaces/sim.h):
//  - IStockModel::CutMove can be an arc (kind, centre, plane), so arcs go into cutBatch with the lines around them
//    (spike S6: per-move calls that thread inside are 11x slower than batches). Existing {from, to, rapid} still works.
//  - cutBatch takes an optional per-move output `nonCuttingHits`: the shank and holder (ToolShape::shankProfile,
//    holderProfile) never remove material, and whether they met material is found in the same ordered pass.
//  - The comments of the stock model and of ToolShape now state the semantics T-070 implements (frame, flute length,
//    rapids, the empty soup, toMesh tiles and budget, versions, snapshots, thread safety).
//
// Changed in T-071 (safety checks; each also in docs/interfaces/sim.h; src/sim/verify.h says what is checked and how):
//  - EventKind::ShankHitsPart, added at the end: the shank meeting the part had no kind (HolderHitsPart did).
//  - SimProgram::toolDefs (may be empty): the library tool of each operation, which the checks need and a ToolShape
//    does not hold (whether it cuts at its centre; its overall length and shank for the stick-out it allows).
//  - SimProgram's solids are in the setup WCS, the toolpaths' frame (it said nothing).
//  - verifyFast: what it checks, against what, and its cut-stock check at a fixed cell (it said "no stock model").
//
// Changed in T-072 (simulator, keyframes, gouges; each also in docs/interfaces/sim.h; src/sim/simulator.h says how):
//  - SimProgram: partSolid must be triangulated within 0.009 mm of the design part (the checks and gouges rely on it).
//  - SimOptions::safetyChecks and SimOptions::gouges, added at the end (both true by default): a run that only carries
//    stock to the next setup (T-075) turns them off. gougeTolMm: what it is measured against and its minimum.
//  - ISimulator::position() and runEnd(), added at the end: where the stock and tool are, and where the run stopped.
//  - The comments of ISimulator now state what run, events, seek, stock, toolPose, totalMoves and finalStock do, the
//    timeline's positions, cancel and threading.
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
// Cycles move as cam.h "Canned cycles" says. `machine` (may be null) gives the peck clearance of the cycles whose move
// leaves it to the control (cam::peckClearanceNm); added by the canned-cycle settlement.
Backplot buildBackplot(const cam::Toolpath&, const Xform& modelFromWcs, double chordTol,
                       const lib::MachineDef* machine = nullptr);

// ---------- Stock model ----------
// From the tier preset: Low 0.2, Medium 0.1, High 0.05 (PLAN.md 6.2). An UPPER BOUND: the model snaps the cell
// per axis to extent / ceil(extent / cellMm) so the stock is spanned exactly (without it S6 saw a +2% volume error).
struct StockResolution { double cellMm; };

// Semantics (settled in T-070; src/sim/tridexel.h says how the CPU model does it):
//  - Frame: one frame for the stock, the moves and every output, in mm (T-072 uses the setup WCS). The tool axis is +Z
//    of that frame and a move's points are the tool TIP (the lowest point on the axis). Material outside `bounds` does
//    not exist.
//  - Cutting: only ToolShape::cuttingProfile removes material, swept exactly (to float rounding of the stored depths)
//    along each line; arcs are split into chords whose sagitta is at most a tenth of the cell. So the flute length
//    limits the cut: what the shank or holder pass through stays, and cutBatch's nonCuttingHits (or intersects) says
//    that they met it. A rapid cuts like a feed move; removedMm3 > 0 on a rapid means it went into material (at this
//    resolution: RapidIntoCutStock is the simulator's verdict, not this number's).
//  - Removed volume per move is the material that move really took away (no double counting of overlapping moves),
//    measured on the rays along Z; it differs from the exact swept volume by at most one cell layer over the cut's
//    surface.
//  - Results (stock contents, removed volumes, hits, meshes) do not depend on thread count, batch size or how moves are
//    split across calls; they are bit-identical between builds of one platform.
//  - Threading: one thread at a time may call the non-const functions; const functions may run together. A Snapshot is
//    immutable and may be kept and released on any thread.
class IStockModel {
public:
    virtual ~IStockModel() = default;
    // Material = inside `stockSolid` (closed triangle soup, either orientation; nonzero winding along the rays) and
    // inside `bounds`. An EMPTY soup means the whole `bounds` box (a block of stock). The cell is snapped per axis.
    virtual void reset(const kernel::TriSoup& stockSolid, const Box3& bounds, StockResolution) = 0;
    // Remove the volume swept by the cutting part of the tool along one move. Returns removed volume in mm^3.
    virtual double cut(const struct ToolShape&, const Vec3& from, const Vec3& to, bool rapid) = 0;
    // An arc as cam.h and buildBackplot read it: plane XY (G17), XZ (G18, seen from +Y), YZ (G19, seen from +X); sweep
    // in (0, 360] degrees (start == end is a full circle); helical when the coordinate along the plane normal changes.
    virtual double cutArc(const struct ToolShape&, const Vec3& from, const Vec3& to, const Vec3& centre, bool cw, cam::ArcPlane) = 0;
    // Batch of moves with one tool, applied in order. This is the fast path: one cut() per move with
    // threading inside was 11x slower in spike S6. The result must not depend on thread count or batch size.
    // `kind` Linear (the default; Rapid reads the same) is a straight move; ArcCW or ArcCCW an arc about `centre` in
    // `plane`, as cutArc. removedMm3: empty, or one entry per move. nonCuttingHits: empty, or one entry per move, set
    // to 1 where the shank or holder of the ToolShape met material during that move (checked before the move cuts when
    // it does not go down, after it when it does, so material its own flutes clear on the way down is no hit), else 0.
    // Like intersects, it is RESOLUTION-DEPENDENT.
    struct CutMove {
        Vec3 from, to; bool rapid;
        cam::MoveKind kind = cam::MoveKind::Linear; Vec3 centre{}; cam::ArcPlane plane = cam::ArcPlane::XY;
    };
    virtual void cutBatch(const struct ToolShape&, std::span<const CutMove>, std::span<double> removedMm3,
                          std::span<std::uint8_t> nonCuttingHits = {}) = 0;
    // RESOLUTION-DEPENDENT: material thinner than one cell between rays can be missed. Not a safety verdict on
    // its own; see EventKind::RapidIntoCutStock. Tests the union of ALL profiles of the shape passed (leave
    // cuttingProfile empty to test only the shank and holder) along a straight move, against the material as it is;
    // the top of the last non-empty profile continues straight up without end (the spindle side). Read-only.
    virtual bool intersects(const struct ToolShape& nonCuttingPart, const Vec3& from, const Vec3& to) const = 0;
    // Incremental display mesh. maxTriangles caps the output (a full 0.05 mm heightfield is 12.4 M triangles).
    // The stock's XY is divided into fixed tiles; a tile is dirty when material in it changed after version
    // `changedSinceVersion` (0: every tile). The mesh covers every tile inside *dirtyOut, the XY box of the dirty tiles
    // (on tile edges) with the stock's full Z range; nothing changed: an empty mesh and an empty box (min > max). Every
    // triangle lies inside one tile's closed XY rectangle, and the triangles of each tile form a CLOSED surface on their
    // own (every edge met by triangles in pairs running opposite ways; outward normals, counter-clockwise from outside):
    // the stock in that tile, capped by walls where the tile's edge cuts material. Mesh::faces has one range of
    // triangles per tile (face SubId{Face, tile number}, tiles numbered row by row from the stock's min corner), so a
    // viewer replaces the old triangles of exactly those tiles, and what it shows stays closed (the caps of neighbouring
    // tiles lie back to back inside the stock). Where the full-resolution mesh would exceed maxTriangles, columns of 2,
    // 4, 8 ... cells (up to one tile) are merged, their material united, until it fits. Never more than maxTriangles,
    // unless even one column per tile exceeds it (then that coarsest mesh).
    virtual kernel::Mesh toMesh(std::uint32_t changedSinceVersion, Box3* dirtyOut, std::uint32_t maxTriangles) const = 0;
    // Carried to the next setup: a closed, welded, outward-oriented soup of the material at full resolution (columns
    // of one cell along Z). Its size grows with 1 / cell^2: carry stock at a coarse, fixed resolution.
    virtual kernel::TriSoup toSolid() const = 0;
    // An independent copy. The CPU model shares unchanged strips copy-on-write, so it costs about what snapshot() does.
    virtual std::unique_ptr<IStockModel> clone() const = 0;
    // Timeline keyframes: cheaper than clone() (copy-on-write strips or deltas); restore() returns to it. restore()
    // ignores a snapshot of another model's grid. version() starts at 1 after reset and grows with every call that
    // can change the stock (reset, cut, cutArc, cutBatch, restore).
    struct Snapshot { virtual ~Snapshot() = default; };
    virtual std::shared_ptr<const Snapshot> snapshot() const = 0;
    virtual void restore(const Snapshot&) = 0;
    virtual std::uint32_t version() const = 0;
};
// CPU tri-dexel (three orthogonal dexel grids) so stock survives re-orientation between setups.
std::unique_ptr<IStockModel> makeTriDexelCpu(IJobSystem&);
// High tier, v2: same interface on GPU compute through render::IComputeBackend. Not part of v1.
std::unique_ptr<IStockModel> makeTriDexelGpu(render::IComputeBackend&);

// Each profile is a (radius, height) polyline in mm, height above the tip, read as the solid of revolution about +Z
// that it bounds (closed along the axis at its lowest and highest points). Heights must not decrease along a profile
// (a level step is a shoulder); any such shape works, convex or not (src/sim/toolshape.h). The three follow each
// other upward: cuttingProfile from the tip (first point (0, 0) or (r, 0)) up to the flute length, the only part that
// removes material; shankProfile from there (neck, shoulder, shank) up to the holder's nose; holderProfile from the
// nose up to the gauge line. The non-cutting part is shank plus holder; where the stock model needs an upper end, the
// top of the last profile goes on upward without end (spindle). Joined, they are render::IViewport::addToolGlyph's
// revolvedProfile.
struct ToolShape {            // revolved profile built from lib::Tool + lib::Holder + stick-out
    std::vector<Vec2> cuttingProfile;      // (radius, height) polyline from tip up to flute length
    std::vector<Vec2> shankProfile;
    std::vector<Vec2> holderProfile;
};
// Shapes follow the toolpath invariant checker (tests/unit/cam/invariants/tool_profile.h) and the tool library's
// drawing: corner and ball arcs as chords within 1 um; `stickOut` is the holder's nose above the tip (0: the tool's
// overall length, else 6 diameters); without a holder holderProfile is empty and the shank ends at the overall length.
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
    ToolTooShort,              // required flute length or stick-out exceeds the tool; suggests minimum safe stick-out
    ShankHitsPart              // Error, tier-independent (added by T-071)
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
    // Closed soups in the setup WCS (the toolpaths' frame), mm. stockSolid is the stock at the start of the setup.
    // partSolid must be triangulated within 0.009 mm of the design part (added by T-072: the checks allow a rapid that
    // much into it, and gouges allow the part's chords that much; a coarser soup reads as gouges and rapids into the
    // part where a pass follows a curved face).
    kernel::TriSoup partSolid, stockSolid;
    std::vector<kernel::TriSoup> fixtureSolids;
    Xform modelFromWcs;
    const lib::MachineDef* machine;
    // Added by T-071: parallel to ops, the library tool (CamState's embedded copy); empty, shorter or null: not known
    // (the checks then take the tool as not cutting at its centre and give no stick-out limit).
    std::vector<const lib::Tool*> toolDefs = {};
};
// resolution: the stock model's cell (the tier preset's; a run that carries stock uses its own fixed one).
// gougeTolMm (settled in T-072): how far the cutting part of the tool may go into the PART (not the part plus stock to
// leave) before it is a Gouge; measured exactly against the part's triangles, not on the stock cells, so it does not
// depend on the resolution; raised to 0.03 mm when smaller (below that the part's and the tool's chords could read as a
// gouge on a pass that follows the design surface). stopOnError: the removal stops at the end of the move of the first
// Error event. Added by T-072: safetyChecks (verifyFast's checks run inside the run, their events in events()) and
// gouges (the gouge test); off, run() only removes material.
struct SimOptions { StockResolution resolution; double gougeTolMm = 0.05; bool stopOnError = false;
                    bool safetyChecks = true; bool gouges = true; };

// A place on the timeline: every move before moveIndex of operation opIndex (and every earlier operation) done, and of
// move moveIndex the fraction t01 of its path's length (arcs and canned cycles as buildBackplot draws them; zero-length
// motions at its start included). (op, m, 1) is (op, m + 1, 0). Settled in T-072.
struct TimelinePosition { std::uint32_t opIndex; std::uint32_t moveIndex; double t01; };   // t01 = fraction along the move

class ISimulator {
public:
    virtual ~ISimulator() = default;
    // Safety checks (T-071, src/sim/verify.h): exact sweeps of the tool against the initial stock, part and fixture
    // triangles (rapids, holder and shank, plunges of tools that do not cut at their centre, tool too short) and the
    // machine's travel. A stock contact the exact test cannot settle, because earlier moves may have cut that
    // material, is checked on the checker's own stock model at a FIXED cell, never the tier's, which its event's
    // message states; that model runs only when there is such a contact. Results do not depend on the tier or the
    // thread count. Fast enough to run after every toolpath generation.
    virtual Result<std::vector<SimEvent>> verifyFast(const SimProgram&, JobContext&) = 0;
    // Full material-removal run (settled in T-072): the checks and the gouge test (SimOptions), then the stock model is
    // reset from stockSolid in its bounding box (the setup WCS) and every move is cut through cutBatch in batches,
    // keeping stock keyframes (snapshots) as it goes so a seek recuts only from the nearest one; the spacing follows the
    // work done and is widened to keep the keyframes' memory within a budget (src/sim/simulator.h, which gives the seek
    // times that follow). Progress on ctx.progress, cancel polled between batches (tens of milliseconds; up to about
    // 0.2 s in long facing moves at 0.05 mm). Afterwards the stock and the tool are at runEnd(). It copies what it needs:
    // the SimProgram may go away after it returns. InvalidArgument without a stock solid. A cancelled or failed run leaves
    // the simulator empty (totalMoves 0, no events, seek refused) and ready for another run; stock() is then undefined.
    // A run replaces the last one (its keyframes too).
    virtual Status run(const SimProgram&, const SimOptions&, JobContext&) = 0;
    // The checks' events and the gouges, sorted by (opIndex, moveIndex), at one move the checks' kinds first. Gouge:
    // amountMm the depth (an upper bound, within about 0.015 mm), moveIndex the first of a run of gouging moves of one
    // operation, the message names the deepest. Kept until the next run.
    virtual std::span<const SimEvent> events() const = 0;               // sorted by (opIndex, moveIndex)
    // Puts the stock and the tool at a position (see TimelinePosition; an operation or move past the end, or past where
    // stopOnError stopped, means runEnd(); t01 is clamped). A forward seek with no keyframe between continues from where
    // the stock is (playback costs the moves played); otherwise the last keyframe at or before the position is restored
    // and the moves from it cut. The stock at a position is the same, bit for bit, however it was reached. No cancel:
    // its time is bounded by the keyframe spacing. InvalidArgument before a successful run.
    virtual Status seek(TimelinePosition) = 0;                          // budget: PLAN.md section 9, via IStockModel::snapshot keyframes
    // The live stock model, in the setup WCS (place its meshes with SimProgram::modelFromWcs). Its version() and
    // toMesh(changedSinceVersion) give the tiles a seek or run changed.
    virtual const IStockModel& stock() const = 0;
    // The tool at position(): modelFromWcs with the tool tip as its origin and the WCS's +Z as the tool axis, ready for
    // render::IViewport::setTransform of a tool glyph (whose tip is at its origin). Before a run: identity.
    virtual Xform toolPose() const = 0;
    virtual std::uint64_t totalMoves() const = 0;                       // IR moves of every operation of the last run
    // The stock at runEnd() (whatever position() is): closed, welded, outward-oriented, in the setup WCS, at the run's
    // resolution (IStockModel::toSolid: its size grows with 1 / cell^2, up to about 0.3 s at 0.1 mm). Carried stock
    // needs a fixed, coarse cell that does not come from the tier: run a simulator of its own at that cell, with
    // safetyChecks and gouges off. Empty before a successful run.
    virtual kernel::TriSoup finalStock() const = 0;                     // feeds cam::FromSetupStock
    // Added by T-072: where the stock and the tool are (the position the last seek resolved to; after a run, runEnd()).
    virtual TimelinePosition position() const = 0;
    // Added by T-072: the end of the last run: the last move of the last operation with moves at t01 1, or the move of
    // the first Error event when stopOnError stopped it there. {0, 0, 0} before a run or for a program without moves.
    virtual TimelinePosition runEnd() const = 0;
};
// The stock model may be null (a CPU tri-dexel model is made). Threads: one at a time calls run, seek and verifyFast;
// the const functions may run together, but not during a run or a seek. Everything is DETERMINISTIC: events, removed
// volumes and the stock do not depend on the worker count, the batching or the keyframe spacing (src/sim/simulator.h).
std::unique_ptr<ISimulator> makeSimulator(IJobSystem&, std::unique_ptr<IStockModel>);

// As-machined vs design comparison (v2): signed distance per display vertex.
struct DeviationMap { std::vector<float> perVertexMm; double maxLeftover, maxGouge; };

} // namespace ray::sim
