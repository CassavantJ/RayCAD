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
// See solver_diagnose.h.
#include "solver_diagnose.h"

// PlaneGCS (third_party/planegcs), unmodified.
#include "SubSystem.h"

#include <Eigen/Dense>
#include <Eigen/OrderingMethods>
#include <Eigen/SparseCore>
#include <Eigen/SparseQR>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace ray::sketch::detail {

namespace {

// A dependent row of the transposed Jacobian's QR is reported when its coefficient over a pivot exceeds this
// (upstream's literal).
constexpr double kGroupTolerance = 1e-10;

Error cancelledError()
{
    return Error{ErrorCode::Cancelled, "Sketch solve cancelled", {"sketch solver"}};
}

bool isCancelled(const CancelToken* cancel)
{
    return cancel != nullptr && cancel->cancelled();
}

// System::eliminateNonZerosOverPivotInUpperTriangularMatrix, unchanged.
void eliminateNonZerosOverPivot(Eigen::MatrixXd& R, int rank)
{
    for (int i = 1; i < rank; i++) {
        for (int row = 0; row < i; row++) {
            if (R(row, i) != 0) {
                const double coef = R(row, i) / R(i, i);
                R.block(row, i + 1, 1, R.cols() - i - 1) -= coef * R.block(i, i + 1, 1, R.cols() - i - 1);
                R(row, i) = 0;
            }
        }
    }
}

struct Component {
    std::vector<int> params; // indices into plist, ascending
    std::vector<int> rows;   // indices into the row list, ascending
    int rank = 0;
    std::unique_ptr<Eigen::FullPivHouseholderQR<Eigen::MatrixXd>> dense; // until its rank is known
    std::vector<std::vector<int>> groups; // per dependent row: the object indices of its group
};

// Union-find over parameters then rows; components numbered in the order of their lowest vertex, as
// boost::connected_components numbers them in System::initSolution.
std::vector<int> componentOf(int np, const std::vector<std::vector<int>>& rowParams)
{
    const int nv = np + static_cast<int>(rowParams.size());
    std::vector<int> parent(static_cast<std::size_t>(nv));
    for (int i = 0; i < nv; ++i) {
        parent[static_cast<std::size_t>(i)] = i;
    }
    auto find = [&](int a) {
        while (parent[static_cast<std::size_t>(a)] != a) {
            parent[static_cast<std::size_t>(a)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(a)])];
            a = parent[static_cast<std::size_t>(a)];
        }
        return a;
    };
    for (std::size_t r = 0; r < rowParams.size(); ++r) {
        for (int p : rowParams[r]) {
            const int a = find(np + static_cast<int>(r));
            const int b = find(p);
            if (a != b) {
                parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
            }
        }
    }
    std::vector<int> out(static_cast<std::size_t>(nv), -1);
    std::vector<int> id(static_cast<std::size_t>(nv), -1);
    int n = 0;
    for (int v = 0; v < nv; ++v) {
        const int r = find(v);
        if (id[static_cast<std::size_t>(r)] < 0) {
            id[static_cast<std::size_t>(r)] = n++;
        }
        out[static_cast<std::size_t>(v)] = id[static_cast<std::size_t>(r)];
    }
    return out;
}

// A sparse matrix with its entries sorted (SparseQR::matrixR() is not; sparse triangular solves need it).
Eigen::SparseMatrix<double> sortedR(const Eigen::SparseMatrix<double>& r)
{
    const Eigen::SparseMatrix<double, Eigen::RowMajor> rowMajor = r;
    return Eigen::SparseMatrix<double>(rowMajor);
}

// Rank and dependent groups of one component's transposed Jacobian, its columns already in band order (colOf[k]: the
// component column at position k), by SparseQR as System::makeSparseQRDecomposition does it: its default pivot
// threshold (upstream sets its own only after compute(), which does not refactor). Upstream's elimination over the
// pivots turns a dependent column R(:, j) into D R11^-1 R(:rank, j) (D the diagonal of R11): here one sparse triangular
// solve per dependent row, R kept sparse.
void sparseDecompose(Component& c, const Eigen::SparseMatrix<double>& JG, const std::vector<int>& colOf,
                     const std::vector<int>& rowObject)
{
    ++solverCounters().sparseDiagnoses;
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::NaturalOrdering<int>> qr;
    qr.compute(JG);
    c.rank = static_cast<int>(qr.rank());
    const int rc = static_cast<int>(JG.cols());
    if (rc > c.rank) {
        const Eigen::SparseMatrix<double> R = sortedR(qr.matrixR());
        const Eigen::SparseMatrix<double> R11 = R.topLeftCorner(c.rank, c.rank);
        const Eigen::VectorXd diag = Eigen::VectorXd(R11.diagonal());
        const auto& perm = qr.colsPermutation().indices();
        auto objectOf = [&](Eigen::Index column) {
            return rowObject[static_cast<std::size_t>(
                c.rows[static_cast<std::size_t>(colOf[static_cast<std::size_t>(column)])])];
        };
        for (int j = c.rank; j < rc; ++j) {
            Eigen::VectorXd z = R.col(j).toDense().head(c.rank);
            if (c.rank > 0) {
                R11.triangularView<Eigen::Upper>().solveInPlace(z);
            }
            std::vector<int> group;
            for (int row = 0; row < c.rank; ++row) {
                if (std::fabs(diag[row] * z[row]) > kGroupTolerance) {
                    group.push_back(objectOf(perm[row]));
                }
            }
            group.push_back(objectOf(perm[j]));
            c.groups.push_back(std::move(group));
        }
    }
}

} // namespace

Result<Diagnosis> diagnose(const GCS::VEC_pD& plist, const std::vector<GCS::Constraint*>& objects,
                           const GCS::System& settings, const CancelToken* cancel, int denseLimit)
{
    Diagnosis d;
    d.setAside.assign(objects.size(), 0);
    const int np = static_cast<int>(plist.size());
    if (np == 0) {
        d.dofs = 0;
        return d;
    }
    if (isCancelled(cancel)) {
        return tl::unexpected(cancelledError());
    }
    std::map<double*, int> pIndex; // looked up only
    for (int i = 0; i < np; ++i) {
        pIndex[plist[static_cast<std::size_t>(i)]] = i;
    }

    // System::makeReducedJacobian: rows are the driving constraints with tag >= 0, in list order; columns are every
    // unknown (no driven parameters are declared). Kept as the unknowns each row touches.
    std::vector<int> rowObject;
    std::map<int, int> tagMultiplicity;
    for (std::size_t k = 0; k < objects.size(); ++k) {
        GCS::Constraint* c = objects[k];
        c->revertParams();
        if (c->getTag() >= 0 && c->isDriving()) {
            rowObject.push_back(static_cast<int>(k));
            const auto [it, fresh] = tagMultiplicity.emplace(c->getTag(), 0);
            if (!fresh) {
                ++it->second;
            }
        }
    }
    d.dofs = np;
    if (rowObject.empty()) {
        return d;
    }
    const int nr = static_cast<int>(rowObject.size());
    std::vector<std::vector<int>> rowParams(static_cast<std::size_t>(nr));
    for (int r = 0; r < nr; ++r) {
        auto& ps = rowParams[static_cast<std::size_t>(r)];
        for (double* p : objects[static_cast<std::size_t>(rowObject[static_cast<std::size_t>(r)])]->params()) {
            const auto it = pIndex.find(p);
            if (it != pIndex.end()) {
                ps.push_back(it->second);
            }
        }
        std::sort(ps.begin(), ps.end());
        ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
    }

    const std::vector<int> comp = componentOf(np, rowParams);
    const int ncomp = 1 + *std::max_element(comp.begin(), comp.end());
    std::vector<Component> comps(static_cast<std::size_t>(ncomp));
    for (int p = 0; p < np; ++p) {
        comps[static_cast<std::size_t>(comp[static_cast<std::size_t>(p)])].params.push_back(p);
    }
    for (int r = 0; r < nr; ++r) {
        comps[static_cast<std::size_t>(comp[static_cast<std::size_t>(np + r)])].rows.push_back(r);
    }

    // The QR of each component's transposed Jacobian (parameters x rows).
    std::vector<int> local(static_cast<std::size_t>(np), -1);
    auto objectOfRow = [&](const Component& c, Eigen::Index column) {
        return rowObject[static_cast<std::size_t>(c.rows[static_cast<std::size_t>(column)])];
    };
    double maxPivot = 0;
    for (Component& c : comps) {
        if (c.rows.empty()) {
            continue;
        }
        if (isCancelled(cancel)) {
            return tl::unexpected(cancelledError());
        }
        ++d.components;
        const int pc = static_cast<int>(c.params.size());
        const int rc = static_cast<int>(c.rows.size());
        d.largestComponent = std::max(d.largestComponent, pc);
        for (int i = 0; i < pc; ++i) {
            local[static_cast<std::size_t>(c.params[static_cast<std::size_t>(i)])] = i;
        }
        if (pc == 0) {
            // Rows that touch no unknown: each one is dependent on its own (upstream's QR of a zero column).
            c.rank = 0;
            for (int j = 0; j < rc; ++j) {
                c.groups.push_back({objectOfRow(c, j)});
            }
            continue;
        }
        if (pc < denseLimit && rc < denseLimit) {
            ++solverCounters().denseDiagnoses;
            Eigen::MatrixXd JG = Eigen::MatrixXd::Zero(pc, rc);
            for (int j = 0; j < rc; ++j) {
                const int row = c.rows[static_cast<std::size_t>(j)];
                GCS::Constraint* constr = objects[static_cast<std::size_t>(rowObject[static_cast<std::size_t>(row)])];
                for (int p : rowParams[static_cast<std::size_t>(row)]) {
                    JG(local[static_cast<std::size_t>(p)], j) = constr->grad(plist[static_cast<std::size_t>(p)]);
                }
            }
            c.dense = std::make_unique<Eigen::FullPivHouseholderQR<Eigen::MatrixXd>>(JG);
            maxPivot = std::max(maxPivot, std::abs(c.dense->maxPivot()));
            continue; // rank and groups once the largest pivot of all dense components is known
        }

        // System::makeSparseQRDecomposition (sparseDecompose), over the band ordering (bandOrder) of the component's
        // rows and unknowns: Eigen's COLAMD left a chain of 900 unknowns with 2.5 times the fill of R and 8 times the
        // time (35 ms against 4.4 ms for one QR).
        std::vector<std::vector<int>> colRows(static_cast<std::size_t>(rc));
        for (int j = 0; j < rc; ++j) {
            for (int p : rowParams[static_cast<std::size_t>(c.rows[static_cast<std::size_t>(j)])]) {
                colRows[static_cast<std::size_t>(j)].push_back(local[static_cast<std::size_t>(p)]);
            }
        }
        std::vector<int> colOf, rowPos;
        bandOrder(colRows, pc, colOf, rowPos);
        std::vector<Eigen::Triplet<double>> trip;
        for (int k = 0; k < rc; ++k) {
            const int j = colOf[static_cast<std::size_t>(k)];
            const int row = c.rows[static_cast<std::size_t>(j)];
            GCS::Constraint* constr = objects[static_cast<std::size_t>(rowObject[static_cast<std::size_t>(row)])];
            for (int p : rowParams[static_cast<std::size_t>(row)]) {
                trip.emplace_back(rowPos[static_cast<std::size_t>(local[static_cast<std::size_t>(p)])], k,
                                  constr->grad(plist[static_cast<std::size_t>(p)]));
            }
        }
        Eigen::SparseMatrix<double> JG(pc, rc);
        JG.setFromTriplets(trip.begin(), trip.end());
        JG.makeCompressed();
        sparseDecompose(c, JG, colOf, rowObject);
    }
    // Dense components: rank against the largest pivot of all of them (upstream's one matrix), then
    // System::identifyConflictingRedundantConstraints's dependent groups.
    for (Component& c : comps) {
        if (!c.dense) {
            continue;
        }
        const auto& qr = *c.dense;
        const double threshold = maxPivot * settings.qrpivotThreshold;
        c.rank = 0;
        for (Eigen::Index i = 0; i < qr.nonzeroPivots(); ++i) {
            c.rank += std::abs(qr.matrixQR().coeff(i, i)) > threshold ? 1 : 0;
        }
        const Eigen::Index rows = qr.rows();
        const Eigen::Index cols = qr.cols();
        if (cols > c.rank) {
            Eigen::MatrixXd R;
            if (cols >= rows) {
                R = qr.matrixQR().triangularView<Eigen::Upper>();
            } else {
                R = qr.matrixQR().topRows(cols).triangularView<Eigen::Upper>();
            }
            eliminateNonZerosOverPivot(R, c.rank);
            const auto& perm = qr.colsPermutation().indices();
            for (Eigen::Index j = c.rank; j < cols; ++j) {
                std::vector<int> group;
                for (int row = 0; row < c.rank; ++row) {
                    if (std::fabs(R(row, j)) > kGroupTolerance) {
                        group.push_back(objectOfRow(c, perm[row]));
                    }
                }
                group.push_back(objectOfRow(c, perm[j]));
                c.groups.push_back(std::move(group));
            }
        }
        c.dense.reset();
    }

    // The popularity contest of System::identifyConflictingRedundantConstraints over all groups: repeatedly skip the
    // constraint (with every other row of its tag) that appears in the most unsatisfied groups; ties go to the tag
    // with fewer rows, then to the higher tag. Tag 0 and internal alignment constraints are never skipped.
    std::vector<std::pair<int, int>> allGroups; // (component, index in its groups)
    for (int ci = 0; ci < ncomp; ++ci) {
        for (std::size_t g = 0; g < comps[static_cast<std::size_t>(ci)].groups.size(); ++g) {
            allGroups.emplace_back(ci, static_cast<int>(g));
        }
    }
    auto groupAt = [&](std::size_t i) -> const std::vector<int>& {
        return comps[static_cast<std::size_t>(allGroups[i].first)]
            .groups[static_cast<std::size_t>(allGroups[i].second)];
    };
    std::set<int> skipped; // object indices
    std::set<std::size_t> satisfied;
    while (true) {
        std::map<int, std::set<std::size_t>> conflictingMap; // object index -> unsatisfied groups it is in
        for (std::size_t i = 0; i < allGroups.size(); ++i) {
            if (satisfied.count(i) != 0) {
                continue;
            }
            for (int obj : groupAt(i)) {
                GCS::Constraint* constr = objects[static_cast<std::size_t>(obj)];
                const bool internal = constr->isInternalAlignment() == GCS::Constraint::Alignment::InternalAlignment;
                if (constr->getTag() != 0 && !internal) {
                    conflictingMap[obj].insert(i);
                }
            }
        }
        if (conflictingMap.empty()) {
            break;
        }
        const auto best =
            std::max_element(conflictingMap.begin(), conflictingMap.end(), [&](const auto& a, const auto& b) {
                const std::size_t na = a.second.size(), nb = b.second.size();
                const int ta = objects[static_cast<std::size_t>(a.first)]->getTag();
                const int tb = objects[static_cast<std::size_t>(b.first)]->getTag();
                const int ma = tagMultiplicity.at(ta), mb = tagMultiplicity.at(tb);
                return nb > na || (nb == na && mb < ma) || (nb == na && mb == ma && tb > ta);
            });
        const int bestTag = objects[static_cast<std::size_t>(best->first)]->getTag();
        for (const auto& [obj, groups] : conflictingMap) {
            if (objects[static_cast<std::size_t>(obj)]->getTag() == bestTag) {
                skipped.insert(obj);
                satisfied.insert(groups.begin(), groups.end());
            }
        }
    }

    // The redundancy solve, per component with skipped rows: solve without them; those that then hold are redundant.
    std::vector<double> reference(static_cast<std::size_t>(np));
    for (int i = 0; i < np; ++i) {
        reference[static_cast<std::size_t>(i)] = *plist[static_cast<std::size_t>(i)];
    }
    auto resetToReference = [&] {
        for (int i = 0; i < np; ++i) {
            *plist[static_cast<std::size_t>(i)] = reference[static_cast<std::size_t>(i)];
        }
    };
    std::set<int> redundant; // object indices
    DogLegOptions redundantSolving;
    redundantSolving.redundantSolving = true;
    redundantSolving.denseLimit = denseLimit;
    for (const Component& c : comps) {
        std::vector<int> picked;
        std::vector<GCS::Constraint*> clistTmp;
        for (int row : c.rows) {
            const int obj = rowObject[static_cast<std::size_t>(row)];
            if (skipped.count(obj) != 0) {
                picked.push_back(obj);
            } else {
                clistTmp.push_back(objects[static_cast<std::size_t>(obj)]);
            }
        }
        if (picked.empty()) {
            continue;
        }
        GCS::VEC_pD params;
        for (int p : c.params) {
            params.push_back(plist[static_cast<std::size_t>(p)]);
        }
        GCS::SubSystem sub(clistTmp, params);
        ++solverCounters().redundancySolves;
        const int res = dogLeg(&sub, settings, cancel, redundantSolving);
        if (res == kSolveCancelled) {
            resetToReference();
            return tl::unexpected(cancelledError());
        }
        if (res == GCS::Success) {
            sub.applySolution();
            for (int obj : picked) {
                const double err = objects[static_cast<std::size_t>(obj)]->error();
                if (err * err < settings.convergenceRedundant) {
                    redundant.insert(obj);
                }
            }
        }
        resetToReference();
    }

    // Groups holding a redundant constraint are explained; the rest conflict.
    int rankTotal = 0;
    int constrTotal = 0;
    int explained = 0;
    std::set<int> conflictingTags;
    for (std::size_t i = 0; i < allGroups.size(); ++i) {
        const std::vector<int>& group = groupAt(i);
        const bool hasRedundant =
            std::any_of(group.begin(), group.end(), [&](int obj) { return redundant.count(obj) != 0; });
        if (hasRedundant) {
            ++explained;
            continue;
        }
        for (int obj : group) {
            GCS::Constraint* constr = objects[static_cast<std::size_t>(obj)];
            const bool internal = constr->isInternalAlignment() == GCS::Constraint::Alignment::InternalAlignment;
            conflictingTags.insert(internal ? 0 : constr->getTag());
        }
    }
    conflictingTags.erase(0);
    for (const Component& c : comps) {
        rankTotal += c.rank;
        constrTotal += static_cast<int>(c.rows.size());
    }
    // System::diagnose: DOF = unknowns - rank, unless the rank is full and more rows than that are not redundant.
    d.dofs = np - rankTotal;
    if (constrTotal > rankTotal) {
        const int nonRedundant = constrTotal - explained;
        if (np == rankTotal && nonRedundant > rankTotal) {
            d.dofs = np - nonRedundant;
        }
    }

    std::set<int> redundantTags, partialTags;
    for (int obj : redundant) {
        d.setAside[static_cast<std::size_t>(obj)] = 1;
        redundantTags.insert(objects[static_cast<std::size_t>(obj)]->getTag());
        partialTags.insert(objects[static_cast<std::size_t>(obj)]->getTag());
    }
    for (std::size_t k = 0; k < objects.size(); ++k) {
        if (redundant.count(static_cast<int>(k)) == 0) {
            redundantTags.erase(objects[k]->getTag());
        }
    }
    for (int t : redundantTags) {
        partialTags.erase(t);
    }
    d.conflictingTags.assign(conflictingTags.begin(), conflictingTags.end());
    d.redundantTags.assign(redundantTags.begin(), redundantTags.end());
    d.partiallyRedundantTags.assign(partialTags.begin(), partialTags.end());
    return d;
}

} // namespace ray::sketch::detail
