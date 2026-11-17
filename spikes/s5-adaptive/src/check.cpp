// S5 spike: independent 2D cutter-sweep checker.
// Shares only the pocket definitions (cases.h) with the generator. No Adaptive code, no Clipper.
//
// Model: raster of the stock at this Z level, cell = 0.02 mm.
//   0 air (outside stock)   1 pocket material (to be removed)   2 keep-out material (walls, islands)
//   3 gouged keep-out       4 removed pocket material
// The cutter disc is swept along the path in steps <= 1 cell. Material removal is applied with a lag of
// kLag mm of path length. At each step the engagement angle is measured by sampling 1440 points on a
// circle of radius (r - kInset) at the current cutter position and counting those that lie in material,
// i.e. material not yet removed by the sweep up to kLag behind the cutter. kInset = kLag^2 / (2 r), which
// makes the sample circle cross the lagged cutter disc exactly at 90 degrees to the direction of motion,
// so the arc is neither clipped nor extended at the trailing end and a wall the cutter is only tangent
// to is not counted.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "cases.h"

using s5::Loop; using s5::Loops; using s5::Pt;
static const double kCell = 0.02;
static const double kLag = 0.45;      // mm of path by which removal lags the engagement sample
static const double kInset = kLag * kLag / s5::kToolDia;  // = kLag^2 / (2 r) = 0.0159 mm
static const int kSamples = 1440;     // 0.25 deg

struct Grid {
    int W = 0, H = 0; double ox = 0, oy = 0;
    std::vector<std::uint8_t> v;
    std::uint8_t& at(int i, int j) { return v[(size_t)j * W + i]; }
    int ix(double x) const { return (int)std::floor((x - ox) / kCell); }
    int iy(double y) const { return (int)std::floor((y - oy) / kCell); }
    bool material(double x, double y) const {
        int i = ix(x), j = iy(y);
        if (i < 0 || j < 0 || i >= W || j >= H) return false;
        std::uint8_t c = v[(size_t)j * W + i];
        return c == 1 || c == 2;
    }
};

// even-odd scanline fill of a set of loops into a 0/1 mask
static std::vector<std::uint8_t> fillMask(const Grid& g, const Loops& loops) {
    std::vector<std::uint8_t> m((size_t)g.W * g.H, 0);
    std::vector<double> xs;
    for (int j = 0; j < g.H; j++) {
        double y = g.oy + (j + 0.5) * kCell;
        xs.clear();
        for (auto& l : loops) {
            size_t n = l.size();
            for (size_t k = 0; k < n; k++) {
                Pt a = l[k], b = l[(k + 1) % n];
                if ((a.second <= y) != (b.second <= y))
                    xs.push_back(a.first + (y - a.second) * (b.first - a.first) / (b.second - a.second));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int i0 = (int)std::ceil((xs[k] - g.ox) / kCell - 0.5), i1 = (int)std::floor((xs[k + 1] - g.ox) / kCell - 0.5);
            i0 = std::max(i0, 0); i1 = std::min(i1, g.W - 1);
            for (int i = i0; i <= i1; i++) m[(size_t)j * g.W + i] = 1;
        }
    }
    return m;
}

// exact Euclidean distance transform (Felzenszwalb-Huttenlocher); returns distance in cells to nearest feature cell
static void dt1d(const double* f, int n, double* d, int* v, double* z) {
    const double INF = 1e20;
    auto isect = [&](int q, int p) { return ((f[q] + (double)q * q) - (f[p] + (double)p * p)) / (2.0 * q - 2.0 * p); };
    int k = 0; v[0] = 0; z[0] = -INF; z[1] = INF;
    for (int q = 1; q < n; q++) {
        double s = isect(q, v[k]);
        while (k > 0 && s <= z[k]) { k--; s = isect(q, v[k]); }
        k++; v[k] = q; z[k] = s; z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = (double)(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}
static std::vector<float> edt(const std::vector<std::uint8_t>& feat, int W, int H) {
    const double INF = 1e20;
    std::vector<float> d((size_t)W * H);
    int n = std::max(W, H);
    std::vector<double> f(n), o(n), z(n + 1); std::vector<int> v(n);
    std::vector<double> tmp((size_t)W * H);
    for (int i = 0; i < W; i++) {
        for (int j = 0; j < H; j++) f[j] = feat[(size_t)j * W + i] ? 0.0 : INF;
        dt1d(f.data(), H, o.data(), v.data(), z.data());
        for (int j = 0; j < H; j++) tmp[(size_t)j * W + i] = o[j];
    }
    for (int j = 0; j < H; j++) {
        dt1d(&tmp[(size_t)j * W], W, o.data(), v.data(), z.data());
        for (int i = 0; i < W; i++) d[(size_t)j * W + i] = (float)std::sqrt(o[i]);
    }
    return d;
}

struct Sweeper {
    Grid& g; double r;
    std::vector<int> oL, oR, nL, nR; int oJ0 = 0, oJ1 = -1;
    long long cut = 0, gouge = 0;
    Sweeper(Grid& g_, double r_) : g(g_), r(r_) {
        int n = (int)(2 * r / kCell) + 8; oL.resize(n); oR.resize(n); nL.resize(n); nR.resize(n);
    }
    void reset() { oJ1 = -1; oJ0 = 0; }
    inline void clear(int j, int a, int b) {
        if (j < 0 || j >= g.H) return;
        a = std::max(a, 0); b = std::min(b, g.W - 1);
        std::uint8_t* row = &g.v[(size_t)j * g.W];
        for (int i = a; i <= b; i++) {
            std::uint8_t c = row[i];
            if (c == 1) { row[i] = 4; cut++; }
            else if (c == 2) { row[i] = 3; gouge++; }
        }
    }
    void stamp(double cx, double cy) {
        int j0 = (int)std::ceil((cy - r - g.oy) / kCell - 0.5), j1 = (int)std::floor((cy + r - g.oy) / kCell - 0.5);
        for (int j = j0; j <= j1; j++) {
            double dy = g.oy + (j + 0.5) * kCell - cy;
            double h2 = r * r - dy * dy;
            int l = 1, rr = 0;
            if (h2 >= 0) {
                double h = std::sqrt(h2);
                l = (int)std::ceil((cx - h - g.ox) / kCell - 0.5); rr = (int)std::floor((cx + h - g.ox) / kCell - 0.5);
            }
            nL[j - j0] = l; nR[j - j0] = rr;
            if (l > rr) continue;
            if (j >= oJ0 && j <= oJ1 && oL[j - oJ0] <= oR[j - oJ0]) {
                int ol = oL[j - oJ0], orr = oR[j - oJ0];
                clear(j, l, std::min(rr, ol - 1));
                clear(j, std::max(l, orr + 1), rr);
            } else clear(j, l, rr);
        }
        std::swap(oL, nL); std::swap(oR, nR); oJ0 = j0; oJ1 = j1;
    }
};

struct Lagged {
    Sweeper& sw; struct P { double x, y, s; }; std::vector<P> q; size_t head = 0; double S = 0;
    explicit Lagged(Sweeper& s_) : sw(s_) {}
    void start(double x, double y) { flushAll(); sw.reset(); q.clear(); head = 0; q.push_back({x, y, S}); }
    void advance(double x, double y, double ds) { S += ds; q.push_back({x, y, S}); }
    void flushTo(double s) { while (head < q.size() && q[head].s <= s) { sw.stamp(q[head].x, q[head].y); head++; }
                             if (head > 4096 && head * 2 > q.size()) { q.erase(q.begin(), q.begin() + head); head = 0; } }
    void flushAll() { flushTo(1e300); }
    void measureReady() { flushTo(S - kLag); }
};

struct EngSampler {
    std::vector<double> cs, sn; double r;
    explicit EngSampler(double r_) : r(r_) {
        cs.resize(kSamples); sn.resize(kSamples);
        for (int k = 0; k < kSamples; k++) { double a = 2 * s5::kPi * k / kSamples; cs[k] = std::cos(a); sn[k] = std::sin(a); }
    }
    double angleDeg(const Grid& g, double px, double py, double dx, double dy) const {
        double bx = px, by = py; (void)dx; (void)dy; int n = 0;
        for (int k = 0; k < kSamples; k++) n += g.material(bx + (r - kInset) * cs[k], by + (r - kInset) * sn[k]);
        return 360.0 * n / kSamples;
    }
};

struct Sample { float ang; float w; };
static double wpercentile(std::vector<Sample> s, double p) {  // by value: sorts a copy
    if (s.empty()) return 0;
    std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.ang < b.ang; });
    double tot = 0; for (auto& x : s) tot += x.w;
    double acc = 0;
    for (auto& x : s) { acc += x.w; if (acc >= p * tot) return x.ang; }
    return s.back().ang;
}
static double wfracAbove(const std::vector<Sample>& s, double thr) {
    double tot = 0, a = 0; for (auto& x : s) { tot += x.w; if (x.ang > thr) a += x.w; }
    return tot > 0 ? 100.0 * a / tot : 0;
}

// ---- minimal PNG writer (stored deflate) ----
static std::uint32_t crcTab[256];
static void crcInit() { for (std::uint32_t n = 0; n < 256; n++) { std::uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crcTab[n] = c; } }
static std::uint32_t crc(const std::uint8_t* p, size_t n, std::uint32_t c = 0xffffffffu) { for (size_t i = 0; i < n; i++) c = crcTab[(c ^ p[i]) & 0xff] ^ (c >> 8); return c; }
static void chunk(FILE* f, const char* type, const std::vector<std::uint8_t>& d) {
    std::uint8_t b[4]; std::uint32_t n = (std::uint32_t)d.size();
    b[0] = n >> 24; b[1] = n >> 16; b[2] = n >> 8; b[3] = n; fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f); if (n) fwrite(d.data(), 1, n, f);
    std::uint32_t c = crc((const std::uint8_t*)type, 4); c = crc(d.data(), n, c) ^ 0xffffffffu;
    b[0] = c >> 24; b[1] = c >> 16; b[2] = c >> 8; b[3] = c; fwrite(b, 1, 4, f);
}
static void writePng(const char* path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    crcInit();
    FILE* f = fopen(path, "wb"); if (!f) return;
    const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    std::vector<std::uint8_t> ih = {(std::uint8_t)(w >> 24), (std::uint8_t)(w >> 16), (std::uint8_t)(w >> 8), (std::uint8_t)w,
                                    (std::uint8_t)(h >> 24), (std::uint8_t)(h >> 16), (std::uint8_t)(h >> 8), (std::uint8_t)h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ih);
    std::vector<std::uint8_t> raw; raw.reserve((size_t)h * (w * 3 + 1));
    for (int j = 0; j < h; j++) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + (size_t)j * w * 3, rgb.begin() + (size_t)(j + 1) * w * 3); }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    size_t pos = 0; std::uint32_t a = 1, b = 0;
    for (auto c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    while (pos < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size()); z.push_back(n & 0xff); z.push_back(n >> 8); z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n); pos += n;
    }
    std::uint32_t ad = (b << 16) | a; z.push_back(ad >> 24); z.push_back(ad >> 16); z.push_back(ad >> 8); z.push_back(ad);
    chunk(f, "IDAT", z); chunk(f, "IEND", {});
    fclose(f);
}

// ---- toolpath file ----
struct TPath { int type; std::vector<Pt> pts; };
struct Region { Pt helix, start; int ret; int flags[7]; std::vector<TPath> paths; };
static std::vector<Region> readTp(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    std::vector<Region> rs; char buf[64]; size_t nr = 0;
    if (fscanf(f, "S5TP 1 regions %zu", &nr) != 1) { fprintf(stderr, "bad header\n"); exit(2); }
    for (size_t i = 0; i < nr; i++) {
        Region r; size_t np = 0;
        fscanf(f, "%63s %lf %lf %lf %lf %d %zu", buf, &r.helix.first, &r.helix.second, &r.start.first, &r.start.second, &r.ret, &np);
        fscanf(f, "%63s %d %d %d %d %d %d %d", buf, &r.flags[0], &r.flags[1], &r.flags[2], &r.flags[3], &r.flags[4], &r.flags[5], &r.flags[6]);
        for (size_t k = 0; k < np; k++) {
            TPath p; size_t n = 0; fscanf(f, "%63s %d %zu", buf, &p.type, &n);
            p.pts.resize(n);
            for (auto& q : p.pts) fscanf(f, "%lf %lf", &q.first, &q.second);
            r.paths.push_back(std::move(p));
        }
        rs.push_back(std::move(r));
    }
    fclose(f); return rs;
}

static double segDist(double px, double py, const Pt& a, const Pt& b) {
    double vx = b.first - a.first, vy = b.second - a.second, wx = px - a.first, wy = py - a.second;
    double L = vx * vx + vy * vy, t = L > 0 ? (wx * vx + wy * vy) / L : 0; t = std::clamp(t, 0.0, 1.0);
    double dx = wx - t * vx, dy = wy - t * vy; return std::sqrt(dx * dx + dy * dy);
}

static int selfTest() {
    const double r = s5::kToolDia / 2;
    printf("selftest: straight passes (expected = acos(1 - ae/r)) and inside-arc passes\n");
    int bad = 0;
    for (double ae : {12.7, 6.35, 2.54, 1.27, 0.635, 0.127}) {
        Grid g; g.ox = 0; g.oy = 0; g.W = (int)(120 / kCell); g.H = (int)(50 / kCell); g.v.assign((size_t)g.W * g.H, 1);
        Sweeper sw(g, r); EngSampler es(r);
        // first pass: full slot along y = 20
        std::vector<Sample> slot, pass;
        for (int phase = 0; phase < 2; phase++) {
            double y = phase == 0 ? 20.0 : 20.0 + ae;
            if (phase == 1 && ae >= 12.7) break;
            Lagged lg(sw); lg.start(10, y);
            for (double x = 10; x < 110; x += kCell) {
                lg.measureReady();
                double a = es.angleDeg(g, x, y, 1, 0);
                if (x > 30 && x < 90) (phase == 0 ? slot : pass).push_back({(float)a, 1});
                lg.advance(x + kCell, y, kCell);
            }
            lg.flushAll();
        }
        if (ae >= 12.7) { double m = wpercentile(slot, 0.5); printf("  slot      expected 180.00 measured median %.2f min %.2f max %.2f\n", m, wpercentile(slot, 0), wpercentile(slot, 1)); bad += std::fabs(m - 180) > 3; }
        else { double e = std::acos(1 - ae / r) * 180 / s5::kPi, m = wpercentile(pass, 0.5);
               printf("  ae=%-6.3f expected %6.2f measured median %.2f min %.2f max %.2f\n", ae, e, m, wpercentile(pass, 0), wpercentile(pass, 1)); bad += std::fabs(m - e) > 3; }
    }
    for (double ae : {2.54, 1.27}) {  // arc: pre-cleared hole R0 = 15, then centre path radius (R0 - r) + ae, CCW
        const double R0 = 15, cx = 30, cy = 30;
        Grid g; g.ox = 0; g.oy = 0; g.W = (int)(60 / kCell); g.H = (int)(60 / kCell); g.v.assign((size_t)g.W * g.H, 1);
        for (int j = 0; j < g.H; j++) for (int i = 0; i < g.W; i++) { double x = (i + .5) * kCell - cx, y = (j + .5) * kCell - cy; if (x * x + y * y < R0 * R0) g.at(i, j) = 4; }
        Sweeper sw(g, r); EngSampler es(r); double rho = R0 - r + ae; std::vector<Sample> s;
        int n = (int)(2 * s5::kPi * rho / kCell); Lagged lg(sw); lg.start(cx + rho, cy);
        for (int k = 0; k < n; k++) {
            double a = 2 * s5::kPi * k / n, a1 = 2 * s5::kPi * (k + 1) / n;
            lg.measureReady();
            double e = es.angleDeg(g, cx + rho * std::cos(a), cy + rho * std::sin(a), -std::sin(a), std::cos(a));
            if (k > n / 8 && k < n * 7 / 8) s.push_back({(float)e, 1});
            lg.advance(cx + rho * std::cos(a1), cy + rho * std::sin(a1), 2 * s5::kPi * rho / n);
        }
        double e = std::acos((R0 * R0 - rho * rho - r * r) / (2 * rho * r)) * 180 / s5::kPi, m = wpercentile(s, 0.5);
        printf("  arc ae=%-5.2f expected %6.2f measured median %.2f min %.2f max %.2f\n", ae, e, m, wpercentile(s, 0), wpercentile(s, 1));
        bad += std::fabs(m - e) > 3;
    }
    printf("selftest %s (pass = median within 3 deg)\n", bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "selftest") return selfTest();
    if (argc < 4) { fprintf(stderr, "usage: s5_check <case> <tpfile> <stepover> [out.png] | selftest\n"); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    auto cases = s5::makeCases();
    const s5::Case* cs = nullptr; for (auto& c : cases) if (c.id == argv[1]) cs = &c;
    if (!cs) { fprintf(stderr, "unknown case\n"); return 2; }
    double stepover = atof(argv[3]);
    const double r = s5::kToolDia / 2;
    const double target = std::acos(1 - 2 * stepover) * 180 / s5::kPi;
    auto regions = readTp(argv[2]);

    // grid
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (auto* ls : {&cs->pocket, &cs->stock}) for (auto& l : *ls) for (auto& p : l) { x0 = std::min(x0, p.first); x1 = std::max(x1, p.first); y0 = std::min(y0, p.second); y1 = std::max(y1, p.second); }
    double margin = r + 3;
    Grid g; g.ox = x0 - margin; g.oy = y0 - margin; g.W = (int)std::ceil((x1 - x0 + 2 * margin) / kCell); g.H = (int)std::ceil((y1 - y0 + 2 * margin) / kCell);
    auto mStock = fillMask(g, cs->stock), mPocket = fillMask(g, cs->pocket);
    g.v.resize((size_t)g.W * g.H);
    long long pocketCells = 0;
    std::vector<std::uint8_t> keep(g.v.size()), notKeep(g.v.size());
    for (size_t k = 0; k < g.v.size(); k++) {
        g.v[k] = mStock[k] ? (mPocket[k] ? 1 : 2) : 0;
        pocketCells += g.v[k] == 1; keep[k] = g.v[k] == 2; notKeep[k] = !keep[k];
    }
    // closed boundary segments for the analytic gouge check: pocket edges not lying on the stock outline
    std::vector<std::pair<Pt, Pt>> walls;
    auto onStock = [&](const Pt& p) { for (auto& l : cs->stock) for (size_t k = 0; k < l.size(); k++) if (segDist(p.first, p.second, l[k], l[(k + 1) % l.size()]) < 1e-6) return true; return false; };
    for (auto& l : cs->pocket) for (size_t k = 0; k < l.size(); k++) { Pt a = l[k], b = l[(k + 1) % l.size()]; if (!(onStock(a) && onStock(b))) walls.push_back({a, b}); }

    Sweeper sw(g, r); EngSampler es(r); Lagged lg(sw);
    std::vector<Sample> cutS, linkS;
    double cutLen = 0, linkLen = 0, rapidLen = 0, helixLen = 0, gapLen = 0, maxGap = 0; int gaps = 0; size_t nPts = 0, nPaths[3] = {0, 0, 0};
    double maxGougeAnalytic = 0; long long centreInKeepout = 0; long long linkCutCells = 0;
    int flagsOr[7] = {0, 0, 0, 0, 0, 0, 0}; int emptyRegions = 0;
    std::vector<std::pair<Pt, int>> trace;  // for the picture
    auto gougeAt = [&](double x, double y) {
        double d = 1e9; for (auto& w : walls) d = std::min(d, segDist(x, y, w.first, w.second));
        if (r - d > maxGougeAnalytic) maxGougeAnalytic = r - d;
        int i = g.ix(x), j = g.iy(y);
        if (i >= 0 && j >= 0 && i < g.W && j < g.H) { std::uint8_t c = g.v[(size_t)j * g.W + i]; if (c == 2 || c == 3) centreInKeepout++; }
    };
    bool havePos = false; Pt pos{0, 0};
    for (auto& rg : regions) {
        for (int k = 0; k < 7; k++) flagsOr[k] |= rg.flags[k];
        if (rg.paths.empty()) { emptyRegions++; continue; }  // generator produced no moves for this region
        if (havePos) rapidLen += std::hypot(rg.start.first - pos.first, rg.start.second - pos.second);
        // helix entry: tool centre circles the helix centre at radius |start - centre|
        double Rh = std::hypot(rg.start.first - rg.helix.first, rg.start.second - rg.helix.second);
        lg.flushAll(); sw.reset();
        if (Rh > 1e-9) {
            double a0 = std::atan2(rg.start.second - rg.helix.second, rg.start.first - rg.helix.first);
            int n = (int)std::ceil(2 * s5::kPi * Rh / kCell);
            for (int k = 0; k <= n; k++) { double a = a0 + 2 * s5::kPi * k / n; double x = rg.helix.first + Rh * std::cos(a), y = rg.helix.second + Rh * std::sin(a); sw.stamp(x, y); gougeAt(x, y); if (k % 8 == 0) trace.push_back({{x, y}, 3}); }
            helixLen += 2 * s5::kPi * Rh;
        } else { sw.stamp(rg.start.first, rg.start.second); gougeAt(rg.start.first, rg.start.second); }
        lg.q.clear(); lg.head = 0; sw.reset();
        pos = rg.start; havePos = true;
        for (auto& p : rg.paths) {
            if (p.pts.empty()) continue;
            nPts += p.pts.size(); if (p.type >= 0 && p.type < 3) nPaths[p.type]++;
            double gap = std::hypot(p.pts[0].first - pos.first, p.pts[0].second - pos.second);
            // A path that does not start where the previous one ended implies a move to its first point
            // (FreeCAD's post step emits one); it is swept with this path's motion type.
            if (gap > 1e-6) { gaps++; gapLen += gap; maxGap = std::max(maxGap, gap); if (p.type == 0 || p.type == 1) p.pts.insert(p.pts.begin(), pos); else rapidLen += gap; }
            bool contiguous = !lg.q.empty();
            if (p.type != 0 && p.type != 1) {  // retract link: not swept
                for (size_t k = 0; k + 1 < p.pts.size(); k++) rapidLen += std::hypot(p.pts[k + 1].first - p.pts[k].first, p.pts[k + 1].second - p.pts[k].second);
                pos = p.pts.back(); lg.flushAll(); lg.q.clear(); lg.head = 0; continue;
            }
            if (p.type == 1) lg.flushAll();  // attribute removal exactly to cut or link moves
            long long cutBefore = sw.cut;
            if (!contiguous) lg.start(p.pts[0].first, p.pts[0].second);
            gougeAt(p.pts[0].first, p.pts[0].second);
            for (size_t k = 0; k + 1 < p.pts.size(); k++) {
                Pt a = p.pts[k], b = p.pts[k + 1];
                double len = std::hypot(b.first - a.first, b.second - a.second); if (len <= 0) continue;
                double dx = (b.first - a.first) / len, dy = (b.second - a.second) / len;
                int n = (int)std::ceil(len / kCell); double h = len / n;
                for (int s = 0; s < n; s++) {
                    double x = a.first + dx * h * s, y = a.second + dy * h * s;
                    lg.measureReady();
                    float ang = (float)es.angleDeg(g, x, y, dx, dy);
                    (p.type == 0 ? cutS : linkS).push_back({ang, (float)h});
                    double xn = a.first + dx * h * (s + 1), yn = a.second + dy * h * (s + 1);
                    lg.advance(xn, yn, h); gougeAt(xn, yn);
                    if (s % 8 == 0) trace.push_back({{x, y}, p.type});
                }
                (p.type == 0 ? cutLen : linkLen) += len;
            }
            if (p.type == 1) { lg.flushAll(); linkCutCells += sw.cut - cutBefore; }
            pos = p.pts.back();
        }
    }

    lg.flushAll();
    // uncut and gouge accounting
    const double ca = kCell * kCell;
    auto dKeep = edt(keep, g.W, g.H);
    std::vector<std::uint8_t> allowed(g.v.size());
    for (size_t k = 0; k < g.v.size(); k++) allowed[k] = dKeep[k] * kCell >= r + kCell;
    auto dAllowed = edt(allowed, g.W, g.H);
    std::vector<std::uint8_t> air(g.v.size());
    for (size_t k = 0; k < g.v.size(); k++) air[k] = !(g.v[k] == 1 || g.v[k] == 2);
    auto dAir = edt(air, g.W, g.H);
    auto dNotKeep = edt(notKeep, g.W, g.H);
    long long uncut = 0, uncutReach = 0, reachCells = 0; double uncutThick = 0, gougeDepth = 0;
    for (size_t k = 0; k < g.v.size(); k++) {
        bool reach = dAllowed[k] * kCell <= r - kCell;
        if ((g.v[k] == 1 || g.v[k] == 4) && reach) reachCells++;
        if (g.v[k] == 1) { uncut++; if (reach) { uncutReach++; uncutThick = std::max(uncutThick, (double)dAir[k] * kCell); } }
        if (g.v[k] == 3) gougeDepth = std::max(gougeDepth, (double)dNotKeep[k] * kCell - kCell / 2);
    }

    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("case=%s target_deg=%.2f\n", cs->id.c_str(), target);
    printf("  empty_regions=%d\n", emptyRegions);
    printf("  points=%zu regions=%zu paths_cut=%zu paths_link_down=%zu paths_link_retract=%zu implicit_joins=%d join_len=%.3f join_max=%.3f\n", nPts, regions.size(), nPaths[0], nPaths[1], nPaths[2], gaps, gapLen, maxGap);
    printf("  flags(StartPointNotFound,LeadPathFailed,UnexpectedRotate,TooManyFailedEngagements,UnclearedAreaRemains,FailedFinishSetup,FinishLeadInFailed)=%d%d%d%d%d%d%d\n", flagsOr[0], flagsOr[1], flagsOr[2], flagsOr[3], flagsOr[4], flagsOr[5], flagsOr[6]);
    printf("  cut_len=%.1f link_down_len=%.1f rapid_len=%.1f helix_len=%.1f\n", cutLen, linkLen, rapidLen, helixLen);
    printf("  eng_cut: median=%.1f p95=%.1f p99=%.1f max=%.1f pct_above_target=%.2f pct_above_1.1x=%.2f pct_above_1.5x=%.2f pct_above_90deg=%.2f pct_air(<2deg)=%.2f pct_above_t+5deg=%.2f pct_above_t+10deg=%.2f pct_above_t+20deg=%.2f\n",
           wpercentile(cutS, 0.5), wpercentile(cutS, 0.95), wpercentile(cutS, 0.99), wpercentile(cutS, 1.0), wfracAbove(cutS, target), wfracAbove(cutS, 1.1 * target), wfracAbove(cutS, 1.5 * target), wfracAbove(cutS, 90), 100 - wfracAbove(cutS, 2), wfracAbove(cutS, target + 5), wfracAbove(cutS, target + 10), wfracAbove(cutS, target + 20));
    printf("  eng_link_down: max=%.1f pct_len_engaged(>2deg)=%.2f material_removed_mm2=%.3f\n", wpercentile(linkS, 1.0), wfracAbove(linkS, 2), linkCutCells * ca);
    {
        double tot = 0; std::vector<double> bins(18, 0);
        for (auto& s : cutS) { int b = std::min(17, (int)(s.ang / 10)); bins[b] += s.w; tot += s.w; }
        printf("  hist_cut_pct_by_10deg:"); for (double b : bins) printf(" %.1f", tot > 0 ? 100 * b / tot : 0); printf("\n");
    }
    printf("  pocket_area=%.1f reachable_area=%.1f uncut_total=%.2f uncut_reachable=%.3f (%.3f%% of reachable) uncut_reachable_max_thickness=%.2f\n",
           pocketCells * ca, reachCells * ca, uncut * ca, uncutReach * ca, reachCells ? 100.0 * uncutReach / reachCells : 0, uncutThick);
    printf("  gouge_area=%.4f gouge_depth_raster=%.3f gouge_depth_analytic=%.4f centre_in_keepout_steps=%lld\n", sw.gouge * ca, gougeDepth, std::max(0.0, maxGougeAnalytic), centreInKeepout);
    printf("  checker_seconds=%.1f grid=%dx%d cell=%.3f\n", sec, g.W, g.H, kCell);

    if (argc > 4) {
        const int ds = 6; int w = g.W / ds, h = g.H / ds;
        std::vector<std::uint8_t> rgb((size_t)w * h * 3, 255);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int cnt[5] = {0, 0, 0, 0, 0};
            for (int b = 0; b < ds; b++) for (int a = 0; a < ds; a++) cnt[g.v[(size_t)(j * ds + b) * g.W + i * ds + a]]++;
            std::uint8_t c[3] = {255, 255, 255};
            if (cnt[3]) { c[0] = 255; c[1] = 0; c[2] = 255; }
            else if (cnt[1]) { c[0] = 230; c[1] = 30; c[2] = 30; }
            else if (cnt[2] * 2 > ds * ds) { c[0] = c[1] = c[2] = 90; }
            else if (cnt[4] * 2 > ds * ds) { c[0] = c[1] = c[2] = 215; }
            std::memcpy(&rgb[((size_t)(h - 1 - j) * w + i) * 3], c, 3);
        }
        for (auto& t : trace) {
            int i = g.ix(t.first.first) / ds, j = g.iy(t.first.second) / ds; if (i < 0 || j < 0 || i >= w || j >= h) continue;
            std::uint8_t* c = &rgb[((size_t)(h - 1 - j) * w + i) * 3];
            if (t.second == 0) { c[0] = 0; c[1] = 60; c[2] = 220; } else if (t.second == 1) { c[0] = 0; c[1] = 170; c[2] = 0; } else { c[0] = 255; c[1] = 140; c[2] = 0; }
        }
        writePng(argv[4], w, h, rgb);
    }
    return 0;
}
