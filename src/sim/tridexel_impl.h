// The CPU tri-dexel model's storage and kernels, shared by its source files (task T-070). What the model does:
// tridexel.h.
#pragma once

#include "tridexel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ray::sim::detail {

// One ray: sorted, disjoint material intervals [lo, hi] (float depths from bounds.min along the ray). Up to two
// inline; more in the tile's pool at `off` with room for `cap` intervals.
struct Dexel {
    std::uint16_t n = 0, cap = 2;
    union {
        float inl[4];
        std::uint32_t off;
    };
    Dexel() : inl{0, 0, 0, 0} {}
};
static_assert(sizeof(Dexel) == 20, "a dexel is 20 bytes");

inline constexpr std::uint16_t kMaxDexelIntervals = 32768;

// One tile of a grid: the rows of one strip by kTileCols columns (fewer in the last), its dexels row by row, owned
// copy-on-write (TileRef). Only the task that holds the tile's strip writes to it, and only after making sure it is
// the tile's only owner (StripTiles::own, TileRef::mut).
struct Tile {
    std::atomic<std::int32_t> refs{1};
    std::vector<Dexel> d;
    std::vector<float> pool;
    std::size_t garbage = 0; // floats of the pool no dexel uses
    // Z grid tiles only: per row, per run of kTopRun rays, the highest material top in it (0 when empty). A cut whose
    // lowest point is above it skips those rays. Derived from the dexels; kept up to date by every cut.
    std::vector<float> top;

    Tile() = default;
    Tile(const Tile& o) : d(o.d), pool(o.pool), garbage(o.garbage), top(o.top) {}
    Tile& operator=(const Tile&) = delete;

    float* data(Dexel& x) { return x.cap <= 2 ? x.inl : pool.data() + x.off; }
    const float* data(const Dexel& x) const { return x.cap <= 2 ? x.inl : pool.data() + x.off; }
    std::uint64_t bytes() const
    {
        return d.capacity() * sizeof(Dexel) + pool.capacity() * sizeof(float) + top.capacity() * sizeof(float) +
               sizeof(Tile);
    }

    // Room for one more interval in x (doubling its capacity in the pool). False at the limit.
    bool grow(Dexel& x)
    {
        if (x.cap >= kMaxDexelIntervals) {
            return false;
        }
        const std::uint16_t ncap = static_cast<std::uint16_t>(x.cap * 2);
        const std::size_t off = pool.size();
        pool.resize(off + 2u * ncap);
        std::memcpy(pool.data() + off, data(x), sizeof(float) * 2u * x.n);
        if (x.cap > 2) {
            garbage += 2u * x.cap;
        }
        x.off = static_cast<std::uint32_t>(off);
        x.cap = ncap;
        return true;
    }
    // Back inline once two intervals or fewer remain.
    void shrink(Dexel& x)
    {
        if (x.cap > 2 && x.n <= 2) {
            float tmp[4] = {0, 0, 0, 0};
            std::memcpy(tmp, pool.data() + x.off, sizeof(float) * 2u * x.n);
            garbage += 2u * x.cap;
            x.cap = 2;
            std::memcpy(x.inl, tmp, sizeof(tmp));
        }
    }
    // The highest material top of dexels [first, first + count).
    float topOf(const Dexel* first, int count) const
    {
        float t = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (first[i].n > 0) {
                t = std::max(t, data(first[i])[2 * first[i].n - 1]);
            }
        }
        return t;
    }
    // Rebuilds `top` for rows of `cols` dexels.
    void buildTop(int cols);
    // Rewrites the pool without the unused blocks, in dexel order, once at least half of it is unused.
    void compactIfWorthIt()
    {
        if (garbage < 4096 || garbage * 2 < pool.size()) {
            return;
        }
        std::vector<float> next;
        next.reserve(pool.size() - garbage);
        for (Dexel& x : d) {
            if (x.cap > 2) {
                const std::size_t off = next.size();
                next.insert(next.end(), pool.begin() + x.off, pool.begin() + x.off + 2u * x.cap);
                x.off = static_cast<std::uint32_t>(off);
            }
        }
        pool = std::move(next);
        garbage = 0;
    }
};

// Tiles a model dropped while no one else held them (internTiles, restore), kept to receive its next copies on write
// instead of being freed and allocated again: on Windows freeing a block of tens of kilobytes gives its pages back
// and the next allocation faults them in again, which made a keyframe that drops ten thousand tiles take 0.2 s. At
// most kTileBinBytes are kept; a tile is taken only for a copy of exactly its dexel count, so its arrays are reused
// as they are. Used between batches (put) and by a batch's tasks (take), under a lock.
inline constexpr std::uint64_t kTileBinBytes = std::uint64_t{256} << 20;
class TileBin {
public:
    TileBin() = default;
    TileBin(const TileBin&) = delete;
    TileBin& operator=(const TileBin&) = delete;
    ~TileBin() { clear(); }
    // `t`, which no one references any more, is kept or deleted.
    void put(Tile* t);
    // A kept tile whose dexel array holds exactly `dexels`, or null. Its contents are stale.
    Tile* take(std::size_t dexels);
    void clear();
    std::uint64_t bytes() const
    {
        const std::lock_guard<std::mutex> lock(m_);
        return bytes_;
    }

private:
    mutable std::mutex m_;
    std::unordered_map<std::size_t, std::vector<Tile*>> spare_; // by dexel array capacity
    std::uint64_t bytes_ = 0;
};

inline void TileBin::put(Tile* t)
{
    if (t->pool.capacity() > 4096) { // a large pool is not kept: copies bring their own
        std::vector<float>().swap(t->pool);
    }
    {
        const std::lock_guard<std::mutex> lock(m_);
        if (bytes_ + t->bytes() <= kTileBinBytes) {
            bytes_ += t->bytes();
            spare_[t->d.capacity()].push_back(t);
            return;
        }
    }
    delete t;
}

inline Tile* TileBin::take(std::size_t dexels)
{
    const std::lock_guard<std::mutex> lock(m_);
    const auto it = spare_.find(dexels);
    if (it == spare_.end() || it->second.empty()) {
        return nullptr;
    }
    Tile* t = it->second.back();
    it->second.pop_back();
    bytes_ -= t->bytes();
    return t;
}

inline void TileBin::clear()
{
    std::unordered_map<std::size_t, std::vector<Tile*>> spare;
    {
        const std::lock_guard<std::mutex> lock(m_);
        spare.swap(spare_);
        bytes_ = 0;
    }
    for (auto& [n, tiles] : spare) {
        for (Tile* t : tiles) {
            delete t;
        }
    }
}

// Intrusive copy-on-write owner of a Tile. Copying the reference shares the tile (one atomic increment); the tile is
// deleted with its last reference.
class TileRef {
public:
    TileRef() = default;
    explicit TileRef(Tile* s) : s_(s) {}
    TileRef(const TileRef& o) : s_(o.s_) { acquire(); }
    TileRef(TileRef&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    TileRef& operator=(const TileRef& o)
    {
        if (this != &o) {
            Tile* old = s_;
            s_ = o.s_;
            acquire();
            releasePtr(old);
        }
        return *this;
    }
    TileRef& operator=(TileRef&& o) noexcept
    {
        if (this != &o) {
            releasePtr(s_);
            s_ = o.s_;
            o.s_ = nullptr;
        }
        return *this;
    }
    ~TileRef() { releasePtr(s_); }
    const Tile* get() const { return s_; }
    const Tile* operator->() const { return s_; }
    // Whether this is the tile's only owner, so that writing to it in place changes no one else's stock. The acquire
    // load pairs with the release in releasePtr, so another owner's last reads of the tile happen before our writes.
    // Once true it stays true while only the holder of this reference copies it (no one else can share it again).
    bool unique() const { return s_->refs.load(std::memory_order_acquire) == 1; }
    // The tile for writing: copied first unless this is its only owner (into a tile of `bin` when it has one of the
    // size: the same contents either way).
    Tile& mut(TileBin* bin = nullptr)
    {
        if (!unique()) {
            Tile* copy = bin != nullptr ? bin->take(s_->d.size()) : nullptr;
            if (copy != nullptr) {
                copy->d = s_->d;
                copy->pool = s_->pool;
                copy->garbage = s_->garbage;
                copy->top = s_->top;
                copy->refs.store(1, std::memory_order_relaxed);
            } else {
                copy = new Tile(*s_);
            }
            releasePtr(s_);
            s_ = copy;
        }
        return *s_;
    }
    // Drops the reference. When it was the last, the tile is not deleted but returned (to the caller's care).
    Tile* dropKeepingLast()
    {
        Tile* t = s_;
        s_ = nullptr;
        if (t != nullptr && t->refs.load(std::memory_order_acquire) == 1) {
            return t;
        }
        releasePtr(t);
        return nullptr;
    }

private:
    void acquire()
    {
        if (s_ != nullptr) {
            s_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static void releasePtr(Tile* s)
    {
        if (s != nullptr && s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete s;
        }
    }
    Tile* s_ = nullptr;
};

// One grid: nu rows (the strip axis) of nv rays; ray (u, v) is at row u, column v, its centre at ((u + 0.5) cu,
// (v + 0.5) cv) in the grid's two cross axes, the ray running from 0 to len. The Z grid's rows are y and its columns
// x; the X and Y grids' rows are layers in z and their columns y and x. Strip s is rows [row0(s), row1(s)); it is held
// as tilesPerStrip tiles, tile t of it columns [col0(t), col1(t)), at tiles[s * tilesPerStrip + t].
struct Grid {
    int nu = 0, nv = 0;
    double cu = 0, cv = 0;
    double len = 0;
    int rowsPerStrip = 1;
    int strips = 0;
    int tilesPerStrip = 1;
    std::vector<TileRef> tiles;
    int stripCount() const { return strips; }
    int row0(int s) const { return s * rowsPerStrip; }
    int row1(int s) const { return std::min(nu, (s + 1) * rowsPerStrip); }
    static int col0(int t) { return t * kTileCols; }
    int col1(int t) const { return std::min(nv, (t + 1) * kTileCols); }
    int cols(int t) const { return col1(t) - col0(t); }
    TileRef* stripTiles(int s) { return tiles.data() + static_cast<std::size_t>(s) * tilesPerStrip; }
    const TileRef* stripTiles(int s) const { return tiles.data() + static_cast<std::size_t>(s) * tilesPerStrip; }
    const Tile& tileOf(int u, int v) const
    {
        return *tiles[static_cast<std::size_t>(u / rowsPerStrip) * tilesPerStrip + v / kTileCols].get();
    }
    const Dexel& at(int u, int v) const
    {
        const int t = v / kTileCols;
        return tileOf(u, v).d[static_cast<std::size_t>(u % rowsPerStrip) * cols(t) + (v - col0(t))];
    }
    const float* data(int u, int v) const { return tileOf(u, v).data(at(u, v)); }
};

// The tiles of one strip as the task that holds the strip sees them (the only one that writes to them during a batch):
// it reads any tile through get() and writes to one only through own(), which copies it first when a snapshot, a
// clone or a keyframe still shares it. A tile is therefore copied only when a cut is about to change it.
// Whether a tile is the task's own is looked up once, when the task starts (no one else can share a tile of the strip
// while the task runs: snapshot and clone are not called during a batch); a tile that becomes its own later is so
// through own().
struct StripTiles {
    TileRef* ref = nullptr;
    int u0 = 0, u1 = 0;          // the strip's rows
    std::vector<const Tile*> rd; // per tile: where to read it
    std::vector<Tile*> wr;       // per tile: where to write it once it is the task's own, else null
    TileBin* bin = nullptr;      // where copies come from (TileRef::mut)
    std::uint64_t writes = 0;    // intervals subtracted from a dexel that lost material (triDexelWrites)
    StripTiles(Grid& g, int s, TileBin* b = nullptr) : ref(g.stripTiles(s)), u0(g.row0(s)), u1(g.row1(s)), bin(b)
    {
        rd.resize(static_cast<std::size_t>(g.tilesPerStrip));
        wr.resize(rd.size());
        for (std::size_t t = 0; t < rd.size(); ++t) {
            wr[t] = ref[t].unique() ? &ref[t].mut() : nullptr; // mut() does not copy a tile it owns alone
            rd[t] = ref[t].get();
        }
    }
    const Tile& get(int t) const { return *rd[static_cast<std::size_t>(t)]; }
    // The tile for writing if it is already the task's own, else null.
    Tile* mine(int t) const { return wr[static_cast<std::size_t>(t)]; }
    // The tile for writing: copied first (TileRef::mut) unless it is the task's own.
    Tile& own(int t)
    {
        Tile*& w = wr[static_cast<std::size_t>(t)];
        if (w == nullptr) {
            w = &ref[t].mut(bin);
            rd[static_cast<std::size_t>(t)] = w;
        }
        return *w;
    }
};

// Grid indices: 0 rays along Z (rows y, columns x), 1 along X (rows z, columns y), 2 along Y (rows z, columns x).
enum GridIndex { kGz = 0, kGx = 1, kGy = 2 };

// Whether subtractInterval(s, x, a, b) would change x: some interval of it overlaps (a, b). When it would not, that
// call returns 0 without writing anything, so a cut can skip it in a tile it does not own yet.
inline bool cutsInterval(const Tile& s, const Dexel& x, float a, float b)
{
    const std::uint32_t n = x.n;
    if (n == 0 || !(a < b)) {
        return false;
    }
    const float* p = s.data(x);
    if (p[2 * n - 1] <= a || p[0] >= b) {
        return false;
    }
    std::uint32_t i = 0;
    while (i < n && p[2 * i + 1] <= a) {
        ++i;
    }
    return i < n && p[2 * i] < b;
}

// Removes [a, b] from x (a part left shorter than kSliverMm goes too). Returns the length removed, more than 0
// exactly when cutsInterval(s, x, a, b); when it is 0 nothing was written.
inline double subtractInterval(Tile& s, Dexel& x, float a, float b)
{
    std::uint32_t n = x.n;
    if (n == 0 || !(a < b)) {
        return 0.0;
    }
    float* p = s.data(x);
    if (p[2 * n - 1] <= a || p[0] >= b) {
        return 0.0;
    }
    std::uint32_t i = 0;
    while (i < n && p[2 * i + 1] <= a) {
        ++i;
    }
    std::uint32_t j = i;
    while (j < n && p[2 * j] < b) {
        ++j;
    }
    if (i == j) {
        return 0.0;
    }
    double removed = 0.0;
    for (std::uint32_t k = i; k < j; ++k) {
        removed += static_cast<double>(std::min(p[2 * k + 1], b)) - static_cast<double>(std::max(p[2 * k], a));
    }
    const float lo0 = p[2 * i], hi1 = p[2 * j - 1];
    bool keepL = lo0<a, keepR = hi1> b;
    if (keepL && static_cast<double>(a) - static_cast<double>(lo0) < kSliverMm) {
        removed += static_cast<double>(a) - static_cast<double>(lo0);
        keepL = false;
    }
    if (keepR && static_cast<double>(hi1) - static_cast<double>(b) < kSliverMm) {
        removed += static_cast<double>(hi1) - static_cast<double>(b);
        keepR = false;
    }
    if (keepL && keepR && j - i == 1 && n + 1 > x.cap) {
        if (s.grow(x)) {
            p = s.data(x);
        } else { // at the interval limit (never reached in practice): remove up to the interval's end
            removed += static_cast<double>(hi1) - static_cast<double>(b);
            keepR = false;
        }
    }
    const std::uint32_t keep = (keepL ? 1u : 0u) + (keepR ? 1u : 0u), span = j - i;
    if (keep > span) { // one interval split in two
        std::memmove(p + 2 * (j + 1), p + 2 * j, sizeof(float) * 2 * (n - j));
        p[2 * i + 1] = a;
        p[2 * i + 2] = b;
        p[2 * i + 3] = hi1;
        x.n = static_cast<std::uint16_t>(n + 1);
        return removed;
    }
    std::uint32_t w = i;
    if (keepL) {
        p[2 * w] = lo0;
        p[2 * w + 1] = a;
        ++w;
    }
    if (keepR) {
        p[2 * w] = b;
        p[2 * w + 1] = hi1;
        ++w;
    }
    if (w < j) {
        std::memmove(p + 2 * w, p + 2 * j, sizeof(float) * 2 * (n - j));
    }
    x.n = static_cast<std::uint16_t>(n - span + keep);
    s.shrink(x);
    return removed;
}

// True when x holds material overlapping (lo, hi) by more than kContactMm.
inline bool overlaps(const Tile& s, const Dexel& x, double lo, double hi)
{
    const float* p = s.data(x);
    for (std::uint32_t k = 0; k < x.n; ++k) {
        const double l = std::max(static_cast<double>(p[2 * k]), lo);
        const double h = std::min(static_cast<double>(p[2 * k + 1]), hi);
        if (h - l > kContactMm) {
            return true;
        }
        if (static_cast<double>(p[2 * k]) >= hi) {
            break;
        }
    }
    return false;
}

inline constexpr int kTopRun = 32; // rays per entry of Tile::top
// A tile's runs are the grid's runs (both start at multiples of kTopRun), so the tops, and what a cut skips, are the
// same whatever the tile width.
static_assert(kTileCols % kTopRun == 0, "tiles hold whole runs");
inline constexpr int topRuns(int cols)
{
    return (cols + kTopRun - 1) / kTopRun;
}

inline void Tile::buildTop(int cols)
{
    const int runs = topRuns(cols);
    const int rows = cols > 0 ? static_cast<int>(d.size() / static_cast<std::size_t>(cols)) : 0;
    top.assign(static_cast<std::size_t>(rows) * runs, 0.0f);
    for (int r = 0; r < rows; ++r) {
        for (int b = 0; b < runs; ++b) {
            const int i0 = b * kTopRun;
            top[static_cast<std::size_t>(r) * runs + b] =
                topOf(d.data() + static_cast<std::size_t>(r) * cols + i0, std::min(kTopRun, cols - i0));
        }
    }
}

inline int firstIdx(double lo, double cell)
{
    return static_cast<int>(std::ceil(lo / cell - 0.5));
}
inline int lastIdx(double hi, double cell)
{
    return static_cast<int>(std::floor(hi / cell - 0.5));
}

// A straight move in the stock's local frame (bounds.min at the origin), with what the kernels need of it.
struct Seg {
    double ax, ay, az, bx, by, bz;
    double dx, dy, dz;
    double lenXY, ux, uy, m; // XY length, XY direction, dz per mm of XY travel
    bool vertical;           // no XY travel
    std::uint32_t move;      // index of the CutMove it comes from
    std::uint32_t slopes;    // offset of its slope tables in the batch's table (Kernels)
};
Seg makeSeg(double ax, double ay, double az, double bx, double by, double bz, std::uint32_t move);

// The slope tables of every piece of a shape for one segment: for piece p, low at tables[off + lowOff[p]], high at
// tables[off + highOff[p]] (prepareSlopes, toolshape.h).
struct ShapeTables {
    std::vector<std::uint32_t> lowOff, highOff;
    std::uint32_t perSeg = 0; // doubles per segment
    void layout(const CutterShape& shape);
    void fill(const CutterShape& shape, double m, double* out) const;
};

// The interval [lo, hi] (local Z) a convex piece swept along `g` covers on the Z ray at (x, y); false when it misses.
bool zInterval(const ConvexPiece& pc, const Seg& g, const double* lowC, const double* highC, double x, double y,
               double& lo, double& hi);

// Per-thread scratch of the lateral kernel.
struct LateralScratch {
    std::vector<DiscHull> hulls;      // of the current layer
    std::vector<std::uint32_t> first; // per piece: first hull and one past its last
    std::vector<double> times;
    std::vector<std::uint8_t> state;  // per piece, this layer and the one below: how the layer meets it
    std::vector<float> ivl;           // per (column, piece): the interval of the current run of layers
    std::vector<std::uint32_t> stamp; // per column: the run its intervals were computed for
};

// Cuts the swept `shape` along `g` out of the layers of one strip `s` of lateral grid `gi` (kGx or kGy).
void cutLateral(const Grid& grid, int gi, StripTiles& s, const CutterShape& shape, const Seg& g,
                LateralScratch& scratch);

// Cuts along the Z rays of one strip `s`; returns the removed length in 1 / kVolumeScale mm and sets the bits of the
// display tiles (tx, ty - tileRow0) in `tileBits` (row-major, tilesX per row) where material went.
std::int64_t cutZ(const Grid& grid, StripTiles& s, const CutterShape& shape, const Seg& g, const double* tables,
                  const ShapeTables& layout, std::vector<std::uint8_t>& tileBits, int tilesX, int tileRow0);
// Whether the swept `shape` (the non-cutting part) meets material on the Z rays of one strip (its tiles, rows [u0,
// u1)).
bool touchesZ(const Grid& grid, const TileRef* t, int u0, int u1, const CutterShape& shape, const Seg& g,
              const double* tables, const ShapeTables& layout);

// Fills every dexel of `grid` (index gi) with the material of `soup` (tridexel_reset.cpp), strips in parallel.
void fillFromSoup(Grid& grid, int gi, const kernel::TriSoup& soup, const Vec3& origin, IJobSystem& jobs);

// The column mesh of the Z grid (tridexel_mesh.cpp): cells [ci0, ci1) x [cj0, cj1) of a grid of cells cx x cy at
// `origin`, columns of `step` x `step` cells aligned to multiples of `step` (step divides kTileCells).
struct MeshRegion {
    int ci0, ci1, cj0, cj1;
    int step;
};
// The triangle count of the region's mesh at its step (without building it).
std::uint64_t columnMeshTriangles(const Grid& gz, const MeshRegion& r, IJobSystem& jobs);
kernel::Mesh columnMesh(const Grid& gz, const MeshRegion& r, double cx, double cy, const Vec3& origin,
                        IJobSystem& jobs);
kernel::TriSoup columnSolid(const Grid& gz, double cx, double cy, const Vec3& origin, IJobSystem& jobs);

} // namespace ray::sim::detail
