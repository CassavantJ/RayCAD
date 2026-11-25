// The on-screen simulation of one setup (task T-074): the simulator of include/ray/sim.h driven for the desktop's
// Simulate workspace (src/ui/cam/sim_tool.h), without Qt. A sibling of ray_api/cam_sim.h; not one of the contract
// headers in include/ray/.
//
// WHY HERE. src/ui may call only the api layer (PLAN.md 1.1): this file holds everything of the simulation that needs
// sim, cam or post (the simulator, the backplot expansion, the cycle-time estimate, the stock's display meshes), and
// hands the UI plain data: positions, times, meshes and transforms.
//
// THE TIMELINE (buildTimeline). Its axis is estimated MACHINE TIME, in seconds. Each operation starts with its tool
// change (cam::estimateCycleTime's toolChangeSec for it, 0 when the tool stays), then its moves one after the other. A
// move lasts what its path takes as buildBackplot draws it (canned cycles expanded, arcs as chords): straight segments
// at the programmed feed (held under the machine's lowest maximum feed of X, Y and Z when known), rapids at each axis's
// own rapid rate (the slowest axis decides, as a dogleg G0 does), plus the dwell of a Dwell move or a cycle. Then each
// operation's moves are scaled together so that the operation lasts what cam::estimateCycleTime says (its acceleration
// and corner rules), so the timeline's total is the CAM tree's cycle-time estimate. Within one move time is spread
// evenly along its path (t01 is a fraction of its length, sim.h TimelinePosition), except in a canned cycle: there the
// time follows the cycle's own motion as buildBackplot expands it (the rapids down to R and back, each feed of a peck,
// each retract, each at its own rate, and the cycle's dwell at the hole's bottom), so a time inside the cycle puts the
// tool where the control would have it (TimelineOperation::profiles; added after T-074). Inside a dwell the tool stands
// at the bottom; secondsAt of that place is the dwell's start. The moves' starts and ends are exact on the axis.
//
// STEPS. "One move per step": a move boundary is "after move f" for f = 0..moveCount (flat move index over the whole
// program); afterMove(0) is the program's start. step(+1) from anywhere goes to the end of the move the position is in
// (or of the next one when it is at an end already); step(-1) to the start of the move it is in, or of the one before.
//
// EVENTS. Each event's place on the timeline (EventPlace): its operation and move, and the fraction t01 of that move's
// path where the tool tip comes nearest the event's position (the tip where the contact or gouge was first found), so
// seeking there shows the tool at the contact. An event of the setup (sim::kSetupEvent) has no move: it is placed at
// the start, before the first move.
//
// THE SESSION (SimSession). One simulator for one run. run() and seek() must be called one at a time (they may be on
// different worker threads): the caller is the serial queue (the UI keeps at most one job on a session). run() builds
// the program (cam_sim::buildSimProgram, so its kernel work is on that worker), runs it at the requested cell, builds
// the timeline, and returns the first display mesh at the run's end. seek() moves the stock and the tool and returns
// what changed of the display mesh, asked as ray_render/viewport.h "Simulation stock" says (MeshAsk): the dirty box
// first at the coarsest level, then that box again with the display's triangle budget times the box's share of the
// stock's area, so a small change uploads a small part of the display; `cap` bounds that per frame during playback (the
// tiles then come coarser, and a full refresh later restores the display's level of detail).
#pragma once

#include <ray/api.h>
#include <ray_api/cam_sim.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ray::api::cam_sim {

// ---------- The timeline ----------
// How time runs along a move whose parts run at different rates (a canned cycle): knots (fraction of the move's time,
// fraction of its path's length) from (0, 0) to (1, 1), both non-decreasing; linear between knots. A dwell is a knot
// pair with the same length.
struct MoveProfile {
    std::uint32_t move = 0;
    std::vector<std::pair<double, double>> knots;
    double lengthAt(double timeFraction) const; // t01 of the place at that share of the move's time
    double timeAt(double lengthFraction) const; // the share of the move's time when the tool first gets there
};

struct TimelineOperation {
    OperationId id;
    std::string name;
    double startSec = 0;               // where its tool change begins
    double toolChangeSec = 0;          // at its start
    double endSec = 0;                 // the end of its last move
    std::uint64_t firstMove = 0;       // flat index of its first move
    std::vector<double> moveEndSec;    // per IR move, when it ends (absolute, non-decreasing)
    std::vector<MoveProfile> profiles; // sorted by move: the moves whose time is not even along the path (cycles)
    double firstMoveSec() const { return startSec + toolChangeSec; }
    std::uint64_t moves() const { return moveEndSec.size(); }
    const MoveProfile* profileOf(std::uint32_t move) const; // null: time is even along that move
};

struct Timeline {
    std::vector<TimelineOperation> ops; // program order (SimProgram::ops)
    double totalSec = 0;
    std::uint64_t moveCount = 0;
    bool scaledToEstimate = false; // the operations were scaled to cam::estimateCycleTime (a machine was known)

    // Seconds from the start at a position, and the position at a time (clamped to the timeline).
    double secondsAt(const sim::TimelinePosition& p) const;
    sim::TimelinePosition positionAt(double sec) const;
    // The start of the program and the end of its last move ({0, 0, 0} without moves).
    sim::TimelinePosition start() const;
    sim::TimelinePosition end() const;
    // Moves wholly done at a position (0 .. moveCount), and the position after `done` moves (see STEPS).
    std::uint64_t movesDone(const sim::TimelinePosition& p) const;
    sim::TimelinePosition afterMoves(std::uint64_t done) const;
    // `moves` whole moves forward (> 0) or back (< 0), as STEPS says.
    sim::TimelinePosition step(const sim::TimelinePosition& p, int moves) const;
    // The same position written one way: t01 in [0, 1], an operation or move past the end clamped.
    sim::TimelinePosition normalised(const sim::TimelinePosition& p) const;
};

struct EventPlace {
    std::uint32_t opIndex = 0;   // into Timeline::ops
    std::uint32_t moveIndex = 0; // the event's move
    double t01 = 0;              // where the tool tip comes nearest the event's position along that move
    double sec = 0;              // its time on the timeline
};

// The timeline of a program (on a worker: it expands every toolpath as buildBackplot does). `estimate` may be null (no
// machine): the moves keep the times worked out from their feeds and the defaults of cam.h's cycle-time section.
// `events` (may be empty) get their places in `places` (parallel).
Timeline buildTimeline(const sim::SimProgram& program, const cam::CycleTime* estimate,
                       std::span<const sim::SimEvent> events = {}, std::vector<EventPlace>* places = nullptr);

// ---------- The session ----------
// How a display mesh is asked for (see the header). budget: the display's triangle budget for the whole stock.
struct MeshAsk {
    std::uint32_t shownVersion = 0; // IStockModel::version() of what the display shows (0: nothing shown)
    bool full = false;              // the whole stock at `budget` (a new item, or the level of detail restored)
    std::uint32_t budget = 400'000;
    std::uint32_t cap = 0; // > 0: at most this many triangles for a dirty region (playback)
};

// What the display shows after a run or a seek.
struct Frame {
    sim::TimelinePosition position{0, 0, 0};  // where the stock and the tool are (ISimulator::position)
    Xform toolPose = Xform::identity();       // ISimulator::toolPose: the tool glyph's transform (model frame)
    std::shared_ptr<const kernel::Mesh> mesh; // null: the display stays as it is
    std::optional<Box3> dirty;                // with a mesh that is not full: its dirty box (stock frame, the WCS)
    bool full = false;                        // the mesh is the whole stock (replaces what is shown)
    bool coarse = false;                      // a dirty region came below the display's level of detail (cap)
    std::uint32_t version = 0;                // IStockModel::version() the mesh brings the display to
    std::uint64_t triangles = 0;              // in `mesh`
    double seekMs = 0, meshMs = 0;            // time taken by the seek and by the meshes (diagnostics)
};

struct RunRequest {
    doc::DocStatePtr state;
    doc::BuildResultPtr build;
    SetupId setup;
    // The setup's operations that are not suppressed, in its order, each with its up-to-date toolpath (the panel's gate
    // says they are) and its tool number for the cycle-time estimate (parallel).
    std::vector<std::shared_ptr<const cam::Toolpath>> toolpaths;
    std::vector<int> toolNumbers;
    // For a setup whose stock comes from an earlier one: ICamEngine::carriedStock's result.
    std::shared_ptr<const cam::CarriedStock> startStock;
    double cellMm = 0.1; // the tier preset's simulation cell (render::TierPreset::simCellMm)
    double gougeTolMm = 0.05;
    MeshAsk mesh; // the first display mesh (full)
    // Tests only (empty in the app; added after T-074): events added to the simulator's before anything reads them, so
    // a kind or a severity that no check raises yet goes the whole way the real ones go (placed on the timeline, in
    // RunResult::events, the panel's markers and rows). `operation` must be one of the program's (or, for a setup's
    // event, nil with moveIndex sim::kSetupEvent).
    std::vector<sim::SimEvent> addedEvents;
    // Tests only (unset in the app; added with the slow-seek notice): replaces the run's sim::SeekReport in RunResult,
    // so the panel's notice can be shown without a program that takes gigabytes to thin its keyframes.
    std::optional<sim::SeekReport> seekReport;
};

struct RunResult {
    SetupId setup;
    std::string setupName;
    std::vector<OperationId> ops;      // the program's operations (SimProgram::ops)
    std::vector<std::string> opNames;  // parallel
    std::vector<Hash256> inputs;       // their toolpaths' input hashes (what the run was made of)
    std::vector<sim::ToolShape> tools; // parallel: the tool glyph of each
    // Parallel: each toolpath's backplot in the model frame, made as the CAM workspace's (api::cam_operation::backplot:
    // sim::buildBackplot with modelFromWcs and the setup's machine, arcs within 0.01 mm), for the Simulate workspace to
    // draw with IViewport::addBackplot (added after T-074). Null for an operation without a toolpath.
    std::vector<std::shared_ptr<const sim::Backplot>> backplots;
    // ISimulator::events. A setup's event (moveIndex sim::kSetupEvent: a fixture against the part or the stock; added
    // after T-074) belongs to no move: it comes first and its place is the start of the timeline ({0, 0, 0}, 0 s).
    std::vector<sim::SimEvent> events;
    std::vector<EventPlace> places; // parallel to events
    // ISimulator::seekReport of the run (added with the slow-seek notice): whether its keyframes were thinned to stay
    // in memory, and an estimate of its slowest seek.
    sim::SeekReport seek;
    Timeline timeline;
    Xform modelFromWcs = Xform::identity();
    Box3 stockBounds; // the stock model's box (setup WCS): the uncut faces of the display
    double cellMm = 0;
    double programSec = 0, runSec = 0, timelineSec = 0; // wall time of the three steps (diagnostics)
    Frame first;                                        // the display at the run's end (full)
};

class SimSession {
public:
    explicit SimSession(IJobSystem& jobs);
    ~SimSession();
    SimSession(const SimSession&) = delete;
    SimSession& operator=(const SimSession&) = delete;

    // Builds the program, runs it, makes the timeline and the first mesh. Progress and cancel through ctx. Errors: the
    // program's (buildSimProgram), the run's (Cancelled), InvalidArgument for a program without moves.
    Result<std::shared_ptr<RunResult>> run(const RunRequest& request, JobContext& ctx);
    // ISimulator::seek, then the display (above). InvalidArgument before a successful run. Not cancellable: its time is
    // bounded by the simulator's keyframe spacing.
    Result<Frame> seek(const sim::TimelinePosition& to, const MeshAsk& ask);
    // The display at the current position without moving (a full refresh after playback).
    Result<Frame> refresh(const MeshAsk& ask);

private:
    Frame frameNow(const MeshAsk& ask);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ray::api::cam_sim
