// The PlaneGCS sketch solver (task T-090): ISketchSolver, full solve with diagnosis. See solver_internal.h.
#include "solver_internal.h"

#include "solver_diagnose.h"
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
    if (options.planeGcsSolve) {
        sys->declareUnknowns(m.unknowns);
    }
    ctx.progress.set(0.05, "Analysing constraints");
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }

    SolveResult r;
    int dofs = 0;
    int res = GCS::Failed;
    if (options.planeGcsSolve) {
        // PlaneGCS's own diagnosis and solve (the reference in tests). Through initSolution, as FreeCAD does: it
        // stores the reference configuration first, so the solve diagnose() runs when constraints are redundant is
        // undone afterwards. Neither can be interrupted.
        sys->initSolution();
        dofs = sys->dofsNumber();
        GCS::VEC_I conflicting, redundant, partial;
        sys->getConflicting(conflicting);
        sys->getRedundant(redundant);
        sys->getPartiallyRedundant(partial);
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
        ctx.progress.set(0.5, "Solving");
        res = sys->solve(); // starts from the reference initSolution stored: the input
        if (res == GCS::Success || res == GCS::Converged) {
            sys->applySolution();
        }
    } else {
        // GCS::System::diagnose per decoupled component (solver_diagnose.h); it leaves the input values in place.
        auto diagnosis = diagnose(m.unknowns, objects, *sys, &ctx.cancel, options.denseLimit);

        if (!diagnosis) {
            return tl::unexpected(diagnosis.error());
        }
        dofs = diagnosis->dofs;

        r.freeDof = std::max(dofs, 0);
        r.conflicting = tagIds(m, diagnosis->conflictingTags);
        r.redundant = tagIds(m, diagnosis->redundantTags);
        r.partiallyRedundant = tagIds(m, diagnosis->partiallyRedundantTags);
        if (!r.conflicting.empty()) {
            r.status = SolveStatus::Conflicting;
            r.solved = m.sketch;
            ctx.progress.set(1.0);
            return r;
        }
        if (ctx.cancel.cancelled()) {
            return tl::unexpected(cancelledError());
        }
        ctx.progress.set(0.5, "Solving");
        // GCS::System::initSolution's partition and System::solve over every object the diagnosis did not set aside.
        std::vector<GCS::Constraint*> kept, setAside;
        for (std::size_t k = 0; k < objects.size(); ++k) {
            (diagnosis->setAside[k] ? setAside : kept).push_back(objects[k]);
        }
        const std::unique_ptr<Prepared> prep = prepare(m.unknowns, kept);
        res = solvePrepared(*prep, *sys, &ctx.cancel, options.denseLimit);
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

Result<bool> constraintsHold(const Sketch& sketch)
{
    detail::pinEigenCacheSizes();
    auto built = detail::buildModel(sketch);
    if (!built) {
        return tl::unexpected(built.error());
    }
    // The same objects solve() hands to PlaneGCS (redundant ones included), evaluated where they are, against the
    // test solve() applies to its own result: a solve from here starts with residuals it would accept as solved,
    // so it moves nothing beyond them (System::solve_DL stops before its first step below DL_tolf).
    std::vector<std::unique_ptr<GCS::Constraint>> owned;
    std::vector<GCS::Constraint*> objects;
    const Status emitted = detail::emitConstraints(**built, [&](GCS::Constraint* c) {
        owned.emplace_back(c);
        objects.push_back(c);
    });
    if (!emitted) {
        return tl::unexpected(emitted.error());
    }
    return detail::maxConstraintError(objects) <= detail::kHoldTolerance;
}

} // namespace ray::sketch
