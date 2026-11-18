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
// See solver_iterate.h. The DogLeg loop is upstream's, statement for statement, so that it produces the same bits;
// its debug output is left out and a cancellation check is added. The sparse drag iteration is upstream's iteration
// with different linear algebra (spike S11).
#include "solver_iterate.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace ray::sketch::detail {

namespace {

// The BFGS matrix B = I + sum_k coef_k w_k w_k^T.
struct LowRankB {
    int n = 0;
    std::vector<Eigen::VectorXd> w;
    std::vector<double> coef;
    Eigen::VectorXd mul(const Eigen::VectorXd& v) const
    {
        Eigen::VectorXd r = v;
        for (std::size_t k = 0; k < w.size(); ++k) {
            r += (coef[k] * w[k].dot(v)) * w[k];
        }
        return r;
    }
};

// Sparse Jacobian of subsystem A over plistAB with a fixed pattern.
struct SparseJac {
    Eigen::SparseMatrix<double, Eigen::RowMajor> J;
    std::vector<GCS::Constraint*> cl;
    std::vector<std::pair<double*, double*>> entries; // (parameter as the constraint sees it, value slot in J)
    std::vector<int> rowOf;
    void build(GCS::SubSystem* A, const GCS::VEC_pD& plistAB, int xsize)
    {
        A->getConstraintList(cl);
        GCS::MAP_pD_pD pmap;
        A->getParamMap(pmap);
        std::map<double*, int> col; // A's value slot -> column
        for (int j = 0; j < xsize; ++j) {
            auto it = pmap.find(plistAB[static_cast<std::size_t>(j)]);
            if (it != pmap.end()) {
                col[it->second] = j;
            }
        }
        std::vector<Eigen::Triplet<double>> trip;
        std::vector<std::pair<int, double*>> raw; // (row, parameter) in triplet order
        for (int i = 0; i < static_cast<int>(cl.size()); ++i) {
            GCS::VEC_pD ps = cl[static_cast<std::size_t>(i)]->params(); // already redirected to A's values
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
        J.resize(static_cast<Eigen::Index>(cl.size()), xsize);
        J.setFromTriplets(trip.begin(), trip.end());
        J.makeCompressed();
        entries.clear();
        rowOf.clear();
        for (std::size_t k = 0; k < trip.size(); ++k) {
            entries.emplace_back(raw[k].second, &J.coeffRef(trip[k].row(), trip[k].col()));
            rowOf.push_back(raw[k].first);
        }
    }
    void eval()
    {
        for (std::size_t k = 0; k < entries.size(); ++k) {
            *entries[k].second = cl[static_cast<std::size_t>(rowOf[k])]->grad(entries[k].first);
        }
    }
};

// The equality-constrained QP of the drag iteration, through K = A A^T. Same solution as qp_eq whenever A has full row
// rank. A's pattern is fixed for a subsystem, so the lower triangle of K, the products that make up each of its
// entries and the fill-reducing ordering are computed once and reused for every iteration and every drag step.
struct SparseQp {
    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower> llt;
    Eigen::SparseMatrix<double> K; // lower triangle
    struct Prod {
        int k, a, b;
    };
    std::vector<Prod> prods;
    bool analysed = false;

    void pattern(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A)
    {
        const int nr = static_cast<int>(A.rows());
        std::vector<std::vector<std::pair<int, int>>> colList(static_cast<std::size_t>(A.cols()));
        for (int i = 0; i < nr; ++i) {
            for (int q = A.outerIndexPtr()[i]; q < A.outerIndexPtr()[i + 1]; ++q) {
                colList[static_cast<std::size_t>(A.innerIndexPtr()[q])].emplace_back(i, q);
            }
        }
        std::vector<Eigen::Triplet<double>> trip;
        for (int i = 0; i < nr; ++i) {
            trip.emplace_back(i, i, 0.0);
        }
        for (const auto& cl : colList) {
            for (const auto& r1 : cl) {
                for (const auto& r2 : cl) {
                    if (r1.first > r2.first) {
                        trip.emplace_back(r1.first, r2.first, 0.0);
                    }
                }
            }
        }
        K.resize(nr, nr);
        K.setFromTriplets(trip.begin(), trip.end());
        K.makeCompressed();
        auto kIndex = [&](int row, int col) {
            const int* b = K.innerIndexPtr() + K.outerIndexPtr()[col];
            const int* e = K.innerIndexPtr() + K.outerIndexPtr()[col + 1];
            return static_cast<int>(std::lower_bound(b, e, row) - K.innerIndexPtr());
        };
        prods.clear();
        for (const auto& cl : colList) {
            for (const auto& r1 : cl) {
                for (const auto& r2 : cl) {
                    if (r1.first >= r2.first) {
                        prods.push_back({kIndex(r1.first, r2.first), r1.second, r2.second});
                    }
                }
            }
        }
        std::sort(prods.begin(), prods.end(), [](const Prod& x, const Prod& y) {
            return x.k != y.k ? x.k < y.k : x.a != y.a ? x.a < y.a : x.b < y.b;
        });
        llt.analyzePattern(K);
        analysed = true;
    }
    int factor(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A)
    {
        if (!analysed) {
            pattern(A);
        }
        double* kv = K.valuePtr();
        std::fill(kv, kv + K.nonZeros(), 0.0);
        const double* av = A.valuePtr();
        for (const Prod& p : prods) {
            kv[p.k] += av[p.a] * av[p.b];
        }
        llt.factorize(K);
        return llt.info() == Eigen::Success ? 0 : -1;
    }
    Eigen::VectorXd Kinv(const Eigen::VectorXd& r) const { return llt.solve(r); }
    // x = argmin 0.5 x'Bx + g'x  subject to  A x = -c
    void solve(const LowRankB& B, const Eigen::SparseMatrix<double, Eigen::RowMajor>& A, const Eigen::VectorXd& g,
               const Eigen::VectorXd& c, Eigen::VectorXd& x) const
    {
        // KKT matrix [B A'; A 0] with B = I + W C W'. For B = I it is solved through K:
        //   nu = K^-1 (A r1 - r2),  x = r1 - A' nu.
        // The BFGS part is a low-rank update of the KKT matrix (Woodbury). Its small m x m system is
        // C^-1 + W' P W with P = I - A' K^-1 A, the projector onto the null space of A, so only the reduced Hessian
        // enters, as in qp_eq (Z' B Z); B itself is never inverted.
        const int m = static_cast<int>(B.w.size());
        const Eigen::VectorXd nu0 = Kinv(c - A * g); // r1 = -g, r2 = -c
        x = -g - A.transpose() * nu0;
        if (m > 0) {
            const int n = B.n;
            Eigen::MatrixXd W(n, m);
            for (int k = 0; k < m; ++k) {
                W.col(k) = B.w[static_cast<std::size_t>(k)];
            }
            Eigen::MatrixXd PW(n, m);
            for (int k = 0; k < m; ++k) {
                PW.col(k) = W.col(k) - A.transpose() * llt.solve(A * W.col(k));
            }
            Eigen::MatrixXd S = W.transpose() * PW;
            for (int k = 0; k < m; ++k) {
                S(k, k) += 1.0 / B.coef[static_cast<std::size_t>(k)];
            }
            const Eigen::VectorXd yv = S.fullPivLu().solve(W.transpose() * x);
            x -= PW * yv;
        }
    }
};

// Restarts of the sparse drag iteration when its result is not first-order optimal, and the optimality tolerance:
// the infinity norm of the projected gradient of the drag objective, which is a distance in mm. Upstream's own
// iteration ends up to a few nanometres short of the optimum (its line search fails at that level), so a tighter
// tolerance would make results more precise than the reference and no longer agree with it to 1e-9 mm; this one only
// catches a stop that is clearly early.
constexpr int kSqpRestarts = 2;
constexpr double kSqpStationarity = 5e-9;

bool cancelled(const CancelToken* cancel)
{
    return cancel != nullptr && cancel->cancelled();
}

} // namespace

struct SqpCache {
    SparseJac SJ;
    SparseQp SQ;
    bool built = false;
};

int sqpSparse(GCS::SubSystem* subsysA, GCS::SubSystem* subsysB, int maxIter, double convergence, SqpCache* cache)
{
    const int xsizeA = subsysA->pSize();
    const int xsizeB = subsysB->pSize();
    const int csizeA = subsysA->cSize();

    GCS::VEC_pD plistAB(static_cast<std::size_t>(xsizeA + xsizeB));
    {
        GCS::VEC_pD plistA, plistB;
        subsysA->getParamList(plistA);
        subsysB->getParamList(plistB);
        std::sort(plistA.begin(), plistA.end());
        std::sort(plistB.begin(), plistB.end());
        const auto it = std::set_union(plistA.begin(), plistA.end(), plistB.begin(), plistB.end(), plistAB.begin());
        plistAB.resize(static_cast<std::size_t>(it - plistAB.begin()));
    }
    const int xsize = static_cast<int>(plistAB.size());

    LowRankB B{xsize, {}, {}};
    SqpCache localCache;
    SqpCache& cc = cache ? *cache : localCache;
    SparseJac& SJ = cc.SJ;
    SparseQp& SQ = cc.SQ;

    Eigen::VectorXd resA(csizeA);
    Eigen::VectorXd lambda(csizeA), lambda0(csizeA), lambdadir(csizeA);
    Eigen::VectorXd x(xsize), x0(xsize), xdir(xsize), xdir1(xsize);
    Eigen::VectorXd grad(xsize);
    Eigen::VectorXd h(xsize);
    Eigen::VectorXd y(xsize);
    Eigen::VectorXd Bh(xsize);

    subsysA->redirectParams();
    subsysB->redirectParams();

    subsysB->getParams(plistAB, x);
    subsysA->getParams(plistAB, x);
    subsysB->setParams(plistAB, x);

    if (!cc.built) {
        SJ.build(subsysA, plistAB, xsize);
        cc.built = true;
    }

    subsysB->calcGrad(plistAB, grad);
    SJ.eval();
    subsysA->calcResidual(resA);

    const double divergingLim = 1e6 * subsysA->error() + 1e12;

    double mu = 0;
    h.setZero();
    // One run of upstream's iteration from the current x, with the BFGS matrix reset to the identity.
    auto iterate = [&] {
        B.w.clear();
        B.coef.clear();
        mu = 0;
        lambda.setZero();
        for (int iter = 1; iter < maxIter; iter++) {
            int status = SQ.factor(SJ.J);
            if (status == 0 && SJ.J.rows() > xsize) {
                status = -1;
            }
            if (status == 0) {
                SQ.solve(B, SJ.J, grad, resA, xdir);
            }
            if (status) {
                break;
            }

            x0 = x;
            lambda0 = lambda;
            lambda = SQ.Kinv(SJ.J * (B.mul(xdir) + grad)); // Y' v = K^-1 A v
            lambdadir = lambda - lambda0;

            // line search
            {
                const double eta = 0.25;
                const double tau = 0.5;
                const double rho = 0.5;
                double alpha = 1;
                alpha = std::min(alpha, subsysA->maxStep(plistAB, xdir));

                const Eigen::VectorXd Bx = B.mul(xdir);
                mu =
                    std::max(mu, (grad.dot(xdir) + std::max(0., 0.5 * xdir.dot(Bx))) / ((1. - rho) * resA.lpNorm<1>()));
                const double f0 = subsysB->error() + mu * resA.lpNorm<1>();
                const double deriv = grad.dot(xdir) - mu * resA.lpNorm<1>();

                x = x0 + alpha * xdir;
                subsysA->setParams(plistAB, x);
                subsysB->setParams(plistAB, x);
                subsysA->calcResidual(resA);
                double f = subsysB->error() + mu * resA.lpNorm<1>();

                const bool first = true;
                while (f > f0 + eta * alpha * deriv) {
                    if (first) {
                        xdir1 = -(SJ.J.transpose() * SQ.Kinv(resA)); // -Y r = -A' K^-1 r
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

            y = grad - SJ.J.transpose() * lambda;
            subsysB->calcGrad(plistAB, grad);
            SJ.eval();
            subsysA->calcResidual(resA);
            y = grad - SJ.J.transpose() * lambda - y;

            if (iter > 1) {
                const double yTh = y.dot(h);
                if (yTh != 0) {
                    Bh = B.mul(h);
                    const double hBh = h.dot(Bh);
                    B.w.push_back(y);
                    B.coef.push_back(1. / yTh);
                    B.w.push_back(Bh);
                    B.coef.push_back(-1. / hBh);
                }
            }

            const double err = subsysA->error();
            if (h.norm() <= convergence && err <= smallF) {
                break;
            }
            if (err > divergingLim || err != err) {
                break;
            }
        }
    };
    auto status = [&] {
        if (subsysA->error() <= smallF) {
            return static_cast<int>(GCS::Success);
        }
        return static_cast<int>(h.norm() <= convergence ? GCS::Converged : GCS::Failed);
    };

    iterate();
    int ret = status();

    // RayCAD addition, not in upstream: the iteration also stops when the line search fails short of the optimum (the
    // step is then 0), which left the dragged point up to 1.2 micrometres short of a reachable target in a 60 mm drag
    // where PlaneGCS's dense run of the same iteration got there (solver_drag_test.cpp). When the drag objective's
    // gradient projected onto the constraints' null space is not small at the result, the iteration is run again
    // from there with a fresh BFGS matrix. grad, resA and the Jacobian are already evaluated at the result.
    for (int restart = 0; restart < kSqpRestarts && ret == GCS::Success; ++restart) {
        if (SQ.factor(SJ.J) != 0) {
            break;
        }
        const Eigen::VectorXd pg = grad - SJ.J.transpose() * SQ.Kinv(SJ.J * grad);
        if (pg.lpNorm<Eigen::Infinity>() <= kSqpStationarity) {
            break;
        }
        iterate();
        ret = status();
    }

    subsysA->revertParams();
    subsysB->revertParams();
    return ret;
}

int dogLeg(GCS::SubSystem* subsys, const GCS::System& settings, const CancelToken* cancel)
{
    const int xsize = subsys->pSize();
    const int csize = subsys->cSize();

    if (xsize == 0) {
        return GCS::Success;
    }

    const double tolg = settings.DL_tolg;
    const double tolx = settings.DL_tolx;
    const double tolf = settings.DL_tolf;

    const int maxIterNumber = (settings.sketchSizeMultiplier ? settings.maxIter * xsize : settings.maxIter);

    Eigen::VectorXd x(xsize), x_new(xsize);
    Eigen::VectorXd fx(csize), fx_new(csize);
    Eigen::MatrixXd Jx(csize, xsize), Jx_new(csize, xsize);
    Eigen::VectorXd g(xsize), h_sd(xsize), h_gn(xsize), h_dl(xsize);

    subsys->redirectParams();

    double err;
    subsys->getParams(x);
    subsys->calcResidual(fx, err);
    subsys->calcJacobi(Jx);

    g = Jx.transpose() * (-fx);

    // get the infinity norm fx_inf and g_inf
    double g_inf = g.lpNorm<Eigen::Infinity>();
    double fx_inf = fx.lpNorm<Eigen::Infinity>();

    const double divergingLim = 1e6 * err + 1e12;

    double delta = 0.1;
    double alpha = 0.;
    double nu = 2.;
    int iter = 0, stop = 0, reduce = 0;
    while (!stop) {
        // check if finished
        if (fx_inf <= tolf) {
            // Success
            stop = 1;
            break;
        } else if (g_inf <= tolg) {
            stop = 2;
            break;
        } else if (delta <= tolx * (tolx + x.norm())) {
            stop = 2;
            break;
        } else if (iter >= maxIterNumber) {
            stop = 4;
            break;
        } else if (err > divergingLim || err != err) {
            // check for diverging and NaN
            stop = 6;
            break;
        }
        if (cancelled(cancel)) {
            subsys->revertParams();
            return kSolveCancelled;
        }

        // get the steepest descent direction
        alpha = g.squaredNorm() / (Jx * g).squaredNorm();
        h_sd = alpha * g;

        // get the gauss-newton step
        switch (settings.dogLegGaussStep) {
        case GCS::FullPivLU:
            h_gn = Jx.fullPivLu().solve(-fx);
            break;
        case GCS::LeastNormFullPivLU:
            h_gn = Jx.adjoint() * (Jx * Jx.adjoint()).fullPivLu().solve(-fx);
            break;
        case GCS::LeastNormLdlt:
            h_gn = Jx.adjoint() * (Jx * Jx.adjoint()).ldlt().solve(-fx);
            break;
        }

        const double rel_error = (Jx * h_gn + fx).norm() / fx.norm();
        if (rel_error > 1e15) {
            break;
        }

        // compute the dogleg step
        if (h_gn.norm() < delta) {
            h_dl = h_gn;
            if (h_dl.norm() <= tolx * (tolx + x.norm())) {
                stop = 5;
                break;
            }
        } else if (alpha * g.norm() >= delta) {
            h_dl = (delta / (alpha * g.norm())) * h_sd;
        } else {
            // compute beta
            double beta = 0;
            Eigen::VectorXd b = h_gn - h_sd;
            const double bb = (b.transpose() * b).norm();
            const double gb = (h_sd.transpose() * b).norm();
            const double c = (delta + h_sd.norm()) * (delta - h_sd.norm());

            if (gb > 0) {
                beta = c / (gb + std::sqrt(gb * gb + c * bb));
            } else {
                beta = (std::sqrt(gb * gb + c * bb) - gb) / bb;
            }

            // and update h_dl and dL with beta
            h_dl = h_sd + beta * b;
        }

        // get the new values
        double err_new;
        x_new = x + h_dl;
        subsys->setParams(x_new);
        subsys->calcResidual(fx_new, err_new);
        subsys->calcJacobi(Jx_new);

        // calculate the linear model and the update ratio
        const double dL = err - 0.5 * (fx + Jx * h_dl).squaredNorm();
        const double dF = err - err_new;
        double rho = dL / dF;

        if (dF > 0 && dL > 0) {
            x = x_new;
            Jx = Jx_new;
            fx = fx_new;
            err = err_new;

            g = Jx.transpose() * (-fx);

            // get infinity norms
            g_inf = g.lpNorm<Eigen::Infinity>();
            fx_inf = fx.lpNorm<Eigen::Infinity>();
        } else {
            rho = -1;
        }

        // update delta
        if (std::fabs(rho - 1.) < 0.2 && h_dl.norm() > delta / 3. && reduce <= 0) {
            delta = 3 * delta;
            nu = 2;
            reduce = 0;
        } else if (rho < 0.25) {
            delta = delta / nu;
            nu = 2 * nu;
            reduce = 2;
        } else {
            reduce--;
        }

        // count this iteration and start again
        iter++;
    }

    subsys->revertParams();

    return (stop == 1) ? GCS::Success : GCS::Failed;
}

Prepared::Prepared() = default;
Prepared::~Prepared() = default;

void Prepared::setReference()
{
    reference.clear();
    reference.reserve(plist.size());
    for (double* p : plist) {
        reference.push_back(*p);
    }
}

void Prepared::resetToReference()
{
    if (reference.size() != plist.size()) {
        return;
    }
    for (std::size_t i = 0; i < plist.size(); ++i) {
        *plist[i] = reference[i];
    }
}

std::unique_ptr<Prepared> prepare(const GCS::VEC_pD& plistIn, const std::vector<GCS::Constraint*>& clistR)
{
    auto P = std::make_unique<Prepared>();
    P->plist = plistIn;
    const GCS::VEC_pD& plist = P->plist;
    P->setReference();
    std::map<double*, int> pIndex;
    for (int i = 0; i < static_cast<int>(plist.size()); ++i) {
        pIndex[plist[static_cast<std::size_t>(i)]] = i;
    }

    // Connected components over parameters and constraints, numbered as boost::connected_components numbers them
    // in initSolution: components in the order of their lowest vertex (parameters first, then constraints).
    const int np = static_cast<int>(plist.size());
    const int nv = np + static_cast<int>(clistR.size());
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
    for (int k = 0; k < static_cast<int>(clistR.size()); ++k) {
        GCS::Constraint* c = clistR[static_cast<std::size_t>(k)];
        c->revertParams();
        for (double* p : c->params()) {
            const auto it = pIndex.find(p);
            if (it != pIndex.end()) {
                const int a = find(np + k);
                const int b = find(it->second);
                if (a != b) {
                    parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
                }
            }
        }
    }
    std::vector<int> comp(static_cast<std::size_t>(nv), -1);
    std::vector<int> rootId(static_cast<std::size_t>(nv), -1);
    int ncomp = 0;
    for (int v = 0; v < nv; ++v) {
        const int r = find(v);
        if (rootId[static_cast<std::size_t>(r)] < 0) {
            rootId[static_cast<std::size_t>(r)] = ncomp++;
        }
        comp[static_cast<std::size_t>(v)] = rootId[static_cast<std::size_t>(r)];
    }

    // Equality constraints between two unknowns (tag >= 0) are eliminated by merging the two parameters.
    std::set<GCS::Constraint*> reducedConstrs;
    P->maps.assign(static_cast<std::size_t>(ncomp), {});
    {
        GCS::VEC_pD reducedParams = plist;
        for (GCS::Constraint* constr : clistR) {
            if (!(constr->getTag() >= 0 && constr->getTypeId() == GCS::Equal)) {
                continue;
            }
            const auto it1 = pIndex.find(constr->params()[0]);
            const auto it2 = pIndex.find(constr->params()[1]);
            if (it1 == pIndex.end() || it2 == pIndex.end()) {
                continue;
            }
            reducedConstrs.insert(constr);
            double* p_kept = reducedParams[static_cast<std::size_t>(it1->second)];
            double* p_replaced = reducedParams[static_cast<std::size_t>(it2->second)];
            std::replace(reducedParams.begin(), reducedParams.end(), p_replaced, p_kept);
        }
        for (std::size_t i = 0; i < plist.size(); ++i) {
            if (plist[i] != reducedParams[i]) {
                P->maps[static_cast<std::size_t>(comp[i])][plist[i]] = reducedParams[i];
            }
        }
    }
    std::vector<std::vector<GCS::Constraint*>> clists(static_cast<std::size_t>(ncomp));
    for (std::size_t k = 0; k < clistR.size(); ++k) {
        if (reducedConstrs.count(clistR[k]) == 0) {
            clists[static_cast<std::size_t>(comp[plist.size() + k])].push_back(clistR[k]);
        }
    }
    P->plists.assign(static_cast<std::size_t>(ncomp), {});
    for (std::size_t i = 0; i < plist.size(); ++i) {
        P->plists[static_cast<std::size_t>(comp[i])].push_back(plist[i]);
    }

    P->clist0.assign(static_cast<std::size_t>(ncomp), {});
    P->clist1.assign(static_cast<std::size_t>(ncomp), {});
    P->sub.resize(static_cast<std::size_t>(ncomp));
    P->aux.resize(static_cast<std::size_t>(ncomp));
    P->caches.resize(static_cast<std::size_t>(ncomp));
    for (std::size_t cid = 0; cid < static_cast<std::size_t>(ncomp); ++cid) {
        for (GCS::Constraint* c : clists[cid]) {
            (c->getTag() >= 0 && c->isDriving() ? P->clist0 : P->clist1)[cid].push_back(c);
        }
        if (!P->clist0[cid].empty()) {
            P->sub[cid] = std::make_unique<GCS::SubSystem>(P->clist0[cid], P->plists[cid], P->maps[cid]);
        }
        if (!P->clist1[cid].empty()) {
            P->aux[cid] = std::make_unique<GCS::SubSystem>(P->clist1[cid], P->plists[cid], P->maps[cid]);
        }
    }
    return P;
}

int solvePrepared(Prepared& p, const GCS::System& settings, const CancelToken* cancel)
{
    bool isReset = false;
    int res = GCS::Success;
    for (std::size_t cid = 0; cid < p.sub.size(); ++cid) {
        if (cancelled(cancel)) {
            return kSolveCancelled;
        }
        GCS::SubSystem* sub = p.sub[cid].get();
        GCS::SubSystem* aux = p.aux[cid].get();
        if ((sub || aux) && !isReset) {
            p.resetToReference();
            isReset = true;
        }
        int r = GCS::Success;
        if (sub && aux) {
            if (!p.caches[cid]) {
                p.caches[cid] = std::make_unique<SqpCache>();
            }
            r = sqpSparse(sub, aux, settings.maxIter, settings.convergence, p.caches[cid].get());
        } else if (sub) {
            r = dogLeg(sub, settings, cancel);
        } else if (aux) {
            r = dogLeg(aux, settings, cancel);
        }
        if (r == kSolveCancelled) {
            return r;
        }
        res = std::max(res, r);
    }
    return res;
}

void applyPrepared(Prepared& p)
{
    for (std::size_t cid = 0; cid < p.sub.size(); ++cid) {
        if (p.aux[cid]) {
            p.aux[cid]->applySolution();
        }
        if (p.sub[cid]) {
            p.sub[cid]->applySolution();
        }
        for (const auto& kv : p.maps[cid]) {
            *(kv.first) = *(kv.second);
        }
    }
}

} // namespace ray::sketch::detail
