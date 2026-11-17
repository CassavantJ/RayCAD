// Spike S7: PlaneGCS outside FreeCAD. Benchmark, determinism check and diagnosis demo.
// Throwaway code. See ../REPORT.md.
#include "GCS.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = std::numbers::pi;

// Own generator so the input is identical on every platform and standard library.
struct Lcg
{
    std::uint64_t s;
    double next()  // [0,1)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym(double amp) { return (next() * 2.0 - 1.0) * amp; }
};

int g_eigenThreads = 0;     // 0 = leave what GCS::System sets (1)
int g_qr = -1;              // -1 auto, 0 dense, 1 sparse
bool g_debugMinimal = false;
int g_gauss = -1;           // -1 default (FullPivLU), 1 LeastNormFullPivLU, 2 LeastNormLdlt
int g_alg = 2;              // full-solve algorithm: 0 BFGS, 1 LM, 2 DogLeg (default)
int g_rows = 4;             // units = 5 * rows; each unit is 6 lines + 4 arcs
int g_dragReps = 3;
int g_statelessSteps = 10;
int g_detSteps = 100;
bool g_noDrag = false;
int g_variants = 15;        // bit 1 nearly, 2 fully, 4 loose, 8 decoupled
int units() { return 5 * g_rows; }
bool freePitch(int i)
{
    for (int c : {units() - 1, 2 * units() / 3, units() / 3}) {
        if (i == (c % 5 == 0 ? c + 1 : c)) {
            return true;
        }
    }
    return false;
}

enum class ParamAlloc { Contiguous, ShuffledHeap };

struct Options
{
    bool nearly = true;            // leave 5 DOF free; false = fully constrained
    bool decoupled = false;        // every unit dimensioned and anchored on its own: 20 independent groups
    bool loose = false;            // no distance dimensions at all (many DOF, still one coupled system)
    double noise = 0.0;            // mm of pseudo-random offset on the starting geometry
    ParamAlloc alloc = ParamAlloc::Contiguous;
    std::uint64_t allocSeed = 0;   // address order of the parameters (ShuffledHeap)
    std::uint64_t heapNoiseSeed = 0;  // != 0: fragment the heap before constraints are created
    int extra = 0;                 // 1 = add a redundant constraint, 2 = add a conflicting one
    const std::vector<double>* init = nullptr;  // start values for the unknowns, creation order
};

struct Unit
{
    int bl, br, tr, tl;                    // arcs
    int bottom, right, top, left, d1, d2;  // lines
};

struct Model
{
    GCS::System sys;
    std::vector<double> store;         // contiguous parameter storage
    std::vector<double*> heapParams;   // individually allocated parameter storage
    std::size_t heapNext = 0;
    std::deque<double> consts;         // dimension values, not unknowns
    std::vector<double*> unknowns;     // creation order
    std::vector<GCS::Line> lines;
    std::vector<GCS::Arc> arcs;
    std::vector<Unit> units;
    std::vector<std::string> tagName;  // index = tag
    std::vector<void*> junk;
    Options opt;
    Lcg rng {12345};
    int userConstraints = 0;

    ~Model()
    {
        sys.clear();
        for (double* p : heapParams) {
            delete p;
        }
        for (void* p : junk) {
            std::free(p);
        }
    }

    double* P(double v)
    {
        const std::size_t idx = unknowns.size();
        if (opt.init) {
            v = (*opt.init)[idx];
        }
        double* p;
        if (opt.alloc == ParamAlloc::Contiguous) {
            store.push_back(v);  // capacity reserved up front, pointers stay valid
            p = &store.back();
        }
        else {
            p = heapParams[heapNext++];
            *p = v;
        }
        unknowns.push_back(p);
        return p;
    }
    double* C(double v)
    {
        consts.push_back(v);
        return &consts.back();
    }
    double n() { return opt.noise > 0 ? rng.sym(opt.noise) : 0.0; }
    int tag(const std::string& name)
    {
        tagName.push_back(name);
        ++userConstraints;
        return int(tagName.size()) - 1;
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
    // Same recipe as FreeCAD Sketch::addAngleAtPointConstraint for an endpoint tangent:
    // coincident endpoints plus an angle-via-point constraint snapped to 0 or pi.
    void tangentAtEnd(GCS::Line& l, GCS::Point& lp, GCS::Arc& a, GCS::Point& ap, const std::string& nm)
    {
        const int t = tag(nm);
        double ang = sys.calculateAngleViaPoint(l, a, lp);
        if (ang > kPi) {
            ang -= 2 * kPi;
        }
        if (ang < -kPi) {
            ang += 2 * kPi;
        }
        double* want = C(std::fabs(ang) > kPi / 2 ? kPi : 0.0);
        sys.addConstraintP2PCoincident(lp, ap, t);
        sys.addConstraintAngleViaPoint(l, a, lp, want, t);
    }
};

constexpr int kCols = 5;                     // default 20 units x (6 lines + 4 arcs) = 200 entities
#define kRows g_rows
constexpr double kW = 30, kH = 20, kR = 4, kPitchX = 40, kPitchY = 30;

void fragmentHeap(Model& m, std::uint64_t seed)
{
    Lcg r {seed};
    std::vector<void*> blocks;
    for (int i = 0; i < 4000; ++i) {
        blocks.push_back(std::malloc(16 + std::size_t(r.next() * 400)));
    }
    for (void*& b : blocks) {
        if (r.next() < 0.5) {
            std::free(b);
        }
        else {
            m.junk.push_back(b);
        }
    }
}

std::unique_ptr<Model> build(const Options& opt)
{
    auto mp = std::make_unique<Model>();
    Model& m = *mp;
    m.opt = opt;
    m.tagName.push_back("(tag 0: arc rules)");
    const std::size_t nParams = std::size_t(kCols * kRows) * (6 * 4 + 4 * 9);
    if (opt.alloc == ParamAlloc::Contiguous) {
        m.store.reserve(nParams);
    }
    else {
        for (std::size_t i = 0; i < nParams; ++i) {
            m.heapParams.push_back(new double(0));
        }
        std::vector<double*> order = m.heapParams;
        Lcg r {opt.allocSeed * 7919 + 1};
        for (std::size_t i = order.size() - 1; i > 0; --i) {
            std::swap(order[i], order[std::size_t(r.next() * double(i + 1))]);
        }
        m.heapParams.swap(order);  // hand-out order is shuffled; ownership is the same set
    }
    m.lines.reserve(kCols * kRows * 6);
    m.arcs.reserve(kCols * kRows * 4);

    // geometry
    for (int row = 0; row < kRows; ++row) {
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
    if (opt.heapNoiseSeed) {
        fragmentHeap(m, opt.heapNoiseSeed);
    }

    // constraints
    auto& S = m.sys;
    auto& L = m.lines;
    auto& A = m.arcs;
    for (int i = 0; i < int(m.units.size()); ++i) {
        const Unit& u = m.units[i];
        const int row = i / kCols, col = i % kCols;
        const std::string U = "u" + std::to_string(i) + " ";
        for (int a : {u.bl, u.br, u.tr, u.tl}) {
            S.addConstraintArcRules(A[a], 0);
        }
        m.tangentAtEnd(L[u.bottom], L[u.bottom].p1, A[u.bl], A[u.bl].end, U + "tangent bottom/BL");
        m.tangentAtEnd(L[u.bottom], L[u.bottom].p2, A[u.br], A[u.br].start, U + "tangent bottom/BR");
        m.tangentAtEnd(L[u.right], L[u.right].p1, A[u.br], A[u.br].end, U + "tangent right/BR");
        m.tangentAtEnd(L[u.right], L[u.right].p2, A[u.tr], A[u.tr].start, U + "tangent right/TR");
        m.tangentAtEnd(L[u.top], L[u.top].p1, A[u.tr], A[u.tr].end, U + "tangent top/TR");
        m.tangentAtEnd(L[u.top], L[u.top].p2, A[u.tl], A[u.tl].start, U + "tangent top/TL");
        m.tangentAtEnd(L[u.left], L[u.left].p1, A[u.tl], A[u.tl].end, U + "tangent left/TL");
        m.tangentAtEnd(L[u.left], L[u.left].p2, A[u.bl], A[u.bl].start, U + "tangent left/BL");
        S.addConstraintP2PCoincident(L[u.d1].p1, A[u.bl].center, m.tag(U + "coincident d1/BL centre"));
        S.addConstraintP2PCoincident(L[u.d1].p2, A[u.tr].center, m.tag(U + "coincident d1/TR centre"));
        S.addConstraintP2PCoincident(L[u.d2].p1, A[u.br].center, m.tag(U + "coincident d2/BR centre"));
        S.addConstraintP2PCoincident(L[u.d2].p2, A[u.tl].center, m.tag(U + "coincident d2/TL centre"));
        S.addConstraintHorizontal(L[u.bottom], m.tag(U + "horizontal bottom"));
        S.addConstraintVertical(L[u.left], m.tag(U + "vertical left"));
        S.addConstraintParallel(L[u.top], L[u.bottom], m.tag(U + "parallel top/bottom"));
        S.addConstraintParallel(L[u.right], L[u.left], m.tag(U + "parallel right/left"));
        S.addConstraintEqualRadius(A[u.br], A[u.bl], m.tag(U + "equal radius BR/BL"));
        S.addConstraintEqualRadius(A[u.tr], A[u.bl], m.tag(U + "equal radius TR/BL"));
        S.addConstraintEqualRadius(A[u.tl], A[u.bl], m.tag(U + "equal radius TL/BL"));
        const Unit* prev = i > 0 ? &m.units[i - 1] : nullptr;
        const bool dec = opt.decoupled, last = i == int(m.units.size()) - 1;
        if (i % 4 == 0 || dec) {
            S.addConstraintArcRadius(A[u.bl], m.C(kR), m.tag(U + "radius BL"));
        }
        else {
            S.addConstraintEqualRadius(A[u.bl], A[prev->bl], m.tag(U + "equal radius BL/prev BL"));
        }
        if (dec) {
            if (!last) {  // the last unit keeps one free dimension so it can be dragged
                S.addConstraintP2PDistance(L[u.bottom].p1, L[u.bottom].p2, m.C(kW - 2 * kR),
                                           m.tag(U + "distance bottom length"));
            }
            S.addConstraintP2PDistance(L[u.left].p1, L[u.left].p2, m.C(kH - 2 * kR),
                                       m.tag(U + "distance left length"));
        }
        else if (i % 2 == 0) {
            if (!(opt.nearly && i == 0) && !opt.loose) {
                S.addConstraintP2PDistance(L[u.bottom].p1, L[u.bottom].p2, m.C(kW - 2 * kR),
                                           m.tag(U + "distance bottom length"));
                S.addConstraintP2PDistance(L[u.left].p1, L[u.left].p2, m.C(kH - 2 * kR),
                                           m.tag(U + "distance left length"));
            }
        }
        else {
            S.addConstraintEqualLength(L[u.bottom], L[prev->bottom], m.tag(U + "equal length bottom/prev"));
            S.addConstraintEqualLength(L[u.left], L[prev->left], m.tag(U + "equal length left/prev"));
        }
        if (i == 0 || dec) {
            S.addConstraintCoordinateX(A[u.bl].center, m.C(col * kPitchX + kR), m.tag(U + "fix x"));
            S.addConstraintCoordinateY(A[u.bl].center, m.C(row * kPitchY + kR), m.tag(U + "fix y"));
        }
        else if (col > 0) {
            S.addConstraintHorizontal(A[u.bl].center, A[prev->bl].center, m.tag(U + "horizontal centres"));
            if (!(opt.nearly && freePitch(i)) && !opt.loose) {
                S.addConstraintP2PDistance(A[u.bl].center, A[prev->bl].center, m.C(kPitchX),
                                           m.tag(U + "distance pitch x"));
            }
        }
        else {
            const Unit& below = m.units[i - kCols];
            S.addConstraintVertical(A[u.bl].center, A[below.bl].center, m.tag(U + "vertical centres"));
            if (!opt.loose) {
                S.addConstraintP2PDistance(A[u.bl].center, A[below.bl].center, m.C(kPitchY),
                                           m.tag(U + "distance pitch y"));
            }
        }
        (void)row;
    }
    if (opt.extra == 1) {  // top of unit 3 is already parallel to a horizontal line
        S.addConstraintHorizontal(L[m.units[3].top], m.tag("EXTRA horizontal top of u3 (redundant)"));
    }
    if (opt.extra == 2) {  // u0 BL already has radius 4
        S.addConstraintArcRadius(A[m.units[0].bl], m.C(kR + 1), m.tag("EXTRA radius 5 on u0 BL (conflicting)"));
    }

    S.debugMode = g_debugMinimal ? GCS::Minimal : GCS::NoDebug;
    if (g_qr >= 0) {
        S.autoChooseAlgorithm = false;
        S.qrAlgorithm = g_qr == 0 ? GCS::EigenDenseQR : GCS::EigenSparseQR;
    }
    if (g_gauss >= 0) {
        S.dogLegGaussStep = GCS::DogLegGaussStep(g_gauss);
    }
    if (g_eigenThreads > 0) {
        Eigen::setNbThreads(g_eigenThreads);  // GCS::System() set it to 1; override for the test
    }
    S.declareUnknowns(m.unknowns);
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

double maxError(Model& m)
{
    double e = 0;
    for (int t = 0; t < int(m.tagName.size()); ++t) {
        const double x = m.sys.calculateConstraintErrorByTag(t);
        if (!std::isnan(x)) {
            e = std::max(e, std::fabs(x));
        }
    }
    return e;
}

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

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
    void print(const char* name)
    {
        std::printf("  %-52s n=%-5zu median %8.3f ms   p95 %8.3f ms   worst %8.3f ms\n", name, v.size(),
                    median(), pct(0.95), worst());
    }
};

bool ok(int res) { return res == GCS::Success || res == GCS::Converged; }

// Drag as FreeCAD Sketch::initMove / movePoint do it: a temporary coincident constraint with
// tag -1 between the dragged point and a target point that is not an unknown.
struct Drag
{
    Model& m;
    GCS::Point pt;
    double tx, ty, x0, y0;
    GCS::Point target;
    Drag(Model& model, GCS::Point p)
        : m(model), pt(p), tx(*p.x), ty(*p.y), x0(*p.x), y0(*p.y), target(&tx, &ty)
    {
        m.sys.addConstraintP2PCoincident(target, pt, GCS::DefaultTemporaryConstraint);
        m.sys.initSolution();
    }
    int to(double x, double y, bool reinit)
    {
        tx = x;
        ty = y;
        const int res = m.sys.solve();
        if (ok(res)) {
            m.sys.applySolution();
            if (reinit) {
                m.sys.initSolution();  // like FreeCAD "RecalculateInitialSolutionWhileMovingPoint"
            }
        }
        return res;
    }
    ~Drag()
    {
        m.sys.clearByTag(GCS::DefaultTemporaryConstraint);
        m.sys.initSolution();
    }
};

struct DragCase
{
    const char* name;
    int variant;  // 0 nearly constrained, 1 fully constrained, 2 loose
    int unit;   // -1 = last unit
    int which;  // 0 = bottom line p2, 1 = right line p2
    double dx, dy;  // per step, mm
};
const DragCase kDrags[] = {
    {"D1 slide: last unit bottom.p2, +0.10/+0.05 mm per step", 0, -1, 0, 0.10, 0.05},
    {"D2 resize: u0 right.p2, +0.05/+0.03 mm per step", 0, 0, 1, 0.05, 0.03},
    {"D3 blocked: fully constrained, last unit bottom.p2", 1, -1, 0, 0.10, 0.05},
    {"D4 loose: no distance dimensions, last unit bottom.p2", 2, -1, 0, 0.10, 0.05},
    {"D5 decoupled: resize last unit, right.p2, +0.05/+0.03 mm per step", 3, -1, 1, 0.05, 0.03},
};
GCS::Point dragPoint(Model& m, const DragCase& d)
{
    const Unit& u = m.units[d.unit < 0 ? m.units.size() - 1 : std::size_t(d.unit)];
    return d.which == 0 ? m.lines[u.bottom].p2 : m.lines[u.right].p2;
}

struct SolveOut
{
    int res = -1, dofs = -1;
    double tBuild = 0, tInit = 0, tSolve = 0;
};
std::unique_ptr<Model> buildAndSolve(const Options& o, SolveOut& out)
{
    const auto t0 = Clock::now();
    auto m = build(o);
    const auto t1 = Clock::now();
    m->sys.initSolution();  // includes diagnose()
    const auto t2 = Clock::now();
    out.dofs = m->sys.dofsNumber();
    out.res = m->sys.solve(true, GCS::Algorithm(g_alg));
    if (ok(out.res)) {
        m->sys.applySolution();
    }
    const auto t3 = Clock::now();
    out.tBuild = ms(t0, t1);
    out.tInit = ms(t1, t2);
    out.tSolve = ms(t2, t3);
    return m;
}

// One complete scenario; returns every solved coordinate that was produced along the way.
std::vector<double> scenario(Options base, int* failures = nullptr)
{
    std::vector<double> all;
    auto append = [&](const Model& m) {
        auto v = values(m);
        all.insert(all.end(), v.begin(), v.end());
    };
    for (bool nearly : {true, false}) {
        Options o = base;
        o.nearly = nearly;
        o.noise = 0.3;
        SolveOut so;
        auto m = buildAndSolve(o, so);
        if (!ok(so.res) && failures) {
            ++*failures;
        }
        append(*m);
        for (const DragCase& d : kDrags) {
            if (d.variant != (nearly ? 0 : 1)) {
                continue;
            }
            for (bool reinit : {false, true}) {
                const auto start = values(*m);
                {
                    Drag drag(*m, dragPoint(*m, d));
                    for (int k = 1; k <= g_detSteps; ++k) {
                        const int r = drag.to(drag.x0 + d.dx * k, drag.y0 + d.dy * k, reinit);
                        if (!ok(r) && failures) {
                            ++*failures;
                        }
                        if (k % 10 == 0 || k == g_detSteps) {
                            append(*m);
                        }
                    }
                }
                for (std::size_t i = 0; i < start.size(); ++i) {
                    *m->unknowns[i] = start[i];  // rewind for the next drag
                }
                m->sys.initSolution();
            }
        }
    }
    return all;
}

std::uint64_t fnv(const std::vector<double>& v)
{
    std::uint64_t h = 1469598103934665603ULL;
    const auto* b = reinterpret_cast<const unsigned char*>(v.data());
    for (std::size_t i = 0; i < v.size() * sizeof(double); ++i) {
        h = (h ^ b[i]) * 1099511628211ULL;
    }
    return h;
}

struct Cmp
{
    int differing = 0;
    double maxAbs = 0;
};
void compare(const std::vector<double>& ref, const std::vector<double>& v, Cmp& c)
{
    if (ref.size() == v.size() && std::memcmp(ref.data(), v.data(), v.size() * sizeof(double)) == 0) {
        return;
    }
    ++c.differing;
    for (std::size_t i = 0; i < std::min(ref.size(), v.size()); ++i) {
        c.maxAbs = std::max(c.maxAbs, std::fabs(ref[i] - v[i]));
    }
}

void printTags(Model& m, const char* what, const GCS::VEC_I& tags)
{
    std::printf("    %s: %zu\n", what, tags.size());
    for (int t : tags) {
        std::printf("      tag %d  %s\n", t, t >= 0 && t < int(m.tagName.size()) ? m.tagName[t].c_str() : "?");
    }
}

void diagnosisDemo()
{
    std::printf("\n== Diagnosis API ==\n");
    struct Case { const char* name; bool nearly; int extra; };
    for (Case c : {Case {"fully constrained", false, 0}, Case {"nearly (5 dimensions left out)", true, 0},
                   Case {"fully constrained + redundant constraint", false, 1},
                   Case {"fully constrained + conflicting constraint", false, 2}}) {
        Options o;
        o.nearly = c.nearly;
        o.extra = c.extra;
        auto m = build(o);
        const auto t0 = Clock::now();
        const int dofs = m->sys.diagnose();
        const auto t1 = Clock::now();
        GCS::VEC_I conf, red, pred;
        m->sys.getConflicting(conf);
        m->sys.getRedundant(red);
        m->sys.getPartiallyRedundant(pred);
        GCS::VEC_pD dep;
        m->sys.getDependentParams(dep);
        std::vector<std::vector<double*>> groups;
        m->sys.getDependentParamsGroups(groups);
        const std::size_t rawCount = dep.size();
        std::sort(dep.begin(), dep.end());
        dep.erase(std::unique(dep.begin(), dep.end()), dep.end());
        std::printf("  %s: diagnose() = %d DOF, %.2f ms, getDependentParams %zu entries (%zu distinct "
                    "parameters), %zu groups\n",
                    c.name, dofs, ms(t0, t1), rawCount, dep.size(), groups.size());
        printTags(*m, "conflicting", conf);
        printTags(*m, "redundant", red);
        printTags(*m, "partially redundant", pred);
        m->sys.initSolution();
        const int res = m->sys.solve();
        std::printf("    solve() after diagnosis = %d (0 Success, 1 Converged, 2 Failed)\n", res);
    }
}

void timing(int runs)
{
    std::printf("\n== Timing (%d full-solve runs, %d x 100 drag steps) ==\n", runs, g_dragReps);
    const char* variantName[] = {"NEARLY constrained", "FULLY constrained", "LOOSE (no distance dimensions)",
                                 "DECOUPLED (20 independent groups, 1 DOF)"};
    for (int variant = 0; variant < 4; ++variant) {
        if (!(g_variants & (1 << variant))) {
            continue;
        }
        Options vo;
        vo.nearly = variant == 0;
        vo.loose = variant == 2;
        vo.decoupled = variant == 3;
        Stats sBuild, sInit, sSolve, sTotal, sDiag;
        std::vector<double> solved;
        int dofs = -1, res = -1, entities = 0, params = 0, cons = 0, arcs = 0;
        double err = 0;
        for (int i = 0; i < runs; ++i) {
            Options o = vo;
            o.noise = 0.3;
            SolveOut so;
            auto m = buildAndSolve(o, so);
            sBuild.add(so.tBuild);
            sInit.add(so.tInit);
            sSolve.add(so.tSolve);
            sTotal.add(so.tBuild + so.tInit + so.tSolve);
            dofs = so.dofs;
            res = so.res;
            if (i == 0) {
                entities = int(m->lines.size() + m->arcs.size());
                arcs = int(m->arcs.size());
                params = int(m->unknowns.size());
                cons = m->userConstraints;
                err = maxError(*m);
                solved = values(*m);
            }
            const auto t0 = Clock::now();
            m->sys.diagnose();
            sDiag.add(ms(t0, Clock::now()));
        }
        std::printf("\n-- %s sketch: %d entities, %d parameters, %d constraints (+%d arc rules), DOF %d, "
                    "solve result %d, max constraint error %.3g --\n",
                    variantName[variant], entities, params, cons, arcs, dofs, res, err);
        if (variant == 1) {  // distance from the noise-free design, point coordinates and radii only
            Options ideal;
            ideal.nearly = false;
            auto mi = build(ideal);
            double d = 0;
            for (std::size_t i = 0; i < solved.size(); ++i) {
                const double x = std::fabs(solved[i] - *mi->unknowns[i]);
                if (x < 1.0) {  // skips angles that differ by 2*pi
                    d = std::max(d, x);
                }
            }
            std::printf("  max |solved - intended design| = %.3g mm\n", d);
        }
        sBuild.print("build system (add constraints, declare unknowns)");
        sInit.print("initSolution (diagnose + partition)");
        sSolve.print("solve + applySolution (from 0.3 mm noise)");
        sTotal.print("FULL SOLVE total");
        sDiag.print("diagnose() alone, repeated");

        for (const DragCase& d : kDrags) {
            if (d.variant != variant || g_noDrag) {
                continue;
            }
            Options o = vo;
            o.noise = 0.3;
            SolveOut so;
            auto m = buildAndSolve(o, so);
            const auto start = values(*m);
            std::vector<double> endDefault;
            // mode 0: FreeCAD default, 1: re-init every step, 2: only the not-yet-determined
            // parameters (System::getDependentParams) declared as unknowns for the drag
            for (int mode = 0; mode < 3; ++mode) {
                const char* modeName[] = {"FreeCAD default (solve from drag start)", "re-init every step",
                                          "REDUCED unknowns (getDependentParams only)"};
                if (mode == 2) {
                    GCS::VEC_pD raw, dep;
                    m->sys.getDependentParams(raw);  // has duplicates: one entry per DOF group
                    for (double* q : m->unknowns) {  // keep creation order, drop duplicates
                        if (std::find(raw.begin(), raw.end(), q) != raw.end()) {
                            dep.push_back(q);
                        }
                    }
                    std::printf("  %s, %s: %zu of %zu parameters are unknowns\n", d.name, modeName[mode], dep.size(),
                                m->unknowns.size());
                    int movedTotal = 0, movedMissing = 0;
                    for (std::size_t i = 0; i < start.size(); ++i) {
                        if (std::fabs(endDefault[i] - start[i]) > 1e-9) {
                            ++movedTotal;
                            movedMissing += std::find(dep.begin(), dep.end(), m->unknowns[i]) == dep.end() ? 1 : 0;
                        }
                    }
                    std::printf("    parameters that moved in default mode: %d, of those missing from "
                                "getDependentParams: %d\n", movedTotal, movedMissing);
                    if (dep.empty()) {
                        continue;
                    }
                    m->sys.declareUnknowns(dep);
                }
                Stats sStep, sBegin;
                int fail = 0;
                double moved = 0, err2 = 0, diff = 0;
                for (int rep = 0; rep < g_dragReps; ++rep) {
                    {
                        const auto tb = Clock::now();
                        Drag drag(*m, dragPoint(*m, d));
                        sBegin.add(ms(tb, Clock::now()));
                        for (int k = 1; k <= 100; ++k) {
                            const auto t0 = Clock::now();
                            const int r = drag.to(drag.x0 + d.dx * k, drag.y0 + d.dy * k, mode == 1);
                            sStep.add(ms(t0, Clock::now()));
                            fail += ok(r) ? 0 : 1;
                        }
                        moved = std::hypot(*drag.pt.x - drag.x0, *drag.pt.y - drag.y0);
                    }
                    err2 = maxError(*m);
                    const auto end = values(*m);
                    if (mode == 0) {
                        endDefault = end;
                    }
                    for (std::size_t i = 0; i < end.size(); ++i) {
                        diff = std::max(diff, std::fabs(end[i] - endDefault[i]));
                        *m->unknowns[i] = start[i];
                    }
                }
                if (mode == 2) {
                    m->sys.declareUnknowns(m->unknowns);
                }
                m->sys.initSolution();
                std::printf("  %s, %s: failed steps %d of %zu, point moved %.3f mm, max constraint error after %.3g, "
                            "max |end - end of default mode| %.3g\n",
                            d.name, modeName[mode], fail, sStep.v.size(), moved, err2, diff);
                sStep.print("  DRAG STEP");
                sBegin.print("  drag begin (add temp constraint + initSolution)");
            }
            // Stateless step, the shape of ISketchSolver::drag(const Sketch&, point, target):
            // rebuild the whole system from coordinates, diagnose, solve one step.
            Stats sStateless;
            std::vector<double> cur = start;
            const double sx = *dragPoint(*m, d).x, sy = *dragPoint(*m, d).y;
            for (int k = 1; k <= g_statelessSteps; ++k) {
                const auto t0 = Clock::now();
                Options o2 = vo;
                o2.init = &cur;
                auto m2 = build(o2);
                GCS::Point p = dragPoint(*m2, d);
                double tx = sx + d.dx * k, ty = sy + d.dy * k;
                GCS::Point target(&tx, &ty);
                m2->sys.addConstraintP2PCoincident(target, p, GCS::DefaultTemporaryConstraint);
                m2->sys.initSolution();
                if (ok(m2->sys.solve())) {
                    m2->sys.applySolution();
                }
                cur = values(*m2);
                m2.reset();
                sStateless.add(ms(t0, Clock::now()));
            }
            sStateless.print("  STATELESS DRAG STEP (rebuild + diagnose + solve)");
        }
    }
}

void determinism(int runs, int mode)
{
    std::printf("\n== Determinism (%d runs per row; each run = 2 full solves + 6 drags of %d steps) ==\n", runs, g_detSteps);
    int failures = 0;
    const std::vector<double> ref = scenario(Options {}, &failures);
    std::printf("  reference: %zu doubles, non-converged solves %d, FNV-1a hash %016llx\n", ref.size(), failures,
                static_cast<unsigned long long>(fnv(ref)));
    Cmp same, shuffled, heap;
    for (int i = 0; i < (mode & 1 ? runs : 0); ++i) {
        compare(ref, scenario(Options {}), same);
    }
    std::printf("  identical input, contiguous parameters:        %d of %d runs differ (max |diff| %.3g)\n",
                same.differing, mode & 1 ? runs : 0, same.maxAbs);
    for (int i = 0; i < (mode & 2 ? runs : 0); ++i) {
        Options o;
        o.heapNoiseSeed = 1000 + i;
        compare(ref, scenario(o), heap);
    }
    std::printf("  heap fragmented before constraints are created: %d of %d runs differ (max |diff| %.3g)\n",
                heap.differing, mode & 2 ? runs : 0, heap.maxAbs);
    for (int i = 0; i < (mode & 4 ? runs : 0); ++i) {
        Options o;
        o.alloc = ParamAlloc::ShuffledHeap;
        o.allocSeed = i;
        compare(ref, scenario(o), shuffled);
    }
    std::printf("  parameters at shuffled heap addresses:          %d of %d runs differ (max |diff| %.3g)\n",
                shuffled.differing, mode & 4 ? runs : 0, shuffled.maxAbs);
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int runs = 100;
    bool doTiming = true, doDet = true, doDiag = true;
    int detMode = 7;
    bool fixCache = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--eigen-threads" && i + 1 < argc) {
            g_eigenThreads = std::atoi(argv[++i]);
        }
        else if (a == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        }
        else if (a == "--qr" && i + 1 < argc) {
            g_qr = std::string(argv[++i]) == "dense" ? 0 : 1;
        }
        else if (a == "--rows" && i + 1 < argc) {
            g_rows = std::atoi(argv[++i]);
        }
        else if (a == "--gauss" && i + 1 < argc) {
            g_gauss = std::atoi(argv[++i]);
        }
        else if (a == "--alg" && i + 1 < argc) {
            g_alg = std::atoi(argv[++i]);
        }
        else if (a == "--drag-reps" && i + 1 < argc) {
            g_dragReps = std::atoi(argv[++i]);
        }
        else if (a == "--stateless-steps" && i + 1 < argc) {
            g_statelessSteps = std::atoi(argv[++i]);
        }
        else if (a == "--det-steps" && i + 1 < argc) {
            g_detSteps = std::atoi(argv[++i]);
        }
        else if (a == "--det-mode" && i + 1 < argc) {
            detMode = std::atoi(argv[++i]);  // bit 1 same input, 2 heap fragmentation, 4 shuffled addresses
        }
        else if (a == "--variants" && i + 1 < argc) {
            g_variants = std::atoi(argv[++i]);
        }
        else if (a == "--fix-cache") {
            // Eigen picks matrix-product block sizes from the CPU cache sizes it detects at first use.
            Eigen::setCpuCacheSizes(32 * 1024, 1024 * 1024, 8 * 1024 * 1024);
            fixCache = true;
        }
        else if (a == "--no-drag") {
            g_noDrag = true;
        }
        else if (a == "--no-diag") {
            doDiag = false;
        }
        else if (a == "--debug-minimal") {
            g_debugMinimal = true;
        }
        else if (a == "--hash-only") {
            doTiming = doDiag = false;
            runs = 0;
        }
        else if (a == "--no-timing") {
            doTiming = false;
        }
        else if (a == "--no-determinism") {
            doDet = false;
        }
    }
    std::printf("PlaneGCS spike S7. Eigen %d.%d.%d, OpenMP in Eigen: %s, SIMD: %s, eigen threads requested: %d, QR: %s, rows %d, gauss %d, alg %d\n",
                EIGEN_WORLD_VERSION, EIGEN_MAJOR_VERSION, EIGEN_MINOR_VERSION,
#ifdef EIGEN_HAS_OPENMP
                "yes",
#else
                "no",
#endif
                Eigen::SimdInstructionSetsInUse(), g_eigenThreads, g_qr < 0 ? "auto" : g_qr == 0 ? "dense" : "sparse", g_rows, g_gauss, g_alg);
    std::printf("Eigen cache sizes used: L1 %td, L2 %td, L3 %td bytes%s\n", Eigen::l1CacheSize(), Eigen::l2CacheSize(),
                Eigen::l3CacheSize(), fixCache ? " (fixed by --fix-cache)" : " (detected)");
    if (doDiag) {
        diagnosisDemo();
    }
    if (doTiming) {
        timing(runs);
    }
    if (doDet) {
        determinism(runs, detMode);
    }
    {
        Options o;
        auto m = build(o);
        std::printf("\nEigen::nbThreads() at exit: %d\n", Eigen::nbThreads());
    }
    return 0;
}
