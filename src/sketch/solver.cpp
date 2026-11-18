// The PlaneGCS sketch solver (task T-090): ISketchSolver, full solve with diagnosis. See solver_internal.h.
#include "solver_internal.h"

#include "solver_iterate.h"
#include "solver_system.h"
#include "solver_translate.h"

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>

namespace ray::sketch::detail {

namespace {

Error cancelledError()
{
    return Error{ErrorCode::Cancelled, "Sketch solve cancelled", {"sketch solver"}};
}

} // namespace

void pinEigenCacheSizes()
{
    static std::once_flag once;
    std::call_once(once, [] { Eigen::setCpuCacheSizes(kEigenL1, kEigenL2, kEigenL3); });
}

std::unique_ptr<GCS::System> newSystem()
{
    pinEigenCacheSizes();
    // GCS::System's constructor sets Eigen's process-wide thread count (Eigen::setNbThreads(1)); constructing two
    // systems at once on different threads would race on that static.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    auto sys = std::make_unique<GCS::System>();
    sys->debugMode = GCS::NoDebug;
    return sys;
}

double maxConstraintError(const std::vector<GCS::Constraint*>& objects)
{
    double e = 0;
    for (GCS::Constraint* c : objects) {
        if (c->getTag() >= 0) {
            const double v = std::fabs(c->error());
            if (!(v <= e)) { // also catches NaN
                e = v;
            }
        }
    }
    return e;
}

SolveStatus statusFromDiagnosis(int dofs, bool redundant)
{
    if (redundant) {
        return SolveStatus::OverConstrained;
    }
    return dofs == 0 ? SolveStatus::FullyConstrained : SolveStatus::UnderConstrained;
}

Result<SolveResult> solveSketch(const Sketch& sketch, JobContext& ctx, const SolveOptions& options)
{
    pinEigenCacheSizes();
    if (auto st = ctx.cancel.check(); !st) {
        return tl::unexpected(st.error());
    }
    auto built = buildModel(sketch);
    if (!built) {
        return tl::unexpected(built.error());
    }
    GcsModel& m = **built;
    std::unique_ptr<GCS::System> sys = newSystem();
    std::vector<GCS::Constraint*> objects;
    if (auto st = emitConstraints(m,
                                  [&](GCS::Constraint* c) {
                                      sys->addConstraint(c);
                                      objects.push_back(c);
                                  });
        !st) {
        return tl::unexpected(st.error());
    }
    sys->declareUnknowns(m.unknowns);
    ctx.progress.set(0.05, "Analysing constraints");
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }

    // Diagnosis: PlaneGCS's own; it cannot be interrupted. Through initSolution, as FreeCAD does: it stores the
    // reference configuration first, so the solve diagnose() runs when constraints are redundant is undone afterwards
    // (diagnose() on its own would leave the parameters at that solve's result).
    sys->initSolution();
    const int dofs = sys->dofsNumber();
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }
    ctx.progress.set(0.5, "Solving");

    GCS::VEC_I conflicting, redundant, partial;
    sys->getConflicting(conflicting);
    sys->getRedundant(redundant);
    sys->getPartiallyRedundant(partial);
    SolveResult r;
    r.freeDof = std::max(dofs, 0);
    r.conflicting = tagIds(m, conflicting);
    r.redundant = tagIds(m, redundant);
    r.partiallyRedundant = tagIds(m, partial);
    if (!r.conflicting.empty()) {
        r.status = SolveStatus::Conflicting;
        r.solved = m.sketch;
        ctx.progress.set(1.0);
        return r;
    }

    int res = GCS::Failed;
    std::unique_ptr<Prepared> prep;
    if (options.planeGcsSolve || !partial.empty()) {
        res = sys->solve(); // starts from the reference initSolution stored: the input
        if (res == GCS::Success || res == GCS::Converged) {
            sys->applySolution();
        }
    } else {
        // The objects GCS::System::initSolution leaves out: those of fully redundant tags (with no partially
        // redundant tag, these are exactly the objects diagnose() set aside).
        const std::set<int> redundantTags(redundant.begin(), redundant.end());
        std::vector<GCS::Constraint*> kept, setAside;
        for (GCS::Constraint* c : objects) {
            (redundantTags.count(c->getTag()) ? setAside : kept).push_back(c);
        }
        prep = prepare(m.unknowns, kept);
        res = solvePrepared(*prep, *sys, &ctx.cancel);
        if (res == kSolveCancelled) {
            return tl::unexpected(cancelledError());
        }
        // The end of GCS::System::solve(): a redundant constraint that does not hold (evaluated, as upstream does,
        // before the solution is applied) turns Success into Converged.
        if (res == GCS::Success) {
            for (GCS::Constraint* c : setAside) {
                const double err = c->error();
                if (err * err > sys->convergence) {
                    res = GCS::Converged;
                    break;
                }
            }
        }
        if (res == GCS::Success || res == GCS::Converged) {
            applyPrepared(*prep);
        }
    }

    // Success means every constraint of the sketch, redundant ones included, holds at the result.
    const bool solved = (res == GCS::Success || res == GCS::Converged) && maxConstraintError(objects) <= kHoldTolerance;
    if (solved) {
        r.status = statusFromDiagnosis(dofs, !r.redundant.empty() || !r.partiallyRedundant.empty());
        r.solved = writeBack(m);
    } else {
        r.status = SolveStatus::Failed;
        r.solved = m.sketch;
    }
    ctx.progress.set(1.0);
    return r;
}

} // namespace ray::sketch::detail

namespace ray::sketch {

namespace {

class PlaneGcsSolver final : public ISketchSolver {
public:
    Result<SolveResult> solve(const Sketch& sketch, JobContext& ctx) override
    {
        return detail::solveSketch(sketch, ctx);
    }
    Result<std::unique_ptr<IDragSession>> beginDrag(const Sketch& sketch, SketchEntId point, JobContext& ctx) override
    {
        return detail::beginDragSession(sketch, point, ctx);
    }
};

} // namespace

std::unique_ptr<ISketchSolver> makePlaneGcsSolver()
{
    detail::pinEigenCacheSizes();
    return std::make_unique<PlaneGcsSolver>();
}

} // namespace ray::sketch
