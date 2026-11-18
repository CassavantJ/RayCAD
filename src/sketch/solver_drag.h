// The drag session of the PlaneGCS sketch solver (task T-090). Private to src/sketch; see DragMode in
// solver_internal.h for how a step is solved.
#pragma once

#include "solver_internal.h"
#include "solver_iterate.h"
#include "solver_translate.h"

#include <array>
#include <memory>
#include <set>
#include <vector>

namespace ray::sketch::detail {

class DragSession final : public ISketchSolver::IDragSession {
public:
    DragSession();
    ~DragSession() override;
    Result<SolveResult> move(Vec2 target) override;
    Sketch end() override;

    static Result<std::unique_ptr<DragSession>> begin(const Sketch&, SketchEntId point, JobContext&,
                                                      const DragOptions&);
    const DragStats& stats() const { return stats_; }

private:
    // The reduced drag system (spike S11): unknowns are only the parameters PlaneGCS reports as still free; rows are
    // every Equal constraint between two free parameters plus a greedily chosen independent set of the others.
    struct Reduced {
        std::vector<double*> free;
        std::unique_ptr<GCS::System> system; // owns the copies of the kept constraint objects
        std::vector<GCS::Constraint*> list;  // system's objects, kept rows then the two temporary ones
        std::unique_ptr<Prepared> prep;      // declared after `system`, so destroyed first
        int keptRows = 0;
        int dependentMergeRows = 0;
        bool usable = false;
        bool blocked = false; // the dragged point has no free parameter, or nothing is free: nothing can move
    };

    Status buildReduced(const std::set<int>& redundantTags, int fullDofs, JobContext& ctx);
    int solveFull();
    void restoreStart();

    std::unique_ptr<GcsModel> model_;
    std::unique_ptr<GCS::System> system_;   // the whole sketch plus the temporary drag constraints
    std::vector<GCS::Constraint*> objects_; // system_'s objects in the order they were added
    std::array<double, 2> target_{};        // the temporary constraints' target (stable address)
    int px_ = -1, py_ = -1;                 // the dragged point's parameters
    std::vector<double> start_;             // parameter values at drag begin
    SolveResult base_;                      // diagnosis from drag begin; solved = drag-start sketch
    Sketch last_;                           // the result of the last successful move
    DragMode mode_ = DragMode::FullSparse;
    std::unique_ptr<Prepared> full_; // the whole sketch for the sparse iteration
    Reduced reduced_;
    DragStats stats_;
};

} // namespace ray::sketch::detail
