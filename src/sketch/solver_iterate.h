/***************************************************************************
 *   RayCAD sketch solver iterations (task T-090).                         *
 *                                                                         *
 *   Adapted from FreeCAD's PlaneGCS, release 1.1.4                        *
 *   (src/Mod/Sketcher/App/planegcs/GCS.cpp: System::initSolution,         *
 *   System::solve(), System::solve_DL and                                 *
 *   System::solve(SubSystem*, SubSystem*, bool, bool); qp_eq.cpp),        *
 *   Copyright (c) 2011 Konstantinos Poulios (the full upstream notice is  *
 *   in third_party/planegcs/src/GCS.cpp), by way of RayCAD spike S11      *
 *   (spikes/s11-sketch-drag/src/sqp.cpp).                                 *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 ***************************************************************************/
// Private to src/sketch. PlaneGCS itself (third_party/planegcs) is not modified; everything here runs on its public
// SubSystem and Constraint API:
//  - prepare(): the partition GCS::System::initSolution builds (equality reduction, decoupled components, one
//    SubSystem per component for tag >= 0 constraints and one for the temporary drag constraints), over a constraint
//    list the caller chooses (System keeps its own private);
//  - dogLeg(): System::solve_DL line for line, plus a cancellation check between iterations. Same bits as PlaneGCS
//    (tested);
//  - sqpSparse(): the drag iteration System::solve(SubSystem*, SubSystem*) with the same QP, line search and BFGS
//    update, solved through a sparse Jacobian, K = A A^T by sparse Cholesky with a cached ordering, and the BFGS matrix
//    kept as identity plus rank-2 updates (spike S11). Agrees with PlaneGCS to 1e-9, not bit for bit.
#pragma once

// PlaneGCS (third_party/planegcs), unmodified.
#include "GCS.h"
#include "SubSystem.h"

#include <ray/core.h>

#include <memory>
#include <vector>

namespace ray::sketch::detail {

// Status codes as GCS::SolveStatus, plus this one.
constexpr int kSolveCancelled = -1;

struct SqpCache; // sparse pattern, product list and Cholesky ordering of one component, kept across calls

struct Prepared {
    GCS::VEC_pD plist;
    GCS::VEC_D reference;
    std::vector<GCS::VEC_pD> plists;
    std::vector<GCS::MAP_pD_pD> maps;
    std::vector<std::vector<GCS::Constraint*>> clist0, clist1;
    std::vector<std::unique_ptr<GCS::SubSystem>> sub, aux;
    std::vector<std::unique_ptr<SqpCache>> caches;
    Prepared();
    ~Prepared();
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    void setReference();
    void resetToReference();
};

// GCS::System::initSolution's partition of `clist` over the unknowns `plist` (which must be in canonical order).
// The reference configuration is the current parameter values. Constraint objects stay owned by the caller.
std::unique_ptr<Prepared> prepare(const GCS::VEC_pD& plist, const std::vector<GCS::Constraint*>& clist);

// GCS::System::solve() over a Prepared system, without the final check of redundant constraints (the caller owns
// that list): resets to the reference, then per component the sparse drag iteration when it has temporary
// constraints, else DogLeg. `settings` supplies maxIter, convergence and the DogLeg tolerances. Returns the worst
// GCS::SolveStatus, or kSolveCancelled once `cancel` (may be null) reads cancelled between iterations.
int solvePrepared(Prepared& p, const GCS::System& settings, const CancelToken* cancel);
// GCS::System::applySolution() over a Prepared system.
void applyPrepared(Prepared& p);

// System::solve_DL(subsys, false), with a cancellation check before every iteration.
int dogLeg(GCS::SubSystem* subsys, const GCS::System& settings, const CancelToken* cancel);

// The sparse form of System::solve(subsysA, subsysB): subsysA holds the sketch constraints, subsysB the temporary
// ones whose error is minimised. `cache` (may be null) keeps the sparse structure of this pair across calls.
int sqpSparse(GCS::SubSystem* subsysA, GCS::SubSystem* subsysB, int maxIter, double convergence, SqpCache* cache);

} // namespace ray::sketch::detail
