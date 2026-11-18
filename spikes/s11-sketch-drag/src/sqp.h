// Spike S11: drag solve outside GCS::System, built only on PlaneGCS's public SubSystem / Constraint API.
// Throwaway code. See ../REPORT.md.
//
// sqp.cpp contains code adapted from FreeCAD's PlaneGCS (GCS.cpp: System::initSolution and
// System::solve(SubSystem*, SubSystem*, ...); qp_eq.cpp), FreeCAD 1.1.4,
// Copyright (c) 2011 Konstantinos Poulios, LGPL-2.1-or-later. The PlaneGCS sources themselves are not modified.
#pragma once

#include "GCS.h"
#include "SubSystem.h"

#include <memory>
#include <vector>

namespace s11
{

// Wall time per part of one drag solve, summed over calls (ms).
struct SqpProfile
{
    double gradB = 0, jacA = 0, resA = 0;            // derivative and residual evaluation
    double qr = 0, formQ = 0, formY = 0, nullsp = 0;  // dense qp_eq parts
    double formK = 0, analyse = 0, factor = 0, kktSolve = 0;  // sparse path: K = A A^T, ordering, Cholesky, solves
    double lambda = 0, lineSearch = 0, bfgs = 0, setup = 0, total = 0;
    long long iters = 0, calls = 0;
    int xsize = 0, csize = 0;
    void add(const SqpProfile& o);
};

enum class LinAlg
{
    Dense,   // PlaneGCS's own algorithm, line for line (FullPivHouseholderQR of A^T, dense BFGS matrix)
    Sparse   // same iteration, sparse A, K = A A^T by sparse Cholesky, BFGS matrix kept as I + low rank
};

// Same iteration as GCS::System::solve(SubSystem* A, SubSystem* B, isFine, isRedundantsolving=false).
// cache: sparse pattern, product list and Cholesky ordering for this (A, B) pair, kept across calls; may be null.
struct SqpCache;
int sqpSolve(GCS::SubSystem* A, GCS::SubSystem* B, int maxIter, double convergence, LinAlg la, SqpProfile* prof,
             SqpCache* cache = nullptr);

// What GCS::System::initSolution builds: equality-constraint reduction, decoupled components, and per
// component a SubSystem for tag >= 0 constraints (A) and one for tag < 0 constraints (B).
// Assumes no redundant constraints (the caller checks).
struct Prepared
{
    GCS::VEC_pD plist;
    GCS::VEC_D reference;
    std::vector<std::vector<GCS::Constraint*>> clist0, clist1;
    std::vector<GCS::VEC_pD> plists;
    std::vector<GCS::MAP_pD_pD> maps;
    std::vector<GCS::SubSystem*> sub, aux;
    std::vector<std::shared_ptr<SqpCache>> caches;
    ~Prepared();
    void setReference();
    void resetToReference();
};
std::unique_ptr<Prepared> prepare(const GCS::VEC_pD& plist, const std::vector<GCS::Constraint*>& clist);

// GCS::System::solve() over a Prepared system; components without a drag constraint go to dl.solve (DogLeg).
int solvePrepared(Prepared& p, GCS::System& dl, LinAlg la, SqpProfile* prof);
// GCS::System::applySolution() over a Prepared system.
void applyPrepared(Prepared& p);

}  // namespace s11
