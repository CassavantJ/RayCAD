// The cutting kernels of the CPU tri-dexel model (task T-070): one straight move against the rays of one strip. The
// geometry is in toolshape.h (convex pieces, envelopes, disc hulls); the threading and ordering in tridexel.h.
// Floating point: this module is built with /fp:strict on MSVC (ray_deterministic). Its hot kernels switch off only
// FENV_ACCESS and floating-point exception semantics, which leaves MSVC's /fp:precise: IEEE operations in source order,
// no reassociation and no fused multiply-add (x64 without /arch:AVX2 has none, and Visual Studio 2022 does not contract
// without /fp:contract), so results are the same bits as under /fp:strict; GCC and Clang build with
// -ffp-contract=off -fno-fast-math, which is the same model. Strict mode costs these loops about a third of their speed
// (S6's prototype: 3.8 s under /fp:precise, 5.6 s under /fp:strict, same stock hash). third_party/dmath does the same.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fenv_access(off)
#pragma float_control(except, off)
#endif
#include "tridexel_impl.h"

#include <cmath>
#include <limits>

namespace ray::sim::detail {

Seg makeSeg(double ax, double ay, double az, double bx, double by, double bz, std::uint32_t move)
{
    Seg g{};
    g.ax = ax;
    g.ay = ay;
    g.az = az;
    g.bx = bx;
    g.by = by;
    g.bz = bz;
    g.dx = bx - ax;
    g.dy = by - ay;
    g.dz = bz - az;
    g.lenXY = std::sqrt(g.dx * g.dx + g.dy * g.dy);
    g.vertical = !(g.lenXY > 1e-12);
    if (!g.vertical) {
        g.ux = g.dx / g.lenXY;
        g.uy = g.dy / g.lenXY;
        g.m = g.dz / g.lenXY;
    }
    g.move = move;
    return g;
}

void ShapeTables::layout(const CutterShape& shape)
{
    lowOff.clear();
    highOff.clear();
    perSeg = 0;
    for (const ConvexPiece& p : shape.pieces) {
        lowOff.push_back(perSeg);
        perSeg += static_cast<std::uint32_t>(p.low.k.size());
        highOff.push_back(perSeg);
        perSeg += static_cast<std::uint32_t>(p.highNeg.k.size());
    }
}

namespace {

void slopes(const Envelope& env, double m, double* c)
{
    const double am = std::abs(m);
    for (std::size_t j = 0; j < env.k.size(); ++j) {
        const double k = env.k[j];
        c[j] = k > am ? k / std::sqrt(k * k - am * am) : -1.0;
    }
}

} // namespace

void ShapeTables::fill(const CutterShape& shape, double m, double* out) const
{
    for (std::size_t p = 0; p < shape.pieces.size(); ++p) {
        slopes(shape.pieces[p].low, m, out + lowOff[p]);
        slopes(shape.pieces[p].highNeg, m, out + highOff[p]);
    }
}

bool zInterval(const ConvexPiece& pc, const Seg& g, const double* lowC, const double* highC, double x, double y,
               double& lo, double& hi)
{
    const double wx = x - g.ax, wy = y - g.ay;
    if (g.vertical) {
        const double d2 = wx * wx + wy * wy;
        if (d2 > pc.rmax * pc.rmax) {
            return false;
        }
        const double d = std::min(std::sqrt(d2), pc.rmax);
        lo = std::min(g.az, g.bz) + pc.low.eval(d);
        hi = std::max(g.az, g.bz) - pc.highNeg.eval(d);
        return true;
    }
    const double p0 = wx * g.ux + wy * g.uy;
    const double e = std::abs(wx * g.uy - wy * g.ux);
    if (e > pc.rmax) {
        return false;
    }
    const double half = std::sqrt(pc.rmax * pc.rmax - e * e);
    const double pa = std::max(0.0, p0 - half), pb = std::min(g.lenXY, p0 + half);
    if (pa > pb) {
        return false;
    }
    lo = g.az + minimiseAlong(pc.low, lowC, g.m, p0, e, pa, pb);
    hi = g.az - minimiseAlong(pc.highNeg, highC, -g.m, p0, e, pa, pb);
    return true;
}

std::int64_t cutZ(const Grid& grid, StripTiles& s, const CutterShape& shape, const Seg& g, const double* tables,
                  const ShapeTables& layout, std::vector<std::uint8_t>& tileBits, int tilesX, int tileRow0)
{
    const int nv = grid.nv;
    const int u0 = s.u0, u1 = s.u1;
    std::int64_t vol = 0;
    // Piece by piece (the order is fixed; what is left is the material minus the union of the pieces' sweeps).
    for (std::size_t p = 0; p < shape.pieces.size(); ++p) {
        const ConvexPiece& pc = shape.pieces[p];
        if (std::min(g.az, g.bz) + pc.h0 >= grid.len || std::max(g.az, g.bz) + pc.h1 <= 0) {
            continue;
        }
        const double R = pc.rmax;
        const double* lowC = tables + layout.lowOff[p];
        const double* highC = tables + layout.highOff[p];
        // A level move: the lowest point over a ray is the envelope at the ray's distance from the path, the highest
        // the top when the piece is widest there; a cylinder (both) gives one interval for every ray it reaches.
        const bool level = g.dz == 0.0 && pc.flatTop;
        const bool constant = level && pc.flatBottom;
        const float cLo = static_cast<float>(g.az + pc.h0), cHi = static_cast<float>(g.az + pc.h1);
        // Runs of rays whose material lies below this: nothing to cut there. For the constant interval exactly its
        // bottom; otherwise the lowest the sweep can reach, less a margin far above rounding.
        const double skipBelow = constant ? static_cast<double>(cLo) : std::min(g.az, g.bz) + pc.h0 - 1e-6;
        const int j0 = std::max(u0, firstIdx(std::min(g.ay, g.by) - R, grid.cu));
        const int j1 = std::min(u1 - 1, lastIdx(std::max(g.ay, g.by) + R, grid.cu));
        for (int j = j0; j <= j1; ++j) {
            const double y = (j + 0.5) * grid.cu;
            double xl = std::numeric_limits<double>::infinity(), xh = -xl;
            if (!stadiumCross(g.ax, g.ay, g.bx, g.by, R, y, xl, xh)) {
                continue;
            }
            const int i0 = std::max(0, firstIdx(xl, grid.cv)), i1 = std::min(nv - 1, lastIdx(xh, grid.cv));
            const std::size_t tileRow = static_cast<std::size_t>(j / kTileCells - tileRow0) * tilesX;
            const double wy = y - g.ay;
            // Tile by tile (a tile's runs are the grid's: Tile::top). A tile still shared with a snapshot is read in
            // place and copied only at the first ray this move takes material from.
            for (int t = i0 / kTileCols; t <= i1 / kTileCols; ++t) {
                const int c0 = Grid::col0(t), w = grid.cols(t);
                const int ia = std::max(i0, c0), ib = std::min(i1, c0 + w - 1);
                const std::size_t rowOff = static_cast<std::size_t>(j - u0) * w;
                const std::size_t topOff = static_cast<std::size_t>(j - u0) * topRuns(w);
                // Read through rt, row and top; write (once the tile is this task's own) through wt, wrow and wtop.
                const Tile* rt = &s.get(t);
                Tile* wt = s.mine(t);
                const Dexel* row = rt->d.data() + rowOff;
                const float* top = rt->top.data() + topOff;
                Dexel* wrow = wt != nullptr ? wt->d.data() + rowOff : nullptr;
                float* wtop = wt != nullptr ? wt->top.data() + topOff : nullptr;
                bool changed = false;
                for (int i = ia; i <= ib; ++i) {
                    const int li = i - c0;
                    if (li % kTopRun == 0 || i == ia) {
                        if (changed) { // the run before: its top again (changed: the tile is ours)
                            const int b = (li - 1) / kTopRun;
                            wtop[b] = wt->topOf(wrow + b * kTopRun, std::min(kTopRun, w - b * kTopRun));
                            changed = false;
                        }
                        if (static_cast<double>(top[li / kTopRun]) <= skipBelow) {
                            i = std::min(ib, c0 + (li / kTopRun + 1) * kTopRun - 1);
                            continue;
                        }
                    }
                    const Dexel& x = row[li];
                    if (x.n == 0) {
                        continue;
                    }
                    float lo = cLo, hi = cHi;
                    if (!constant) {
                        const double xc = (i + 0.5) * grid.cv;
                        if (level) {
                            const double wx = xc - g.ax;
                            double d2 = wx * wx + wy * wy;
                            if (!g.vertical) {
                                const double p0 = std::clamp(wx * g.ux + wy * g.uy, 0.0, g.lenXY);
                                const double ex = wx - p0 * g.ux, ey = wy - p0 * g.uy;
                                d2 = ex * ex + ey * ey;
                            }
                            if (d2 > R * R) {
                                continue;
                            }
                            lo = static_cast<float>(g.az + pc.low.eval(std::sqrt(d2)));
                        } else {
                            if (!g.vertical) {
                                // The sweep's bottom over this ray is at least the tip's lowest height plus the
                                // envelope at the ray's distance from the path's line: nothing to do when the material
                                // is lower.
                                const double wx = xc - g.ax;
                                const double e = std::abs(wx * g.uy - wy * g.ux);
                                if (e > R) {
                                    continue;
                                }
                                const double bound = std::min(g.az, g.bz) + pc.low.eval(e);
                                if (static_cast<double>(rt->data(x)[2 * x.n - 1]) <= bound) {
                                    continue;
                                }
                            }
                            double dlo = 0, dhi = 0;
                            if (!zInterval(pc, g, lowC, highC, xc, y, dlo, dhi)) {
                                continue;
                            }
                            lo = static_cast<float>(dlo);
                            hi = static_cast<float>(dhi);
                        }
                    }
                    // A ray this move takes nothing from is left alone (and its tile is not copied when shared).
                    if (!cutsInterval(*rt, x, lo, hi)) {
                        continue;
                    }
                    if (wt == nullptr) { // shared: written into a copy
                        wt = &s.own(t);
                        rt = wt;
                        wrow = wt->d.data() + rowOff;
                        wtop = wt->top.data() + topOff;
                        row = wrow;
                        top = wtop;
                    }
                    const double removed = subtractInterval(*wt, wrow[li], lo, hi);
                    ++s.writes;
                    if (removed > 0) {
                        vol += static_cast<std::int64_t>(removed * kVolumeScale + 0.5);
                        tileBits[tileRow + static_cast<std::size_t>(i / kTileCells)] = 1;
                        changed = true;
                    }
                }
                if (changed) {
                    const int b = (ib - c0) / kTopRun;
                    wtop[b] = wt->topOf(wrow + b * kTopRun, std::min(kTopRun, w - b * kTopRun));
                }
            }
        }
    }
    return vol;
}

bool touchesZ(const Grid& grid, const TileRef* tiles, int u0, int u1, const CutterShape& shape, const Seg& g,
              const double* tables, const ShapeTables& layout)
{
    const double R = shape.rmax;
    if (std::min(g.az, g.bz) + shape.hmin >= grid.len || std::max(g.az, g.bz) + shape.hmax <= 0) {
        return false;
    }
    const int j0 = std::max(u0, firstIdx(std::min(g.ay, g.by) - R, grid.cu));
    const int j1 = std::min(u1 - 1, lastIdx(std::max(g.ay, g.by) + R, grid.cu));
    const int nv = grid.nv;
    for (int j = j0; j <= j1; ++j) {
        const double y = (j + 0.5) * grid.cu;
        double xl = std::numeric_limits<double>::infinity(), xh = -xl;
        if (!stadiumCross(g.ax, g.ay, g.bx, g.by, R, y, xl, xh)) {
            continue;
        }
        const int i0 = std::max(0, firstIdx(xl, grid.cv)), i1 = std::min(nv - 1, lastIdx(xh, grid.cv));
        for (int i = i0; i <= i1; ++i) {
            const int t = i / kTileCols;
            const Tile& s = *tiles[t].get();
            const Dexel& x = s.d[static_cast<std::size_t>(j - u0) * grid.cols(t) + (i - Grid::col0(t))];
            if (x.n == 0) {
                continue;
            }
            const double xc = (i + 0.5) * grid.cv;
            for (std::size_t p = 0; p < shape.pieces.size(); ++p) {
                double lo = 0, hi = 0;
                if (zInterval(shape.pieces[p], g, tables + layout.lowOff[p], tables + layout.highOff[p], xc, y, lo,
                              hi) &&
                    overlaps(s, x, lo, hi)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void cutLateral(const Grid& grid, int gi, StripTiles& s, const CutterShape& shape, const Seg& g,
                LateralScratch& scratch)
{
    const int u0 = s.u0, u1 = s.u1;
    // In the grid's own axes: rows are layers (z), columns run across the rays (y for the X grid, x for the Y grid),
    // r along the ray.
    const bool alongX = gi == kGx;
    const double ar = alongX ? g.ax : g.ay, av = alongX ? g.ay : g.ax;
    const double dr = alongX ? g.dx : g.dy, dv = alongX ? g.dy : g.dx;
    const double br = ar + dr, bv = av + dv;
    const double R = shape.rmax;
    const int nv = grid.nv;
    const int j0 = std::max(0, firstIdx(std::min(av, bv) - R, grid.cv));
    const int j1 = std::min(nv - 1, lastIdx(std::max(av, bv) + R, grid.cv));
    const int k0 = std::max(u0, firstIdx(std::min(g.az, g.bz) + shape.hmin, grid.cu));
    const int k1 = std::min(u1 - 1, lastIdx(std::max(g.az, g.bz) + shape.hmax, grid.cu));
    if (j0 > j1 || k0 > k1) {
        return;
    }
    // Layer by layer: the hulls of each piece's slice at that height swept over the move. A layer where every piece is
    // missed entirely or met only on its widest span (a plateau: the cylinder of a flat or ball end mill) in the same
    // way as the layer below has the layer below's hulls, so they are built only where that is not so (near the tip
    // and the top of the flutes, through a ball or a cone), and each column's interval is kept for the whole run.
    const std::size_t np = shape.pieces.size();
    const std::size_t cols = static_cast<std::size_t>(j1 - j0 + 1);
    std::vector<DiscHull>& hulls = scratch.hulls;
    std::vector<std::uint32_t>& hb = scratch.first; // per piece: first hull and one past the last (current layer)
    std::vector<double>& times = scratch.times;
    std::vector<std::uint8_t>& state = scratch.state; // per piece, this layer and the one below: how the layer meets it
    std::vector<float>& ivl = scratch.ivl;            // per (column, piece): lo, hi of the current run
    std::vector<std::uint32_t>& stamp = scratch.stamp; // per column: the run its intervals belong to (+1)
    hb.assign(2 * np, 0);
    state.assign(2 * np, 3);
    ivl.resize(2 * cols * np);
    stamp.assign(cols, 0);
    std::uint32_t run = 0;
    const double zLo = std::min(g.az, g.bz), zHi = std::max(g.az, g.bz);
    for (int k = k0; k <= k1; ++k) {
        const std::size_t L = static_cast<std::size_t>(k - k0);
        const double z = (k + 0.5) * grid.cu;
        const double hLoAll = z - zHi, hHiAll = z - zLo; // the heights on the tool this layer meets over the move
        bool reuse = k > k0;
        std::uint8_t* cur = state.data() + (L % 2) * np;
        const std::uint8_t* prev = state.data() + ((L + 1) % 2) * np;
        for (std::size_t p = 0; p < np; ++p) {
            const ConvexPiece& pc = shape.pieces[p];
            std::uint8_t code = 2;
            if (hHiAll < pc.h0 || hLoAll > pc.h1) {
                code = 0;
            } else if (hLoAll >= pc.plateauLo && hHiAll <= pc.plateauHi) {
                code = 1;
            }
            cur[p] = code;
            reuse = reuse && code != 2 && code == prev[p];
        }
        if (!reuse) {
            ++run;
            hulls.clear();
            for (std::size_t p = 0; p < np; ++p) {
                hb[2 * p] = static_cast<std::uint32_t>(hulls.size());
                const ConvexPiece& pc = shape.pieces[p];
                if (cur[p] == 0) {
                    hb[2 * p + 1] = static_cast<std::uint32_t>(hulls.size());
                    continue;
                }
                if (g.dz == 0.0) {
                    const double rho = pc.radiusAt(z - g.az);
                    if (rho > 0) {
                        DiscHull hull{ar, av, rho, br, bv, rho, 0, 0, false, {}, {}};
                        hull.build();
                        hulls.push_back(hull);
                    }
                    hb[2 * p + 1] = static_cast<std::uint32_t>(hulls.size());
                    continue;
                }
                // h(t) = z - az - dz t lies in [h0, h1] for t in [ta, tb].
                const double base = z - g.az;
                const double tA = (base - pc.h0) / g.dz, tB = (base - pc.h1) / g.dz;
                const double ta = std::max(0.0, std::min(tA, tB)), tb = std::min(1.0, std::max(tA, tB));
                if (ta <= tb) {
                    times.clear();
                    times.push_back(ta);
                    // The profile's breakpoints strictly inside the heights the move covers, in the order of t.
                    const double hta = base - g.dz * ta, htb = base - g.dz * tb;
                    const double hLo = std::min(hta, htb), hHi = std::max(hta, htb);
                    const auto b = std::upper_bound(pc.ph.begin(), pc.ph.end(), hLo);
                    const auto e = std::lower_bound(pc.ph.begin(), pc.ph.end(), hHi);
                    if (b < e) {
                        const std::size_t ib = static_cast<std::size_t>(b - pc.ph.begin());
                        const std::size_t ie = static_cast<std::size_t>(e - pc.ph.begin());
                        if (g.dz > 0) { // t grows as h falls
                            for (std::size_t i = ie; i-- > ib;) {
                                times.push_back((base - pc.ph[i]) / g.dz);
                            }
                        } else {
                            for (std::size_t i = ib; i < ie; ++i) {
                                times.push_back((base - pc.ph[i]) / g.dz);
                            }
                        }
                    }
                    times.push_back(tb);
                    for (std::size_t q = 0; q + 1 < times.size(); ++q) {
                        const double t0 = std::clamp(times[q], ta, tb), t1 = std::clamp(times[q + 1], ta, tb);
                        if (t1 < t0 || (t1 == t0 && times.size() > 2)) {
                            continue;
                        }
                        const double r0 = pc.radiusAt(base - g.dz * t0), r1 = pc.radiusAt(base - g.dz * t1);
                        DiscHull hull{ar + dr * t0, av + dv * t0, r0, ar + dr * t1, av + dv * t1, r1, 0, 0,
                                      false,        {},           {}};
                        hull.build();
                        hulls.push_back(hull);
                    }
                }
                hb[2 * p + 1] = static_cast<std::uint32_t>(hulls.size());
            }
        }
        // Tile by tile; a tile still shared with a snapshot is copied only at the first ray that loses material.
        for (int t = j0 / kTileCols; t <= j1 / kTileCols; ++t) {
            const int c0 = Grid::col0(t), w = grid.cols(t);
            const int ja = std::max(j0, c0), jb = std::min(j1, c0 + w - 1);
            const std::size_t rowOff = static_cast<std::size_t>(k - u0) * w;
            const Tile* rt = &s.get(t);
            Tile* wt = s.mine(t);
            const Dexel* row = rt->d.data() + rowOff; // read; written through wrow once the tile is ours
            Dexel* wrow = wt != nullptr ? wt->d.data() + rowOff : nullptr;
            for (int j = ja; j <= jb; ++j) {
                if (row[j - c0].n == 0) {
                    continue;
                }
                const std::size_t c = static_cast<std::size_t>(j - j0);
                float* iv = ivl.data() + 2 * c * np;
                if (stamp[c] != run) {
                    // The column's intervals for this run, computed when the run first meets material in it.
                    stamp[c] = run;
                    const double y = (j + 0.5) * grid.cv;
                    for (std::size_t p = 0; p < np; ++p) {
                        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                        for (std::uint32_t h = hb[2 * p]; h < hb[2 * p + 1]; ++h) {
                            hulls[h].cross(y, lo, hi);
                        }
                        iv[2 * p] = lo <= hi ? static_cast<float>(lo) : 1.0f;
                        iv[2 * p + 1] = lo <= hi ? static_cast<float>(hi) : 0.0f;
                    }
                }
                for (std::size_t p = 0; p < np; ++p) {
                    // A piece that takes nothing from the ray writes nothing (tested inline: most calls in a pocket's
                    // re-sweeps are such); the first that does in a shared tile writes into a copy.
                    if (iv[2 * p] < iv[2 * p + 1] && cutsInterval(*rt, row[j - c0], iv[2 * p], iv[2 * p + 1])) {
                        if (wt == nullptr) {
                            wt = &s.own(t);
                            rt = wt;
                            wrow = wt->d.data() + rowOff;
                            row = wrow;
                        }
                        Dexel& x = wrow[j - c0];
                        subtractInterval(*wt, x, iv[2 * p], iv[2 * p + 1]);
                        ++s.writes;
                        if (x.n == 0) {
                            break;
                        }
                    }
                }
            }
        }
    }
}

} // namespace ray::sim::detail
