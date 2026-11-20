// The PlaneGCS sketch solver behind ISketchSolver (task T-090). Private to src/sketch and its tests.
//
// Full solve (solveSketch): translate (solver_translate.h), diagnose per decoupled component (solver_diagnose.h:
// GCS::System::diagnose on PlaneGCS's public API), then solve with the same partition and DogLeg iteration as
// GCS::System::solve, run here (solver_iterate.h) so that cancellation is checked between iterations. A component below
// kDenseLimit unknowns gets PlaneGCS's own linear algebra and the same bits as System (tested); a larger one a sparse
// QR diagnosis and a sparse DogLeg step (kDenseLimit). The constraint objects the diagnosis sets aside are known here,
// so partially redundant constraints need no special path.
//
// Cancellation latency: cancel is checked before the diagnosis, between its components, between DogLeg iterations of
// its redundancy solve, between solve components and before every DogLeg iteration. One component's QR cannot be
// interrupted. Before the pre-M2 work the whole diagnosis could not be, and for one coupled group of 200 curves took
// up to 0.29 s from cancel to return (0.53 s under load); the whole solve now takes tens of milliseconds there
// (solver_cancel_test.cpp still allows the larger of 1 s and twice the uncancelled solve). Drag begin still runs
// PlaneGCS's own diagnosis (solver_drag.cpp), which cannot be interrupted. A drag step (move) takes no JobContext: it
// is bounded by its time budget instead (PLAN.md section 9), except in DragMode::PlaneGcs, which takes PlaneGCS's own
// time (spike S7: 0.4 to 0.8 s per step at 200 curves).
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

// Size switch between PlaneGCS's dense linear algebra and a sparse one (pre-M2 performance work). A decoupled component
// of the system with fewer unknowns than this (and fewer constraint rows) is diagnosed with FullPivHouseholderQR and
// solved with DogLeg's FullPivLU Gauss step, exactly as GCS::System does (same bits). A larger one is diagnosed with a
// sparse QR and solved with a sparse Gauss step: dense QR and LU cost the cube of the size (at 900 unknowns one dense
// DogLeg iteration took 150 ms and one diagnosis 400 ms). The sparse Gauss step is the least-norm solution of J h = -f:
// the same step as FullPivLU's when J is square and regular; on an under-constrained component FullPivLU's step moves
// only its pivot unknowns, so the solve ends at another point of the same solution set.
constexpr int kDenseLimit = 200;

struct SolveOptions {
    // Test hook: diagnose and solve with GCS::System (initSolution, solve) instead of the per-component diagnosis and
    // the cancellable copy of its iteration.
    bool planeGcsSolve = false;
    // Test hook: components with this many unknowns or more take the sparse diagnosis and DogLeg.
    int denseLimit = kDenseLimit;
};
Result<SolveResult> solveSketch(const Sketch&, JobContext&, const SolveOptions& = {});

// Counters of the solver's work on the calling thread (tests and benchmarks; they never affect a result). Reset by
// assigning {}.
struct SolverCounters {
    int denseDogLegs = 0;     // DogLeg runs with PlaneGCS's dense Jacobian and FullPivLU step
    int sparseDogLegs = 0;    // DogLeg runs with the sparse Jacobian and step
    int dogLegIterations = 0; // over both
    int denseDiagnoses = 0;   // components diagnosed with a dense QR
    int sparseDiagnoses = 0;  // components diagnosed with a sparse QR
    int redundancySolves = 0; // diagnosis solves that tell redundant rows from conflicting ones
};
SolverCounters& solverCounters();

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
