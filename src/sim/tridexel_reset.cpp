// Filling the CPU tri-dexel grids from a triangle soup (task T-070, IStockModel::reset).
//
// Every ray is cast against the soup: a triangle counts for a ray when the ray's point lies inside its projection on
// the plane across the ray, with the edge functions computed from the edge's two ends in one fixed (lexicographic)
// order and a fixed rule for a point exactly on an edge, so that two triangles sharing an edge always see it with
// exactly opposite values and exactly one of them takes a point on it (the "top-left" rule of rasterisers). A ray
// through an edge or a vertex of a closed soup therefore crosses it exactly once. Each crossing adds +1 (the soup's
// normal against the ray) or -1 to a winding number; material is where it is not 0, so either orientation of the soup
// works.
#include "tridexel_impl.h"

#include <limits>

namespace ray::sim::detail {

namespace {

// Coordinates across the ray (p, q) and along it (r), right-handed: Z rays (x, y; z), X rays (y, z; x), Y rays (z, x;
// y).
struct Frame {
    int p, q, r;
};
Frame frameOf(int gi)
{
    switch (gi) {
    case kGx:
        return {1, 2, 0};
    case kGy:
        return {2, 0, 1};
    default:
        return {0, 1, 2};
    }
}

struct P2 {
    double p, q;
};

inline bool lexLess(const P2& a, const P2& b)
{
    return a.p < b.p || (a.p == b.p && a.q < b.q);
}

// orient(a, b, x): positive when x is left of a -> b; computed from the edge's ends in lexicographic order.
inline double edgeFn(const P2& a, const P2& b, const P2& x)
{
    if (lexLess(b, a)) {
        return -((a.p - b.p) * (x.q - b.q) - (a.q - b.q) * (x.p - b.p));
    }
    return (b.p - a.p) * (x.q - a.q) - (b.q - a.q) * (x.p - a.p);
}

struct Tri {
    P2 v[3];
    double r[3];
    double area; // signed, twice the projected area
    double umin, umax, vmin, vmax;
};

struct Cross {
    int v; // column of the ray in the row
    double depth;
    int sign; // +1 entering (normal against the ray), -1 leaving
};

} // namespace

void fillFromSoup(Grid& grid, int gi, const kernel::TriSoup& soup, const Vec3& origin, IJobSystem& jobs)
{
    const Frame f = frameOf(gi);
    // Which of p, q is the grid's row axis u: Z grid rows y = q, X grid rows z = q, Y grid rows z = p.
    const bool uIsP = gi == kGy;
    const double o[3] = {origin.x, origin.y, origin.z};
    std::vector<Tri> tris;
    const std::size_t nv = soup.positions.size() / 3;
    tris.reserve(soup.indices.size() / 3);
    for (std::size_t t = 0; t + 2 < soup.indices.size(); t += 3) {
        Tri tri{};
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const std::size_t i = soup.indices[t + k];
            if (i >= nv) {
                ok = false;
                break;
            }
            const double c[3] = {soup.positions[3 * i] - o[0], soup.positions[3 * i + 1] - o[1],
                                 soup.positions[3 * i + 2] - o[2]};
            if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
                ok = false;
                break;
            }
            tri.v[k] = {c[f.p], c[f.q]};
            tri.r[k] = c[f.r];
        }
        if (!ok) {
            continue;
        }
        tri.area = (tri.v[1].p - tri.v[0].p) * (tri.v[2].q - tri.v[0].q) -
                   (tri.v[1].q - tri.v[0].q) * (tri.v[2].p - tri.v[0].p);
        if (tri.area == 0.0) {
            continue; // seen edge-on: its neighbours carry the crossing
        }
        const double pmin = std::min({tri.v[0].p, tri.v[1].p, tri.v[2].p});
        const double pmax = std::max({tri.v[0].p, tri.v[1].p, tri.v[2].p});
        const double qmin = std::min({tri.v[0].q, tri.v[1].q, tri.v[2].q});
        const double qmax = std::max({tri.v[0].q, tri.v[1].q, tri.v[2].q});
        tri.umin = uIsP ? pmin : qmin;
        tri.umax = uIsP ? pmax : qmax;
        tri.vmin = uIsP ? qmin : pmin;
        tri.vmax = uIsP ? qmax : pmax;
        tris.push_back(tri);
    }
    // Triangles per strip, in soup order.
    const int strips = grid.stripCount();
    std::vector<std::vector<std::uint32_t>> bucket(static_cast<std::size_t>(strips));
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const int j0 = std::max(0, firstIdx(tris[t].umin, grid.cu));
        const int j1 = std::min(grid.nu - 1, lastIdx(tris[t].umax, grid.cu));
        if (j0 > j1) {
            continue;
        }
        for (int s = j0 / grid.rowsPerStrip; s <= j1 / grid.rowsPerStrip; ++s) {
            bucket[static_cast<std::size_t>(s)].push_back(static_cast<std::uint32_t>(t));
        }
    }
    JobContext ctx;
    (void)jobs.parallelChunks(
        static_cast<std::size_t>(strips), 1, ctx, [&](std::size_t chunk, std::size_t, std::size_t) -> Status {
            const int s = static_cast<int>(chunk);
            StripTiles st(grid, s);
            const int u0 = st.u0, u1 = st.u1;
            std::vector<Cross> cross;
            std::vector<float> iv;
            for (int j = u0; j < u1; ++j) {
                const double u = (j + 0.5) * grid.cu;
                cross.clear();
                for (std::uint32_t t : bucket[chunk]) {
                    const Tri& tri = tris[t];
                    if (u < tri.umin || u > tri.umax) {
                        continue;
                    }
                    const int i0 = std::max(0, firstIdx(tri.vmin, grid.cv));
                    const int i1 = std::min(grid.nv - 1, lastIdx(tri.vmax, grid.cv));
                    const double sigma = tri.area > 0 ? 1.0 : -1.0;
                    for (int i = i0; i <= i1; ++i) {
                        const double v = (i + 0.5) * grid.cv;
                        const P2 x = uIsP ? P2{u, v} : P2{v, u};
                        double w[3];
                        bool inside = true;
                        for (int e = 0; e < 3 && inside; ++e) {
                            const P2& a = tri.v[e];
                            const P2& b = tri.v[(e + 1) % 3];
                            w[e] = edgeFn(a, b, x);
                            const double sw = sigma * w[e];
                            if (sw < 0) {
                                inside = false;
                            } else if (sw == 0) {
                                // On the edge: taken when the edge, run with the triangle on its left, points up (+q)
                                // or exactly along -p.
                                const double ep = sigma * (b.p - a.p), eq = sigma * (b.q - a.q);
                                inside = eq > 0 || (eq == 0 && ep < 0);
                            }
                        }
                        if (!inside) {
                            continue;
                        }
                        // Barycentric weights: w[e] belongs to the vertex opposite edge e.
                        const double depth = (w[1] * tri.r[0] + w[2] * tri.r[1] + w[0] * tri.r[2]) / tri.area;
                        cross.push_back({i, depth, tri.area > 0 ? -1 : +1});
                    }
                }
                std::sort(cross.begin(), cross.end(), [](const Cross& a, const Cross& b) {
                    return a.v < b.v || (a.v == b.v && (a.depth < b.depth || (a.depth == b.depth && a.sign > b.sign)));
                });
                for (int t = 0; t < grid.tilesPerStrip; ++t) {
                    Tile& tile = st.own(t);
                    const int w = grid.cols(t);
                    Dexel* row = tile.d.data() + static_cast<std::size_t>(j - u0) * w;
                    for (int i = 0; i < w; ++i) {
                        row[i] = Dexel{};
                    }
                }
                for (std::size_t c = 0; c < cross.size();) {
                    const int v = cross[c].v;
                    iv.clear();
                    int wind = 0;
                    double start = 0;
                    while (c < cross.size() && cross[c].v == v) {
                        const double z = cross[c].depth;
                        int sum = 0;
                        while (c < cross.size() && cross[c].v == v && cross[c].depth == z) {
                            sum += cross[c].sign;
                            ++c;
                        }
                        const int next = wind + sum;
                        if (wind == 0 && next != 0) {
                            start = z;
                        } else if (wind != 0 && next == 0) {
                            const float a = static_cast<float>(std::clamp(start, 0.0, grid.len));
                            const float b = static_cast<float>(std::clamp(z, 0.0, grid.len));
                            if (b > a) {
                                if (!iv.empty() && iv.back() >= a) {
                                    iv.back() = std::max(iv.back(), b);
                                } else {
                                    iv.push_back(a);
                                    iv.push_back(b);
                                }
                            }
                        }
                        wind = next;
                    }
                    const int t = v / kTileCols;
                    Tile& tile = st.own(t);
                    Dexel& x = tile.d[static_cast<std::size_t>(j - u0) * grid.cols(t) + (v - Grid::col0(t))];
                    const std::size_t n = iv.size() / 2;
                    while (x.cap < n && x.cap < kMaxDexelIntervals) {
                        tile.grow(x);
                    }
                    const std::size_t keep = std::min<std::size_t>(n, x.cap);
                    if (keep > 0) { // memcpy from an empty vector's null data() is undefined even for 0 bytes
                        std::memcpy(tile.data(x), iv.data(), sizeof(float) * 2 * keep);
                    }
                    x.n = static_cast<std::uint16_t>(keep);
                }
            }
            for (int t = 0; t < grid.tilesPerStrip; ++t) {
                st.own(t).compactIfWorthIt();
            }
            return Status{};
        });
}

} // namespace ray::sim::detail
