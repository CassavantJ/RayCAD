// The PlaneGCS sketch solver behind ISketchSolver (task T-090). Private to src/sketch and its tests.
//
// Full solve (solveSketch): translate (solver_translate.h), diagnose with PlaneGCS (GCS::System::diagnose), then
// solve with the same partition and DogLeg iteration as GCS::System::solve, run here (solver_iterate.h) so that
// cancellation is checked between iterations. The result has the same bits as System::solve (tested). When PlaneGCS
// reports partially redundant constraints, the constraint objects it set aside are not known outside System, so
// System::solve itself runs (it cannot be interrupted).
//
// Cancellation latency: cancel is checked before and after the diagnosis, between components and before every
// DogLeg iteration. The diagnosis (one or two QR decompositions, and a DogLeg solve when constraints are redundant)
// cannot be interrupted; it is the longest step. Documented bound: one diagnosis, which for one coupled group of 200
// curves on the reference machine (Core i9-13900K) took at most 0.29 s from cancel to return with the machine lightly
// loaded and 0.53 s with other builds running, against 0.46 to 0.82 s for the whole uncancelled solve
// (solver_cancel_test.cpp, which allows the larger of 1 s and twice the uncancelled solve). Drag begin likewise. A drag
// step (move) takes no JobContext: it is bounded by its time budget instead (PLAN.md section 9), except in
// DragMode::PlaneGcs, which takes PlaneGCS's own time (spike S7: 0.4 to 0.8 s per step at 200 curves).
//
// Drag session (solver_drag.cpp): see DragMode.
#pragma once

#include <ray/sketch.h>

#include <memory>
#include <optional>

namespace ray::sketch::detail {

// Pins Eigen's cache sizes (adapter rule 2 of PLAN.md section 8) once per process. Every solver entry point calls it
// before any Eigen code runs; it is cheap after the first call.
void pinEigenCacheSizes();
// The pinned values (bytes): L1, L2, L3.
constexpr long long kEigenL1 = 32 * 1024;
constexpr long long kEigenL2 = 1024 * 1024;
constexpr long long kEigenL3 = 8 * 1024 * 1024;

struct SolveOptions {
    // Test hook: solve with GCS::System::solve instead of the cancellable copy of its iteration.
    bool planeGcsSolve = false;
};
Result<SolveResult> solveSketch(const Sketch&, JobContext&, const SolveOptions& = {});

// How a drag session solves a step. Every step solves from the drag-start state.
enum class DragMode {
    // Spike S11's recommendation: the reduced drag system (only the parameters still free, an independent subset of
    // the rows) solved with the sparse drag iteration; a step is accepted only if it succeeded, every constraint of
    // the whole sketch holds to 1e-9 and the point moved when it could. Otherwise that step is a FullSparse step.
    // Used when the structural check at drag begin passes.
    Reduced,
    // The whole sketch with the sparse drag iteration (same iteration as PlaneGCS, sparse linear algebra).
    FullSparse,
    // GCS::System::solve with FreeCAD's temporary coincident constraint: PlaneGCS's own drag. Used when PlaneGCS
    // reports partially redundant constraints, and as the reference in tests.
    PlaneGcs,
};
const char* dragModeName(DragMode);

struct DragOptions {
    std::optional<DragMode> force; // test hook: use this mode (Reduced still falls back per step)
};

struct DragStats {
    DragMode mode = DragMode::FullSparse;
    bool reducedUsable = false; // the structural check at drag begin passed
    bool pointFree = false;     // the dragged point has a free parameter
    int unknowns = 0;
    int freeParams = 0;
    int keptRows = 0;
    int moves = 0;
    int reducedSteps = 0; // moves answered by the reduced system
    int fullSteps = 0;    // moves solved on the full sketch (FullSparse or PlaneGcs, including fallbacks)
    int fallbacks = 0;    // Reduced-mode moves that fell back to the full sparse step
    int failedMoves = 0;
};

Result<std::unique_ptr<ISketchSolver::IDragSession>> beginDragSession(const Sketch&, SketchEntId point, JobContext&,
                                                                      const DragOptions& = {});
// Statistics of a session made by beginDragSession (or by the solver's beginDrag); empty for any other session.
std::optional<DragStats> dragStats(const ISketchSolver::IDragSession&);

} // namespace ray::sketch::detail
