// The drag session of the PlaneGCS sketch solver (task T-090). See solver_drag.h and DragMode in solver_internal.h.
//
// The reduced drag system and its acceptance test follow spike S11 (spikes/s11-sketch-drag/src/bench.cpp,
// makeReduced and runCase), which measured 0 of 43,200 steps differing from PlaneGCS by more than 1e-9 mm.
#include "solver_drag.h"

#include "solver_system.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace ray::sketch::detail {

namespace {

constexpr double kRowTolerance = 1e-6; // relative tolerance of the greedy independent-row selection (S11)

bool okStatus(int r)
{
    return r == GCS::Success || r == GCS::Converged;
}

Error cancelledError()
{
    return Error{ErrorCode::Cancelled, "Sketch drag cancelled", {"sketch solver"}};
}

} // namespace

const char* dragModeName(DragMode m)
{
    switch (m) {
    case DragMode::Reduced:
        return "reduced";
    case DragMode::FullSparse:
        return "full sparse";
    case DragMode::PlaneGcs:
        return "PlaneGCS";
    }
    return "?";
}

DragSession::DragSession() = default;
DragSession::~DragSession() = default;

Result<std::unique_ptr<DragSession>> DragSession::begin(const Sketch& sketch, SketchEntId point, JobContext& ctx,
                                                        const DragOptions& options)
{
    pinEigenCacheSizes();
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }
    auto s = std::unique_ptr<DragSession>(new DragSession());
    auto built = buildModel(sketch);
    if (!built) {
        return tl::unexpected(built.error());
    }
    s->model_ = std::move(*built);
    GcsModel& m = *s->model_;
    const int pi = m.pointIndex(point);
    if (pi < 0 || m.geo[static_cast<std::size_t>(pi)].kind != EntityKind::Point) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument,
                                    "Only a sketch point can be dragged; " + idText(point.value) + " is not one",
                                    {"sketch solver"}});
    }
    s->px_ = m.geo[static_cast<std::size_t>(pi)].firstParam;
    s->py_ = s->px_ + 1;

    s->system_ = newSystem();
    GCS::System& sys = *s->system_;
    if (auto st = emitConstraints(m,
                                  [&](GCS::Constraint* c) {
                                      sys.addConstraint(c);
                                      s->objects_.push_back(c);
                                  });
        !st) {
        return tl::unexpected(st.error());
    }
    sys.declareUnknowns(m.unknowns);
    ctx.progress.set(0.05, "Analysing constraints");
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }
    // Diagnosis through initSolution, as in solveSketch (it restores the parameters after diagnose's own solve).
    // It cannot be interrupted.
    sys.initSolution();
    const int dofs = sys.dofsNumber();
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }
    ctx.progress.set(0.6, "Preparing drag");

    GCS::VEC_I conflicting, redundant, partial;
    sys.getConflicting(conflicting);
    sys.getRedundant(redundant);
    sys.getPartiallyRedundant(partial);
    SolveResult& base = s->base_;
    base.freeDof = std::max(dofs, 0);
    base.conflicting = tagIds(m, conflicting);
    base.redundant = tagIds(m, redundant);
    base.partiallyRedundant = tagIds(m, partial);
    if (!base.conflicting.empty()) {
        return tl::unexpected(Error{ErrorCode::Overconstrained,
                                    "The sketch has conflicting constraints; fix them before dragging",
                                    {"sketch solver"}});
    }
    base.status = statusFromDiagnosis(dofs, !base.redundant.empty() || !base.partiallyRedundant.empty());
    base.solved = m.sketch;
    s->last_ = m.sketch;

    // FreeCAD's drag (Sketch::initMove): a temporary coincident constraint, tag -1, between the point and a target
    // that starts where the point is. Temporary constraints do not invalidate the diagnosis.
    s->target_[0] = m.params[static_cast<std::size_t>(s->px_)];
    s->target_[1] = m.params[static_cast<std::size_t>(s->py_)];
    double* px = &m.params[static_cast<std::size_t>(s->px_)];
    double* py = &m.params[static_cast<std::size_t>(s->py_)];
    for (auto [t, p] : {std::pair{&s->target_[0], px}, std::pair{&s->target_[1], py}}) {
        auto* c = new GCS::ConstraintEqual(t, p);
        c->setTag(GCS::DefaultTemporaryConstraint);
        c->setDriving(true);
        sys.addConstraint(c);
        s->objects_.push_back(c);
    }
    sys.initSolution(); // reference = the drag-start state
    s->start_ = m.params;

    const std::set<int> redundantTags(redundant.begin(), redundant.end());
    if (options.force) {
        s->mode_ = *options.force;
    } else {
        s->mode_ = partial.empty() ? DragMode::Reduced : DragMode::PlaneGcs;
    }
    if (s->mode_ != DragMode::PlaneGcs) {
        // Every object except those of fully redundant tags (with no partially redundant tag, exactly what
        // initSolution keeps), temporary ones included.
        std::vector<GCS::Constraint*> kept;
        for (GCS::Constraint* c : s->objects_) {
            if (!redundantTags.count(c->getTag())) {
                kept.push_back(c);
            }
        }
        s->full_ = prepare(m.unknowns, kept);
    }
    if (s->mode_ == DragMode::Reduced) {
        if (auto st = s->buildReduced(redundantTags, dofs, ctx); !st) {
            return tl::unexpected(st.error());
        }
        if (!s->reduced_.usable && !s->reduced_.blocked && !options.force) {
            s->mode_ = DragMode::FullSparse;
        }
    }
    s->restoreStart();

    DragStats& st = s->stats_;
    st.mode = s->mode_;
    st.reducedUsable = s->reduced_.usable;
    st.pointFree = !s->reduced_.blocked;
    st.unknowns = static_cast<int>(m.params.size());
    st.freeParams = static_cast<int>(s->reduced_.free.size());
    st.keptRows = s->reduced_.keptRows;
    ctx.progress.set(1.0);
    return s;
}

Status DragSession::buildReduced(const std::set<int>& redundantTags, int fullDofs, JobContext& ctx)
{
    GcsModel& m = *model_;
    Reduced& R = reduced_;
    GCS::VEC_pD raw;
    system_->getDependentParams(raw);
    const std::set<double*> freeSet(raw.begin(), raw.end());
    for (double* p : m.unknowns) {
        if (freeSet.count(p)) {
            R.free.push_back(p);
        }
    }
    double* px = &m.params[static_cast<std::size_t>(px_)];
    double* py = &m.params[static_cast<std::size_t>(py_)];
    const bool pointFree = freeSet.count(px) || freeSet.count(py);
    R.blocked = !pointFree || R.free.empty();
    if (R.blocked) {
        return {};
    }

    // Greedy row selection in canonical constraint order: keep a row when its gradient over the free parameters is
    // not (numerically) a combination of the rows already kept. PlaneGCS eliminates an Equal constraint between two
    // unknowns by merging them, which changes the metric of the QP step, so the reduced system must merge exactly the
    // same free parameters: those Equal rows are always kept (pass 0); the others are then chosen greedily (pass 1).
    std::map<double*, int> col;
    for (int j = 0; j < static_cast<int>(R.free.size()); ++j) {
        col[R.free[static_cast<std::size_t>(j)]] = j;
    }
    const int nf = static_cast<int>(R.free.size());
    Eigen::MatrixXd basis(nf, std::max(nf, 1));
    int rank = 0;
    std::vector<char> keep(objects_.size(), 0);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t k = 0; k < objects_.size(); ++k) {
            GCS::Constraint* c = objects_[k];
            if (c->getTag() < 0 || redundantTags.count(c->getTag())) {
                continue;
            }
            GCS::VEC_pD ps = c->params();
            const bool mergeRow = c->getTypeId() == GCS::Equal && col.count(ps[0]) && col.count(ps[1]);
            if ((pass == 0) != mergeRow) {
                continue;
            }
            std::sort(ps.begin(), ps.end());
            ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
            Eigen::VectorXd v = Eigen::VectorXd::Zero(nf);
            bool touches = false;
            for (double* p : ps) {
                const auto it = col.find(p);
                if (it != col.end()) {
                    v[it->second] = c->grad(p);
                    touches = true;
                }
            }
            if (!touches) {
                continue;
            }
            const double nv = v.norm();
            Eigen::VectorXd w = v;
            for (int again = 0; again < 2 && rank > 0; ++again) {
                w -= basis.leftCols(rank) * (basis.leftCols(rank).transpose() * w);
            }
            const double nw = w.norm();
            const bool independent = nv > 0 && rank < nf && nw > kRowTolerance * nv;
            if (independent) {
                basis.col(rank++) = w / nw;
            }
            if (independent || mergeRow) {
                keep[k] = 1;
                if (!independent) {
                    ++R.dependentMergeRows;
                }
            }
        }
        if (ctx.cancel.cancelled()) {
            return tl::unexpected(cancelledError());
        }
    }

    // A second set of the kept constraint objects, created at the drag-start state like the first, for a System over
    // the free parameters only.
    R.system = newSystem();
    std::size_t index = 0;
    if (auto st = emitConstraints(m,
                                  [&](GCS::Constraint* c) {
                                      if (index < keep.size() && keep[index]) {
                                          R.system->addConstraint(c);
                                          R.list.push_back(c);
                                      } else {
                                          delete c;
                                      }
                                      ++index;
                                  });
        !st) {
        return st;
    }
    R.keptRows = static_cast<int>(R.list.size());
    R.system->declareUnknowns(R.free);
    const int rsDofs = R.system->diagnose();
    restoreStart(); // diagnose() on its own leaves the result of its solve in place when rows are redundant
    // The kept rows must be exactly independent: as many as free parameters minus the sketch's DOF, and the reduced
    // system's own diagnosis must agree (same DOF, nothing redundant or conflicting).
    R.usable = R.keptRows - R.dependentMergeRows == nf - fullDofs && R.dependentMergeRows == 0 && rsDofs == fullDofs &&
               !R.system->hasRedundant() && !R.system->hasConflicting();
    for (auto [t, p] : {std::pair{&target_[0], px}, std::pair{&target_[1], py}}) {
        auto* c = new GCS::ConstraintEqual(t, p);
        c->setTag(GCS::DefaultTemporaryConstraint);
        c->setDriving(true);
        R.system->addConstraint(c);
        R.list.push_back(c);
    }
    R.system->initSolution();
    R.prep = prepare(R.free, R.list);
    if (ctx.cancel.cancelled()) {
        return tl::unexpected(cancelledError());
    }
    return {};
}

void DragSession::restoreStart()
{
    std::copy(start_.begin(), start_.end(), model_->params.begin());
}

int DragSession::solveFull()
{
    restoreStart();
    int r = GCS::Failed;
    if (mode_ == DragMode::PlaneGcs) {
        r = system_->solve(); // resets to the reference (drag start) itself
        if (okStatus(r)) {
            system_->applySolution();
        }
    } else {
        r = solvePrepared(*full_, *system_, nullptr);
        if (okStatus(r)) {
            applyPrepared(*full_);
        }
    }
    return r;
}

Result<SolveResult> DragSession::move(Vec2 target)
{
    if (!std::isfinite(target.x) || !std::isfinite(target.y)) {
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Drag target is not finite", {"sketch solver"}});
    }
    ++stats_.moves;
    target_[0] = target.x;
    target_[1] = target.y;
    int r = GCS::Failed;
    if (mode_ == DragMode::Reduced) {
        restoreStart();
        Reduced& R = reduced_;
        if (!R.blocked) {
            r = solvePrepared(*R.prep, *R.system, nullptr);
            if (okStatus(r)) {
                applyPrepared(*R.prep);
            }
        } else {
            r = GCS::Success; // nothing can move; accepted below only if the start state holds
        }
        // Accept the reduced result only if every constraint of the whole sketch holds and the point moved whenever
        // it can move. Otherwise solve the whole sketch.
        bool accept = (R.usable || R.blocked) && okStatus(r) && maxConstraintError(objects_) <= kHoldTolerance;
        if (accept && !R.blocked) {
            const double sx = start_[static_cast<std::size_t>(px_)];
            const double sy = start_[static_cast<std::size_t>(py_)];
            const bool wantMove = dmath::hypot(target.x - sx, target.y - sy) > 1e-12;
            const bool didMove = model_->params[static_cast<std::size_t>(px_)] != sx ||
                                 model_->params[static_cast<std::size_t>(py_)] != sy;
            accept = !wantMove || didMove;
        }
        if (accept) {
            ++stats_.reducedSteps;
        } else {
            ++stats_.fallbacks;
            ++stats_.fullSteps;
            const DragMode saved = mode_;
            mode_ = DragMode::FullSparse;
            r = solveFull();
            mode_ = saved;
        }
    } else {
        ++stats_.fullSteps;
        r = solveFull();
    }

    SolveResult out;
    out.freeDof = base_.freeDof;
    out.conflicting = base_.conflicting;
    out.redundant = base_.redundant;
    out.partiallyRedundant = base_.partiallyRedundant;
    if (okStatus(r) && maxConstraintError(objects_) <= kHoldTolerance) {
        out.status = base_.status;
        out.solved = writeBack(*model_);
        last_ = out.solved;
    } else {
        // The step failed: report it with the drag-start geometry; the session goes on with the next target.
        ++stats_.failedMoves;
        out.status = SolveStatus::Failed;
        out.solved = base_.solved;
    }
    return out;
}

Sketch DragSession::end()
{
    restoreStart();
    return last_;
}

Result<std::unique_ptr<ISketchSolver::IDragSession>> beginDragSession(const Sketch& sketch, SketchEntId point,
                                                                      JobContext& ctx, const DragOptions& options)
{
    auto s = DragSession::begin(sketch, point, ctx, options);
    if (!s) {
        return tl::unexpected(s.error());
    }
    return std::unique_ptr<ISketchSolver::IDragSession>(std::move(*s));
}

std::optional<DragStats> dragStats(const ISketchSolver::IDragSession& session)
{
    if (const auto* s = dynamic_cast<const DragSession*>(&session)) {
        return s->stats();
    }
    return std::nullopt;
}

} // namespace ray::sketch::detail
