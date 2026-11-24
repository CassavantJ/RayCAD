// CPU tri-dexel stock model (task T-070): the model, its batches, snapshots and hooks. How it works: tridexel.h.
#include "tridexel_impl.h"

#include "backplot.h"

#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace ray::sim {

namespace detail {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// The chord ends of an arc after its start, the last exactly at `to` (as buildBackplot: backplot.cpp).
void arcPoints(const IStockModel::CutMove& mv, double tol, std::vector<Vec3>& out)
{
    out.clear();
    int u = 0, v = 1, w = 2;
    if (mv.plane == cam::ArcPlane::XZ) {
        u = 2;
        v = 0;
        w = 1;
    } else if (mv.plane == cam::ArcPlane::YZ) {
        u = 1;
        v = 2;
        w = 0;
    }
    const double s[3] = {mv.from.x, mv.from.y, mv.from.z};
    const double e[3] = {mv.to.x, mv.to.y, mv.to.z};
    const double c[3] = {mv.centre.x, mv.centre.y, mv.centre.z};
    const double au = s[u] - c[u], av = s[v] - c[v], bu = e[u] - c[u], bv = e[v] - c[v];
    const double r0 = dmath::sqrt(au * au + av * av), r1 = dmath::sqrt(bu * bu + bv * bv);
    if (!(r0 > 0) || !(r1 > 0)) {
        out.push_back(mv.to); // no radius at one end: a straight move
        return;
    }
    const bool ccw = mv.kind == cam::MoveKind::ArcCCW;
    const double sweep = arcSweep(au, av, bu, bv, ccw);
    const std::uint32_t n = arcSegmentCount(sweep, arcEffectiveRadius(r0, r1, sweep), tol);
    const double nD = static_cast<double>(n);
    const double step = ccw ? sweep / nD : -(sweep / nD);
    const double cs = dmath::cos(step), sn = dmath::sin(step);
    double du = au / r0, dv = av / r0;
    const double dr = (r1 - r0) / nD, dw = (e[w] - s[w]) / nD;
    for (std::uint32_t k = 1; k < n; ++k) {
        const double nu = du * cs - dv * sn, nv = du * sn + dv * cs;
        du = nu;
        dv = nv;
        const double kD = static_cast<double>(k);
        const double r = r0 + dr * kD;
        double p[3];
        p[u] = c[u] + r * du;
        p[v] = c[v] + r * dv;
        p[w] = s[w] + dw * kD;
        out.push_back({p[0], p[1], p[2]});
    }
    out.push_back(mv.to);
}

bool finite(const Vec3& p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// A tile's contents as internTiles compares them (tridexel.h): its dexel count, each dexel's interval count and the
// bits of its intervals (not their room in the pool, nor the unused inline floats), and its top table.
std::uint64_t contentHash(const Tile& t)
{
    constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;
    std::uint64_t h = (static_cast<std::uint64_t>(t.d.size()) + 1) * kMul;
    for (const Dexel& x : t.d) {
        h = (h ^ x.n) * kMul;
        const float* p = t.data(x);
        for (std::uint32_t k = 0; k < x.n; ++k) {
            std::uint64_t w = 0;
            std::memcpy(&w, p + 2 * k, 8);
            h = (h ^ w ^ (h >> 29)) * kMul;
        }
    }
    for (const float f : t.top) {
        std::uint32_t w = 0;
        std::memcpy(&w, &f, 4);
        h = (h ^ w) * kMul;
    }
    return h ^ (h >> 31);
}

bool sameContents(const Tile& a, const Tile& b)
{
    if (a.d.size() != b.d.size() || a.top.size() != b.top.size()) {
        return false;
    }
    if (!a.top.empty() && std::memcmp(a.top.data(), b.top.data(), sizeof(float) * a.top.size()) != 0) {
        return false;
    }
    for (std::size_t k = 0; k < a.d.size(); ++k) {
        const Dexel& x = a.d[k];
        const Dexel& y = b.d[k];
        if (x.n != y.n || (x.n > 0 && std::memcmp(a.data(x), b.data(y), sizeof(float) * 2u * x.n) != 0)) {
            return false;
        }
    }
    return true;
}

} // namespace

// internTiles' table (tridexel.h): per content hash, the tiles met with it, first met first (a later one is there only
// when its contents differ from the earlier ones').
struct InternTable {
    std::unordered_map<std::uint64_t, std::vector<TileRef>> buckets;
};

class TriDexelCpu;

class TriDexelSnapshot final : public IStockModel::Snapshot {
public:
    int n[3] = {0, 0, 0};
    double c[3] = {0, 0, 0};
    Vec3 origin;
    std::vector<TileRef> tiles[3];
};

class TriDexelCpu final : public IStockModel {
public:
    explicit TriDexelCpu(IJobSystem& jobs) : jobs_(jobs) {}

    void reset(const kernel::TriSoup& soup, const Box3& bounds, StockResolution res) override;
    double cut(const ToolShape& shape, const Vec3& from, const Vec3& to, bool rapid) override
    {
        const CutMove mv{from, to, rapid};
        double removed = 0;
        cutBatch(shape, std::span<const CutMove>(&mv, 1), std::span<double>(&removed, 1), {});
        return removed;
    }
    double cutArc(const ToolShape& shape, const Vec3& from, const Vec3& to, const Vec3& centre, bool cw,
                  cam::ArcPlane plane) override
    {
        CutMove mv{from, to, false};
        mv.kind = cw ? cam::MoveKind::ArcCW : cam::MoveKind::ArcCCW;
        mv.centre = centre;
        mv.plane = plane;
        double removed = 0;
        cutBatch(shape, std::span<const CutMove>(&mv, 1), std::span<double>(&removed, 1), {});
        return removed;
    }
    void cutBatch(const ToolShape& shape, std::span<const CutMove> moves, std::span<double> removedMm3,
                  std::span<std::uint8_t> nonCuttingHits) override;
    bool intersects(const ToolShape& part, const Vec3& from, const Vec3& to) const override;
    kernel::Mesh toMesh(std::uint32_t changedSinceVersion, Box3* dirtyOut, std::uint32_t maxTriangles) const override;
    kernel::TriSoup toSolid() const override
    {
        if (!ready_) {
            return {};
        }
        return columnSolid(g_[kGz], c_[0], c_[1], origin_, jobs_);
    }
    std::unique_ptr<IStockModel> clone() const override
    {
        auto copy = std::make_unique<TriDexelCpu>(jobs_);
        for (int g = 0; g < 3; ++g) {
            copy->g_[g] = g_[g];
            copy->n_[g] = n_[g];
            copy->c_[g] = c_[g];
            copy->ext_[g] = ext_[g];
        }
        copy->origin_ = origin_;
        copy->ready_ = ready_;
        copy->version_ = version_;
        copy->tileVersion_ = tileVersion_;
        copy->tilesX_ = tilesX_;
        copy->tilesY_ = tilesY_;
        copy->buildTasks();
        return copy;
    }
    std::shared_ptr<const Snapshot> snapshot() const override
    {
        auto s = std::make_shared<TriDexelSnapshot>();
        for (int g = 0; g < 3; ++g) {
            s->n[g] = n_[g];
            s->c[g] = c_[g];
            s->tiles[g] = g_[g].tiles;
        }
        s->origin = origin_;
        return s;
    }
    void restore(const Snapshot& snap) override;
    std::uint32_t version() const override { return version_; }

    // Hooks (tridexel.h).
    TriDexelInfo info() const;
    std::uint64_t hash() const;
    std::uint64_t privateBytes(const TriDexelSnapshot& s) const;
    SnapshotCost cost(const TriDexelSnapshot& s) const;
    std::uint64_t intern(const TriDexelSnapshot* prev);
    std::uint64_t writes() const { return writes_; }
    std::uint64_t meshTriangles(int step) const
    {
        return ready_ ? columnMeshTriangles(g_[kGz], MeshRegion{0, n_[0], 0, n_[1], step}, jobs_) : 0;
    }

private:
    struct Entry {
        std::uint32_t seg;
        std::uint8_t cut, check;
    };
    struct Task {
        int grid = 0, strip = 0;
        std::vector<Entry> list;
        std::vector<std::int64_t> vol;
        std::vector<std::uint8_t> hit;
        std::vector<std::uint8_t> tiles; // Z tasks: dirty tiles of the strip's tile rows
        std::uint64_t writes = 0;        // StripTiles::writes of the last batch
        int tileRow0 = 0;
    };
    bool sameGrid(const TriDexelSnapshot& s) const
    {
        return ready_ && s.n[0] == n_[0] && s.n[1] == n_[1] && s.n[2] == n_[2] && s.c[0] == c_[0] && s.c[1] == c_[1] &&
               s.c[2] == c_[2] && s.origin.x == origin_.x && s.origin.y == origin_.y && s.origin.z == origin_.z;
    }
    // One task per grid strip, kept between batches.
    void buildTasks()
    {
        tasks_.clear();
        for (int gi = 0; gi < 3; ++gi) {
            taskBase_[gi] = static_cast<int>(tasks_.size());
            for (int s = 0; s < g_[gi].stripCount(); ++s) {
                Task t;
                t.grid = gi;
                t.strip = s;
                t.tileRow0 = g_[gi].row0(s) / kTileCells;
                tasks_.push_back(std::move(t));
            }
        }
    }
    // Marks the display tiles over Z rays [row0, row1) x [col0, col1).
    void markTiles(int row0, int row1, int col0, int col1, std::uint32_t version)
    {
        for (int ty = row0 / kTileCells; ty <= (row1 - 1) / kTileCells && ty < tilesY_; ++ty) {
            for (int tx = col0 / kTileCells; tx <= (col1 - 1) / kTileCells && tx < tilesX_; ++tx) {
                tileVersion_[static_cast<std::size_t>(ty) * tilesX_ + tx] = version;
            }
        }
    }

    IJobSystem& jobs_;
    bool ready_ = false;
    Grid g_[3];
    Vec3 origin_;
    double ext_[3] = {0, 0, 0}; // x, y, z
    int n_[3] = {0, 0, 0};
    double c_[3] = {0, 0, 0};
    std::uint32_t version_ = 0;
    std::uint64_t writes_ = 0; // triDexelWrites
    std::vector<std::uint32_t> tileVersion_;
    int tilesX_ = 0, tilesY_ = 0;
    std::vector<Task> tasks_; // per grid and strip, reused between batches
    int taskBase_[3] = {0, 0, 0};
    InternTable table_; // internTiles
    TileBin bin_;       // tiles dropped by internTiles and restore, for the next copies
};

void TriDexelCpu::reset(const kernel::TriSoup& soup, const Box3& bounds, StockResolution res)
{
    ++version_;
    ready_ = false;
    tasks_.clear();
    table_.buckets.clear();
    bin_.clear();
    for (Grid& g : g_) {
        g = Grid{};
    }
    tileVersion_.clear();
    tilesX_ = tilesY_ = 0;
    const double ext[3] = {bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z};
    for (double e : ext) {
        if (!(e > 0) || !std::isfinite(e)) {
            ray::log(LogLevel::Warn, "sim", "stock model reset with an empty or invalid box: no material");
            return;
        }
    }
    double cell = res.cellMm > 0 && std::isfinite(res.cellMm) ? res.cellMm : 0.2;
    double n[3] = {1, 1, 1};
    for (;;) {
        for (int a = 0; a < 3; ++a) {
            n[a] = std::max(1.0, std::ceil(ext[a] / cell - 1e-9));
        }
        const double dexels = n[0] * n[1] + n[1] * n[2] + n[0] * n[2];
        if (dexels <= static_cast<double>(kMaxDexels)) {
            break;
        }
        cell *= 2;
        ray::log(LogLevel::Warn, "sim",
                 "stock model: the cell was coarsened to " + std::to_string(cell) + " mm to stay within memory");
    }
    origin_ = bounds.min;
    for (int a = 0; a < 3; ++a) {
        ext_[a] = ext[a];
        n_[a] = static_cast<int>(n[a]);
        c_[a] = ext[a] / n[a];
    }
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const double cx = c_[0], cy = c_[1], cz = c_[2];
    const struct {
        int nu, nv;
        double cu, cv, len;
    } dims[3] = {{ny, nx, cy, cx, ext[2]}, {nz, ny, cz, cy, ext[0]}, {nz, nx, cz, cx, ext[1]}};
    const bool block = soup.indices.empty() || soup.positions.empty();
    for (int gi = 0; gi < 3; ++gi) {
        Grid& g = g_[gi];
        g.nu = dims[gi].nu;
        g.nv = dims[gi].nv;
        g.cu = dims[gi].cu;
        g.cv = dims[gi].cv;
        g.len = dims[gi].len;
        g.rowsPerStrip = std::max(1, static_cast<int>(std::lround((gi == kGz ? kStripMm : kLayerStripMm) / g.cu)));
        g.strips = (g.nu + g.rowsPerStrip - 1) / g.rowsPerStrip;
        g.tilesPerStrip = (g.nv + kTileCols - 1) / kTileCols;
        Dexel full;
        full.n = block ? 1 : 0;
        full.inl[0] = 0.0f;
        full.inl[1] = static_cast<float>(g.len);
        g.tiles.reserve(static_cast<std::size_t>(g.strips) * g.tilesPerStrip);
        for (int s = 0; s < g.strips; ++s) {
            for (int t = 0; t < g.tilesPerStrip; ++t) {
                auto* tile = new Tile();
                tile->d.assign(static_cast<std::size_t>(g.row1(s) - g.row0(s)) * g.cols(t), full);
                g.tiles.emplace_back(tile);
            }
        }
        if (!block) {
            fillFromSoup(g, gi, soup, origin_, jobs_);
        }
        if (gi == kGz) {
            for (std::size_t k = 0; k < g.tiles.size(); ++k) {
                g.tiles[k].mut().buildTop(g.cols(static_cast<int>(k % static_cast<std::size_t>(g.tilesPerStrip))));
            }
        }
    }
    buildTasks();
    tilesX_ = (nx + kTileCells - 1) / kTileCells;
    tilesY_ = (ny + kTileCells - 1) / kTileCells;
    tileVersion_.assign(static_cast<std::size_t>(tilesX_) * tilesY_, version_);
    ready_ = true;
}

void TriDexelCpu::cutBatch(const ToolShape& shape, std::span<const CutMove> moves, std::span<double> removedMm3,
                           std::span<std::uint8_t> nonCuttingHits)
{
    ++version_;
    const bool wantVol = removedMm3.size() == moves.size();
    const bool wantHits = nonCuttingHits.size() == moves.size() && !moves.empty();
    for (double& v : removedMm3) {
        v = 0;
    }
    for (std::uint8_t& h : nonCuttingHits) {
        h = 0;
    }
    if (!ready_ || moves.empty()) {
        return;
    }
    const CutterShape cutting = compileProfile(shape.cuttingProfile, false);
    CutterShape nonCutting;
    if (wantHits) {
        const std::vector<Vec2>* parts[] = {&shape.shankProfile, &shape.holderProfile};
        nonCutting = compileShape(parts, true);
    }
    if (cutting.empty() && nonCutting.empty()) {
        return;
    }
    ShapeTables cutLayout, nonLayout;
    cutLayout.layout(cutting);
    nonLayout.layout(nonCutting);
    const std::uint32_t perSeg = cutLayout.perSeg + nonLayout.perSeg;

    // Moves to straight segments in the local frame.
    std::vector<Seg> segs;
    segs.reserve(moves.size());
    std::vector<Vec3> pts;
    const double tol = kArcTolCells * std::min({c_[0], c_[1], c_[2]});
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const CutMove& mv = moves[i];
        if (!finite(mv.from) || !finite(mv.to)) {
            continue;
        }
        const bool arc = mv.kind == cam::MoveKind::ArcCW || mv.kind == cam::MoveKind::ArcCCW;
        if (arc && finite(mv.centre)) {
            arcPoints(mv, tol, pts);
        } else {
            pts.assign(1, mv.to);
        }
        Vec3 a = mv.from;
        for (const Vec3& b : pts) {
            segs.push_back(makeSeg(a.x - origin_.x, a.y - origin_.y, a.z - origin_.z, b.x - origin_.x, b.y - origin_.y,
                                   b.z - origin_.z, static_cast<std::uint32_t>(i)));
            a = b;
        }
    }
    std::vector<double> tables(segs.size() * perSeg);
    for (std::size_t s = 0; s < segs.size(); ++s) {
        segs[s].slopes = static_cast<std::uint32_t>(s * perSeg);
        cutLayout.fill(cutting, segs[s].m, tables.data() + s * perSeg);
        nonLayout.fill(nonCutting, segs[s].m, tables.data() + s * perSeg + cutLayout.perSeg);
    }

    // Lists per strip, in program order.
    for (Task& t : tasks_) {
        t.list.clear();
    }
    const auto add = [&](int gi, double lo, double hi, std::uint32_t seg, bool cut, bool check) {
        const Grid& g = g_[gi];
        const int j0 = std::max(0, firstIdx(lo, g.cu)), j1 = std::min(g.nu - 1, lastIdx(hi, g.cu));
        if (j0 > j1) {
            return;
        }
        for (int s = j0 / g.rowsPerStrip; s <= j1 / g.rowsPerStrip; ++s) {
            std::vector<Entry>& list = tasks_[static_cast<std::size_t>(taskBase_[gi] + s)].list;
            if (!list.empty() && list.back().seg == seg) {
                list.back().cut |= static_cast<std::uint8_t>(cut);
                list.back().check |= static_cast<std::uint8_t>(check);
            } else {
                list.push_back({seg, static_cast<std::uint8_t>(cut), static_cast<std::uint8_t>(check)});
            }
        }
    };
    for (std::uint32_t s = 0; s < segs.size(); ++s) {
        const Seg& g = segs[s];
        const double xlo = std::min(g.ax, g.bx), xhi = std::max(g.ax, g.bx);
        const double ylo = std::min(g.ay, g.by), yhi = std::max(g.ay, g.by);
        const double zlo = std::min(g.az, g.bz), zhi = std::max(g.az, g.bz);
        const auto reaches = [&](const CutterShape& sh) {
            const double r = sh.rmax;
            return !sh.empty() && xhi + r >= 0 && xlo - r <= ext_[0] && yhi + r >= 0 && ylo - r <= ext_[1] &&
                   zlo + sh.hmin < ext_[2] && zhi + sh.hmax > 0;
        };
        const bool cut = reaches(cutting);
        const bool check = wantHits && reaches(nonCutting);
        if (cut) {
            add(kGx, zlo + cutting.hmin, zhi + cutting.hmax, s, true, false);
            add(kGy, zlo + cutting.hmin, zhi + cutting.hmax, s, true, false);
            add(kGz, ylo - cutting.rmax, yhi + cutting.rmax, s, true, false);
        }
        if (check) {
            add(kGz, ylo - nonCutting.rmax, yhi + nonCutting.rmax, s, false, true);
        }
    }
    std::vector<std::size_t> live;
    for (std::size_t t = 0; t < tasks_.size(); ++t) {
        if (!tasks_[t].list.empty()) {
            live.push_back(t);
        }
    }
    JobContext ctx;
    (void)jobs_.parallelChunks(live.size(), 1, ctx, [&](std::size_t chunk, std::size_t, std::size_t) -> Status {
        Task& task = tasks_[live[chunk]];
        Grid& grid = g_[task.grid];
        // This task alone reads and writes the tiles of its strip (and its own entries of tasks_); a tile is copied
        // only when a cut is about to change it while a snapshot or clone shares it (StripTiles).
        StripTiles strip(grid, task.strip, &bin_);
        const int u0 = strip.u0, u1 = strip.u1;
        if (task.grid == kGz) {
            task.vol.assign(task.list.size(), 0);
            task.hit.assign(task.list.size(), 0);
            const int tileRows = (u1 - 1) / kTileCells - task.tileRow0 + 1;
            task.tiles.assign(static_cast<std::size_t>(tileRows) * tilesX_, 0);
            for (std::size_t e = 0; e < task.list.size(); ++e) {
                const Entry& en = task.list[e];
                const Seg& g = segs[en.seg];
                const double* tab = tables.data() + g.slopes;
                const bool down = g.bz < g.az;
                bool hit = false;
                if (en.check && !down) {
                    hit = touchesZ(grid, strip.ref, u0, u1, nonCutting, g, tab + cutLayout.perSeg, nonLayout);
                }
                if (en.cut) {
                    task.vol[e] = cutZ(grid, strip, cutting, g, tab, cutLayout, task.tiles, tilesX_, task.tileRow0);
                }
                if (en.check && down) {
                    hit = touchesZ(grid, strip.ref, u0, u1, nonCutting, g, tab + cutLayout.perSeg, nonLayout);
                }
                task.hit[e] = hit ? 1 : 0;
            }
        } else {
            thread_local LateralScratch scratch;
            for (const Entry& en : task.list) {
                cutLateral(grid, task.grid, strip, cutting, segs[en.seg], scratch);
            }
        }
        task.writes = strip.writes;
        // Tiles this task wrote are its own; a tile still shared was not written and is left as it is.
        for (int t = 0; t < grid.tilesPerStrip; ++t) {
            if (Tile* tile = strip.mine(t)) {
                tile->compactIfWorthIt();
            }
        }
        return Status{};
    });

    for (const std::size_t t : live) {
        writes_ += tasks_[t].writes;
    }
    // Merge in integers, then scale.
    std::vector<std::int64_t> vol(moves.size(), 0);
    for (int s = 0; s < g_[kGz].stripCount(); ++s) {
        const Task& task = tasks_[static_cast<std::size_t>(taskBase_[kGz] + s)];
        if (task.list.empty()) {
            continue;
        }
        for (std::size_t e = 0; e < task.list.size(); ++e) {
            const std::uint32_t mv = segs[task.list[e].seg].move;
            vol[mv] += task.vol[e];
            if (wantHits && task.hit[e] != 0) {
                nonCuttingHits[mv] = 1;
            }
        }
        for (std::size_t b = 0; b < task.tiles.size(); ++b) {
            if (task.tiles[b] != 0) {
                const std::size_t ty = static_cast<std::size_t>(task.tileRow0) + b / static_cast<std::size_t>(tilesX_);
                tileVersion_[ty * static_cast<std::size_t>(tilesX_) + b % static_cast<std::size_t>(tilesX_)] = version_;
            }
        }
    }
    if (wantVol) {
        const double cellArea = c_[0] * c_[1];
        for (std::size_t i = 0; i < moves.size(); ++i) {
            removedMm3[i] = static_cast<double>(vol[i]) / kVolumeScale * cellArea;
        }
    }
}

bool TriDexelCpu::intersects(const ToolShape& part, const Vec3& from, const Vec3& to) const
{
    if (!ready_ || !finite(from) || !finite(to)) {
        return false;
    }
    const std::vector<Vec2>* parts[] = {&part.cuttingProfile, &part.shankProfile, &part.holderProfile};
    const CutterShape shape = compileShape(parts, true);
    if (shape.empty()) {
        return false;
    }
    ShapeTables layout;
    layout.layout(shape);
    const Seg g = makeSeg(from.x - origin_.x, from.y - origin_.y, from.z - origin_.z, to.x - origin_.x,
                          to.y - origin_.y, to.z - origin_.z, 0);
    std::vector<double> tables(layout.perSeg);
    layout.fill(shape, g.m, tables.data());
    const Grid& grid = g_[kGz];
    const double R = shape.rmax;
    const int j0 = std::max(0, firstIdx(std::min(g.ay, g.by) - R, grid.cu));
    const int j1 = std::min(grid.nu - 1, lastIdx(std::max(g.ay, g.by) + R, grid.cu));
    if (j0 > j1) {
        return false;
    }
    for (int s = j0 / grid.rowsPerStrip; s <= j1 / grid.rowsPerStrip; ++s) {
        if (touchesZ(grid, grid.stripTiles(s), grid.row0(s), grid.row1(s), shape, g, tables.data(), layout)) {
            return true;
        }
    }
    return false;
}

kernel::Mesh TriDexelCpu::toMesh(std::uint32_t changedSinceVersion, Box3* dirtyOut, std::uint32_t maxTriangles) const
{
    const auto emptyBox = [&] {
        if (dirtyOut != nullptr) {
            *dirtyOut = Box3{{kInf, kInf, kInf}, {-kInf, -kInf, -kInf}};
        }
    };
    if (!ready_) {
        emptyBox();
        return {};
    }
    int tx0 = tilesX_, tx1 = -1, ty0 = tilesY_, ty1 = -1;
    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            if (tileVersion_[static_cast<std::size_t>(ty) * tilesX_ + tx] > changedSinceVersion) {
                tx0 = std::min(tx0, tx);
                tx1 = std::max(tx1, tx);
                ty0 = std::min(ty0, ty);
                ty1 = std::max(ty1, ty);
            }
        }
    }
    if (tx1 < 0) {
        emptyBox();
        return {};
    }
    MeshRegion r{tx0 * kTileCells, std::min(n_[0], (tx1 + 1) * kTileCells), ty0 * kTileCells,
                 std::min(n_[1], (ty1 + 1) * kTileCells), 1};
    if (dirtyOut != nullptr) {
        *dirtyOut = Box3{{origin_.x + r.ci0 * c_[0], origin_.y + r.cj0 * c_[1], origin_.z},
                         {origin_.x + r.ci1 * c_[0], origin_.y + r.cj1 * c_[1], origin_.z + ext_[2]}};
    }
    // The finest step that fits: start from an estimate of four triangles per column, then double while over.
    const double cells = static_cast<double>(r.ci1 - r.ci0) * static_cast<double>(r.cj1 - r.cj0);
    while (r.step < kTileCells && 4.0 * cells / (static_cast<double>(r.step) * r.step) > maxTriangles) {
        r.step *= 2;
    }
    while (r.step < kTileCells && columnMeshTriangles(g_[kGz], r, jobs_) > maxTriangles) {
        r.step *= 2;
    }
    return columnMesh(g_[kGz], r, c_[0], c_[1], origin_, jobs_);
}

void TriDexelCpu::restore(const Snapshot& snapBase)
{
    const auto* snap = dynamic_cast<const TriDexelSnapshot*>(&snapBase);
    if (snap == nullptr || !sameGrid(*snap)) {
        ray::log(LogLevel::Warn, "sim", "stock model: a snapshot of another grid was not restored");
        return;
    }
    ++version_;
    // The display tiles over every Z tile that is not the snapshot's (a tile is replaced only after a cut changed it).
    const Grid& gz = g_[kGz];
    for (int s = 0; s < gz.stripCount(); ++s) {
        for (int t = 0; t < gz.tilesPerStrip; ++t) {
            const std::size_t k = static_cast<std::size_t>(s) * gz.tilesPerStrip + t;
            if (gz.tiles[k].get() != snap->tiles[kGz][k].get()) {
                markTiles(gz.row0(s), gz.row1(s), Grid::col0(t), gz.col1(t), version_);
            }
        }
    }
    for (int g = 0; g < 3; ++g) {
        // A tile only the model held goes to the bin, for the copies of the next cuts.
        std::vector<TileRef>& tiles = g_[g].tiles;
        for (std::size_t k = 0; k < tiles.size() && k < snap->tiles[g].size(); ++k) {
            if (tiles[k].get() != snap->tiles[g][k].get()) {
                if (Tile* dropped = tiles[k].dropKeepingLast()) {
                    bin_.put(dropped);
                }
            }
        }
        tiles = snap->tiles[g];
    }
}

namespace {

// Calls f(tile, dexel) for every dexel of `g` row by row, each row in column order (the grid's order, whatever its
// tiles).
template <class F>
void forEachDexel(const Grid& g, F&& f)
{
    for (int s = 0; s < g.stripCount(); ++s) {
        for (int r = 0; r < g.row1(s) - g.row0(s); ++r) {
            for (int t = 0; t < g.tilesPerStrip; ++t) {
                const Tile& tile = *g.stripTiles(s)[t].get();
                const int w = g.cols(t);
                const Dexel* row = tile.d.data() + static_cast<std::size_t>(r) * w;
                for (int i = 0; i < w; ++i) {
                    f(tile, row[i]);
                }
            }
        }
    }
}

} // namespace

TriDexelInfo TriDexelCpu::info() const
{
    TriDexelInfo in;
    in.nx = n_[0];
    in.ny = n_[1];
    in.nz = n_[2];
    in.cx = c_[0];
    in.cy = c_[1];
    in.cz = c_[2];
    in.origin = origin_;
    in.tilesX = tilesX_;
    in.tilesY = tilesY_;
    in.bytes = sizeof(*this) + tileVersion_.capacity() * sizeof(std::uint32_t) + bin_.bytes();
    for (const Task& t : tasks_) {
        in.bytes += sizeof(Task) + t.list.capacity() * sizeof(Entry) + t.vol.capacity() * sizeof(std::int64_t) +
                    t.hit.capacity() + t.tiles.capacity();
    }
    for (int gi = 0; gi < 3; ++gi) {
        const Grid& g = g_[gi];
        in.strips += static_cast<std::uint64_t>(g.stripCount());
        in.tiles += g.tiles.size();
        for (const TileRef& t : g.tiles) {
            in.bytes += t->bytes() + sizeof(TileRef);
        }
        in.bytes += g.tiles.capacity() * sizeof(TileRef) - g.tiles.size() * sizeof(TileRef);
        double len = 0;
        forEachDexel(g, [&](const Tile& tile, const Dexel& x) {
            ++in.dexels;
            in.intervals[gi] += x.n;
            in.maxIntervals[gi] = std::max<std::uint32_t>(in.maxIntervals[gi], x.n);
            const float* p = tile.data(x);
            for (std::uint32_t k = 0; k < x.n; ++k) {
                len += static_cast<double>(p[2 * k + 1]) - static_cast<double>(p[2 * k]);
            }
        });
        in.volume[gi] = len * g.cu * g.cv;
    }
    return in;
}

std::uint64_t TriDexelCpu::hash() const
{
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint32_t w) {
        h = (h ^ w) * 1099511628211ull;
    };
    for (int g = 0; g < 3; ++g) {
        mix(static_cast<std::uint32_t>(g_[g].nu));
        mix(static_cast<std::uint32_t>(g_[g].nv));
        forEachDexel(g_[g], [&](const Tile& tile, const Dexel& x) {
            mix(x.n);
            const float* p = tile.data(x);
            for (std::uint32_t k = 0; k < 2u * x.n; ++k) {
                std::uint32_t w = 0;
                std::memcpy(&w, p + k, 4);
                mix(w);
            }
        });
    }
    return h;
}

std::uint64_t TriDexelCpu::privateBytes(const TriDexelSnapshot& s) const
{
    std::uint64_t bytes = 0;
    for (int g = 0; g < 3; ++g) {
        for (std::size_t i = 0; i < s.tiles[g].size(); ++i) {
            const bool shared = i < g_[g].tiles.size() && g_[g].tiles[i].get() == s.tiles[g][i].get();
            if (!shared) {
                bytes += s.tiles[g][i]->bytes();
            }
        }
    }
    return bytes;
}

SnapshotCost TriDexelCpu::cost(const TriDexelSnapshot& s) const
{
    SnapshotCost c;
    if (!sameGrid(s)) {
        return c;
    }
    for (int g = 0; g < 3; ++g) {
        for (std::size_t i = 0; i < s.tiles[g].size(); ++i) {
            const Tile* a = s.tiles[g][i].get();
            const Tile* b = g_[g].tiles[i].get();
            if (a == b) {
                continue;
            }
            c.privateBytes[g] += a->bytes();
            for (std::size_t k = 0; k < a->d.size(); ++k) {
                const Dexel& x = a->d[k];
                const Dexel& y = b->d[k];
                bool same = x.n == y.n;
                if (same && x.n > 0) {
                    same = std::memcmp(a->data(x), b->data(y), sizeof(float) * 2u * x.n) == 0;
                }
                if (!same) {
                    ++c.changedDexels[g];
                    c.changedBytes[g] += sizeof(Dexel) + (x.cap > 2 ? sizeof(float) * 2u * x.cap : 0u);
                }
            }
        }
    }
    return c;
}

std::uint64_t TriDexelCpu::intern(const TriDexelSnapshot* prev)
{
    if (!ready_) {
        return 0;
    }
    // Tiles no one but the table holds any more go.
    for (auto it = table_.buckets.begin(); it != table_.buckets.end();) {
        std::vector<TileRef>& b = it->second;
        b.erase(std::remove_if(b.begin(), b.end(), [](const TileRef& t) { return t.unique(); }), b.end());
        it = b.empty() ? table_.buckets.erase(it) : std::next(it);
    }
    // The tiles changed since prev, in grid order.
    struct Item {
        int g;
        std::size_t i;
        std::uint64_t hash;
        const TileRef* match; // the first tile met with this hash, to be compared; null when none or this one
        bool same;
    };
    std::vector<Item> items;
    const bool all = prev == nullptr || !sameGrid(*prev);
    for (int g = 0; g < 3; ++g) {
        const std::vector<TileRef>& tiles = g_[g].tiles;
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (all || prev->tiles[g][i].get() != tiles[i].get()) {
                items.push_back({g, i, 0, nullptr, false});
            }
        }
    }
    if (items.empty()) {
        return 0;
    }
    constexpr std::size_t kGrain = 16;
    JobContext ctx;
    (void)jobs_.parallelChunks(items.size(), kGrain, ctx, [&](std::size_t, std::size_t b, std::size_t e) -> Status {
        for (std::size_t k = b; k < e; ++k) {
            items[k].hash = contentHash(*g_[items[k].g].tiles[items[k].i].get());
        }
        return Status{};
    });
    // In grid order: the first tile met with a hash is the one the later ones with it are compared with.
    for (Item& it : items) {
        const TileRef& own = g_[it.g].tiles[it.i];
        std::vector<TileRef>& bucket = table_.buckets[it.hash];
        if (bucket.empty()) {
            bucket.push_back(own);
        } else if (bucket.front().get() != own.get()) {
            it.match = &bucket.front();
        }
    }
    // Compared in parallel (the table is not changed meanwhile), then replaced in grid order. A tile dropped that no
    // one else held goes to the bin, for the next copies.
    (void)jobs_.parallelChunks(items.size(), kGrain, ctx, [&](std::size_t, std::size_t b, std::size_t e) -> Status {
        for (std::size_t k = b; k < e; ++k) {
            Item& it = items[k];
            if (it.match != nullptr) {
                it.same = sameContents(*g_[it.g].tiles[it.i].get(), *it.match->get());
            }
        }
        return Status{};
    });
    std::uint64_t replaced = 0;
    for (const Item& it : items) {
        if (it.match != nullptr && it.same) {
            TileRef& own = g_[it.g].tiles[it.i];
            if (Tile* dropped = own.dropKeepingLast()) {
                bin_.put(dropped);
            }
            own = *it.match;
            ++replaced;
        }
    }
    // The same hash, other contents (after the loop above: a bucket that grows may move the tile it compared with):
    // kept as it is, and in the table, though never matched.
    for (const Item& it : items) {
        if (it.match != nullptr && !it.same) {
            table_.buckets[it.hash].push_back(g_[it.g].tiles[it.i]);
        }
    }
    return replaced;
}

struct KeyframeLedger::Impl {
    // Per tile a keyframe holds: the places (grid, index) at which a run of keyframes holding it there starts.
    std::unordered_map<const Tile*, std::uint64_t> held;
    std::uint64_t heldBytes = 0;
    // Per tile of the newest keyframe: the places where it holds it.
    std::unordered_map<const Tile*, std::uint64_t> newest;
    std::uint64_t newestBytes = 0;
    std::vector<const Tile*> last[3]; // the newest keyframe's tiles
};

KeyframeLedger::KeyframeLedger() : impl_(std::make_unique<Impl>()) {}
KeyframeLedger::~KeyframeLedger() = default;

void KeyframeLedger::clear()
{
    impl_ = std::make_unique<Impl>();
}

void KeyframeLedger::add(const IStockModel::Snapshot& snapBase)
{
    const auto& snap = dynamic_cast<const TriDexelSnapshot&>(snapBase);
    Impl& m = *impl_;
    const auto leaveNewest = [&m](const Tile* q) {
        const auto it = m.newest.find(q);
        if (--it->second == 0) {
            m.newestBytes -= q->bytes();
            m.newest.erase(it);
        }
    };
    for (int g = 0; g < 3; ++g) {
        const std::vector<TileRef>& tiles = snap.tiles[g];
        std::vector<const Tile*>& last = m.last[g];
        if (last.size() != tiles.size()) { // the first keyframe (or one of another grid: its places counted afresh)
            for (const Tile* q : last) {
                leaveNewest(q);
            }
            last.assign(tiles.size(), nullptr);
        }
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const Tile* p = tiles[i].get();
            const Tile* q = last[i];
            if (p == q) {
                continue;
            }
            if (m.held[p]++ == 0) {
                m.heldBytes += p->bytes();
            }
            if (q != nullptr) {
                leaveNewest(q);
            }
            if (m.newest[p]++ == 0) {
                m.newestBytes += p->bytes();
            }
            last[i] = p;
        }
    }
}

void KeyframeLedger::rebuild(const std::vector<const IStockModel::Snapshot*>& snaps)
{
    clear();
    for (const IStockModel::Snapshot* s : snaps) {
        add(*s);
    }
}

std::uint64_t KeyframeLedger::pinnedBytes() const
{
    // Every tile of the newest keyframe is among those held.
    return impl_->heldBytes - impl_->newestBytes;
}

namespace {
const TriDexelCpu& asCpu(const IStockModel& m)
{
    return dynamic_cast<const TriDexelCpu&>(m);
}
} // namespace

TriDexelInfo triDexelInfo(const IStockModel& m)
{
    return asCpu(m).info();
}

std::uint64_t triDexelHash(const IStockModel& m)
{
    return asCpu(m).hash();
}

std::uint64_t snapshotPrivateBytes(const IStockModel& m, const IStockModel::Snapshot& snap)
{
    return asCpu(m).privateBytes(dynamic_cast<const TriDexelSnapshot&>(snap));
}

SnapshotCost snapshotCost(const IStockModel& m, const IStockModel::Snapshot& snap)
{
    return asCpu(m).cost(dynamic_cast<const TriDexelSnapshot&>(snap));
}

std::uint64_t triDexelMeshTriangles(const IStockModel& m, int step)
{
    return asCpu(m).meshTriangles(step);
}

std::uint64_t triDexelWrites(const IStockModel& m)
{
    return asCpu(m).writes();
}

std::uint64_t internTiles(IStockModel& m, const IStockModel::Snapshot* prev)
{
    return dynamic_cast<TriDexelCpu&>(m).intern(dynamic_cast<const TriDexelSnapshot*>(prev));
}

} // namespace detail

std::unique_ptr<IStockModel> makeTriDexelCpu(IJobSystem& jobs)
{
    return std::make_unique<detail::TriDexelCpu>(jobs);
}

} // namespace ray::sim
