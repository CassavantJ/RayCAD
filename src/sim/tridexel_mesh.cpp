// Display mesh and carried solid of the CPU tri-dexel model (task T-070): the boundary of the union of Z-ray columns.
//
// A column is a block of step x step Z rays (one ray at full resolution) and holds the union of their intervals. Its
// boundary is: per interval a top face and a bottom face, and on each side the walls where this column has material
// and its neighbour has none (the neighbour outside the region or the stock is empty). Walls of one side are split at
// every height any of the four columns around each vertical corner line has an interval end at, so every edge of the
// surface is met by triangles in pairs (no T-junctions): a closed, outward-oriented surface. For display every tile is
// also capped where its edge cuts material, so each tile's triangles are closed on their own and a viewer can replace
// any set of tiles (the caps of neighbouring tiles lie back to back inside the stock). Faces are emitted per
// column row in parallel chunks and joined in row order, so the result does not depend on the thread count.
#include "tridexel_impl.h"

#include <cmath>
#include <limits>

namespace ray::sim::detail {

namespace {

// The columns of a region: block (bx, by) covers cells [cellX(bx), cellX(bx + 1)) x [cellY(by), cellY(by + 1)).
struct Columns {
    int bx, by; // blocks per row, rows
    int ci0, ci1, cj0, cj1, step;
    std::vector<std::vector<float>> data;        // per block row: the intervals of its blocks, one after another
    std::vector<std::vector<std::uint32_t>> off; // per block row: bx + 1 offsets into data, in intervals
    int cellX(int b) const { return std::clamp((ci0 / step + b) * step, ci0, ci1); }
    int cellY(int b) const { return std::clamp((cj0 / step + b) * step, cj0, cj1); }
    // The intervals of block (x, y) as [lo, hi] pairs; empty outside the region.
    std::span<const float> at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= bx || y >= by) {
            return {};
        }
        const auto& o = off[static_cast<std::size_t>(y)];
        const auto& d = data[static_cast<std::size_t>(y)];
        return {d.data() + 2u * o[static_cast<std::size_t>(x)],
                2u * (o[static_cast<std::size_t>(x) + 1] - o[static_cast<std::size_t>(x)])};
    }
};

// Appends the intervals of the Z rays [i0, i1) of row j, tile by tile.
void appendRow(const Grid& gz, int j, int i0, int i1, std::vector<std::pair<float, float>>& out)
{
    for (int i = i0; i < i1;) {
        const int t = i / kTileCols;
        const Tile& tile = gz.tileOf(j, i);
        const int end = std::min(i1, gz.col1(t));
        const Dexel* x =
            tile.d.data() + static_cast<std::size_t>(j % gz.rowsPerStrip) * gz.cols(t) + (i - Grid::col0(t));
        for (; i < end; ++i, ++x) {
            const float* p = tile.data(*x);
            for (std::uint32_t k = 0; k < x->n; ++k) {
                out.emplace_back(p[2 * k], p[2 * k + 1]);
            }
        }
    }
}

Columns buildColumns(const Grid& gz, const MeshRegion& r, IJobSystem& jobs)
{
    Columns c;
    c.ci0 = r.ci0;
    c.ci1 = r.ci1;
    c.cj0 = r.cj0;
    c.cj1 = r.cj1;
    c.step = r.step;
    c.bx = (r.ci1 + r.step - 1) / r.step - r.ci0 / r.step;
    c.by = (r.cj1 + r.step - 1) / r.step - r.cj0 / r.step;
    c.data.resize(static_cast<std::size_t>(std::max(c.by, 0)));
    c.off.resize(static_cast<std::size_t>(std::max(c.by, 0)));
    JobContext ctx;
    (void)jobs.parallelChunks(static_cast<std::size_t>(std::max(c.by, 0)), 1, ctx,
                              [&](std::size_t row, std::size_t, std::size_t) -> Status {
                                  const int y = static_cast<int>(row);
                                  std::vector<float>& d = c.data[row];
                                  std::vector<std::uint32_t>& o = c.off[row];
                                  d.clear();
                                  o.assign(1, 0);
                                  std::vector<std::pair<float, float>> all;
                                  for (int x = 0; x < c.bx; ++x) {
                                      all.clear();
                                      for (int j = c.cellY(y); j < c.cellY(y + 1); ++j) {
                                          appendRow(gz, j, c.cellX(x), c.cellX(x + 1), all);
                                      }
                                      if (c.step > 1) {
                                          std::sort(all.begin(), all.end());
                                      }
                                      std::size_t n = 0;
                                      for (const auto& [lo, hi] : all) {
                                          if (n > 0 && lo <= d.back()) {
                                              d.back() = std::max(d.back(), hi);
                                          } else {
                                              d.push_back(lo);
                                              d.push_back(hi);
                                              ++n;
                                          }
                                      }
                                      o.push_back(o.back() + static_cast<std::uint32_t>(n));
                                  }
                                  return Status{};
                              });
    return c;
}

// The heights of the interval ends of the (up to) four columns around corner (x, y), sorted, without repeats.
void cornerHeights(const Columns& c, int x, int y, std::vector<float>& out)
{
    out.clear();
    for (int dy = -1; dy <= 0; ++dy) {
        for (int dx = -1; dx <= 0; ++dx) {
            for (float z : c.at(x + dx, y + dy)) {
                out.push_back(z);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The parts of `a` not covered by `b` (both sorted, disjoint), as [lo, hi] pairs.
void difference(std::span<const float> a, std::span<const float> b, std::vector<float>& out)
{
    out.clear();
    std::size_t j = 0;
    for (std::size_t i = 0; i < a.size(); i += 2) {
        float lo = a[i];
        const float hi = a[i + 1];
        while (j < b.size() && b[j + 1] <= lo) {
            j += 2;
        }
        std::size_t k = j;
        while (lo < hi) {
            if (k >= b.size() || b[k] >= hi) {
                out.push_back(lo);
                out.push_back(hi);
                break;
            }
            if (b[k] > lo) {
                out.push_back(lo);
                out.push_back(b[k]);
            }
            lo = std::max(lo, b[k + 1]);
            k += 2;
        }
    }
}

struct CornerPt {
    int x, y; // corner of the column lattice
    float z;
};

// Emits the faces of the blocks [x0, x1) x [y0, y1) into `out`, which provides
//   void face(float nx, float ny, float nz);           a new face with this normal
//   std::uint32_t vertex(const CornerPt&);            a vertex of the current face
//   void tri(std::uint32_t, std::uint32_t, std::uint32_t);
template <class Out>
// Each block emits its top and bottom faces, the walls of its -x and -y sides shared with a neighbour (both ways),
// and its own walls against nothing on every side that is an edge: the region's, or with capTiles a tile's (so every
// triangle belongs to the tile of the block that emits it, and each tile is closed on its own).
void emitBlocks(const Columns& c, double cellX, double cellY, int x0, int x1, int y0, int y1, bool capTiles, Out& out)
{
    std::vector<float> diff, chainP, chainQ;
    const std::size_t w = static_cast<std::size_t>(x1 - x0) + 1;
    std::vector<std::vector<float>> rowA(w), rowB(w); // corner sets of the current corner row and the next
    const auto fillRow = [&](int y, std::vector<std::vector<float>>& row) {
        for (int x = x0; x <= x1; ++x) {
            cornerHeights(c, x, y, row[static_cast<std::size_t>(x - x0)]);
        }
    };
    const auto edgeX = [&](int x) { // the line x of the block lattice is an edge
        return x == 0 || x == c.bx || (capTiles && c.cellX(x) % kTileCells == 0);
    };
    const auto edgeY = [&](int y) {
        return y == 0 || y == c.by || (capTiles && c.cellY(y) % kTileCells == 0);
    };
    const auto chain = [](const std::vector<float>& set, float z1, float z2, std::vector<float>& ch) {
        ch.clear();
        ch.push_back(z1);
        auto it = std::upper_bound(set.begin(), set.end(), z1);
        for (; it != set.end() && *it < z2; ++it) {
            ch.push_back(*it);
        }
        ch.push_back(z2);
    };
    // A wall from corner P to corner Q (the material on the left of P -> Q seen from above, the normal (nx, ny) to the
    // right), heights [z1, z2], split at the heights of both corner lines.
    const auto wall = [&](int px, int py, const std::vector<float>& setP, int qx, int qy,
                          const std::vector<float>& setQ, float z1, float z2, float nx, float ny) {
        chain(setP, z1, z2, chainP);
        chain(setQ, z1, z2, chainQ);
        out.face(nx, ny, 0.0f);
        std::vector<std::uint32_t> ip(chainP.size()), iq(chainQ.size());
        for (std::size_t i = 0; i < chainP.size(); ++i) {
            ip[i] = out.vertex({px, py, chainP[i]});
        }
        for (std::size_t i = 0; i < chainQ.size(); ++i) {
            iq[i] = out.vertex({qx, qy, chainQ[i]});
        }
        std::size_t i = 0, j = 0;
        while (i + 1 < chainP.size() || j + 1 < chainQ.size()) {
            const bool advQ = i + 1 >= chainP.size() || (j + 1 < chainQ.size() && chainQ[j + 1] < chainP[i + 1]);
            if (advQ) {
                out.tri(ip[i], iq[j], iq[j + 1]);
                ++j;
            } else {
                out.tri(ip[i], iq[j], ip[i + 1]);
                ++i;
            }
        }
    };
    const float stepW = static_cast<float>(cellX * c.step), stepH = static_cast<float>(cellY * c.step);
    const auto topOf = [&](int x, int y, float own) {
        const std::span<const float> s = c.at(x, y);
        if (s.empty()) {
            return std::pair<bool, float>{false, own};
        }
        const float t = s.back();
        return std::pair<bool, float>{std::abs(t - own) <= 2.0f * std::max(stepW, stepH), t};
    };
    std::vector<std::vector<float>>* cur = &rowA;
    std::vector<std::vector<float>>* next = &rowB;
    fillRow(y0, *cur);
    for (int y = y0; y < y1; ++y) {
        fillRow(y + 1, *next);
        for (int x = x0; x < x1; ++x) {
            const std::span<const float> mine = c.at(x, y);
            const std::vector<float>& c00 = (*cur)[static_cast<std::size_t>(x - x0)];
            const std::vector<float>& c10 = (*cur)[static_cast<std::size_t>(x - x0) + 1];
            const std::vector<float>& c01 = (*next)[static_cast<std::size_t>(x - x0)];
            const std::vector<float>& c11 = (*next)[static_cast<std::size_t>(x - x0) + 1];
            // Top and bottom faces.
            for (std::size_t k = 0; k < mine.size(); k += 2) {
                const float lo = mine[k], hi = mine[k + 1];
                float nx = 0, ny = 0, nz = 1;
                if (k + 2 == mine.size()) {
                    // The top face: tilted by the neighbours' tops (shading only).
                    const auto l = topOf(x - 1, y, hi), r = topOf(x + 1, y, hi);
                    const auto d = topOf(x, y - 1, hi), u = topOf(x, y + 1, hi);
                    const int nxv = (l.first ? 1 : 0) + (r.first ? 1 : 0), nyv = (d.first ? 1 : 0) + (u.first ? 1 : 0);
                    const float gx = nxv > 0 ? (r.second - l.second) / (static_cast<float>(nxv) * stepW) : 0.0f;
                    const float gy = nyv > 0 ? (u.second - d.second) / (static_cast<float>(nyv) * stepH) : 0.0f;
                    const float len = std::sqrt(gx * gx + gy * gy + 1.0f);
                    nx = -gx / len;
                    ny = -gy / len;
                    nz = 1.0f / len;
                }
                out.face(nx, ny, nz);
                const std::uint32_t a = out.vertex({x, y, hi}), b = out.vertex({x + 1, y, hi});
                const std::uint32_t cc = out.vertex({x + 1, y + 1, hi}), d = out.vertex({x, y + 1, hi});
                out.tri(a, b, cc);
                out.tri(a, cc, d);
                out.face(0, 0, -1);
                const std::uint32_t e = out.vertex({x, y, lo}), f = out.vertex({x, y + 1, lo});
                const std::uint32_t g = out.vertex({x + 1, y + 1, lo}), h = out.vertex({x + 1, y, lo});
                out.tri(e, f, g);
                out.tri(e, g, h);
            }
            // The side x (between column x - 1 and x), from corner (x, y) to (x, y + 1).
            const std::span<const float> none{};
            const bool ex = edgeX(x);
            if (!ex) {
                difference(c.at(x - 1, y), mine, diff); // material of the left column facing +x
                for (std::size_t k = 0; k < diff.size(); k += 2) {
                    wall(x, y, c00, x, y + 1, c01, diff[k], diff[k + 1], 1, 0);
                }
            }
            difference(mine, ex ? none : c.at(x - 1, y), diff); // facing -x
            for (std::size_t k = 0; k < diff.size(); k += 2) {
                wall(x, y + 1, c01, x, y, c00, diff[k], diff[k + 1], -1, 0);
            }
            // The side y (between row y - 1 and y), from corner (x, y) to (x + 1, y).
            const bool ey = edgeY(y);
            if (!ey) {
                difference(c.at(x, y - 1), mine, diff); // facing +y
                for (std::size_t k = 0; k < diff.size(); k += 2) {
                    wall(x + 1, y, c10, x, y, c00, diff[k], diff[k + 1], 0, 1);
                }
            }
            difference(mine, ey ? none : c.at(x, y - 1), diff); // facing -y
            for (std::size_t k = 0; k < diff.size(); k += 2) {
                wall(x, y, c00, x + 1, y, c10, diff[k], diff[k + 1], 0, -1);
            }
            if (edgeX(x + 1)) { // an edge on +x: this block's own cap
                for (std::size_t k = 0; k < mine.size(); k += 2) {
                    wall(x + 1, y, c10, x + 1, y + 1, c11, mine[k], mine[k + 1], 1, 0);
                }
            }
            if (edgeY(y + 1)) { // an edge on +y
                for (std::size_t k = 0; k < mine.size(); k += 2) {
                    wall(x + 1, y + 1, c11, x, y + 1, c01, mine[k], mine[k + 1], 0, 1);
                }
            }
        }
        std::swap(cur, next);
    }
}

// Counts triangles only.
struct CountOut {
    std::uint64_t tris = 0;
    void face(float, float, float) {}
    std::uint32_t vertex(const CornerPt&) { return 0; }
    void tri(std::uint32_t, std::uint32_t, std::uint32_t) { ++tris; }
};

// Display vertices: one per face corner, with the face's normal.
struct MeshOut {
    const Columns* c;
    double ox, oy, oz, cellX, cellY;
    std::vector<float> pos, nrm;
    std::vector<std::uint32_t> idx;
    float n[3] = {0, 0, 1};
    void face(float x, float y, float z)
    {
        n[0] = x;
        n[1] = y;
        n[2] = z;
    }
    std::uint32_t vertex(const CornerPt& p)
    {
        const std::uint32_t i = static_cast<std::uint32_t>(pos.size() / 3);
        pos.push_back(static_cast<float>(ox + c->cellX(p.x) * cellX));
        pos.push_back(static_cast<float>(oy + c->cellY(p.y) * cellY));
        pos.push_back(static_cast<float>(oz + static_cast<double>(p.z)));
        nrm.insert(nrm.end(), {n[0], n[1], n[2]});
        return i;
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t d) { idx.insert(idx.end(), {a, b, d}); }
};

// Chunks of block rows for the solid's parallel pass: a fixed size, so the split never depends on the thread count.
constexpr int kRowsPerChunk = 8;

// The display tiles of a region (tridexel.h): tile (tx, ty) holds blocks [bx0, bx1) x [by0, by1).
struct TileSpan {
    int tx, ty, bx0, bx1, by0, by1;
};
std::vector<TileSpan> tilesOf(const Columns& c)
{
    std::vector<TileSpan> out;
    const auto blockOf = [&](int cell, int origin) {
        return (cell - origin + c.step - 1) / c.step;
    };
    for (int cj = c.cj0; cj < c.cj1; cj += kTileCells) {
        for (int ci = c.ci0; ci < c.ci1; ci += kTileCells) {
            const int ci1 = std::min(c.ci1, ci + kTileCells), cj1 = std::min(c.cj1, cj + kTileCells);
            out.push_back({ci / kTileCells, cj / kTileCells, blockOf(ci, c.ci0), blockOf(ci1, c.ci0),
                           blockOf(cj, c.cj0), blockOf(cj1, c.cj0)});
        }
    }
    return out;
}

} // namespace

std::uint64_t columnMeshTriangles(const Grid& gz, const MeshRegion& r, IJobSystem& jobs)
{
    const Columns c = buildColumns(gz, r, jobs);
    const std::vector<TileSpan> tiles = tilesOf(c);
    std::vector<std::uint64_t> counts(tiles.size(), 0);
    JobContext ctx;
    (void)jobs.parallelChunks(tiles.size(), 1, ctx, [&](std::size_t t, std::size_t, std::size_t) -> Status {
        CountOut out;
        const TileSpan& ts = tiles[t];
        emitBlocks(c, 1.0, 1.0, ts.bx0, ts.bx1, ts.by0, ts.by1, true, out);
        counts[t] = out.tris;
        return Status{};
    });
    std::uint64_t total = 0;
    for (std::uint64_t n : counts) {
        total += n;
    }
    return total;
}

kernel::Mesh columnMesh(const Grid& gz, const MeshRegion& r, double cx, double cy, const Vec3& origin, IJobSystem& jobs)
{
    const Columns c = buildColumns(gz, r, jobs);
    const std::vector<TileSpan> tiles = tilesOf(c);
    const std::uint32_t tilesX = static_cast<std::uint32_t>((gz.nv + kTileCells - 1) / kTileCells);
    std::vector<MeshOut> parts(tiles.size());
    JobContext ctx;
    (void)jobs.parallelChunks(tiles.size(), 1, ctx, [&](std::size_t t, std::size_t, std::size_t) -> Status {
        MeshOut& out = parts[t];
        out.c = &c;
        out.ox = origin.x;
        out.oy = origin.y;
        out.oz = origin.z;
        out.cellX = cx;
        out.cellY = cy;
        const TileSpan& ts = tiles[t];
        emitBlocks(c, cx, cy, ts.bx0, ts.bx1, ts.by0, ts.by1, true, out);
        return Status{};
    });
    kernel::Mesh mesh;
    std::size_t nv = 0, ni = 0;
    for (const MeshOut& p : parts) {
        nv += p.pos.size();
        ni += p.idx.size();
    }
    mesh.positions.reserve(nv);
    mesh.normals.reserve(nv);
    mesh.indices.reserve(ni);
    for (std::size_t t = 0; t < parts.size(); ++t) {
        MeshOut& p = parts[t];
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.positions.size() / 3);
        const std::uint32_t first = static_cast<std::uint32_t>(mesh.indices.size());
        mesh.positions.insert(mesh.positions.end(), p.pos.begin(), p.pos.end());
        mesh.normals.insert(mesh.normals.end(), p.nrm.begin(), p.nrm.end());
        for (std::uint32_t i : p.idx) {
            mesh.indices.push_back(base + i);
        }
        // One face range per tile: SubId{Face, ty * tilesX + tx}.
        mesh.faces.push_back({kernel::SubId{kernel::SubKind::Face, static_cast<std::uint32_t>(tiles[t].ty) * tilesX +
                                                                       static_cast<std::uint32_t>(tiles[t].tx)},
                              first, static_cast<std::uint32_t>(p.idx.size())});
        p = MeshOut{};
    }
    return mesh;
}

namespace {

// Welded vertices: one per corner line and height in its set (cornerHeights), numbered corner by corner.
struct SolidOut {
    const Columns* c;
    const std::vector<std::uint64_t>* base;      // per corner (y * (bx + 1) + x): first vertex
    const std::vector<std::vector<float>>* sets; // per corner: its heights
    std::vector<std::uint32_t> idx;
    void face(float, float, float) {}
    std::uint32_t vertex(const CornerPt& p)
    {
        const std::size_t k = static_cast<std::size_t>(p.y) * (static_cast<std::size_t>(c->bx) + 1) + p.x;
        const std::vector<float>& s = (*sets)[k];
        const auto it = std::lower_bound(s.begin(), s.end(), p.z);
        return static_cast<std::uint32_t>((*base)[k] + static_cast<std::uint64_t>(it - s.begin()));
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t d) { idx.insert(idx.end(), {a, b, d}); }
};

} // namespace

kernel::TriSoup columnSolid(const Grid& gz, double cx, double cy, const Vec3& origin, IJobSystem& jobs)
{
    const MeshRegion r{0, gz.nv, 0, gz.nu, 1};
    const Columns c = buildColumns(gz, r, jobs);
    const std::size_t corners = (static_cast<std::size_t>(c.bx) + 1) * (static_cast<std::size_t>(c.by) + 1);
    std::vector<std::vector<float>> sets(corners);
    JobContext ctx;
    (void)jobs.parallelChunks(
        static_cast<std::size_t>(c.by) + 1, 1, ctx, [&](std::size_t y, std::size_t, std::size_t) -> Status {
            for (int x = 0; x <= c.bx; ++x) {
                cornerHeights(c, x, static_cast<int>(y), sets[y * (static_cast<std::size_t>(c.bx) + 1) + x]);
            }
            return Status{};
        });
    std::vector<std::uint64_t> base(corners + 1, 0);
    for (std::size_t k = 0; k < corners; ++k) {
        base[k + 1] = base[k] + sets[k].size();
    }
    kernel::TriSoup soup;
    soup.positions.resize(3 * base[corners]);
    for (std::size_t k = 0; k < corners; ++k) {
        const int x = static_cast<int>(k % (static_cast<std::size_t>(c.bx) + 1));
        const int y = static_cast<int>(k / (static_cast<std::size_t>(c.bx) + 1));
        const double px = origin.x + c.cellX(x) * cx, py = origin.y + c.cellY(y) * cy;
        for (std::size_t i = 0; i < sets[k].size(); ++i) {
            double* p = soup.positions.data() + 3 * (base[k] + i);
            p[0] = px;
            p[1] = py;
            p[2] = origin.z + static_cast<double>(sets[k][i]);
        }
    }
    const std::size_t chunks = static_cast<std::size_t>((c.by + kRowsPerChunk - 1) / kRowsPerChunk);
    std::vector<SolidOut> parts(chunks);
    (void)jobs.parallelChunks(chunks, 1, ctx, [&](std::size_t chunk, std::size_t, std::size_t) -> Status {
        SolidOut& out = parts[chunk];
        out.c = &c;
        out.base = &base;
        out.sets = &sets;
        const int y0 = static_cast<int>(chunk) * kRowsPerChunk;
        emitBlocks(c, cx, cy, 0, c.bx, y0, std::min(c.by, y0 + kRowsPerChunk), false, out);
        return Status{};
    });
    for (SolidOut& p : parts) {
        soup.indices.insert(soup.indices.end(), p.idx.begin(), p.idx.end());
        p.idx = {};
    }
    return soup;
}

} // namespace ray::sim::detail
