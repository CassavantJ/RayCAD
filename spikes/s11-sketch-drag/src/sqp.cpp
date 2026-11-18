/***************************************************************************
 *   Spike S11 (RayCAD). Throwaway code; see ../REPORT.md.                 *
 *                                                                         *
 *   Parts of this file are adapted from FreeCAD's PlaneGCS, release 1.1.4 *
 *   (src/Mod/Sketcher/App/planegcs/GCS.cpp: System::initSolution and      *
 *   System::solve(SubSystem*, SubSystem*, bool, bool); qp_eq.cpp).        *
 *   Copyright (c) 2011 Konstantinos Poulios <logari81@gmail.com>          *
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
// The Dense path is PlaneGCS's drag iteration line for line, with timers added, so the profile is of the
// real algorithm. The Sparse path runs the same iteration (same QP, same line search, same BFGS update)
// but solves the equality-constrained QP through K = A A^T (sparse Cholesky) and keeps the BFGS matrix as
// identity plus its rank-2 updates. It uses only PlaneGCS's public SubSystem / Constraint API.
#include "sqp.h"

#include <Eigen/Dense>
#include <Eigen/QR>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace s11
{
using namespace Eigen;
using Clock = std::chrono::steady_clock;

namespace
{
double msSince(Clock::time_point& t)
{
    const auto n = Clock::now();
    const double d = std::chrono::duration<double, std::milli>(n - t).count();
    t = n;
    return d;
}

// qp_eq.cpp, with timers. Minimises 0.5 x'Hx + g'x subject to A x + c = 0.
int qpEqDense(MatrixXd& H, VectorXd& g, MatrixXd& A, VectorXd& c, VectorXd& x, MatrixXd& Y, MatrixXd& Z, SqpProfile* pf)
{
    auto t = Clock::now();
    FullPivHouseholderQR<MatrixXd> qrAT(A.transpose());
    if (pf) pf->qr += msSince(t);
    MatrixXd Q = qrAT.matrixQ();
    if (pf) pf->formQ += msSince(t);

    size_t params_num = qrAT.rows();
    size_t constr_num = qrAT.cols();
    size_t rank = qrAT.rank();

    if (rank != constr_num || constr_num > params_num) {
        return -1;
    }
    Y = qrAT.matrixQR()
            .topRows(constr_num)
            .triangularView<Upper>()
            .transpose()
            .solve<OnTheRight>(Q.leftCols(rank))
        * qrAT.colsPermutation().transpose();
    if (pf) pf->formY += msSince(t);
    if (params_num == rank) {
        x = -Y * c;
    }
    else {
        Z = Q.rightCols(params_num - rank);
        MatrixXd ZTHZ = Z.transpose() * H * Z;
        VectorXd rhs = Z.transpose() * (H * Y * c - g);
        VectorXd y = ZTHZ.colPivHouseholderQr().solve(rhs);
        x = -Y * c + Z * y;
    }
    if (pf) pf->nullsp += msSince(t);
    return 0;
}

// BFGS matrix B = I + sum_k coef_k w_k w_k'
struct LowRankB
{
    int n;
    std::vector<VectorXd> w;
    std::vector<double> coef;
    VectorXd mul(const VectorXd& v) const
    {
        VectorXd r = v;
        for (size_t k = 0; k < w.size(); ++k) {
            r += (coef[k] * w[k].dot(v)) * w[k];
        }
        return r;
    }
};

// Sparse Jacobian of A over plistAB with a fixed pattern.
struct SparseJac
{
    SparseMatrix<double, RowMajor> J;
    std::vector<GCS::Constraint*> cl;
    std::vector<std::pair<double*, double*>> entries;  // (param pointer as the constraint sees it, value slot)
    std::vector<int> rowOf;
    void build(GCS::SubSystem* A, const GCS::VEC_pD& plistAB, int xsize)
    {
        A->getConstraintList(cl);
        GCS::MAP_pD_pD pmap;
        A->getParamMap(pmap);
        std::map<double*, int> col;  // pvals pointer -> column
        for (int j = 0; j < xsize; ++j) {
            auto it = pmap.find(plistAB[j]);
            if (it != pmap.end()) {
                col[it->second] = j;
            }
        }
        std::vector<Triplet<double>> trip;
        std::vector<std::pair<int, double*>> raw;  // (row, ptr) in triplet order
        for (int i = 0; i < int(cl.size()); ++i) {
            GCS::VEC_pD ps = cl[i]->params();  // already redirected to A's pvals
            std::sort(ps.begin(), ps.end());
            ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
            for (double* p : ps) {
                auto it = col.find(p);
                if (it != col.end()) {
                    trip.emplace_back(i, it->second, 0.0);
                    raw.emplace_back(i, p);
                }
            }
        }
        J.resize(int(cl.size()), xsize);
        J.setFromTriplets(trip.begin(), trip.end());
        J.makeCompressed();
        entries.clear();
        for (size_t k = 0; k < trip.size(); ++k) {
            entries.emplace_back(raw[k].second, &J.coeffRef(trip[k].row(), trip[k].col()));
            rowOf.push_back(raw[k].first);
        }
    }
    void eval()
    {
        for (size_t k = 0; k < entries.size(); ++k) {
            *entries[k].second = cl[rowOf[k]]->grad(entries[k].first);
        }
    }
};

// Equality-constrained QP through K = A A^T. Same solution as qp_eq whenever A has full row rank.
// The pattern of A is fixed for a subsystem, so the lower triangle of K, the list of products that make up
// each entry and the fill-reducing ordering are computed once and reused for every iteration and step.
struct SparseQp
{
    SimplicialLLT<SparseMatrix<double>, Lower> llt;
    SparseMatrix<double> K;  // lower triangle
    struct Prod { int k, a, b; };
    std::vector<Prod> prods;
    bool analysed = false;
    void pattern(const SparseMatrix<double, RowMajor>& A)
    {
        const int nr = int(A.rows());
        std::vector<std::vector<std::pair<int, int>>> colList(A.cols());  // column -> (row, value index)
        for (int i = 0; i < nr; ++i) {
            for (int q = A.outerIndexPtr()[i]; q < A.outerIndexPtr()[i + 1]; ++q) {
                colList[A.innerIndexPtr()[q]].emplace_back(i, q);
            }
        }
        std::vector<Triplet<double>> trip;
        for (int i = 0; i < nr; ++i) trip.emplace_back(i, i, 0.0);
        for (auto& cl : colList) {
            for (auto& r1 : cl) {
                for (auto& r2 : cl) {
                    if (r1.first > r2.first) trip.emplace_back(r1.first, r2.first, 0.0);
                }
            }
        }
        K.resize(nr, nr);
        K.setFromTriplets(trip.begin(), trip.end());
        K.makeCompressed();
        auto kIndex = [&](int row, int col) {
            const int* b = K.innerIndexPtr() + K.outerIndexPtr()[col];
            const int* e = K.innerIndexPtr() + K.outerIndexPtr()[col + 1];
            return int(std::lower_bound(b, e, row) - K.innerIndexPtr());
        };
        prods.clear();
        for (auto& cl : colList) {
            for (auto& r1 : cl) {
                for (auto& r2 : cl) {
                    if (r1.first >= r2.first) prods.push_back({kIndex(r1.first, r2.first), r1.second, r2.second});
                }
            }
        }
        std::sort(prods.begin(), prods.end(), [](const Prod& x, const Prod& y) {
            return x.k != y.k ? x.k < y.k : x.a != y.a ? x.a < y.a : x.b < y.b;
        });
        llt.analyzePattern(K);
        analysed = true;
    }
    int factor(const SparseMatrix<double, RowMajor>& A, SqpProfile* pf)
    {
        auto t = Clock::now();
        if (!analysed) {
            pattern(A);
            if (pf) pf->analyse += msSince(t);
        }
        double* kv = K.valuePtr();
        std::fill(kv, kv + K.nonZeros(), 0.0);
        const double* av = A.valuePtr();
        for (const Prod& p : prods) kv[p.k] += av[p.a] * av[p.b];
        if (pf) pf->formK += msSince(t);
        llt.factorize(K);
        if (pf) pf->factor += msSince(t);
        return llt.info() == Success ? 0 : -1;
    }
    VectorXd Kinv(const VectorXd& r) const { return llt.solve(r); }
    // x = argmin 0.5 x'Bx + g'x  s.t.  A x = -c
    void solve(const LowRankB& B, const SparseMatrix<double, RowMajor>& A, const VectorXd& g, const VectorXd& c,
               VectorXd& x, SqpProfile* pf)
    {
        auto t = Clock::now();
        // KKT matrix [B A'; A 0] with B = I + W C W'. For B = I it is solved through K:
        //   nu = K^-1 (A r1 - r2),  x = r1 - A' nu.
        // The BFGS part is a low-rank update of the KKT matrix (Woodbury). Its small m x m system is
        // C^-1 + W' P W with P = I - A' K^-1 A the projector onto the null space of A, i.e. only the reduced
        // Hessian enters, as in qp_eq (Z' B Z); B itself is never inverted.
        const int m = int(B.w.size());
        VectorXd nu0 = Kinv(c - A * g);  // r1 = -g, r2 = -c
        x = -g - A.transpose() * nu0;
        if (m > 0) {
            const int n = B.n;
            MatrixXd W(n, m);
            for (int k = 0; k < m; ++k) {
                W.col(k) = B.w[k];
            }
            MatrixXd PW(n, m);
            for (int k = 0; k < m; ++k) {
                PW.col(k) = W.col(k) - A.transpose() * llt.solve(A * W.col(k));
            }
            MatrixXd S = W.transpose() * PW;
            for (int k = 0; k < m; ++k) {
                S(k, k) += 1.0 / B.coef[k];
            }
            VectorXd yv = S.fullPivLu().solve(W.transpose() * x);
            x -= PW * yv;
        }
        if (pf) pf->kktSolve += msSince(t);
    }
};
}  // namespace

struct SqpCache
{
    SparseJac SJ;
    SparseQp SQ;
    bool built = false;
};

void SqpProfile::add(const SqpProfile& o)
{
    gradB += o.gradB; jacA += o.jacA; resA += o.resA;
    qr += o.qr; formQ += o.formQ; formY += o.formY; nullsp += o.nullsp;
    formK += o.formK; analyse += o.analyse; factor += o.factor; kktSolve += o.kktSolve;
    lambda += o.lambda; lineSearch += o.lineSearch; bfgs += o.bfgs; setup += o.setup; total += o.total;
    iters += o.iters; calls += o.calls;
    xsize = std::max(xsize, o.xsize);
    csize = std::max(csize, o.csize);
}

int sqpSolve(GCS::SubSystem* subsysA, GCS::SubSystem* subsysB, int maxIter, double convergence, LinAlg la,
             SqpProfile* pf, SqpCache* cache)
{
    const auto tStart = Clock::now();
    auto t = tStart;
    int xsizeA = subsysA->pSize();
    int xsizeB = subsysB->pSize();
    int csizeA = subsysA->cSize();

    GCS::VEC_pD plistAB(xsizeA + xsizeB);
    {
        GCS::VEC_pD plistA, plistB;
        subsysA->getParamList(plistA);
        subsysB->getParamList(plistB);
        std::sort(plistA.begin(), plistA.end());
        std::sort(plistB.begin(), plistB.end());
        GCS::VEC_pD::const_iterator it;
        it = std::set_union(plistA.begin(), plistA.end(), plistB.begin(), plistB.end(), plistAB.begin());
        plistAB.resize(it - plistAB.begin());
    }
    int xsize = int(plistAB.size());

    const bool dense = la == LinAlg::Dense;
    MatrixXd B;           // dense path
    LowRankB Blr {xsize, {}, {}};  // sparse path
    if (dense) {
        B = MatrixXd::Identity(xsize, xsize);
    }
    MatrixXd JA;          // dense path
    SqpCache localCache;  // sparse path; reused across calls when the caller passes a cache
    SqpCache& cc = cache ? *cache : localCache;
    SparseJac& SJ = cc.SJ;
    SparseQp& SQ = cc.SQ;
    MatrixXd Y, Z;

    VectorXd resA(csizeA);
    VectorXd lambda(csizeA), lambda0(csizeA), lambdadir(csizeA);
    VectorXd x(xsize), x0(xsize), xdir(xsize), xdir1(xsize);
    VectorXd grad(xsize);
    VectorXd h(xsize);
    VectorXd y(xsize);
    VectorXd Bh(xsize);

    subsysA->redirectParams();
    subsysB->redirectParams();

    subsysB->getParams(plistAB, x);
    subsysA->getParams(plistAB, x);
    subsysB->setParams(plistAB, x);

    if (!dense && !cc.built) {
        SJ.build(subsysA, plistAB, xsize);
        cc.built = true;
    }
    if (pf) pf->setup += msSince(t);

    subsysB->calcGrad(plistAB, grad);
    if (pf) pf->gradB += msSince(t);
    if (dense) {
        subsysA->calcJacobi(plistAB, JA);
    }
    else {
        SJ.eval();
    }
    if (pf) pf->jacA += msSince(t);
    subsysA->calcResidual(resA);
    if (pf) pf->resA += msSince(t);

    int maxIterNumber = maxIter;
    double divergingLim = 1e6 * subsysA->error() + 1e12;

    double mu = 0;
    lambda.setZero();
    long long iters = 0;
    for (int iter = 1; iter < maxIterNumber; iter++) {
        ++iters;
        t = Clock::now();
        int status;
        if (dense) {
            status = qpEqDense(B, grad, JA, resA, xdir, Y, Z, pf);
        }
        else {
            status = SQ.factor(SJ.J, pf);
            if (status == 0 && SJ.J.rows() > xsize) {
                status = -1;
            }
            if (status == 0) {
                SQ.solve(Blr, SJ.J, grad, resA, xdir, pf);
            }
        }
        t = Clock::now();
        if (status) {
            break;
        }

        x0 = x;
        lambda0 = lambda;
        if (dense) {
            lambda = Y.transpose() * (B * xdir + grad);
        }
        else {
            lambda = SQ.Kinv(SJ.J * (Blr.mul(xdir) + grad));  // Y' v = K^-1 A v
        }
        lambdadir = lambda - lambda0;
        if (pf) pf->lambda += msSince(t);

        // line search
        {
            double eta = 0.25;
            double tau = 0.5;
            double rho = 0.5;
            double alpha = 1;
            alpha = std::min(alpha, subsysA->maxStep(plistAB, xdir));

            const VectorXd Bx = dense ? VectorXd(B * xdir) : Blr.mul(xdir);
            mu = std::max(mu, (grad.dot(xdir) + std::max(0., 0.5 * xdir.dot(Bx))) / ((1. - rho) * resA.lpNorm<1>()));
            double f0 = subsysB->error() + mu * resA.lpNorm<1>();
            double deriv = grad.dot(xdir) - mu * resA.lpNorm<1>();

            x = x0 + alpha * xdir;
            subsysA->setParams(plistAB, x);
            subsysB->setParams(plistAB, x);
            subsysA->calcResidual(resA);
            double f = subsysB->error() + mu * resA.lpNorm<1>();

            bool first = true;
            while (f > f0 + eta * alpha * deriv) {
                if (first) {
                    if (dense) {
                        xdir1 = -Y * resA;
                    }
                    else {
                        xdir1 = -(SJ.J.transpose() * SQ.Kinv(resA));  // -Y r = -A' K^-1 r
                    }
                    x += xdir1;
                    subsysA->setParams(plistAB, x);
                    subsysB->setParams(plistAB, x);
                    subsysA->calcResidual(resA);
                    f = subsysB->error() + mu * resA.lpNorm<1>();
                    if (f < f0 + eta * alpha * deriv) {
                        break;
                    }
                }
                alpha = tau * alpha;
                if (alpha < 1e-8) {
                    alpha = 0.;
                }
                x = x0 + alpha * xdir;
                subsysA->setParams(plistAB, x);
                subsysB->setParams(plistAB, x);
                subsysA->calcResidual(resA);
                f = subsysB->error() + mu * resA.lpNorm<1>();
                if (alpha < 1e-8) {
                    break;
                }
            }
            lambda = lambda0 + alpha * lambdadir;
        }
        h = x - x0;
        if (pf) pf->lineSearch += msSince(t);

        if (dense) {
            y = grad - JA.transpose() * lambda;
        }
        else {
            y = grad - SJ.J.transpose() * lambda;
        }
        if (pf) pf->bfgs += msSince(t);
        subsysB->calcGrad(plistAB, grad);
        if (pf) pf->gradB += msSince(t);
        if (dense) {
            subsysA->calcJacobi(plistAB, JA);
        }
        else {
            SJ.eval();
        }
        if (pf) pf->jacA += msSince(t);
        subsysA->calcResidual(resA);
        if (pf) pf->resA += msSince(t);
        if (dense) {
            y = grad - JA.transpose() * lambda - y;
        }
        else {
            y = grad - SJ.J.transpose() * lambda - y;
        }

        if (iter > 1) {
            double yTh = y.dot(h);
            if (yTh != 0) {
                if (dense) {
                    Bh = B * h;
                    B += 1. / yTh * y * y.transpose();
                    B -= 1. / h.dot(Bh) * (Bh * Bh.transpose());
                }
                else {
                    Bh = Blr.mul(h);
                    const double hBh = h.dot(Bh);
                    Blr.w.push_back(y);
                    Blr.coef.push_back(1. / yTh);
                    Blr.w.push_back(Bh);
                    Blr.coef.push_back(-1. / hBh);
                }
            }
        }
        if (pf) pf->bfgs += msSince(t);

        double err = subsysA->error();
        if (h.norm() <= convergence && err <= smallF) {
            break;
        }
        if (err > divergingLim || err != err) {
            break;
        }
    }

    int ret;
    if (subsysA->error() <= smallF) {
        ret = GCS::Success;
    }
    else if (h.norm() <= convergence) {
        ret = GCS::Converged;
    }
    else {
        ret = GCS::Failed;
    }

    subsysA->revertParams();
    subsysB->revertParams();
    if (pf) {
        pf->total += std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
        pf->iters += iters;
        pf->calls += 1;
        pf->xsize = std::max(pf->xsize, xsize);
        pf->csize = std::max(pf->csize, csizeA);
    }
    return ret;
}

Prepared::~Prepared()
{
    for (auto* s : sub) delete s;
    for (auto* s : aux) delete s;
}
void Prepared::setReference()
{
    reference.clear();
    for (double* p : plist) reference.push_back(*p);
}
void Prepared::resetToReference()
{
    for (size_t i = 0; i < plist.size() && i < reference.size(); ++i) *plist[i] = reference[i];
}

std::unique_ptr<Prepared> prepare(const GCS::VEC_pD& plistIn, const std::vector<GCS::Constraint*>& clistR)
{
    auto P = std::make_unique<Prepared>();
    P->plist = plistIn;
    const auto& plist = P->plist;
    P->setReference();
    std::map<double*, int> pIndex;
    for (int i = 0; i < int(plist.size()); ++i) pIndex[plist[i]] = i;

    // connected components over parameters + constraints, numbered like boost::connected_components
    // (a new component id whenever a vertex is met whose component has none yet, in vertex order)
    const int nv = int(plist.size() + clistR.size());
    std::vector<int> parent(nv);
    for (int i = 0; i < nv; ++i) parent[i] = i;
    auto find = [&](int a) {
        while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
        return a;
    };
    for (int k = 0; k < int(clistR.size()); ++k) {
        clistR[k]->revertParams();
        for (double* p : clistR[k]->params()) {
            auto it = pIndex.find(p);
            if (it != pIndex.end()) {
                int a = find(int(plist.size()) + k), b = find(it->second);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    std::vector<int> comp(nv, -1), rootId(nv, -1);
    int ncomp = 0;
    for (int v = 0; v < nv; ++v) {
        int r = find(v);
        if (rootId[r] < 0) rootId[r] = ncomp++;
        comp[v] = rootId[r];
    }

    std::set<GCS::Constraint*> reducedConstrs;
    P->maps.assign(ncomp, {});
    {
        GCS::VEC_pD reducedParams = plist;
        for (auto* constr : clistR) {
            if (!(constr->getTag() >= 0 && constr->getTypeId() == GCS::Equal)) continue;
            auto it1 = pIndex.find(constr->params()[0]);
            auto it2 = pIndex.find(constr->params()[1]);
            if (it1 == pIndex.end() || it2 == pIndex.end()) continue;
            reducedConstrs.insert(constr);
            double* p_kept = reducedParams[it1->second];
            double* p_replaced = reducedParams[it2->second];
            std::replace(reducedParams.begin(), reducedParams.end(), p_replaced, p_kept);
        }
        for (size_t i = 0; i < plist.size(); ++i) {
            if (plist[i] != reducedParams[i]) P->maps[comp[i]][plist[i]] = reducedParams[i];
        }
    }
    std::vector<std::vector<GCS::Constraint*>> clists(ncomp);
    for (size_t k = 0; k < clistR.size(); ++k) {
        if (reducedConstrs.count(clistR[k]) == 0) clists[comp[plist.size() + k]].push_back(clistR[k]);
    }
    P->plists.assign(ncomp, {});
    for (size_t i = 0; i < plist.size(); ++i) P->plists[comp[i]].push_back(plist[i]);

    P->clist0.assign(ncomp, {});
    P->clist1.assign(ncomp, {});
    P->sub.assign(ncomp, nullptr);
    P->aux.assign(ncomp, nullptr);
    for (int cid = 0; cid < ncomp; ++cid) {
        for (auto* c : clists[cid]) {
            (c->getTag() >= 0 && c->isDriving() ? P->clist0 : P->clist1)[cid].push_back(c);
        }
        if (!P->clist0[cid].empty()) P->sub[cid] = new GCS::SubSystem(P->clist0[cid], P->plists[cid], P->maps[cid]);
        if (!P->clist1[cid].empty()) P->aux[cid] = new GCS::SubSystem(P->clist1[cid], P->plists[cid], P->maps[cid]);
    }
    return P;
}

int solvePrepared(Prepared& p, GCS::System& dl, LinAlg la, SqpProfile* prof)
{
    bool isReset = false;
    int res = GCS::Success;
    for (size_t cid = 0; cid < p.sub.size(); ++cid) {
        if ((p.sub[cid] || p.aux[cid]) && !isReset) {
            p.resetToReference();
            isReset = true;
        }
        if (p.sub[cid] && p.aux[cid]) {
            if (p.caches.size() != p.sub.size()) {
                p.caches.resize(p.sub.size());
            }
            if (!p.caches[cid]) {
                p.caches[cid] = std::make_shared<SqpCache>();
            }
            res = std::max(res, sqpSolve(p.sub[cid], p.aux[cid], dl.maxIter, dl.convergence, la, prof,
                                         la == LinAlg::Sparse ? p.caches[cid].get() : nullptr));
        }
        else if (p.sub[cid]) {
            res = std::max(res, dl.solve(p.sub[cid], true, GCS::DogLeg));
        }
        else if (p.aux[cid]) {
            res = std::max(res, dl.solve(p.aux[cid], true, GCS::DogLeg));
        }
    }
    return res;
}

void applyPrepared(Prepared& p)
{
    for (size_t cid = 0; cid < p.sub.size(); ++cid) {
        if (p.aux[cid]) p.aux[cid]->applySolution();
        if (p.sub[cid]) p.sub[cid]->applySolution();
        for (auto& kv : p.maps[cid]) *(kv.first) = *(kv.second);
    }
}

}  // namespace s11
