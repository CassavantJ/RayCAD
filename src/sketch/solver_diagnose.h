/***************************************************************************
 *   RayCAD sketch solver diagnosis (pre-M2 performance work).             *
 *                                                                         *
 *   Adapted from FreeCAD's PlaneGCS, release 1.1.4                        *
 *   (src/Mod/Sketcher/App/planegcs/GCS.cpp: System::diagnose,             *
 *   System::makeReducedJacobian, System::makeDenseQRDecomposition,        *
 *   System::makeSparseQRDecomposition,                                    *
 *   System::eliminateNonZerosOverPivotInUpperTriangularMatrix and         *
 *   System::identifyConflictingRedundantConstraints),                     *
 *   Copyright (c) 2011 Konstantinos Poulios (the full upstream notice is  *
 *   in third_party/planegcs/src/GCS.cpp).                                 *
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
// Private to src/sketch. GCS::System::diagnose (what initSolution runs first) on PlaneGCS's public Constraint and
// SubSystem API, per decoupled component:
//  - The reduced Jacobian is block diagonal over the components of the constraint graph, so its rank, its dependent
//    rows and the solve that tells redundant rows from conflicting ones are worked out per component. Upstream builds
//    one dense matrix over the whole sketch (a gradient call per row and unknown) and runs two dense QRs of it (one
//    only to list dependent parameters, which the full solve does not use); both cost the cube of the sketch's size.
//  - A component below kDenseLimit unknowns is decomposed exactly as upstream does it (FullPivHouseholderQR of the
//    transposed Jacobian, rank threshold relative to the largest pivot of the whole matrix, the same elimination over
//    the pivots), so a sketch whose components are all small gets upstream's diagnosis. A larger one uses upstream's
//    SparseQR branch (which upstream takes from 1000 unknowns) with R kept sparse: the dependent rows come from a
//    sparse triangular solve instead of a dense elimination.
//  - The popularity heuristic that picks which constraint of a dependent group to call redundant is upstream's,
//    keyed by object index instead of pointer value.
//  - The redundancy solve (DogLeg with the picked constraints left out, then the picked ones that hold are
//    redundant) runs per component that has picks, through detail::dogLeg (sparse above kDenseLimit), and checks
//    cancellation. Upstream solves the whole sketch at once; if that solve fails no pick is redundant anywhere, here
//    only in the component whose solve failed.
#pragma once

// PlaneGCS (third_party/planegcs), unmodified.
#include "GCS.h"

#include "solver_iterate.h"

#include <ray/core.h>

#include <vector>

namespace ray::sketch::detail {

struct Diagnosis {
    int dofs = 0;               // GCS::System::dofsNumber()
    std::vector<char> setAside; // per object: diagnosed redundant (System's `redundant` set); left out of the solve
    std::vector<int> conflictingTags, redundantTags, partiallyRedundantTags; // ascending, as System reports them
    int components = 0;       // decoupled components with at least one constraint row
    int largestComponent = 0; // unknowns of the largest one
};

// Diagnoses `objects` (every constraint object of the sketch, in the order GCS::System would hold them; rows are those
// with tag >= 0 that are driving) over the unknowns `plist` at their current values, and leaves the values as they
// were. `settings` supplies the QR pivot threshold and the redundancy solve's DogLeg settings. Cancelled when `cancel`
// (may be null) reads cancelled between components or between DogLeg iterations.
Result<Diagnosis> diagnose(const GCS::VEC_pD& plist, const std::vector<GCS::Constraint*>& objects,
                           const GCS::System& settings, const CancelToken* cancel, int denseLimit = kDenseLimit);

} // namespace ray::sketch::detail
