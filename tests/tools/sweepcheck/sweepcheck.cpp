// The S5 sweep checker as a library (sweepcheck.h). The model and the numbers are spikes/s5-adaptive/src/check.cpp's;
// what changed: any tool diameter (S5 fixed 12.7 mm), an already-cleared area, cuts left out of the statistics, the
// 1.25x fraction, a bucket grid for the analytic gouge (S5 measured every step against every wall edge), and a report
// struct instead of printed text.
#include "sweepcheck.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>

namespace ray::test::sweep {
namespace {

// Cell states.
constexpr std::uint8_t kAir = 0, kPocket = 1, kKeep = 2, kGouged = 3, kRemoved = 4;

struct Grid {
    int W = 0, H = 0;
    double ox = 0, oy = 0, cell = kDefaultCell;
    std::vector<std::uint8_t> v;
    int ix(double x) const { return static_cast<int>(std::floor((x - ox) / cell)); }
    int iy(double y) const { return static_cast<int>(std::floor((y - oy) / cell)); }
    bool material(double x, double y) const
    {
        int i = ix(x), j = iy(y);
        if (i < 0 || j < 0 || i >= W || j >= H) {
            return false;
        }
        std::uint8_t c = v[static_cast<std::size_t>(j) * W + i];
        return c == kPocket || c == kKeep;
    }
};

// Even-odd scanline fill of loops into a 0/1 mask (cell centres).
std::vector<std::uint8_t> fillMask(const Grid& g, const Loops& loops)
{
    std::vector<std::uint8_t> m(static_cast<std::size_t>(g.W) * g.H, 0);
    std::vector<double> xs;
    for (int j = 0; j < g.H; j++) {
        double y = g.oy + (j + 0.5) * g.cell;
        xs.clear();
        for (const Loop& l : loops) {
            std::size_t n = l.size();
            for (std::size_t k = 0; k < n; k++) {
                Pt a = l[k], b = l[(k + 1) % n];
                if ((a.second <= y) != (b.second <= y)) {
                    xs.push_back(a.first + (y - a.second) * (b.first - a.first) / (b.second - a.second));
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            int i0 = static_cast<int>(std::ceil((xs[k] - g.ox) / g.cell - 0.5));
            int i1 = static_cast<int>(std::floor((xs[k + 1] - g.ox) / g.cell - 0.5));
            i0 = std::max(i0, 0);
            i1 = std::min(i1, g.W - 1);
            for (int i = i0; i <= i1; i++) {
                m[static_cast<std::size_t>(j) * g.W + i] = 1;
            }
        }
    }
    return m;
}

// Exact Euclidean distance transform (Felzenszwalb-Huttenlocher): distance in cells to the nearest feature cell.
void dt1d(const double* f, int n, double* d, int* v, double* z)
{
    const double inf = 1e20;
    auto isect = [&](int q, int p) {
        return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p);
    };
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; q++) {
        double s = isect(q, v[k]);
        while (k > 0 && s <= z[k]) {
            k--;
            s = isect(q, v[k]);
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) {
            k++;
        }
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}
std::vector<float> edt(const std::vector<std::uint8_t>& feat, int W, int H)
{
    const double inf = 1e20;
    std::vector<float> d(static_cast<std::size_t>(W) * H);
    int n = std::max(W, H);
    std::vector<double> f(n), o(n), z(n + 1);
    std::vector<int> v(n);
    std::vector<double> tmp(static_cast<std::size_t>(W) * H);
    for (int i = 0; i < W; i++) {
        for (int j = 0; j < H; j++) {
            f[j] = feat[static_cast<std::size_t>(j) * W + i] ? 0.0 : inf;
        }
        dt1d(f.data(), H, o.data(), v.data(), z.data());
        for (int j = 0; j < H; j++) {
            tmp[static_cast<std::size_t>(j) * W + i] = o[j];
        }
    }
    for (int j = 0; j < H; j++) {
        dt1d(&tmp[static_cast<std::size_t>(j) * W], W, o.data(), v.data(), z.data());
        for (int i = 0; i < W; i++) {
            d[static_cast<std::size_t>(j) * W + i] = static_cast<float>(std::sqrt(o[i]));
        }
    }
    return d;
}

// Removes the disc of radius r at each position, row spans only where they were not removed by the previous stamp.
struct Sweeper {
    Grid& g;
    double r;
    std::vector<int> oL, oR, nL, nR;
    int oJ0 = 0, oJ1 = -1;
    long long cut = 0, gouge = 0;
    Sweeper(Grid& g_, double r_) : g(g_), r(r_)
    {
        int n = static_cast<int>(2 * r / g.cell) + 8;
        oL.resize(n);
        oR.resize(n);
        nL.resize(n);
        nR.resize(n);
    }
    void reset()
    {
        oJ1 = -1;
        oJ0 = 0;
    }
    void clear(int j, int a, int b)
    {
        if (j < 0 || j >= g.H) {
            return;
        }
        a = std::max(a, 0);
        b = std::min(b, g.W - 1);
        std::uint8_t* row = &g.v[static_cast<std::size_t>(j) * g.W];
        for (int i = a; i <= b; i++) {
            std::uint8_t c = row[i];
            if (c == kPocket) {
                row[i] = kRemoved;
                cut++;
            } else if (c == kKeep) {
                row[i] = kGouged;
                gouge++;
            }
        }
    }
    void stamp(double cx, double cy)
    {
        int j0 = static_cast<int>(std::ceil((cy - r - g.oy) / g.cell - 0.5));
        int j1 = static_cast<int>(std::floor((cy + r - g.oy) / g.cell - 0.5));
        for (int j = j0; j <= j1; j++) {
            double dy = g.oy + (j + 0.5) * g.cell - cy;
            double h2 = r * r - dy * dy;
            int l = 1, rr = 0;
            if (h2 >= 0) {
                double h = std::sqrt(h2);
                l = static_cast<int>(std::ceil((cx - h - g.ox) / g.cell - 0.5));
                rr = static_cast<int>(std::floor((cx + h - g.ox) / g.cell - 0.5));
            }
            nL[j - j0] = l;
            nR[j - j0] = rr;
            if (l > rr) {
                continue;
            }
            if (j >= oJ0 && j <= oJ1 && oL[j - oJ0] <= oR[j - oJ0]) {
                int ol = oL[j - oJ0], orr = oR[j - oJ0];
                clear(j, l, std::min(rr, ol - 1));
                clear(j, std::max(l, orr + 1), rr);
            } else {
                clear(j, l, rr);
            }
        }
        std::swap(oL, nL);
        std::swap(oR, nR);
        oJ0 = j0;
        oJ1 = j1;
    }
};

// Removal lagging the engagement sample by kLag mm of path.
struct Lagged {
    Sweeper& sw;
    struct P {
        double x, y, s;
    };
    std::vector<P> q;
    std::size_t head = 0;
    double S = 0;
    explicit Lagged(Sweeper& s_) : sw(s_) {}
    void start(double x, double y)
    {
        flushAll();
        sw.reset();
        q.clear();
        head = 0;
        q.push_back({x, y, S});
    }
    void advance(double x, double y, double ds)
    {
        S += ds;
        q.push_back({x, y, S});
    }
    void flushTo(double s)
    {
        while (head < q.size() && q[head].s <= s) {
            sw.stamp(q[head].x, q[head].y);
            head++;
        }
        if (head > 4096 && head * 2 > q.size()) {
            q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(head));
            head = 0;
        }
    }
    void flushAll() { flushTo(1e300); }
    void measureReady() { flushTo(S - kLag); }
    void drop()
    {
        q.clear();
        head = 0;
    }
};

struct EngSampler {
    std::vector<double> cs, sn;
    double rs;
    EngSampler(double r, double inset) : rs(r - inset)
    {
        cs.resize(kSamples);
        sn.resize(kSamples);
        for (int k = 0; k < kSamples; k++) {
            double a = 2 * kPi * k / kSamples;
            cs[k] = std::cos(a);
            sn[k] = std::sin(a);
        }
    }
    double angleDeg(const Grid& g, double px, double py) const
    {
        int n = 0;
        for (int k = 0; k < kSamples; k++) {
            n += g.material(px + rs * cs[k], py + rs * sn[k]);
        }
        return 360.0 * n / kSamples;
    }
};

struct Sample {
    float ang, w;
};
double wpercentile(std::vector<Sample> s, double p)
{
    if (s.empty()) {
        return 0;
    }
    std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.ang < b.ang; });
    double tot = 0;
    for (const Sample& x : s) {
        tot += x.w;
    }
    double acc = 0;
    for (const Sample& x : s) {
        acc += x.w;
        if (acc >= p * tot) {
            return x.ang;
        }
    }
    return s.back().ang;
}
double wfracAbove(const std::vector<Sample>& s, double thr)
{
    double tot = 0, a = 0;
    for (const Sample& x : s) {
        tot += x.w;
        if (x.ang > thr) {
            a += x.w;
        }
    }
    return tot > 0 ? 100.0 * a / tot : 0;
}

double segDist(double px, double py, const Pt& a, const Pt& b)
{
    double vx = b.first - a.first, vy = b.second - a.second, wx = px - a.first, wy = py - a.second;
    double L = vx * vx + vy * vy;
    double t = L > 0 ? std::clamp((wx * vx + wy * vy) / L, 0.0, 1.0) : 0;
    double dx = wx - t * vx, dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

// Wall edges in square buckets of `size` mm, for the nearest-wall query within the tool radius.
struct Walls {
    std::vector<std::pair<Pt, Pt>> edges;
    double ox = 0, oy = 0, size = 1;
    int nx = 0, ny = 0;
    std::vector<std::vector<int>> buckets;
    void build(const Grid& g, double bucket)
    {
        size = bucket;
        ox = g.ox;
        oy = g.oy;
        nx = static_cast<int>(std::ceil(g.W * g.cell / size)) + 1;
        ny = static_cast<int>(std::ceil(g.H * g.cell / size)) + 1;
        buckets.assign(static_cast<std::size_t>(nx) * ny, {});
        for (int e = 0; e < static_cast<int>(edges.size()); e++) {
            const Pt& a = edges[e].first;
            const Pt& b = edges[e].second;
            int i0 = std::clamp(static_cast<int>(std::floor((std::min(a.first, b.first) - ox) / size)), 0, nx - 1);
            int i1 = std::clamp(static_cast<int>(std::floor((std::max(a.first, b.first) - ox) / size)), 0, nx - 1);
            int j0 = std::clamp(static_cast<int>(std::floor((std::min(a.second, b.second) - oy) / size)), 0, ny - 1);
            int j1 = std::clamp(static_cast<int>(std::floor((std::max(a.second, b.second) - oy) / size)), 0, ny - 1);
            for (int j = j0; j <= j1; j++) {
                for (int i = i0; i <= i1; i++) {
                    buckets[static_cast<std::size_t>(j) * nx + i].push_back(e);
                }
            }
        }
    }
    // The smallest distance to an edge if one lies within `reach` of (x, y) (bucket-exact), else a value >= reach.
    double nearest(double x, double y, double reach) const
    {
        double d = 1e30;
        int i0 = std::clamp(static_cast<int>(std::floor((x - reach - ox) / size)), 0, nx - 1);
        int i1 = std::clamp(static_cast<int>(std::floor((x + reach - ox) / size)), 0, nx - 1);
        int j0 = std::clamp(static_cast<int>(std::floor((y - reach - oy) / size)), 0, ny - 1);
        int j1 = std::clamp(static_cast<int>(std::floor((y + reach - oy) / size)), 0, ny - 1);
        for (int j = j0; j <= j1; j++) {
            for (int i = i0; i <= i1; i++) {
                for (int e : buckets[static_cast<std::size_t>(j) * nx + i]) {
                    d = std::min(d, segDist(x, y, edges[e].first, edges[e].second));
                }
            }
        }
        return d;
    }
};

} // namespace

double straightEngagementDeg(double ae, double r)
{
    return std::acos(std::clamp(1 - ae / r, -1.0, 1.0)) * 180 / kPi;
}

Report check(const Input& in)
{
    Report rep;
    if (!(in.toolDiameter > 0) || !(in.cell > 0)) {
        return rep;
    }
    const double r = in.toolDiameter / 2;
    const double cell = in.cell;
    const double inset = kLag * kLag / (2 * r);
    const double target = in.targetDeg;

    // grid
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Loops* ls : {&in.pocket, &in.stock}) {
        for (const Loop& l : *ls) {
            for (const Pt& p : l) {
                x0 = std::min(x0, p.first);
                x1 = std::max(x1, p.first);
                y0 = std::min(y0, p.second);
                y1 = std::max(y1, p.second);
            }
        }
    }
    if (x0 > x1) {
        return rep;
    }
    const double margin = r + 3;
    Grid g;
    g.cell = cell;
    g.ox = x0 - margin;
    g.oy = y0 - margin;
    g.W = static_cast<int>(std::ceil((x1 - x0 + 2 * margin) / cell));
    g.H = static_cast<int>(std::ceil((y1 - y0 + 2 * margin) / cell));
    const auto mStock = fillMask(g, in.stock);
    const auto mPocket = fillMask(g, in.pocket);
    const auto mCleared = in.cleared.empty() ? std::vector<std::uint8_t>(mStock.size(), 0) : fillMask(g, in.cleared);
    g.v.resize(static_cast<std::size_t>(g.W) * g.H);
    long long pocketCells = 0;
    std::vector<std::uint8_t> keep(g.v.size()), notKeep(g.v.size());
    for (std::size_t k = 0; k < g.v.size(); k++) {
        std::uint8_t c = mStock[k] ? (mPocket[k] ? kPocket : kKeep) : kAir;
        pocketCells += c == kPocket;
        keep[k] = c == kKeep;
        notKeep[k] = !keep[k];
        if (mCleared[k]) {
            c = c == kPocket ? kRemoved : kAir;
        }
        g.v[k] = c;
    }
    // Pocket edges that are walls: not lying on the stock outline (an open side).
    Walls walls;
    auto onStock = [&](const Pt& p) {
        for (const Loop& l : in.stock) {
            for (std::size_t k = 0; k < l.size(); k++) {
                if (segDist(p.first, p.second, l[k], l[(k + 1) % l.size()]) < 1e-6) {
                    return true;
                }
            }
        }
        return false;
    };
    for (const Loop& l : in.pocket) {
        for (std::size_t k = 0; k < l.size(); k++) {
            Pt a = l[k], b = l[(k + 1) % l.size()];
            if (!(onStock(a) && onStock(b))) {
                walls.edges.push_back({a, b});
            }
        }
    }
    walls.build(g, std::max(1.0, r));

    Sweeper sw(g, r);
    EngSampler es(r, inset);
    Lagged lg(sw);
    std::vector<Sample> cutS, linkS;
    long long linkCutCells = 0;
    float maxCut = -1, maxLink = -1;
    double maxGouge = -1e300;
    auto gougeAt = [&](double x, double y) {
        double d = walls.nearest(x, y, r + cell);
        if (r - d > maxGouge) {
            maxGouge = r - d;
        }
        int i = g.ix(x), j = g.iy(y);
        if (i >= 0 && j >= 0 && i < g.W && j < g.H) {
            std::uint8_t c = g.v[static_cast<std::size_t>(j) * g.W + i];
            if (c == kKeep || c == kGouged) {
                rep.centreInKeepoutSteps++;
            }
        }
    };
    bool havePos = false;
    Pt pos{0, 0};
    for (const Region& rg : in.regions) {
        if (rg.paths.empty()) {
            continue;
        }
        if (havePos) {
            rep.retractLength += std::hypot(rg.start.first - pos.first, rg.start.second - pos.second);
        }
        lg.flushAll();
        sw.reset();
        if (rg.helixRadius > 1e-9) {
            double Rh = rg.helixRadius;
            double a0 = std::atan2(rg.start.second - rg.helixCentre.second, rg.start.first - rg.helixCentre.first);
            int n = static_cast<int>(std::ceil(2 * kPi * Rh / cell));
            for (int k = 0; k <= n; k++) {
                double a = a0 + 2 * kPi * k / n;
                double x = rg.helixCentre.first + Rh * std::cos(a), y = rg.helixCentre.second + Rh * std::sin(a);
                sw.stamp(x, y);
                gougeAt(x, y);
            }
            rep.helixLength += 2 * kPi * Rh;
        } else {
            sw.stamp(rg.start.first, rg.start.second);
            gougeAt(rg.start.first, rg.start.second);
        }
        lg.drop();
        sw.reset();
        pos = rg.start;
        havePos = true;
        for (const Path& p0 : rg.paths) {
            if (p0.points.empty()) {
                continue;
            }
            Path p = p0;
            rep.points += p.points.size();
            double gap = std::hypot(p.points[0].first - pos.first, p.points[0].second - pos.second);
            if (gap > 1e-6) {
                rep.implicitJoins++;
                rep.joinLength += gap;
                rep.joinMax = std::max(rep.joinMax, gap);
                if (p.kind == Kind::Retract) {
                    rep.retractLength += gap;
                } else {
                    p.points.insert(p.points.begin(), pos);
                }
            }
            bool contiguous = !lg.q.empty();
            if (p.kind == Kind::Retract) {
                for (std::size_t k = 0; k + 1 < p.points.size(); k++) {
                    rep.retractLength += std::hypot(p.points[k + 1].first - p.points[k].first,
                                                    p.points[k + 1].second - p.points[k].second);
                }
                pos = p.points.back();
                lg.flushAll();
                lg.drop();
                continue;
            }
            if (p.kind == Kind::LinkDown) {
                lg.flushAll(); // attribute removal exactly to cuts or links
            }
            long long cutBefore = sw.cut;
            if (!contiguous) {
                lg.start(p.points[0].first, p.points[0].second);
            }
            gougeAt(p.points[0].first, p.points[0].second);
            for (std::size_t k = 0; k + 1 < p.points.size(); k++) {
                Pt a = p.points[k], b = p.points[k + 1];
                double len = std::hypot(b.first - a.first, b.second - a.second);
                if (len <= 0) {
                    continue;
                }
                double dx = (b.first - a.first) / len, dy = (b.second - a.second) / len;
                int n = static_cast<int>(std::ceil(len / cell));
                double h = len / n;
                for (int s = 0; s < n; s++) {
                    double x = a.first + dx * h * s, y = a.second + dy * h * s;
                    lg.measureReady();
                    float ang = static_cast<float>(es.angleDeg(g, x, y));
                    if (p.kind == Kind::Cut) {
                        if (p.measured) {
                            cutS.push_back({ang, static_cast<float>(h)});
                            if (ang > maxCut) {
                                maxCut = ang;
                                rep.maxAt = {x, y};
                            }
                        }
                    } else {
                        linkS.push_back({ang, static_cast<float>(h)});
                        if (ang > maxLink) {
                            maxLink = ang;
                            rep.linkMaxAt = {x, y};
                        }
                    }
                    double xn = a.first + dx * h * (s + 1), yn = a.second + dy * h * (s + 1);
                    lg.advance(xn, yn, h);
                    gougeAt(xn, yn);
                }
                (p.kind == Kind::Cut ? rep.cutLength : rep.linkDownLength) += len;
            }
            if (p.kind == Kind::LinkDown) {
                lg.flushAll();
                linkCutCells += sw.cut - cutBefore;
            }
            pos = p.points.back();
        }
    }
    lg.flushAll();

    const double ca = cell * cell;
    rep.median = wpercentile(cutS, 0.5);
    rep.p95 = wpercentile(cutS, 0.95);
    rep.p99 = wpercentile(cutS, 0.99);
    rep.max = wpercentile(cutS, 1.0);
    rep.pctAboveTarget = wfracAbove(cutS, target);
    rep.pctAbove1_1x = wfracAbove(cutS, 1.1 * target);
    rep.pctAbove1_25x = wfracAbove(cutS, 1.25 * target);
    rep.pctAbove1_5x = wfracAbove(cutS, 1.5 * target);
    rep.pctAboveTargetPlus5 = wfracAbove(cutS, target + 5);
    rep.pctAboveTargetPlus10 = wfracAbove(cutS, target + 10);
    rep.pctAboveTargetPlus20 = wfracAbove(cutS, target + 20);
    rep.pctAbove90 = wfracAbove(cutS, 90);
    rep.pctAir = cutS.empty() ? 0 : 100 - wfracAbove(cutS, 2);
    {
        double tot = 0;
        for (const Sample& s : cutS) {
            int b = std::min(17, static_cast<int>(s.ang / 10));
            rep.histogram[b] += s.w;
            tot += s.w;
        }
        for (double& b : rep.histogram) {
            b = tot > 0 ? 100 * b / tot : 0;
        }
    }
    rep.linkMaxDeg = wpercentile(linkS, 1.0);
    rep.linkPctEngaged = wfracAbove(linkS, 2);
    rep.linkRemovedMm2 = static_cast<double>(linkCutCells) * ca;

    // uncut and gouge accounting
    auto dKeep = edt(keep, g.W, g.H);
    std::vector<std::uint8_t> allowed(g.v.size());
    for (std::size_t k = 0; k < g.v.size(); k++) {
        allowed[k] = dKeep[k] * cell >= r + cell;
    }
    auto dAllowed = edt(allowed, g.W, g.H);
    std::vector<std::uint8_t> air(g.v.size());
    for (std::size_t k = 0; k < g.v.size(); k++) {
        air[k] = !(g.v[k] == kPocket || g.v[k] == kKeep);
    }
    auto dAir = edt(air, g.W, g.H);
    auto dNotKeep = edt(notKeep, g.W, g.H);
    long long uncut = 0, uncutReach = 0, reachCells = 0;
    for (std::size_t k = 0; k < g.v.size(); k++) {
        bool reach = dAllowed[k] * cell <= r - cell;
        if ((g.v[k] == kPocket || g.v[k] == kRemoved) && reach) {
            reachCells++;
        }
        if (g.v[k] == kPocket) {
            uncut++;
            if (reach) {
                uncutReach++;
                rep.uncutMaxThickness = std::max(rep.uncutMaxThickness, static_cast<double>(dAir[k]) * cell);
            }
        }
        if (g.v[k] == kGouged) {
            rep.gougeDepthRaster = std::max(rep.gougeDepthRaster, static_cast<double>(dNotKeep[k]) * cell - cell / 2);
        }
    }
    rep.pocketArea = static_cast<double>(pocketCells) * ca;
    rep.reachableArea = static_cast<double>(reachCells) * ca;
    rep.uncutTotal = static_cast<double>(uncut) * ca;
    rep.uncutReachable = static_cast<double>(uncutReach) * ca;
    rep.gougeAnalytic = std::max(0.0, maxGouge);
    rep.gougeArea = static_cast<double>(sw.gouge) * ca;
    return rep;
}

std::string summary(const Report& r)
{
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "cut=%.1f link_down=%.1f retract=%.1f helix=%.1f points=%zu joins=%d join_max=%.3f | median=%.2f "
                  "p95=%.2f p99=%.2f max=%.2f >target=%.2f%% >1.1x=%.2f%% >1.25x=%.2f%% >1.5x=%.2f%% >t+10=%.3f%% "
                  "air=%.2f%% | link: max=%.1f engaged=%.2f%% removed=%.3f mm2 | uncut_reach=%.3f mm2 (thick %.3f) "
                  "of %.1f | gouge=%.4f mm (area %.4f, raster %.3f) | max at (%.2f, %.2f), link max at (%.2f, %.2f)",
                  r.cutLength, r.linkDownLength, r.retractLength, r.helixLength, r.points, r.implicitJoins, r.joinMax,
                  r.median, r.p95, r.p99, r.max, r.pctAboveTarget, r.pctAbove1_1x, r.pctAbove1_25x, r.pctAbove1_5x,
                  r.pctAboveTargetPlus10, r.pctAir, r.linkMaxDeg, r.linkPctEngaged, r.linkRemovedMm2, r.uncutReachable,
                  r.uncutMaxThickness, r.reachableArea, r.gougeAnalytic, r.gougeArea, r.gougeDepthRaster, r.maxAt.first,
                  r.maxAt.second, r.linkMaxAt.first, r.linkMaxAt.second);
    return buf;
}

} // namespace ray::test::sweep
