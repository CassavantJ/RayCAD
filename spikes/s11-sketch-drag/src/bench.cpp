// Spike S11: faster sketch drag on top of unmodified PlaneGCS. Throwaway code. See ../REPORT.md.
//
// The test sketches are S7's (spikes/s7-planegcs/src/bench.cpp): a 5 x rows grid of rounded rectangles with
// two diagonals, every unit linked to its neighbour, so the sketch is one coupled group. The only change to
// the recipe is that every PlaneGCS constraint is created here (the same constructor calls the System::add*
// helpers make, in the same order) so that the spike keeps a list of the constraint objects and can create a
// second copy of any of them for a reduced drag system.
#include "GCS.h"
#include "SubSystem.h"
#include "sqp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = std::numbers::pi;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Lcg
{
    std::uint64_t s;
    double next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym(double amp) { return (next() * 2.0 - 1.0) * amp; }
};

int g_rows = 4;
constexpr int kCols = 5;
int units() { return kCols * g_rows; }
bool freePitch(int i)
{
    for (int c : {units() - 1, 2 * units() / 3, units() / 3}) {
        if (i == (c % 5 == 0 ? c + 1 : c)) {
            return true;
        }
    }
    return false;
}

struct Options
{
    bool nearly = true;
    bool decoupled = false;
    bool loose = false;
    double noise = 0.0;
    int extra = 0;  // 1 = add a redundant constraint (S7's case: horizontal on the top of unit 3)
    const std::vector<double>* init = nullptr;
};

struct Unit
{
    int bl, br, tr, tl;
    int bottom, right, top, left, d1, d2;
};

// One PlaneGCS constraint object, with a way to create another copy of it.
struct Rec
{
    int tag;
    std::function<GCS::Constraint*()> make;
    GCS::Constraint* c;
};

struct Model
{
    GCS::System sys;
    std::vector<double> store;
    std::deque<double> consts;
    std::vector<double*> unknowns;
    std::vector<GCS::Line> lines;
    std::vector<GCS::Arc> arcs;
    std::vector<Unit> units;
    std::vector<Rec> cons;  // every constraint object in sys, in the order it was added
    int tags = 1;           // tag 0 = arc rules
    Options opt;
    Lcg rng {12345};

    ~Model() { sys.clear(); }

    double* P(double v)
    {
        const std::size_t idx = unknowns.size();
        if (opt.init) {
            v = (*opt.init)[idx];
        }
        store.push_back(v);
        double* p = &store.back();
        unknowns.push_back(p);
        return p;
    }
    double* C(double v)
    {
        consts.push_back(v);
        return &consts.back();
    }
    double n() { return opt.noise > 0 ? rng.sym(opt.noise) : 0.0; }
    int tag() { return tags++; }

    void add(int t, std::function<GCS::Constraint*()> f)
    {
        GCS::Constraint* c = f();
        c->setTag(t);
        c->setDriving(true);
        sys.addConstraint(c);
        cons.push_back({t, std::move(f), c});
    }
    // The same constraint objects the System::addConstraint* helpers create (GCS.cpp, FreeCAD 1.1.4).
    void equal(double* a, double* b, int t)
    {
        add(t, [=] { return new GCS::ConstraintEqual(a, b); });
    }
    void coincident(GCS::Point p1, GCS::Point p2, int t)
    {
        equal(p1.x, p2.x, t);
        equal(p1.y, p2.y, t);
    }
    void arcRules(GCS::Arc a, int t)
    {
        add(t, [=]() mutable { return new GCS::ConstraintCurveValue(a.start, a.start.x, a, a.startAngle); });
        add(t, [=]() mutable { return new GCS::ConstraintCurveValue(a.start, a.start.y, a, a.startAngle); });
        add(t, [=]() mutable { return new GCS::ConstraintCurveValue(a.end, a.end.x, a, a.endAngle); });
        add(t, [=]() mutable { return new GCS::ConstraintCurveValue(a.end, a.end.y, a, a.endAngle); });
    }
    void angleViaPoint(GCS::Line l, GCS::Arc a, GCS::Point p, double* ang, int t)
    {
        add(t, [=]() mutable { return new GCS::ConstraintAngleViaPoint(l, a, p, ang); });
    }
    void parallel(GCS::Line l1, GCS::Line l2, int t)
    {
        add(t, [=]() mutable { return new GCS::ConstraintParallel(l1, l2); });
    }
    void equalLength(GCS::Line l1, GCS::Line l2, int t)
    {
        add(t, [=]() mutable { return new GCS::ConstraintEqualLineLength(l1, l2); });
    }
    void distance(GCS::Point p1, GCS::Point p2, double* d, int t)
    {
        add(t, [=]() mutable { return new GCS::ConstraintP2PDistance(p1, p2, d); });
    }

    int addLine(double x1, double y1, double x2, double y2)
    {
        GCS::Line l;
        l.p1 = GCS::Point(P(x1 + n()), P(y1 + n()));
        l.p2 = GCS::Point(P(x2 + n()), P(y2 + n()));
        lines.push_back(l);
        return int(lines.size()) - 1;
    }
    int addArc(double cx, double cy, double r, double a0, double a1)
    {
        GCS::Arc a;
        const double an = opt.noise > 0 ? 0.02 : 0.0;
        a.center = GCS::Point(P(cx + n()), P(cy + n()));
        a.rad = P(r + n() * 0.5);
        a.startAngle = P(a0 + (an > 0 ? rng.sym(an) : 0.0));
        a.endAngle = P(a1 + (an > 0 ? rng.sym(an) : 0.0));
        a.start = GCS::Point(P(cx + r * std::cos(a0) + n()), P(cy + r * std::sin(a0) + n()));
        a.end = GCS::Point(P(cx + r * std::cos(a1) + n()), P(cy + r * std::sin(a1) + n()));
        arcs.push_back(a);
        return int(arcs.size()) - 1;
    }
    void tangentAtEnd(GCS::Line& l, GCS::Point& lp, GCS::Arc& a, GCS::Point& ap)
    {
        const int t = tag();
        double ang = sys.calculateAngleViaPoint(l, a, lp);
        if (ang > kPi) {
            ang -= 2 * kPi;
        }
        if (ang < -kPi) {
            ang += 2 * kPi;
        }
        double* want = C(std::fabs(ang) > kPi / 2 ? kPi : 0.0);
        coincident(lp, ap, t);
        angleViaPoint(l, a, lp, want, t);
    }
};

constexpr double kW = 30, kH = 20, kR = 4, kPitchX = 40, kPitchY = 30;

std::unique_ptr<Model> build(const Options& opt)
{
    auto mp = std::make_unique<Model>();
    Model& m = *mp;
    m.opt = opt;
    m.store.reserve(std::size_t(kCols * g_rows) * (6 * 4 + 4 * 9));
    m.lines.reserve(kCols * g_rows * 6);
    m.arcs.reserve(kCols * g_rows * 4);
    for (int row = 0; row < g_rows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const double ox = col * kPitchX, oy = row * kPitchY;
            const double w = kW, h = kH, r = kR;
            Unit u;
            u.bl = m.addArc(ox + r, oy + r, r, kPi, 1.5 * kPi);
            u.bottom = m.addLine(ox + r, oy, ox + w - r, oy);
            u.br = m.addArc(ox + w - r, oy + r, r, 1.5 * kPi, 2 * kPi);
            u.right = m.addLine(ox + w, oy + r, ox + w, oy + h - r);
            u.tr = m.addArc(ox + w - r, oy + h - r, r, 0, 0.5 * kPi);
            u.top = m.addLine(ox + w - r, oy + h, ox + r, oy + h);
            u.tl = m.addArc(ox + r, oy + h - r, r, 0.5 * kPi, kPi);
            u.left = m.addLine(ox, oy + h - r, ox, oy + r);
            u.d1 = m.addLine(ox + r, oy + r, ox + w - r, oy + h - r);
            u.d2 = m.addLine(ox + w - r, oy + r, ox + r, oy + h - r);
            m.units.push_back(u);
        }
    }
    auto& L = m.lines;
    auto& A = m.arcs;
    for (int i = 0; i < int(m.units.size()); ++i) {
        const Unit& u = m.units[i];
        const int row = i / kCols, col = i % kCols;
        for (int a : {u.bl, u.br, u.tr, u.tl}) {
            m.arcRules(A[a], 0);
        }
        m.tangentAtEnd(L[u.bottom], L[u.bottom].p1, A[u.bl], A[u.bl].end);
        m.tangentAtEnd(L[u.bottom], L[u.bottom].p2, A[u.br], A[u.br].start);
        m.tangentAtEnd(L[u.right], L[u.right].p1, A[u.br], A[u.br].end);
        m.tangentAtEnd(L[u.right], L[u.right].p2, A[u.tr], A[u.tr].start);
        m.tangentAtEnd(L[u.top], L[u.top].p1, A[u.tr], A[u.tr].end);
        m.tangentAtEnd(L[u.top], L[u.top].p2, A[u.tl], A[u.tl].start);
        m.tangentAtEnd(L[u.left], L[u.left].p1, A[u.tl], A[u.tl].end);
        m.tangentAtEnd(L[u.left], L[u.left].p2, A[u.bl], A[u.bl].start);
        m.coincident(L[u.d1].p1, A[u.bl].center, m.tag());
        m.coincident(L[u.d1].p2, A[u.tr].center, m.tag());
        m.coincident(L[u.d2].p1, A[u.br].center, m.tag());
        m.coincident(L[u.d2].p2, A[u.tl].center, m.tag());
        m.equal(L[u.bottom].p1.y, L[u.bottom].p2.y, m.tag());  // horizontal
        m.equal(L[u.left].p1.x, L[u.left].p2.x, m.tag());      // vertical
        m.parallel(L[u.top], L[u.bottom], m.tag());
        m.parallel(L[u.right], L[u.left], m.tag());
        m.equal(A[u.br].rad, A[u.bl].rad, m.tag());  // equal radius
        m.equal(A[u.tr].rad, A[u.bl].rad, m.tag());
        m.equal(A[u.tl].rad, A[u.bl].rad, m.tag());
        const Unit* prev = i > 0 ? &m.units[i - 1] : nullptr;
        const bool dec = opt.decoupled, last = i == int(m.units.size()) - 1;
        if (i % 4 == 0 || dec) {
            m.equal(A[u.bl].rad, m.C(kR), m.tag());  // radius
        }
        else {
            m.equal(A[u.bl].rad, A[prev->bl].rad, m.tag());
        }
        if (dec) {
            if (!last) {
                m.distance(L[u.bottom].p1, L[u.bottom].p2, m.C(kW - 2 * kR), m.tag());
            }
            m.distance(L[u.left].p1, L[u.left].p2, m.C(kH - 2 * kR), m.tag());
        }
        else if (i % 2 == 0) {
            if (!(opt.nearly && i == 0) && !opt.loose) {
                m.distance(L[u.bottom].p1, L[u.bottom].p2, m.C(kW - 2 * kR), m.tag());
                m.distance(L[u.left].p1, L[u.left].p2, m.C(kH - 2 * kR), m.tag());
            }
        }
        else {
            m.equalLength(L[u.bottom], L[prev->bottom], m.tag());
            m.equalLength(L[u.left], L[prev->left], m.tag());
        }
        if (i == 0 || dec) {
            m.equal(A[u.bl].center.x, m.C(col * kPitchX + kR), m.tag());  // coordinate x
            m.equal(A[u.bl].center.y, m.C(row * kPitchY + kR), m.tag());
        }
        else if (col > 0) {
            m.equal(A[u.bl].center.y, A[prev->bl].center.y, m.tag());  // horizontal centres
            if (!(opt.nearly && freePitch(i)) && !opt.loose) {
                m.distance(A[u.bl].center, A[prev->bl].center, m.C(kPitchX), m.tag());
            }
        }
        else {
            const Unit& below = m.units[i - kCols];
            m.equal(A[u.bl].center.x, A[below.bl].center.x, m.tag());  // vertical centres
            if (!opt.loose) {
                m.distance(A[u.bl].center, A[below.bl].center, m.C(kPitchY), m.tag());
            }
        }
    }
    if (opt.extra == 1) {  // the top of unit 3 is already parallel to its horizontal bottom
        m.equal(L[m.units[3].top].p1.y, L[m.units[3].top].p2.y, m.tag());
    }
    m.sys.debugMode = GCS::NoDebug;
    m.sys.declareUnknowns(m.unknowns);
    return mp;
}

std::vector<double> values(const Model& m)
{
    std::vector<double> v;
    v.reserve(m.unknowns.size());
    for (double* p : m.unknowns) {
        v.push_back(*p);
    }
    return v;
}
void setValues(Model& m, const std::vector<double>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i) {
        *m.unknowns[i] = v[i];
    }
}
double maxErrAll(Model& m)
{
    double e = 0;
    for (const Rec& r : m.cons) {
        if (r.tag >= 0) {
            e = std::max(e, std::fabs(r.c->error()));
        }
    }
    return e;
}
bool ok(int res) { return res == GCS::Success || res == GCS::Converged; }

struct Stats
{
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double pct(double p)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[std::min(v.size() - 1, std::size_t(p * double(v.size())))];
    }
    double median() { return pct(0.5); }
    double worst() { return pct(1.0); }
};

std::uint64_t fnv(std::uint64_t h, const std::vector<double>& v)
{
    const auto* b = reinterpret_cast<const unsigned char*>(v.data());
    for (std::size_t i = 0; i < v.size() * sizeof(double); ++i) {
        h = (h ^ b[i]) * 1099511628211ULL;
    }
    return h;
}

struct DragCase
{
    const char* name;
    int variant;  // 0 nearly, 1 fully, 2 loose, 3 decoupled
    int unit;     // -1 = last
    int which;    // 0 bottom.p2, 1 right.p2
    double dx, dy;
};
const DragCase kDrags[] = {
    {"D1 slide (nearly constrained, last unit bottom.p2)", 0, -1, 0, 0.10, 0.05},
    {"D2 resize (nearly constrained, u0 right.p2)", 0, 0, 1, 0.05, 0.03},
    {"D3 blocked (fully constrained, last unit bottom.p2)", 1, -1, 0, 0.10, 0.05},
    {"D4 loose (no distance dimensions, last unit bottom.p2)", 2, -1, 0, 0.10, 0.05},
    {"D5 decoupled (20 groups, last unit right.p2)", 3, -1, 1, 0.05, 0.03},
    {"D6 redundant (D1 plus a redundant horizontal on unit 3)", 4, -1, 0, 0.10, 0.05},
};
GCS::Point dragPoint(Model& m, const DragCase& d)
{
    const Unit& u = m.units[d.unit < 0 ? m.units.size() - 1 : std::size_t(d.unit)];
    return d.which == 0 ? m.lines[u.bottom].p2 : m.lines[u.right].p2;
}

enum Method
{
    FullPG,         // reference: GCS::System::solve(), as FreeCAD drags
    FullReplica,    // the same iteration run from our code over our own SubSystems, with timers (profile)
    FullSparse,     // same iteration with sparse linear algebra, full parameter set
    ReducedPG,      // GCS::System over free parameters and independent rows only; fallback FullPG
    ReducedSparse,  // the same reduced system, sparse linear algebra; fallback FullSparse
    kMethods
};
const char* kMethodName[kMethods] = {"full, PlaneGCS System::solve (reference)", "full, replica with timers",
                                     "full, sparse", "reduced, PlaneGCS System::solve + fallback",
                                     "reduced, sparse + fallback"};

int g_methods = 0x1F;
int g_steps = 100;
int g_reps = 3;
double g_rowTol = 1e-6;     // relative tolerance of the greedy row selection
double g_acceptTol = 1e-9;  // reduced result: max |constraint error| over the WHOLE sketch
double g_mismatchTol = 1e-9;
bool g_verbose = false;
bool g_keepEqual = true;    // --naive-rows: plain greedy selection, Equal rows not forced (first attempt)

// The reduced drag system for one drag session.
struct Reduced
{
    std::vector<double*> F;        // free parameters (getDependentParams), canonical order
    std::set<double*> Fset;
    int candidates = 0;            // constraints that touch a free parameter
    int dependentMergeRows = 0;    // Equal rows kept although numerically dependent
    std::unique_ptr<GCS::System> rs;
    std::vector<GCS::Constraint*> list;  // rs's constraint objects in add order (kept clones + 2 temp)
    std::unique_ptr<s11::Prepared> prep;
    bool pointFree = false;
    bool usable = false;           // structural check passed; otherwise every step takes the full path
    int components = 0, rsDofs = -1;
    bool rsRedundant = false, rsConflicting = false;
    double tDep = 0, tRows = 0, tSys = 0, tInit = 0, tPrep = 0;
};

std::unique_ptr<Reduced> makeReduced(Model& m, GCS::Point pt, GCS::Point target, const GCS::VEC_I& redTags)
{
    auto R = std::make_unique<Reduced>();
    auto t0 = Clock::now();
    GCS::VEC_pD raw;
    m.sys.getDependentParams(raw);
    R->Fset.insert(raw.begin(), raw.end());
    for (double* p : m.unknowns) {
        if (R->Fset.count(p)) {
            R->F.push_back(p);
        }
    }
    R->pointFree = R->Fset.count(pt.x) || R->Fset.count(pt.y);
    auto t1 = Clock::now();
    R->tDep = ms(t0, t1);
    // Greedy row selection in canonical constraint order: keep a constraint when its gradient over the free
    // parameters is not (numerically) a combination of the rows already kept.
    std::map<double*, int> col;
    for (int j = 0; j < int(R->F.size()); ++j) {
        col[R->F[j]] = j;
    }
    const int nf = int(R->F.size());
    Eigen::MatrixXd Qb(nf, std::max(nf, 1));
    int rank = 0;
    std::vector<char> keep(m.cons.size(), 0);
    // PlaneGCS eliminates an Equal constraint between two unknowns by merging the two variables, which changes
    // the metric of the QP step. The reduced system must merge exactly the same free variables, so those
    // Equal rows are always kept (pass 0); the other rows are then selected greedily (pass 1).
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t k = 0; k < m.cons.size(); ++k) {
            const Rec& r = m.cons[k];
            if (r.tag < 0 || std::find(redTags.begin(), redTags.end(), r.tag) != redTags.end()) {
                continue;
            }
            GCS::VEC_pD ps = r.c->params();
            const bool mergeRow = g_keepEqual && r.c->getTypeId() == GCS::Equal && col.count(ps[0]) && col.count(ps[1]);
            if ((pass == 0) != mergeRow) {
                continue;
            }
            std::sort(ps.begin(), ps.end());
            ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
            Eigen::VectorXd v = Eigen::VectorXd::Zero(nf);
            bool touches = false;
            for (double* p : ps) {
                auto it = col.find(p);
                if (it != col.end()) {
                    v[it->second] = r.c->grad(p);
                    touches = true;
                }
            }
            if (!touches) {
                continue;
            }
            ++R->candidates;
            const double nv = v.norm();
            Eigen::VectorXd w = v;
            for (int it2 = 0; it2 < 2 && rank > 0; ++it2) {
                w -= Qb.leftCols(rank) * (Qb.leftCols(rank).transpose() * w);
            }
            const double nw = w.norm();
            const bool independent = nv > 0 && rank < nf && nw > g_rowTol * nv;
            if (independent) {
                Qb.col(rank++) = w / nw;
            }
            if (independent || mergeRow) {
                keep[k] = 1;
                if (!independent) {
                    ++R->dependentMergeRows;
                }
            }
        }
    }
    std::vector<const Rec*> kept;  // canonical order
    for (std::size_t k = 0; k < m.cons.size(); ++k) {
        if (keep[k]) {
            kept.push_back(&m.cons[k]);
        }
    }
    auto t2 = Clock::now();
    R->tRows = ms(t1, t2);
    if (!R->pointFree || R->F.empty()) {
        return R;
    }
    R->rs = std::make_unique<GCS::System>();
    R->rs->debugMode = GCS::NoDebug;
    for (const Rec* r : kept) {
        GCS::Constraint* c = r->make();  // created at the drag-start state, like the full system's objects
        c->setTag(r->tag);
        c->setDriving(true);
        R->rs->addConstraint(c);
        R->list.push_back(c);
    }
    R->rs->declareUnknowns(R->F);
    auto t3 = Clock::now();
    R->tSys = ms(t2, t3);
    R->rsDofs = R->rs->diagnose();
    R->rsRedundant = R->rs->hasRedundant();
    R->rsConflicting = R->rs->hasConflicting();
    // The kept rows must be exactly independent: as many as free parameters minus the sketch's DOF, and the
    // reduced system's own diagnosis must agree (same DOF, nothing redundant or conflicting).
    const int fullDofs = m.sys.dofsNumber();
    R->usable = int(kept.size()) - R->dependentMergeRows == nf - fullDofs && R->dependentMergeRows == 0 &&
                R->rsDofs == fullDofs && !R->rsRedundant && !R->rsConflicting;
    GCS::Constraint* cx = new GCS::ConstraintEqual(target.x, pt.x);
    GCS::Constraint* cy = new GCS::ConstraintEqual(target.y, pt.y);
    for (GCS::Constraint* c : {cx, cy}) {
        c->setTag(GCS::DefaultTemporaryConstraint);
        c->setDriving(true);
        R->rs->addConstraint(c);
        R->list.push_back(c);
    }
    R->rs->initSolution();
    auto t4 = Clock::now();
    R->tInit = ms(t3, t4);
    R->prep = s11::prepare(R->F, R->list);
    R->components = int(R->prep->sub.size());
    R->tPrep = ms(t4, Clock::now());
    return R;
}

struct MethodResult
{
    Stats step;
    int mismatches = 0, fallbacks = 0, failed = 0, steps = 0;
    double maxDiff = 0;
    std::uint64_t hash = 1469598103934665603ULL;
    s11::SqpProfile prof;
};

struct CaseResult
{
    MethodResult mr[kMethods];
    Stats beginStateless, beginTemp, beginReduced, beginPrep;
    int nParams = 0, nCons = 0, nFree = 0, nCand = 0, nKept = 0, dofs = 0, rsDofs = 0, comps = 0, nMergeDep = 0;
    bool rsRed = false, rsConf = false, pointFree = false, usable = false;
    int replicaDiffers = 0;
};

void runCase(const DragCase& d, bool reinit, CaseResult& cr)
{
    Options vo;
    vo.nearly = d.variant == 0;
    vo.loose = d.variant == 2;
    vo.decoupled = d.variant == 3;
    vo.nearly = vo.nearly || d.variant == 4;
    vo.extra = d.variant == 4 ? 1 : 0;
    // Solved starting sketch, as the document would hold it.
    std::vector<double> solved;
    {
        Options o = vo;
        o.noise = 0.3;
        auto m0 = build(o);
        m0->sys.initSolution();
        if (ok(m0->sys.solve())) {
            m0->sys.applySolution();
        }
        solved = values(*m0);
    }
    for (int rep = 0; rep < g_reps; ++rep) {
        // beginDrag(const Sketch&): build the system from the sketch and diagnose it.
        Options o = vo;
        o.init = &solved;
        const auto tb0 = Clock::now();
        auto mp = build(o);
        Model& m = *mp;
        cr.dofs = m.sys.diagnose();
        const auto tb1 = Clock::now();
        cr.beginStateless.add(ms(tb0, tb1));
        cr.nParams = int(m.unknowns.size());
        cr.nCons = int(m.cons.size());

        GCS::Point pt = dragPoint(m, d);
        double tx = *pt.x, ty = *pt.y;
        const double x0 = tx, y0 = ty;
        GCS::Point target(&tx, &ty);
        const auto tt0 = Clock::now();
        m.coincident(target, pt, GCS::DefaultTemporaryConstraint);
        m.sys.initSolution();
        cr.beginTemp.add(ms(tt0, Clock::now()));
        const std::vector<double> start = values(m);

        // System::initSolution leaves out the constraint objects diagnose() found redundant. Only tags are public:
        // getRedundant() lists tags whose objects are ALL redundant, getPartiallyRedundant() tags where some are.
        // The replica leaves out every object of a fully redundant tag; partially redundant tags are not handled.
        GCS::VEC_I redTags, partTags;
        m.sys.getRedundant(redTags);
        m.sys.getPartiallyRedundant(partTags);
        std::vector<GCS::Constraint*> fullList;
        for (const Rec& r : m.cons) {
            if (std::find(redTags.begin(), redTags.end(), r.tag) == redTags.end()) {
                fullList.push_back(r.c);
            }
        }
        if (rep == 0 && (!redTags.empty() || !partTags.empty() || m.sys.hasConflicting())) {
            std::printf("  full system: %zu redundant tags (left out of the replica), %zu partially redundant, "
                        "conflicting %d\n", redTags.size(), partTags.size(), int(m.sys.hasConflicting()));
        }
        std::unique_ptr<Reduced> R;
        if (g_methods & ((1 << ReducedPG) | (1 << ReducedSparse))) {
            const auto tr0 = Clock::now();
            R = makeReduced(m, pt, target, redTags);
            cr.beginReduced.add(ms(tr0, Clock::now()) - R->tPrep);
            cr.beginPrep.add(R->tPrep);
            cr.nFree = int(R->F.size());
            cr.nCand = R->candidates;
            cr.nMergeDep = R->dependentMergeRows;
            cr.nKept = int(R->list.size() >= 2 ? R->list.size() - 2 : 0);
            cr.rsDofs = R->rsDofs;
            cr.rsRed = R->rsRedundant;
            cr.rsConf = R->rsConflicting;
            cr.comps = R->components;
            cr.pointFree = R->pointFree;
            cr.usable = R->usable;
            setValues(m, start);
        }
        const auto tp0 = Clock::now();
        auto fullPrep = s11::prepare(m.unknowns, fullList);
        if (rep == 0) {
            std::printf("  full prepare(): %.2f ms, %zu components\n", ms(tp0, Clock::now()), fullPrep->sub.size());
        }

        const auto ix = std::find(m.unknowns.begin(), m.unknowns.end(), pt.x) - m.unknowns.begin();
        const auto iy = std::find(m.unknowns.begin(), m.unknowns.end(), pt.y) - m.unknowns.begin();
        std::vector<double> state = start;
        std::vector<double> res[kMethods];
        for (int k = 1; k <= g_steps; ++k) {
            const double ntx = x0 + d.dx * k, nty = y0 + d.dy * k;
            for (int mth = 0; mth < kMethods; ++mth) {
                if (!(g_methods & (1 << mth)) && mth != FullPG) {
                    continue;
                }
                MethodResult& M = cr.mr[mth];
                setValues(m, state);
                tx = ntx;
                ty = nty;
                int r = GCS::Success;
                bool fellBack = false;
                const auto t0 = Clock::now();
                auto fullStep = [&](s11::LinAlg la, bool replica) {
                    if (!replica && la == s11::LinAlg::Dense) {
                        if (reinit) {
                            m.sys.initSolution();
                        }
                        r = m.sys.solve();
                        if (ok(r)) {
                            m.sys.applySolution();
                        }
                    }
                    else {
                        if (reinit) {
                            fullPrep->setReference();
                        }
                        r = s11::solvePrepared(*fullPrep, m.sys, la, &M.prof);
                        if (ok(r)) {
                            s11::applyPrepared(*fullPrep);
                        }
                    }
                };
                switch (mth) {
                case FullPG: fullStep(s11::LinAlg::Dense, false); break;
                case FullReplica: fullStep(s11::LinAlg::Dense, true); break;
                case FullSparse: fullStep(s11::LinAlg::Sparse, true); break;
                case ReducedPG:
                case ReducedSparse: {
                    if (R->rs && R->usable) {
                        if (mth == ReducedPG) {
                            if (reinit) {
                                R->rs->initSolution();
                            }
                            r = R->rs->solve();
                            if (ok(r)) {
                                R->rs->applySolution();
                            }
                        }
                        else {
                            if (reinit) {
                                R->prep->setReference();
                            }
                            r = s11::solvePrepared(*R->prep, *R->rs, s11::LinAlg::Sparse, &M.prof);
                            if (ok(r)) {
                                s11::applyPrepared(*R->prep);
                            }
                        }
                    }
                    // Accept the reduced result only if every constraint of the whole sketch holds and the point
                    // moved whenever it can move. Otherwise solve the full system.
                    bool accept = (!R->rs || R->usable) && ok(r) && maxErrAll(m) <= g_acceptTol;
                    if (accept && R->rs) {
                        const double sx = state[ix], sy = state[iy];
                        const bool wantMove = std::hypot(ntx - sx, nty - sy) > 1e-12;
                        const bool didMove = *pt.x != sx || *pt.y != sy;
                        accept = !wantMove || didMove;
                    }
                    if (!accept) {
                        fellBack = true;
                        setValues(m, state);
                        fullStep(mth == ReducedPG ? s11::LinAlg::Dense : s11::LinAlg::Sparse, mth == ReducedSparse);
                    }
                    break;
                }
                default: break;
                }
                const auto t1 = Clock::now();
                M.step.add(ms(t0, t1));
                M.steps++;
                M.failed += ok(r) ? 0 : 1;
                M.fallbacks += fellBack ? 1 : 0;
                res[mth] = values(m);
                M.hash = fnv(M.hash, res[mth]);
            }
            for (int mth = 0; mth < kMethods; ++mth) {
                if (!(g_methods & (1 << mth)) || mth == FullPG) {
                    continue;
                }
                double diff = 0;
                std::size_t at = 0;
                for (std::size_t i = 0; i < res[mth].size(); ++i) {
                    const double di = std::fabs(res[mth][i] - res[FullPG][i]);
                    if (di > diff) {
                        diff = di;
                        at = i;
                    }
                }
                if (g_verbose && diff > g_mismatchTol && R) {
                    double movedOutsideF = 0;
                    for (std::size_t i = 0; i < start.size(); ++i) {
                        if (!R->Fset.count(m.unknowns[i])) {
                            movedOutsideF = std::max(movedOutsideF, std::fabs(res[FullPG][i] - state[i]));
                        }
                    }
                    std::printf("    step %d: largest diff at parameter %zu (%s free set); full solve moved "
                                "parameters outside the free set by up to %.3g; drag error full %.3g, this %.3g\n",
                                k, at, R->Fset.count(m.unknowns[at]) ? "in" : "NOT in", movedOutsideF,
                                std::hypot(res[FullPG][ix] - ntx, res[FullPG][iy] - nty),
                                std::hypot(res[mth][ix] - ntx, res[mth][iy] - nty));
                }
                MethodResult& M = cr.mr[mth];
                M.maxDiff = std::max(M.maxDiff, diff);
                M.mismatches += diff > g_mismatchTol ? 1 : 0;
                if (mth == FullReplica && std::memcmp(res[mth].data(), res[FullPG].data(), res[mth].size() * 8) != 0) {
                    cr.replicaDiffers++;
                }
                if (g_verbose && diff > g_mismatchTol) {
                    std::printf("    step %d %s: max |diff| %.3g\n", k, kMethodName[mth], diff);
                }
            }
            state = reinit ? res[FullPG] : start;
        }
        setValues(m, start);
        m.sys.clearByTag(GCS::DefaultTemporaryConstraint);
    }
}

void printCase(const DragCase& d, bool reinit, CaseResult& cr)
{
    std::printf("\n-- rows %d (%d curves), %s, %s --\n", g_rows, kCols * g_rows * 10, d.name,
                reinit ? "re-init every step" : "FreeCAD default (solve from drag start)");
    std::printf("  %d parameters, %d constraint objects, DOF %d; free parameters %d, constraints touching them %d, "
                "rows kept %d (of which dependent Equal rows %d); reduced system: DOF %d, redundant %d, conflicting %d, components %d, "
                "dragged point free %d, reduced system usable %d\n",
                cr.nParams, cr.nCons, cr.dofs, cr.nFree, cr.nCand, cr.nKept, cr.nMergeDep, cr.rsDofs, int(cr.rsRed),
                int(cr.rsConf), cr.comps, int(cr.pointFree), int(cr.usable));
    std::printf("  begin: stateless build+diagnose median %.2f ms; add temp constraint + initSolution %.2f ms; "
                "reduced system setup %.2f ms; prepare(reduced) %.2f ms\n",
                cr.beginStateless.median(), cr.beginTemp.median(), cr.beginReduced.median(), cr.beginPrep.median());
    for (int mth = 0; mth < kMethods; ++mth) {
        if (!(g_methods & (1 << mth)) && mth != FullPG) {
            continue;
        }
        MethodResult& M = cr.mr[mth];
        std::printf("  STEP %-44s n=%-4d median %9.3f ms  p95 %9.3f  worst %9.3f | mismatch>%.0e: %d (max %.3g) | "
                    "fallbacks %d | failed %d | hash %016llx\n",
                    kMethodName[mth], M.steps, M.step.median(), M.step.pct(0.95), M.step.worst(), g_mismatchTol,
                    M.mismatches, M.maxDiff, M.fallbacks, M.failed, static_cast<unsigned long long>(M.hash));
    }
    if (g_methods & (1 << FullReplica)) {
        std::printf("  replica differs bitwise from System::solve in %d of %d steps\n", cr.replicaDiffers,
                    cr.mr[FullReplica].steps);
    }
    for (int mth : {FullReplica, FullSparse, ReducedSparse}) {
        if (!(g_methods & (1 << mth))) {
            continue;
        }
        const s11::SqpProfile& p = cr.mr[mth].prof;
        if (p.calls == 0) {
            continue;
        }
        const double c = double(p.calls);
        std::printf("  PROFILE %s: %lld SQP calls, %.2f iterations per call, xsize %d, csize %d; per call ms: "
                    "total %.3f = setup %.3f + gradB %.3f + jacA %.3f + resA %.3f + QR %.3f + formQ %.3f + "
                    "formY %.3f + nullspace %.3f + formK %.3f + analyseK %.3f + factorK %.3f + kktSolve %.3f + lambda %.3f + linesearch %.3f + "
                    "bfgs %.3f\n",
                    kMethodName[mth], p.calls, double(p.iters) / c, p.xsize, p.csize, p.total / c, p.setup / c,
                    p.gradB / c, p.jacA / c, p.resA / c, p.qr / c, p.formQ / c, p.formY / c, p.nullsp / c,
                    p.formK / c, p.analyse / c, p.factor / c, p.kktSolve / c, p.lambda / c, p.lineSearch / c, p.bfgs / c);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Detected on the core this thread runs on (P-core L1D 48 KiB, E-core 32 KiB on this CPU), then pinned:
    // adapter determinism rule from S7.
    std::printf("Eigen detected cache sizes before pinning: L1 %td L2 %td L3 %td\n", Eigen::l1CacheSize(),
                Eigen::l2CacheSize(), Eigen::l3CacheSize());
    Eigen::setCpuCacheSizes(32 * 1024, 1024 * 1024, 8 * 1024 * 1024);
    std::vector<int> cases = {0, 1, 2, 3};
    std::vector<int> rowsList = {1, 2, 3, 4};
    int modes = 3;  // bit 1 FreeCAD default, bit 2 re-init every step
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto list = [&](std::vector<int>& out) {
            out.clear();
            for (const char* s = argv[++i]; *s;) {
                out.push_back(std::atoi(s));
                while (*s && *s != ',') ++s;
                if (*s) ++s;
            }
        };
        if (a == "--rows" && i + 1 < argc) list(rowsList);
        else if (a == "--cases" && i + 1 < argc) list(cases);
        else if (a == "--methods" && i + 1 < argc) g_methods = std::strtol(argv[++i], nullptr, 0);
        else if (a == "--modes" && i + 1 < argc) modes = std::atoi(argv[++i]);
        else if (a == "--steps" && i + 1 < argc) g_steps = std::atoi(argv[++i]);
        else if (a == "--reps" && i + 1 < argc) g_reps = std::atoi(argv[++i]);
        else if (a == "--row-tol" && i + 1 < argc) g_rowTol = std::atof(argv[++i]);
        else if (a == "--accept-tol" && i + 1 < argc) g_acceptTol = std::atof(argv[++i]);
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--naive-rows") g_keepEqual = false;
    }
    std::printf("S11 sketch drag. Eigen %d.%d.%d, SIMD %s, cache sizes L1 %td L2 %td L3 %td (fixed). methods 0x%x, "
                "steps %d, reps %d, row tol %.1e, accept tol %.1e, mismatch tol %.1e\n",
                EIGEN_WORLD_VERSION, EIGEN_MAJOR_VERSION, EIGEN_MINOR_VERSION, Eigen::SimdInstructionSetsInUse(),
                Eigen::l1CacheSize(), Eigen::l2CacheSize(), Eigen::l3CacheSize(), g_methods, g_steps, g_reps,
                g_rowTol, g_acceptTol, g_mismatchTol);
    for (int rows : rowsList) {
        g_rows = rows;
        for (int ci : cases) {
            for (int mode = 0; mode < 2; ++mode) {
                if (!(modes & (1 << mode))) {
                    continue;
                }
                CaseResult cr;
                runCase(kDrags[ci], mode == 1, cr);
                printCase(kDrags[ci], mode == 1, cr);
            }
        }
    }
    return 0;
}
