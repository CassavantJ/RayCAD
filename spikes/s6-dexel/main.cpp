// Spike S6: CPU tri-dexel stock model prototype. Throwaway code, standard library + Win32 only.
// Usage: s6dexel <cellMm> [full|quick]
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

static const double PI = 3.14159265358979323846;
static const float BIG = 1e30f;
static std::atomic<long long> g_overflowBytes{0};

// ---------------------------------------------------------------- dexel
// One ray. Sorted, disjoint material intervals [lo,hi] as floats. Two intervals inline, more on the heap.
struct Dexel {
    std::uint32_t n;
    std::uint32_t cap;
    union { float inl[4]; float* ptr; };
    float* data() { return cap <= 2 ? inl : ptr; }
    const float* data() const { return cap <= 2 ? inl : ptr; }
};
static_assert(sizeof(Dexel) == 24, "dexel size");

static void growDexel(Dexel& d) {
    std::uint32_t nc = d.cap * 2;
    float* np = static_cast<float*>(std::malloc(sizeof(float) * 2 * nc));
    std::memcpy(np, d.data(), sizeof(float) * 2 * d.n);
    long long delta = static_cast<long long>(sizeof(float)) * 2 * nc;
    if (d.cap > 2) { std::free(d.ptr); delta -= static_cast<long long>(sizeof(float)) * 2 * d.cap; }
    d.ptr = np; d.cap = nc;
    g_overflowBytes.fetch_add(delta, std::memory_order_relaxed);
}

// Remove [a,b] from the dexel. Returns removed length.
static inline float subtractInterval(Dexel& d, float a, float b) {
    std::uint32_t n = d.n;
    if (!n) return 0.f;
    float* p = d.data();
    if (p[2 * n - 1] <= a || p[0] >= b) return 0.f;
    std::uint32_t i = 0;
    while (i < n && p[2 * i + 1] <= a) ++i;
    std::uint32_t j = i;
    while (j < n && p[2 * j] < b) ++j;
    if (i == j) return 0.f;
    float removed = 0.f;
    for (std::uint32_t k = i; k < j; ++k) removed += std::min(p[2 * k + 1], b) - std::max(p[2 * k], a);
    const float lo0 = p[2 * i], hi1 = p[2 * j - 1];
    const bool keepL = lo0 < a, keepR = hi1 > b;
    const std::uint32_t keep = (keepL ? 1u : 0u) + (keepR ? 1u : 0u), span = j - i;
    if (keep > span) {                       // split one interval into two
        if (n + 1 > d.cap) { growDexel(d); p = d.data(); }
        std::memmove(p + 2 * (j + 1), p + 2 * j, sizeof(float) * 2 * (n - j));
        p[2 * i + 1] = a; p[2 * i + 2] = b; p[2 * i + 3] = hi1;
        d.n = n + 1;
    } else {
        std::uint32_t w = i;
        if (keepL) { p[2 * w] = lo0; p[2 * w + 1] = a; ++w; }
        if (keepR) { p[2 * w] = b; p[2 * w + 1] = hi1; ++w; }
        if (w < j) std::memmove(p + 2 * w, p + 2 * j, sizeof(float) * 2 * (n - j));
        d.n = n - span + keep;
    }
    return removed;
}

// ---------------------------------------------------------------- grids
struct Grid {
    int nu = 0, nv = 0;            // dexel (u,v) at index u*nv+v. u is the strip (threading) axis.
    double cu = 0, cv = 0;         // cell size along u and v (stock extent / cell count, so the stock is spanned exactly)
    std::vector<Dexel> d;
    Grid() = default;
    Grid(const Grid&) = delete;
    Grid& operator=(const Grid&) = delete;
    ~Grid() { clear(); }
    void clear() {
        for (auto& x : d) if (x.cap > 2) { g_overflowBytes.fetch_sub(8ll * x.cap); std::free(x.ptr); }
        d.clear();
    }
    void init(int nu_, int nv_, double cu_, double cv_, float lo, float hi) {
        clear(); nu = nu_; nv = nv_; cu = cu_; cv = cv_;
        Dexel e; e.n = 1; e.cap = 2; e.inl[0] = lo; e.inl[1] = hi; e.inl[2] = 0; e.inl[3] = 0;
        d.assign(static_cast<size_t>(nu) * nv, e);
    }
    void cloneFrom(const Grid& o) {
        clear(); nu = o.nu; nv = o.nv; cu = o.cu; cv = o.cv; d = o.d;
        for (auto& x : d) if (x.cap > 2) {
            float* np = static_cast<float*>(std::malloc(8ull * x.cap));
            std::memcpy(np, x.ptr, 8ull * x.n); x.ptr = np; g_overflowBytes.fetch_add(8ll * x.cap);
        }
    }
    double volume() const {
        double s = 0;
        for (const auto& x : d) { const float* p = x.data(); for (std::uint32_t k = 0; k < x.n; ++k) s += double(p[2 * k + 1]) - double(p[2 * k]); }
        return s * cu * cv;
    }
    size_t intervals() const { size_t s = 0; for (const auto& x : d) s += x.n; return s; }
    std::uint32_t maxIntervals() const { std::uint32_t s = 0; for (const auto& x : d) s = std::max(s, x.n); return s; }
    void hash(std::uint64_t& h) const {
        for (const auto& x : d) {
            h = (h ^ x.n) * 1099511628211ull;
            const float* p = x.data();
            for (std::uint32_t k = 0; k < 2 * x.n; ++k) { std::uint32_t w; std::memcpy(&w, p + k, 4); h = (h ^ w) * 1099511628211ull; }
        }
    }
};

struct Stock {
    double sx, sy, sz, cell;       // cell = requested size; cx, cy, cz = actual per-axis size (<= cell)
    int nx, ny, nz;
    double cx, cy, cz;
    Grid gz;   // rays along Z, u = y, v = x
    Grid gx;   // rays along X, u = y, v = z
    Grid gy;   // rays along Y, u = x, v = z
    Stock(double sx_, double sy_, double sz_, double cell_) : sx(sx_), sy(sy_), sz(sz_), cell(cell_) {
        nx = static_cast<int>(std::ceil(sx / cell - 1e-9)); ny = static_cast<int>(std::ceil(sy / cell - 1e-9)); nz = static_cast<int>(std::ceil(sz / cell - 1e-9));
        cx = sx / nx; cy = sy / ny; cz = sz / nz;
        gz.init(ny, nx, cy, cx, 0.f, float(sz));
        gx.init(ny, nz, cy, cz, 0.f, float(sx));
        gy.init(nx, nz, cx, cz, 0.f, float(sy));
    }
    std::uint64_t hash() const { std::uint64_t h = 1469598103934665603ull; gz.hash(h); gx.hash(h); gy.hash(h); return h; }
    size_t dexels() const { return gz.d.size() + gx.d.size() + gy.d.size(); }
    size_t baseBytes() const { return dexels() * sizeof(Dexel); }
};

struct Tool { double R; bool ball; };
struct Move { double ax, ay, az, bx, by, bz; int tool; };

static inline int firstIdx(double lo, double cell) { return static_cast<int>(std::ceil(lo / cell - 0.5)); }
static inline int lastIdx(double hi, double cell) { return static_cast<int>(std::floor(hi / cell - 0.5)); }

// ---------------------------------------------------------------- geometry
// Intersection of the line y = c with the 2D stadium (segment A-B thickened by R). Interval along x.
static inline bool stadiumLine(double ax, double ay, double bx, double by, double R, double c, double& lo, double& hi) {
    lo = 1e300; hi = -1e300;
    const double da = c - ay, db = c - by;
    if (std::fabs(da) <= R) { double s = std::sqrt(R * R - da * da); lo = ax - s; hi = ax + s; }
    if (std::fabs(db) <= R) { double s = std::sqrt(R * R - db * db); lo = std::min(lo, bx - s); hi = std::max(hi, bx + s); }
    const double dx = bx - ax, dy = by - ay;
    if (dy != 0.0) {
        const double len = std::sqrt(dx * dx + dy * dy), len2 = dx * dx + dy * dy;
        double x0 = (da * dx - R * len) / dy, x1 = (da * dx + R * len) / dy;
        if (x0 > x1) std::swap(x0, x1);
        bool ok = true;
        if (dx != 0.0) {
            double y0 = (0.0 - da * dy) / dx, y1 = (len2 - da * dy) / dx;
            if (y0 > y1) std::swap(y0, y1);
            x0 = std::max(x0, y0); x1 = std::min(x1, y1);
        } else {
            const double t = da * dy; ok = (t >= 0.0 && t <= len2);
        }
        if (ok && x0 <= x1) { lo = std::min(lo, ax + x0); hi = std::max(hi, ax + x1); }
    }
    return lo <= hi;
}

// Ray along coordinate r at fixed (a,b) against the capsule with axis C0 -> C0+D, radius R. Hull is added to [lo,hi].
static inline bool capsuleRay(double c0r, double c0a, double c0b, double dr, double da, double db,
                              double pa, double pb, double R, double& lo, double& hi) {
    bool hit = false;
    const double wa = pa - c0a, wb = pb - c0b;
    const double q0 = R * R - wa * wa - wb * wb;
    if (q0 >= 0) { double s = std::sqrt(q0); lo = std::min(lo, c0r - s); hi = std::max(hi, c0r + s); hit = true; }
    const double wa1 = wa - da, wb1 = wb - db;
    const double q1 = R * R - wa1 * wa1 - wb1 * wb1;
    if (q1 >= 0) { double s = std::sqrt(q1); lo = std::min(lo, c0r + dr - s); hi = std::max(hi, c0r + dr + s); hit = true; }
    const double dd = dr * dr + da * da + db * db;
    if (dd > 0) {
        const double A = 1.0 - dr * dr / dd;
        if (A > 1e-12) {
            const double m = wa * da + wb * db;
            const double Bh = -(m * dr) / dd;
            const double C = wa * wa + wb * wb - m * m / dd - R * R;
            const double disc = Bh * Bh - A * C;
            if (disc >= 0) {
                const double s = std::sqrt(disc);
                double x0 = (-Bh - s) / A, x1 = (-Bh + s) / A;
                if (dr > 0) { x0 = std::max(x0, (0.0 - m) / dr); x1 = std::min(x1, (dd - m) / dr); }
                else if (dr < 0) { x0 = std::max(x0, (dd - m) / dr); x1 = std::min(x1, (0.0 - m) / dr); }
                else if (m < 0 || m > dd) { x0 = 1; x1 = 0; }
                if (x0 <= x1) { lo = std::min(lo, c0r + x0); hi = std::max(hi, c0r + x1); hit = true; }
            }
        }
    }
    return hit;
}

// ---------------------------------------------------------------- Z grid scan (rays along the tool axis)
// For every Z dexel under the swept footprint, calls op(dexel, zlow) where zlow is the lowest point of the
// swept tool over that ray. The tool is treated as unbounded upwards (flute + shank clear everything above).
// op returns true to stop early (used by the read-only query).
template <class DexelT, class Op>
static bool scanZ(DexelT* cells, int nv, int nu, double cu, double cv, const Move& m, const Tool& t, int j0s, int j1s, double ztop, Op&& op) {
    const double R = t.R;
    if (std::min(m.az, m.bz) >= ztop) return false;
    int j0 = std::max(j0s, firstIdx(std::min(m.ay, m.by) - R, cu));
    int j1 = std::min(std::min(j1s, nu) - 1, lastIdx(std::max(m.ay, m.by) + R, cu));
    const double dx = m.bx - m.ax, dy = m.by - m.ay, dz = m.bz - m.az;
    const double d2 = dx * dx + dy * dy;
    const bool horizontal = (dz == 0.0);
    for (int j = j0; j <= j1; ++j) {
        const double y = (j + 0.5) * cu;
        double lo, hi;
        if (!stadiumLine(m.ax, m.ay, m.bx, m.by, R, y, lo, hi)) continue;
        const int i0 = std::max(0, firstIdx(lo, cv)), i1 = std::min(nv - 1, lastIdx(hi, cv));
        DexelT* row = cells + static_cast<size_t>(j) * nv;
        if (!t.ball && horizontal) {
            for (int i = i0; i <= i1; ++i) if (op(row[i], m.az)) return true;
        } else if (!t.ball) {
            const double wy = y - m.ay;
            for (int i = i0; i <= i1; ++i) {
                if (!row[i].n) continue;
                const double wx = (i + 0.5) * cv - m.ax;
                double tlo = 0, thi = 1;
                if (d2 > 0) {
                    const double b = wx * dx + wy * dy, c = wx * wx + wy * wy - R * R;
                    double disc = b * b - d2 * c; if (disc < 0) disc = 0;
                    const double s = std::sqrt(disc);
                    tlo = std::max(0.0, (b - s) / d2); thi = std::min(1.0, (b + s) / d2);
                    if (tlo > thi) continue;
                }
                if (op(row[i], m.az + dz * (dz < 0 ? thi : tlo))) return true;
            }
        } else {
            for (int i = i0; i <= i1; ++i) {
                if (!row[i].n) continue;
                const double x = (i + 0.5) * cv;
                double zl = 1e300, zh = -1e300;
                // permuted coordinates: ray = z, a = x, b = y. Sphere centre is R above the tip.
                if (!capsuleRay(m.az + R, m.ax, m.ay, dz, dx, dy, x, y, R, zl, zh)) continue;
                if (op(row[i], zl)) return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- lateral grids (rays perpendicular to the tool axis)
// Move is given in permuted coordinates: r = ray axis, u = strip axis, z = tool axis. Grid index = u*nz + k.
static void cutLateral(Grid& g, double ar, double au, double az, double br, double bu, double bz, const Tool& t, int j0s, int j1s) {
    const double R = t.R, cu = g.cu, cz = g.cv;
    const int nz = g.nv;
    const double zmin = std::min(az, bz);
    const int k0 = std::max(0, firstIdx(zmin, cz));
    if (k0 >= nz) return;
    const int j0 = std::max(j0s, firstIdx(std::min(au, bu) - R, cu));
    const int j1 = std::min(std::min(j1s, g.nu) - 1, lastIdx(std::max(au, bu) + R, cu));
    const double dr = br - ar, du = bu - au, dz = bz - az;
    const double h0 = t.ball ? R : 0.0;          // height of the cylindrical part above the tip
    for (int j = j0; j <= j1; ++j) {
        const double u = (j + 0.5) * cu;
        Dexel* col = g.d.data() + static_cast<size_t>(j) * nz;
        if (dz == 0.0) {
            double lo, hi;
            if (!stadiumLine(ar, au, br, bu, R, u, lo, hi)) continue;
            const float flo = float(lo), fhi = float(hi);
            int kc = k0;
            if (t.ball) {
                kc = std::min(nz, std::max(k0, firstIdx(az + R, cz)));
                for (int k = k0; k < kc; ++k) {
                    if (!col[k].n) continue;
                    double l = 1e300, h = -1e300;
                    if (capsuleRay(ar, au, az + R, dr, du, 0.0, u, (k + 0.5) * cz, R, l, h)) subtractInterval(col[k], float(l), float(h));
                }
            }
            for (int k = kc; k < nz; ++k) subtractInterval(col[k], flo, fhi);
        } else {
            const double zc = std::max(az, bz) + R;
            for (int k = k0; k < nz; ++k) {
                if (!col[k].n) continue;
                const double z = (k + 0.5) * cz;
                double lo = 1e300, hi = -1e300;
                // cylindrical part: positions t where the cylinder base is at or below z
                double t0 = 0, t1 = 1;
                const double tz = (z - az - h0) / dz;
                if (dz > 0) t1 = std::min(1.0, tz); else t0 = std::max(0.0, tz);
                if (t0 <= t1) {
                    double l, h;
                    if (stadiumLine(ar + t0 * dr, au + t0 * du, ar + t1 * dr, au + t1 * du, R, u, l, h)) { lo = l; hi = h; }
                }
                if (t.ball && z < zc) capsuleRay(ar, au, az + R, dr, du, dz, u, z, R, lo, hi);
                if (lo <= hi) subtractInterval(col[k], float(lo), float(hi));
            }
        }
    }
}

// ---------------------------------------------------------------- thread pool
class Pool {
    std::vector<std::thread> th_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    std::function<void(int)> job_;
    int gen_ = 0, running_ = 0;
    bool stop_ = false;
public:
    explicit Pool(int n) {
        for (int i = 0; i < n; ++i) th_.emplace_back([this, i] {
            int seen = 0;
            for (;;) {
                std::function<void(int)> f;
                { std::unique_lock<std::mutex> l(m_); cv_.wait(l, [&] { return stop_ || gen_ != seen; }); if (stop_) return; seen = gen_; f = job_; }
                f(i);
                { std::lock_guard<std::mutex> l(m_); if (--running_ == 0) done_.notify_one(); }
            }
        });
    }
    ~Pool() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); for (auto& t : th_) t.join(); }
    int size() const { return static_cast<int>(th_.size()); }
    void run(const std::function<void(int)>& f) {
        if (th_.empty()) { f(0); return; }
        { std::lock_guard<std::mutex> l(m_); job_ = f; ++gen_; running_ = static_cast<int>(th_.size()); }
        cv_.notify_all();
        std::unique_lock<std::mutex> l(m_); done_.wait(l, [&] { return running_ == 0; });
    }
};

// ---------------------------------------------------------------- batched cutter
// Moves are applied in batches. Each grid is cut in fixed strips of rows; a strip applies the batch's moves
// in program order to its own dexels only, so the result does not depend on the thread count or batch size.
class Cutter {
    struct Task { int grid, u0, u1; std::vector<std::uint32_t> list; };
    Stock& s_;
    std::vector<Tool> tools_;
    Pool pool_;
    int nthreads_, mask_, stripRows_;
    std::vector<Task> tasks_;
    int firstTask_[3] = {0, 0, 0}, nStrips_[3] = {0, 0, 0};
    std::vector<std::vector<long long>> vol_;      // per thread, per move in batch, fixed-point mm (2^-20)
public:
    Cutter(Stock& s, std::vector<Tool> tools, int nthreads, int mask)
        : s_(s), tools_(std::move(tools)), pool_(nthreads > 1 ? nthreads : 0), nthreads_(nthreads), mask_(mask) {
        stripRows_ = std::max(1, static_cast<int>(std::lround(2.0 / s.cell)));
        const int nu[3] = {s.gz.nu, s.gx.nu, s.gy.nu};
        for (int g = 0; g < 3; ++g) {
            firstTask_[g] = static_cast<int>(tasks_.size());
            nStrips_[g] = (nu[g] + stripRows_ - 1) / stripRows_;
            for (int k = 0; k < nStrips_[g]; ++k) tasks_.push_back({g, k * stripRows_, std::min(nu[g], (k + 1) * stripRows_), {}});
        }
        vol_.resize(nthreads);
    }
    // volOut (optional): removed volume per move in mm^3, from the Z grid.
    void cutBatch(const Move* mv, size_t n, double* volOut) {
        for (auto& t : tasks_) t.list.clear();
        for (size_t i = 0; i < n; ++i) {
            const Move& m = mv[i]; const double R = tools_[m.tool].R;
            if (std::min(m.az, m.bz) >= s_.sz) continue;
            if (std::max(m.ax, m.bx) + R < 0 || std::min(m.ax, m.bx) - R > s_.sx || std::max(m.ay, m.by) + R < 0 || std::min(m.ay, m.by) - R > s_.sy) continue;
            const double ulo[3] = {std::min(m.ay, m.by) - R, std::min(m.ay, m.by) - R, std::min(m.ax, m.bx) - R};
            const double uhi[3] = {std::max(m.ay, m.by) + R, std::max(m.ay, m.by) + R, std::max(m.ax, m.bx) + R};
            for (int g = 0; g < 3; ++g) {
                if (!(mask_ & (1 << g))) continue;
                const double cu = g == 2 ? s_.cx : s_.cy;
                const int a = std::max(0, firstIdx(ulo[g], cu)) / stripRows_;
                const int b = std::min(nStrips_[g] - 1, lastIdx(uhi[g], cu) / stripRows_);
                for (int k = a; k <= b; ++k) tasks_[firstTask_[g] + k].list.push_back(static_cast<std::uint32_t>(i));
            }
        }
        std::vector<int> live;
        for (int i = 0; i < static_cast<int>(tasks_.size()); ++i) if (!tasks_[i].list.empty()) live.push_back(i);
        for (auto& v : vol_) v.assign(n, 0);
        std::atomic<int> next{0};
        pool_.run([&](int tid) {
            long long* vol = vol_[tid].data();
            for (;;) {
                const int q = next.fetch_add(1, std::memory_order_relaxed);
                if (q >= static_cast<int>(live.size())) break;
                Task& t = tasks_[live[q]];
                for (std::uint32_t mi : t.list) {
                    const Move& m = mv[mi]; const Tool& tool = tools_[m.tool];
                    if (t.grid == 0) {
                        double removed = 0;
                        scanZ(s_.gz.d.data(), s_.gz.nv, s_.gz.nu, s_.cy, s_.cx, m, tool, t.u0, t.u1, s_.sz,
                              [&](Dexel& d, double zl) { removed += subtractInterval(d, float(zl), BIG); return false; });
                        vol[mi] += static_cast<long long>(removed * 1048576.0 + 0.5);
                    } else if (t.grid == 1) {
                        cutLateral(s_.gx, m.ax, m.ay, m.az, m.bx, m.by, m.bz, tool, t.u0, t.u1);
                    } else {
                        cutLateral(s_.gy, m.ay, m.ax, m.az, m.by, m.bx, m.bz, tool, t.u0, t.u1);
                    }
                }
            }
        });
        if (volOut) for (size_t i = 0; i < n; ++i) { long long v = 0; for (auto& tv : vol_) v += tv[i]; volOut[i] = double(v) / 1048576.0 * s_.cx * s_.cy; }
    }
};

// Read-only query: does the swept tool touch remaining stock? Z grid only (enough for a tool along Z).
static bool intersectsZ(const Stock& s, const Move& m, const Tool& t) {
    if (std::max(m.ax, m.bx) + t.R < 0 || std::min(m.ax, m.bx) - t.R > s.sx) return false;
    return scanZ(s.gz.d.data(), s.gz.nv, s.gz.nu, s.cy, s.cx, m, t, 0, s.gz.nu, s.sz,
                 [](const Dexel& d, double zl) { return d.n && d.data()[2 * d.n - 1] > float(zl); });
}

// ---------------------------------------------------------------- display mesh (heightfield from the Z grid)
struct Vtx { float x, y, z, nx, ny, nz; };
static void meshRegion(const Stock& s, int i0, int i1, int j0, int j1, int threads, std::vector<Vtx>& v, std::vector<std::uint32_t>& idx) {
    const int w = i1 - i0 + 1, h = j1 - j0 + 1;
    v.resize(static_cast<size_t>(w) * h);
    idx.resize(static_cast<size_t>(w - 1) * (h - 1) * 6);
    const Grid& g = s.gz;
    auto H = [&](int i, int j) -> float {
        i = std::clamp(i, 0, g.nv - 1); j = std::clamp(j, 0, g.nu - 1);
        const Dexel& d = g.d[static_cast<size_t>(j) * g.nv + i];
        return d.n ? d.data()[2 * d.n - 1] : 0.f;
    };
    auto rows = [&](int ja, int jb) {
        const float cx = float(s.cx), cy = float(s.cy), invx = 1.f / (2.f * cx), invy = 1.f / (2.f * cy);
        for (int j = ja; j < jb; ++j) for (int i = 0; i < w; ++i) {
            const int gi = i0 + i, gj = j0 + j;
            Vtx& p = v[static_cast<size_t>(j) * w + i];
            p.x = (gi + 0.5f) * cx; p.y = (gj + 0.5f) * cy; p.z = H(gi, gj);
            const float gxn = (H(gi + 1, gj) - H(gi - 1, gj)) * invx, gyn = (H(gi, gj + 1) - H(gi, gj - 1)) * invy;
            const float l = 1.f / std::sqrt(gxn * gxn + gyn * gyn + 1.f);
            p.nx = -gxn * l; p.ny = -gyn * l; p.nz = l;
            if (j < h - 1 && i < w - 1) {
                std::uint32_t* q = idx.data() + (static_cast<size_t>(j) * (w - 1) + i) * 6;
                const std::uint32_t a = std::uint32_t(j) * w + i, b = a + 1, cc = a + w, dd = cc + 1;
                q[0] = a; q[1] = b; q[2] = dd; q[3] = a; q[4] = dd; q[5] = cc;
            }
        }
    };
    if (threads <= 1) { rows(0, h); return; }
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) th.emplace_back(rows, h * t / threads, h * (t + 1) / threads);
    for (auto& t : th) t.join();
}

// ---------------------------------------------------------------- workload
struct Lcg { std::uint32_t s; double next() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0 / 16777216.0); } double seg() { return 0.5 + 1.5 * next(); } };

struct Workload { std::vector<Move> moves; size_t nFacing = 0, nAdaptive = 0, nBall = 0, nSloped = 0; double length = 0; };

static Workload makeWorkload() {
    Workload w; Lcg rng{12345u};
    double px = 0, py = 0, pz = 0; bool have = false; int tool = 0;
    auto jump = [&](double x, double y, double z) { px = x; py = y; pz = z; have = true; };
    auto to = [&](double x, double y, double z) {
        if (have) { w.moves.push_back({px, py, pz, x, y, z, tool}); w.length += std::sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz)); if (z != pz) ++w.nSloped; }
        px = x; py = y; pz = z; have = true;
    };
    // 1. Facing, 0.5 in flat, 1 mm deep, 60 % stepover, long zigzag moves.
    tool = 0;
    { const double z = 24.4; bool fwd = true; jump(-8, 2.0, z);
      for (double y = 2.0; y < 101.6; y += 7.62) { to(fwd ? -8 : 160.4, y, z); to(fwd ? 160.4 : -8, y, z); fwd = !fwd; } }
    w.nFacing = w.moves.size();
    // 2. Adaptive-like pocket 90 x 80 mm, six 3 mm levels, trochoidal loops of radius 3 mm, moves 0.5..2 mm.
    { const double rho = 3.0, adv = 0.95, R = 6.35;
      const double x0 = 10 + R + rho, x1 = 100 - R - rho, y0 = 10 + R + rho, y1 = 90 - R - rho;
      std::vector<std::pair<double, double>> spine; bool fwd = true; const int lanes = 11;
      for (int l = 0; l < lanes; ++l) { const double y = y0 + (y1 - y0) * l / (lanes - 1); spine.push_back({fwd ? x0 : x1, y}); spine.push_back({fwd ? x1 : x0, y}); fwd = !fwd; }
      for (int level = 1; level <= 6; ++level) {
          const double z = 24.4 - 3.0 * level; double th = 0;
          const double cx = spine[0].first, cy = spine[0].second;
          jump(cx + rho, cy, z + 3.0);
          const double helixLen = 2 * 2 * PI * rho; double done = 0;       // two-turn helical ramp
          while (done < helixLen) { const double sl = rng.seg(); done = std::min(helixLen, done + sl); th = done / rho; to(cx + rho * std::cos(th), cy + rho * std::sin(th), z + 3.0 * (1 - done / helixLen)); }
          for (size_t k = 0; k + 1 < spine.size(); ++k) {
              const double ax = spine[k].first, ay = spine[k].second, bx = spine[k + 1].first, by = spine[k + 1].second;
              const double len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)); double sdone = 0;
              while (sdone < len) { const double sl = rng.seg(); th += sl / rho; sdone = std::min(len, sdone + adv * sl / (2 * PI * rho));
                  const double f = sdone / len; to(ax + (bx - ax) * f + rho * std::cos(th), ay + (by - ay) * f + rho * std::sin(th), z); }
          }
      } }
    w.nAdaptive = w.moves.size() - w.nFacing;
    // 3. Ball-mill scallop passes, 0.25 in ball, 0.25 mm stepover, points 0.5..2 mm apart on a wavy surface.
    tool = 1;
    { have = false; bool fwd = true;                                         // (a) wavy surface cut from the faced top
      auto zf = [](double x, double y) { return 22.9 + 1.5 * std::sin(2 * PI * x / 38.0) * std::cos(2 * PI * y / 45.0); };
      for (double x = 108.0; x <= 146.0; x += 0.25) { double y = fwd ? 6.0 : 96.0; to(x, y, zf(x, y));
          for (;;) { const double sl = rng.seg(); y += fwd ? sl : -sl; const bool end = fwd ? y >= 96.0 : y <= 6.0; if (end) y = fwd ? 96.0 : 6.0; to(x, y, zf(x, y)); if (end) break; }
          fwd = !fwd; } }
    { have = false; bool fwd = true;                                         // (b) wavy pocket floor
      auto zf = [](double x, double y) { return 5.6 + 0.6 * std::sin(2 * PI * x / 30.0) * std::cos(2 * PI * y / 25.0); };
      const double xa = 13.4, xb = 96.6;
      for (double y = 13.4; y <= 86.6; y += 0.25) { double x = fwd ? xa : xb; to(x, y, zf(x, y));
          for (;;) { const double sl = rng.seg(); x += fwd ? sl : -sl; const bool end = fwd ? x >= xb : x <= xa; if (end) x = fwd ? xb : xa; to(x, y, zf(x, y)); if (end) break; }
          fwd = !fwd; } }
    w.nBall = w.moves.size() - w.nFacing - w.nAdaptive;
    return w;
}

// ---------------------------------------------------------------- helpers
static double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double peakWsMb() { PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)); return double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0); }
static double curWsMb() { PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)); return double(pmc.WorkingSetSize) / (1024.0 * 1024.0); }

static const double SX = 152.4, SY = 101.6, SZ = 25.4;
static const std::vector<Tool> TOOLS = {{6.35, false}, {3.175, true}};

static void accuracy(double cell) {
    std::printf("\n== accuracy (cell %.3g mm): removed volume vs analytic, percent error per grid ==\n", cell);
    Stock s(SX, SY, SZ, cell);
    Cutter c(s, TOOLS, 4, 7);
    const double Rf = 6.35, Rb = 3.175, L = 90.0, ca = std::cos(15 * PI / 180), sa = std::sin(15 * PI / 180);
    struct Case { const char* name; Move m; double exact; };
    const double slotV = (2 * Rf * L + PI * Rf * Rf) * SZ;
    const double grooveV = (PI * Rb * Rb / 2 + 2 * Rb * (5.0 - Rb)) * L + (2.0 / 3.0) * PI * Rb * Rb * Rb + PI * Rb * Rb * (5.0 - Rb);
    const Case cases[] = {
        {"a  slot, flat 12.7, full depth, along X", {30.13, 20.77, -1, 120.13, 20.77, -1, 0}, slotV},
        {"a' slot, flat 12.7, full depth, 15 deg ", {30.13, 60.77, -1, 30.13 + L * ca, 60.77 + L * sa, -1, 0}, slotV},
        {"b  groove, ball 6.35, 5 deep, along X  ", {30.13, 40.77, 20.4, 120.13, 40.77, 20.4, 1}, grooveV},
        {"b' groove, ball 6.35, 5 deep, 15 deg   ", {33.24, 49.18, 20.4, 33.24 + L * ca, 49.18 + L * sa, 20.4, 1}, grooveV},
        {"c  plunge, flat 12.7, 10 deep          ", {140.07, 20.03, 30, 140.07, 20.03, 15.4, 0}, PI * Rf * Rf * 10.0},
        {"c' plunge, ball 6.35, 10 deep          ", {140.07, 45.03, 30, 140.07, 45.03, 15.4, 1}, (2.0 / 3.0) * PI * Rb * Rb * Rb + PI * Rb * Rb * (10.0 - Rb)},
    };
    double vz = s.gz.volume(), vx = s.gx.volume(), vy = s.gy.volume();
    for (const Case& k : cases) {
        double vol = 0; c.cutBatch(&k.m, 1, &vol);
        const double nz = s.gz.volume(), nx = s.gx.volume(), ny = s.gy.volume();
        const double rz = vz - nz, rx = vx - nx, ry = vy - ny;
        std::printf("%s exact %10.3f mm3 | Z %+8.4f%% | X %+8.4f%% | Y %+8.4f%% | cut() return %+8.4f%%\n", k.name, k.exact,
                    100 * (rz - k.exact) / k.exact, 100 * (rx - k.exact) / k.exact, 100 * (ry - k.exact) / k.exact, 100 * (vol - k.exact) / k.exact);
        vz = nz; vx = nx; vy = ny;
    }
}

struct RunResult { double seconds; std::uint64_t hash; };
static RunResult runWorkload(Stock& s, const Workload& w, int threads, int mask, size_t batch, double* volSum) {
    Cutter c(s, TOOLS, threads, mask);
    std::vector<double> vol(batch);
    double sum = 0;
    const double t0 = nowS();
    for (size_t i = 0; i < w.moves.size(); i += batch) {
        const size_t n = std::min(batch, w.moves.size() - i);
        c.cutBatch(w.moves.data() + i, n, vol.data());
        for (size_t k = 0; k < n; ++k) sum += vol[k];
    }
    const double t1 = nowS();
    if (volSum) *volSum = sum;
    return {t1 - t0, s.hash()};
}

int main(int argc, char** argv) {
    const double cell = argc > 1 ? std::atof(argv[1]) : 0.5;
    const bool quick = argc > 2 && std::strcmp(argv[2], "quick") == 0;
    // Pin the process to four physical P-cores (logical 0,2,4,6 on the i9-13900K): "exactly 4 threads", no SMT, no E-cores.
    if (!SetProcessAffinityMask(GetCurrentProcess(), 0x55)) std::printf("WARNING: affinity not set\n");
    std::printf("S6 tri-dexel spike. cell %.3g mm, stock %.1f x %.1f x %.1f mm, affinity mask 0x55\n", cell, SX, SY, SZ);

    { Stock t(SX, SY, SZ, cell); std::printf("grid %d x %d x %d cells, actual cell %.5f x %.5f x %.5f mm\n", t.nx, t.ny, t.nz, t.cx, t.cy, t.cz); }
    accuracy(cell);

    const Workload w = makeWorkload();
    std::printf("\n== workload ==\nmoves %zu (facing %zu, adaptive-like %zu, ball scallop %zu), sloped moves %zu, path length %.0f mm, mean move %.3f mm\n",
                w.moves.size(), w.nFacing, w.nAdaptive, w.nBall, w.nSloped, w.length, w.length / w.moves.size());

    std::printf("\n== throughput ==\n");
    struct Cfg { const char* name; int threads, mask; size_t batch; };
    std::vector<Cfg> cfgs = {
        {"tri-dexel 1 thread  batch 512", 1, 7, 512},
        {"tri-dexel 4 threads batch 512", 4, 7, 512},
        {"tri-dexel 4 threads batch 512 (repeat)", 4, 7, 512},
        {"tri-dexel 4 threads batch 64", 4, 7, 64},
        {"tri-dexel 4 threads batch 1", 4, 7, 1},
        {"tri-dexel 1 thread  batch 1", 1, 7, 1},
        {"Z grid only 1 thread  batch 512", 1, 1, 512},
        {"Z grid only 4 threads batch 512", 4, 1, 512},
    };
    if (quick) cfgs.resize(2);
    const bool t8 = argc > 2 && std::strcmp(argv[2], "t8") == 0;   // extra data point: 8 threads on 8 P-cores
    if (t8) cfgs = {{"tri-dexel 1 thread  batch 512", 1, 7, 512}, {"tri-dexel 4 threads batch 512", 4, 7, 512}, {"tri-dexel 8 threads batch 512 (8 P-cores)", 8, 7, 512}};
    std::uint64_t refHash = 0; bool allSame = true;
    const int reps = argc > 3 ? std::atoi(argv[3]) : (cell >= 0.2 ? 5 : 1);
    std::printf("each line is the best of %d run(s)\n", reps);
    std::unique_ptr<Stock> keep;
    for (size_t ci = 0; ci < cfgs.size(); ++ci) {
        const Cfg& c = cfgs[ci];
        SetProcessAffinityMask(GetCurrentProcess(), c.threads == 8 ? 0x5555 : 0x55);
        auto s = std::make_unique<Stock>(SX, SY, SZ, cell);
        const double v0 = s->gz.volume(), v0x = s->gx.volume(), v0y = s->gy.volume();
        double volSum = 0;
        RunResult r = runWorkload(*s, w, c.threads, c.mask, c.batch, &volSum);
        for (int rep = 1; rep < reps; ++rep) {        // best of `reps`: other processes share this machine
            Stock s2(SX, SY, SZ, cell);
            const RunResult r2 = runWorkload(s2, w, c.threads, c.mask, c.batch, nullptr);
            if (r2.hash != r.hash) allSame = false;
            r.seconds = std::min(r.seconds, r2.seconds);
        }
        std::printf("%-40s %9.3f s  %10.0f moves/s  hash %016llx\n", c.name, r.seconds, w.moves.size() / r.seconds, static_cast<unsigned long long>(r.hash));
        if (c.mask == 7) {
            if (ci == 0) {
                refHash = r.hash;
                const double rz = v0 - s->gz.volume(), rx = v0x - s->gx.volume(), ry = v0y - s->gy.volume();
                std::printf("    removed volume: Z grid %.2f, X grid %.2f (%+.4f%% vs Z), Y grid %.2f (%+.4f%% vs Z), sum of cut() returns %.2f mm3\n",
                            rz, rx, 100 * (rx - rz) / rz, ry, 100 * (ry - rz) / rz, volSum);
                std::printf("    dexels %zu (Z %zu, X %zu, Y %zu); intervals Z %zu X %zu Y %zu; max intervals per dexel Z %u X %u Y %u\n",
                            s->dexels(), s->gz.d.size(), s->gx.d.size(), s->gy.d.size(), s->gz.intervals(), s->gx.intervals(), s->gy.intervals(),
                            s->gz.maxIntervals(), s->gx.maxIntervals(), s->gy.maxIntervals());
                std::printf("    memory: dexel arrays %.1f MB + overflow lists %.1f MB; process peak working set %.1f MB\n",
                            s->baseBytes() / 1048576.0, g_overflowBytes.load() / 1048576.0, peakWsMb());
            } else if (r.hash != refHash) allSame = false;
            if (ci == 1 || cfgs.size() == 1) keep = std::move(s);
        }
    }
    std::printf("determinism: all full tri-dexel runs (1 thread, 4 threads, repeat, other batch sizes) hash identical: %s\n", allSame ? "YES" : "NO");
    std::printf("process peak working set after throughput runs: %.1f MB\n", peakWsMb());
    if (!keep) return 0;
    Stock& s = *keep;

    std::printf("\n== clone (timeline keyframe) ==\n");
    { double best = 1e9; std::uint64_t h = 0;
      for (int k = 0; k < 3; ++k) { const double t0 = nowS(); Stock c2(SX, SY, SZ, cell); const double t1 = nowS(); c2.gz.cloneFrom(s.gz); c2.gx.cloneFrom(s.gx); c2.gy.cloneFrom(s.gy); const double t2 = nowS(); best = std::min(best, t2 - t1); h = c2.hash(); (void)t0; }
      std::printf("deep copy of the cut stock: %.2f ms (best of 3), hash equal: %s, size %.1f MB\n", best * 1e3, h == s.hash() ? "YES" : "NO", (s.baseBytes() + g_overflowBytes.load()) / 1048576.0); }

    std::printf("\n== rapid-through-stock query (read-only, Z grid) ==\n");
    { struct Q { const char* name; Move m; };
      const Q qs[] = {
          {"rapid above stock (z=30), 170 mm              ", {-10, 50, 30, 160, 50, 30, 0}},
          {"rapid inside pocket, clear (z=7.5), 60 mm      ", {25, 50, 7.5, 85, 50, 7.5, 0}},
          {"rapid across stock through walls (z=10), 170 mm", {-10, 50, 10, 160, 50, 10, 0}},
          {"rapid below pocket floor (z=4.9), 60 mm        ", {25, 50, 4.9, 85, 50, 4.9, 0}},
          {"plunge rapid onto uncut corner, 2 mm deep      ", {148, 98, 30, 148, 98, 22.4, 0}},
      };
      for (const Q& q : qs) {
          const int reps = cell < 0.1 ? 50 : 2000; bool hit = false; const double t0 = nowS();
          for (int k = 0; k < reps; ++k) hit = intersectsZ(s, q.m, TOOLS[q.m.tool]);
          const double dt = (nowS() - t0) / reps;
          std::printf("%s -> %s  %10.2f us per query\n", q.name, hit ? "HIT " : "miss", dt * 1e6);
      } }

    std::printf("\n== display mesh (heightfield from Z dexels, 2 triangles per cell, per-vertex normals) ==\n");
    { const double before = curWsMb();
      const int n20 = static_cast<int>(std::lround(20.0 / cell));
      const int ci0 = static_cast<int>(40.0 / cell), cj0 = static_cast<int>(40.0 / cell);
      for (int threads : {1, 4}) {
          for (int whole = 0; whole < 2; ++whole) {
              double best = 1e9; size_t nv = 0, nt = 0;
              for (int k = 0; k < 3; ++k) {
                  std::vector<Vtx> v; std::vector<std::uint32_t> idx;
                  const double t0 = nowS();
                  if (whole) meshRegion(s, 0, s.nx - 1, 0, s.ny - 1, threads, v, idx); else meshRegion(s, ci0, ci0 + n20 - 1, cj0, cj0 + n20 - 1, threads, v, idx);
                  best = std::min(best, nowS() - t0); nv = v.size(); nt = idx.size() / 3;
              }
              std::printf("%s, %d thread(s): %9.3f ms  %zu vertices, %zu triangles, %.1f MB\n", whole ? "whole stock      " : "dirty 20 x 20 mm ", threads, best * 1e3, nv, nt, (nv * sizeof(Vtx) + nt * 12) / 1048576.0);
          }
      }
      std::printf("working set before meshing %.1f MB, process peak after meshing %.1f MB\n", before, peakWsMb()); }
    return 0;
}
