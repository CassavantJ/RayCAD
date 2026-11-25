// The setup checks (added after T-074): fixtures against the part and the initial stock. What and why: verify.h, SETUP.
#include "verify_impl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ray::sim::detail {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
// Triangles of the other solid near a patch above which a patch wider than the smallest is cut at once (its corners
// would settle nothing among so many).
constexpr int kMaxNear = 32;
// Pieces (piecesIn) per chunk of the parallel loop.
constexpr std::size_t kGrain = 4;

Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 madd(const Vec3& a, const Vec3& d, double t)
{
    return {a.x + d.x * t, a.y + d.y * t, a.z + d.z * t};
}
Vec3 mid(const Vec3& a, const Vec3& b)
{
    return {0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.5 * (a.z + b.z)};
}
double dist2(const Vec3& a, const Vec3& b)
{
    const Vec3 d = sub(a, b);
    return dot(d, d);
}

// Squared distance from p to segment ab.
double segmentDist2(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = sub(b, a);
    const double len2 = dot(ab, ab);
    const double t = len2 > 0 ? std::clamp(dot(sub(p, a), ab) / len2, 0.0, 1.0) : 0.0;
    return dist2(p, madd(a, ab, t));
}

// Squared distance from p to the (filled) triangle t (Ericson, Real-Time Collision Detection 5.1.5, by Voronoi regions;
// a triangle without area by its edges).
double triangleDist2(const Vec3& p, const Vec3* t)
{
    const Vec3& a = t[0];
    const Vec3& b = t[1];
    const Vec3& c = t[2];
    const Vec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        return dot(ap, ap);
    }
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        return dot(bp, bp);
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        return d1 - d3 > 0 ? dist2(p, madd(a, ab, d1 / (d1 - d3))) : segmentDist2(p, a, b);
    }
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        return dot(cp, cp);
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        return d2 - d6 > 0 ? dist2(p, madd(a, ac, d2 / (d2 - d6))) : segmentDist2(p, a, c);
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        const double den = (d4 - d3) + (d5 - d6);
        return den > 0 ? dist2(p, madd(b, sub(c, b), (d4 - d3) / den)) : segmentDist2(p, b, c);
    }
    const double den = va + vb + vc;
    if (!(den > 0)) {
        return std::min({segmentDist2(p, a, b), segmentDist2(p, b, c), segmentDist2(p, c, a)});
    }
    const double v = vb / den, w = vc / den;
    return dist2(p, madd(madd(a, ab, v), ac, w));
}

Box3 triangleBox(const Vec3* v)
{
    return {
        {std::min({v[0].x, v[1].x, v[2].x}), std::min({v[0].y, v[1].y, v[2].y}), std::min({v[0].z, v[1].z, v[2].z})},
        {std::max({v[0].x, v[1].x, v[2].x}), std::max({v[0].y, v[1].y, v[2].y}), std::max({v[0].z, v[1].z, v[2].z})}};
}

// The distance from p to the soup's surface (+inf for an empty soup): boxes about p growing from `r` until a triangle
// lies within the box's half-width.
double surfaceDistance(const SoupIndex& soup, const Vec3& p, double r)
{
    const Box3& b = soup.bounds();
    const double span = std::max({b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z, 1.0});
    double best2 = kInf;
    for (; r < 4.0 * span + std::abs(p.x) + std::abs(p.y) + std::abs(p.z); r *= 4.0) {
        soup.any(grown(Box3{p, p}, r), [&](std::uint32_t, const Vec3* v) {
            best2 = std::min(best2, triangleDist2(p, v));
            return false;
        });
        if (best2 <= r * r) {
            break;
        }
    }
    return std::sqrt(best2);
}

// Whether p, which lies more than `clear` from the soup's surface, is inside it. SoupIndex::contains casts one ray
// along +Z, which is unsure where that ray runs along a face of the soup (a point on the plane of a vertical face, as
// where a jaw's clamping face meets the stock's side): the vote of three rays from points within 0.4 `clear` of p,
// none of which can be on the other side of the surface.
bool insideClear(const SoupIndex& soup, const Vec3& p, double clear)
{
    const double s = std::max(clear, 0.0);
    const Vec3 at[3] = {{p.x + 0.31 * s, p.y + 0.17 * s, p.z},
                        {p.x - 0.23 * s, p.y + 0.29 * s, p.z},
                        {p.x + 0.13 * s, p.y - 0.37 * s, p.z}};
    int votes = 0;
    for (const Vec3& q : at) {
        votes += soup.contains(q) ? 1 : 0;
    }
    return votes >= 2;
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct P2 {
    double x, y;
};
double orient(const P2& a, const P2& b, const P2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
// Inclusive: on an edge counts as inside (either winding).
bool inTriangle(const P2& p, const P2* t)
{
    const double a = orient(t[0], t[1], p), b = orient(t[1], t[2], p), c = orient(t[2], t[0], p);
    return (a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0);
}
// Whether segments pq and rs meet (touching counts).
bool segmentsMeet(const P2& p, const P2& q, const P2& r, const P2& s)
{
    const double d1 = orient(r, s, p), d2 = orient(r, s, q), d3 = orient(p, q, r), d4 = orient(p, q, s);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    const auto on = [](const P2& a, const P2& b, const P2& c, double o) {
        return o == 0 && std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= c.y &&
               c.y <= std::max(a.y, b.y);
    };
    return on(r, s, p, d1) || on(r, s, q, d2) || on(p, q, r, d3) || on(p, q, s, d4);
}
bool segmentMeetsTriangle(const P2& a, const P2& b, const P2* t)
{
    return inTriangle(a, t) || inTriangle(b, t) || segmentsMeet(a, b, t[0], t[1]) || segmentsMeet(a, b, t[1], t[2]) ||
           segmentsMeet(a, b, t[2], t[0]);
}
bool samePoint(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Whether every point of patch `p` lies within tol of the union of the triangles `close` (all the other solid's
// triangles near it) through one plane: the triangles lying in the plane of `ref` (one of them) form a region of a
// face; when the patch's corners lie within tol of that plane and the patch's shadow on it lies inside the region (no
// edge of the region's boundary crosses the shadow, and its centre's shadow is in the region), every point of the patch
// is within tol of the region (straight down onto it; the distance to the plane is affine, so the corners bound it).
// What lets a patch lying on a face of many triangles (a face's fan, the two triangles of a rectangle) settle at once.
bool coveredByFace(const Vec3* p, const Vec3& c, const Vec3* const* close, int n, const Vec3* ref, double tol)
{
    const Vec3 nrm = cross(sub(ref[1], ref[0]), sub(ref[2], ref[0]));
    const double len = std::sqrt(dot(nrm, nrm));
    if (!(len > 0)) {
        return false;
    }
    const Vec3 nz{nrm.x / len, nrm.y / len, nrm.z / len};
    const double off = dot(nz, ref[0]);
    constexpr double kCoplanarMm = 1e-7;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(dot(nz, p[k]) - off) > tol - 2 * kCoplanarMm) {
            return false;
        }
    }
    // A basis of the plane.
    const Vec3 any = std::abs(nz.x) < 0.6 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 u = cross(nz, any);
    const double ul = std::sqrt(dot(u, u));
    u = {u.x / ul, u.y / ul, u.z / ul};
    const Vec3 v = cross(nz, u);
    const auto flat = [&](const Vec3& q) {
        return P2{dot(q, u), dot(q, v)};
    };
    // The triangles in the plane.
    const Vec3* group[kMaxNear];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const Vec3* t = close[i];
        if (std::abs(dot(nz, t[0]) - off) <= kCoplanarMm && std::abs(dot(nz, t[1]) - off) <= kCoplanarMm &&
            std::abs(dot(nz, t[2]) - off) <= kCoplanarMm) {
            group[m++] = t;
        }
    }
    const P2 shadow[3] = {flat(p[0]), flat(p[1]), flat(p[2])};
    // The region's boundary: edges of the group that no other triangle of it shares.
    for (int i = 0; i < m; ++i) {
        for (int e = 0; e < 3; ++e) {
            const Vec3& a = group[i][e];
            const Vec3& b = group[i][(e + 1) % 3];
            bool shared = false;
            for (int j = 0; j < m && !shared; ++j) {
                if (j == i) {
                    continue;
                }
                for (int f = 0; f < 3 && !shared; ++f) {
                    const Vec3& x = group[j][f];
                    const Vec3& y = group[j][(f + 1) % 3];
                    shared = (samePoint(a, y) && samePoint(b, x)) || (samePoint(a, x) && samePoint(b, y));
                }
            }
            if (!shared && segmentMeetsTriangle(flat(a), flat(b), shadow)) {
                return false;
            }
        }
    }
    const P2 centre = flat(c);
    for (int i = 0; i < m; ++i) {
        const P2 t[3] = {flat(group[i][0]), flat(group[i][1]), flat(group[i][2])};
        if (inTriangle(centre, t)) {
            return true;
        }
    }
    return false;
}

// One solid's triangles as patches against the other's surface (verify.h, SETUP).
struct Searcher {
    const SoupIndex& other;
    double tol;
    double rhoMin;

    // A point of the triangle inside `other` farther than tol from its surface, or nothing. `patches` counts the
    // patches looked at; `capped` is set when kFixturePatchesPerTriangle was reached.
    std::optional<Vec3> triangle(const Vec3* t, std::uint64_t& patches, bool& capped) const
    {
        struct Patch {
            Vec3 v[3];
            int level;
        };
        std::vector<Patch> stack;
        stack.push_back({{t[0], t[1], t[2]}, 0});
        const double tol2 = tol * tol;
        std::uint32_t count = 0;
        while (!stack.empty()) {
            if (++count > kFixturePatchesPerTriangle) {
                capped = true;
                return std::nullopt;
            }
            ++patches;
            const Patch p = stack.back();
            stack.pop_back();
            if (!boxesOverlap(triangleBox(p.v), other.bounds())) {
                continue; // outside the other solid's box: outside it
            }
            const Vec3 c{(p.v[0].x + p.v[1].x + p.v[2].x) / 3.0, (p.v[0].y + p.v[1].y + p.v[2].y) / 3.0,
                         (p.v[0].z + p.v[1].z + p.v[2].z) / 3.0};
            const double rho = std::sqrt(std::max({dist2(c, p.v[0]), dist2(c, p.v[1]), dist2(c, p.v[2])}));
            if (!std::isfinite(rho)) {
                continue;
            }
            double best2 = kInf;
            bool covered = false, crowded = false;
            const Vec3* close[kMaxNear];
            const Vec3* nearest = nullptr;
            int closeCount = 0;
            bool incomplete = false; // more near triangles than `close` holds (the smallest patches go on measuring)
            other.any(grown(triangleBox(p.v), tol), [&](std::uint32_t, const Vec3* w) {
                if (closeCount >= kMaxNear) {
                    if (rho > rhoMin) {
                        crowded = true;
                        return true;
                    }
                    incomplete = true;
                } else {
                    close[closeCount++] = w;
                }
                const double d2 = triangleDist2(c, w);
                if (d2 < best2) {
                    best2 = d2;
                    nearest = w;
                }
                // Within tol of one triangle at all three corners: within it everywhere (the distance is convex).
                if (d2 <= tol2 && triangleDist2(p.v[0], w) <= tol2 && triangleDist2(p.v[1], w) <= tol2 &&
                    triangleDist2(p.v[2], w) <= tol2) {
                    covered = true;
                    return true;
                }
                return false;
            });
            if (covered) {
                continue;
            }
            if (!crowded) {
                // d: the distance from c to the surface when it is at most tol (triangles outside the box are farther
                // than tol from every point of the patch); more than tol otherwise.
                const double d = std::sqrt(best2);
                if (d + rho <= tol) {
                    continue; // all within tol of the surface
                }
                if (d <= tol && !incomplete && nearest != nullptr &&
                    coveredByFace(p.v, c, close, closeCount, nearest, tol)) {
                    continue; // on a face of several triangles, within tol of it
                }
                if (d > rho) {
                    // The patch does not meet the surface: all of it inside, or all outside.
                    if (!insideClear(other, c, std::min(d, tol))) {
                        continue;
                    }
                    // Inside, every point at least d - rho deep.
                    if (d - rho > tol) {
                        return c;
                    }
                    if (rho <= rhoMin || p.level >= 96) {
                        if (d > tol) {
                            return c;
                        }
                        continue;
                    }
                } else if (rho <= rhoMin || p.level >= 96) {
                    continue; // it meets the surface: c is within rhoMin of it, and so is the rest within tol
                }
            }
            // Two halves across the longest edge (so a sliver of a triangle becomes well shaped instead of staying a
            // sliver ever smaller), the first looked at first.
            const double e0 = dist2(p.v[0], p.v[1]), e1 = dist2(p.v[1], p.v[2]), e2 = dist2(p.v[2], p.v[0]);
            const int k = e0 >= e1 && e0 >= e2 ? 0 : e1 >= e2 ? 1 : 2;
            const Vec3& a = p.v[k];
            const Vec3& b = p.v[(k + 1) % 3];
            const Vec3& o = p.v[(k + 2) % 3];
            const Vec3 m = mid(a, b);
            const int l = p.level + 1;
            stack.push_back({{m, b, o}, l});
            stack.push_back({{a, m, o}, l});
        }
        return std::nullopt;
    }
};

// The pieces of the triangles of `soup` that lie in `box`, as the units of the parallel search: each finite triangle
// whose box meets `box`, cut in two across its longest edge (pieces whose box misses `box` dropped: no point of them is
// inside the solid that box bounds) until a piece is at most kUnitMm in radius, in soup order and then in order of
// the cuts. So a large face near the other solid (a vise's floor under the stock) is shared among the workers.
constexpr double kUnitMm = 2.0;
std::vector<std::array<Vec3, 3>> piecesIn(const kernel::TriSoup& soup, const Box3& box, std::uint64_t* triangles)
{
    std::vector<std::array<Vec3, 3>> out;
    std::vector<std::array<Vec3, 3>> stack;
    const std::size_t nv = soup.positions.size() / 3;
    for (std::size_t t = 0; t + 2 < soup.indices.size(); t += 3) {
        std::array<Vec3, 3> tri;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const std::size_t i = soup.indices[t + static_cast<std::size_t>(k)];
            if (i >= nv) {
                ok = false;
                break;
            }
            tri[static_cast<std::size_t>(k)] = {soup.positions[3 * i], soup.positions[3 * i + 1],
                                                soup.positions[3 * i + 2]};
            ok = ok && std::isfinite(tri[static_cast<std::size_t>(k)].x) &&
                 std::isfinite(tri[static_cast<std::size_t>(k)].y) && std::isfinite(tri[static_cast<std::size_t>(k)].z);
        }
        if (!ok || !boxesOverlap(triangleBox(tri.data()), box)) {
            continue;
        }
        ++*triangles;
        stack.assign(1, tri);
        while (!stack.empty()) {
            const std::array<Vec3, 3> p = stack.back();
            stack.pop_back();
            if (!boxesOverlap(triangleBox(p.data()), box)) {
                continue;
            }
            const double e0 = dist2(p[0], p[1]), e1 = dist2(p[1], p[2]), e2 = dist2(p[2], p[0]);
            // The piece's radius about its centre is at most its longest edge.
            if (std::max({e0, e1, e2}) <= kUnitMm * kUnitMm || out.size() + stack.size() > (1u << 22)) {
                out.push_back(p);
                continue;
            }
            const std::size_t k = e0 >= e1 && e0 >= e2 ? 0 : e1 >= e2 ? 1 : 2;
            const Vec3 m = mid(p[k], p[(k + 1) % 3]);
            stack.push_back({m, p[(k + 1) % 3], p[(k + 2) % 3]});
            stack.push_back({p[k], m, p[(k + 2) % 3]});
        }
    }
    return out;
}

// The first piece of `tris` (by index) with a point inside `other` deeper than tol, searched in parallel.
Result<std::optional<Vec3>> firstDeep(const std::vector<std::array<Vec3, 3>>& tris, const SoupIndex& other, double tol,
                                      IJobSystem& jobs, JobContext& ctx, OverlapStats& stats)
{
    if (tris.empty()) {
        return std::optional<Vec3>{};
    }
    const Searcher search{other, tol, tol * kFixturePatchFraction};
    std::atomic<std::size_t> best{std::numeric_limits<std::size_t>::max()};
    std::atomic<std::uint64_t> patches{0}, capped{0};
    std::mutex lock;
    std::optional<Vec3> found;
    std::size_t foundAt = std::numeric_limits<std::size_t>::max();
    const Status st =
        jobs.parallelChunks(tris.size(), kGrain, ctx, [&](std::size_t, std::size_t begin, std::size_t end) -> Status {
            if (Status s = ctx.cancel.check(); !s) {
                return s;
            }
            std::uint64_t n = 0, cap = 0;
            for (std::size_t i = begin; i < end; ++i) {
                if (i > best.load(std::memory_order_relaxed)) {
                    break; // an earlier piece has one: this one cannot be the first
                }
                bool c = false;
                const std::optional<Vec3> hit = search.triangle(tris[i].data(), n, c);
                cap += c ? 1 : 0;
                if (hit) {
                    std::size_t was = best.load();
                    while (i < was && !best.compare_exchange_weak(was, i)) {
                    }
                    const std::lock_guard<std::mutex> guard(lock);
                    if (i < foundAt) {
                        foundAt = i;
                        found = hit;
                    }
                    break;
                }
            }
            patches += n;
            capped += cap;
            return Status{};
        });
    if (!st) {
        return tl::unexpected(st.error());
    }
    stats.patches += patches.load();
    stats.capped += capped.load();
    return found;
}

// A coordinate to 0.1 mm for a message.
std::string tenth(double v)
{
    return mmText(std::round(v * 10.0) / 10.0);
}

// overlapDeeperThan with the solids' indexes made (the part's and the stock's once for every fixture).
Result<std::optional<Overlap>> overlapIndexed(const kernel::TriSoup& a, const SoupIndex& ia, const kernel::TriSoup& b,
                                              const SoupIndex& ib, double tolMm, IJobSystem& jobs, JobContext& ctx,
                                              OverlapStats* statsOut)
{
    OverlapStats stats;
    std::optional<Overlap> out;
    if (!ia.empty() && !ib.empty() && boxesOverlap(ia.bounds(), ib.bounds())) {
        const double tol = std::max(tolMm, 0.0);
        // a's surface in b, then b's in a (which finds b wholly inside a).
        for (int pass = 0; pass < 2 && !out; ++pass) {
            const kernel::TriSoup& surface = pass == 0 ? a : b;
            const SoupIndex& other = pass == 0 ? ib : ia;
            const std::vector<std::array<Vec3, 3>> tris = piecesIn(surface, other.bounds(), &stats.triangles);
            Result<std::optional<Vec3>> hit = firstDeep(tris, other, tol, jobs, ctx, stats);
            if (!hit) {
                return tl::unexpected(hit.error());
            }
            if (*hit) {
                out = Overlap{**hit, surfaceDistance(other, **hit, std::max(tol, 1e-3))};
            }
        }
    }
    if (statsOut != nullptr) {
        statsOut->triangles += stats.triangles;
        statsOut->patches += stats.patches;
        statsOut->capped += stats.capped;
    }
    return out;
}

} // namespace

Result<std::optional<Overlap>> overlapDeeperThan(const kernel::TriSoup& a, const kernel::TriSoup& b, double tolMm,
                                                 IJobSystem& jobs, JobContext& ctx, OverlapStats* stats)
{
    const SoupIndex ia(a), ib(b);
    return overlapIndexed(a, ia, b, ib, tolMm, jobs, ctx, stats);
}

Result<std::vector<SimEvent>> setupEvents(const VerifyInput& in, IJobSystem& jobs, JobContext& ctx, OverlapStats* stats)
{
    std::vector<SimEvent> out;
    const bool havePart = in.part != nullptr && !in.part->indices.empty();
    const bool haveStock = in.stock != nullptr && !in.stock->indices.empty();
    const double stockTol =
        kFixtureOverlapMm + 0.5 * std::max(0.0, std::isfinite(in.stockCellMm) ? in.stockCellMm : 0.0);
    if (in.fixtures.empty()) {
        return out;
    }
    const SoupIndex part = havePart ? SoupIndex(*in.part) : SoupIndex();
    const SoupIndex stock = haveStock ? SoupIndex(*in.stock) : SoupIndex();
    for (std::size_t f = 0; f < in.fixtures.size(); ++f) {
        ctx.progress.set(static_cast<double>(f) / static_cast<double>(in.fixtures.size()), "Checking fixtures");
        const kernel::TriSoup* fixture = in.fixtures[f];
        if (fixture == nullptr || fixture->indices.empty()) {
            continue;
        }
        const SoupIndex fixtureIndex(*fixture);
        const std::string name = "Fixture " + std::to_string(f + 1);
        const auto at = [](const Vec3& p) {
            return "X" + tenth(p.x) + " Y" + tenth(p.y) + " Z" + tenth(p.z);
        };
        if (havePart) {
            Result<std::optional<Overlap>> r =
                overlapIndexed(*fixture, fixtureIndex, *in.part, part, kFixtureOverlapMm, jobs, ctx, stats);
            if (!r) {
                return tl::unexpected(r.error());
            }
            if (*r) {
                SimEvent e;
                e.kind = EventKind::FixtureHitsPart;
                e.severity = Severity::Error;
                e.operation = OperationId{};
                e.moveIndex = kSetupEvent;
                e.position = (*r)->point;
                e.amountMm = (*r)->depth;
                e.message = name + " cuts into the part at " + at((*r)->point) +
                            " (setup coordinates): the part cannot sit there. Move the fixture, or check where the "
                            "part sits in the setup. (Deeper than " +
                            mmText(kFixtureOverlapMm) + " mm; touching is fine.)";
                out.push_back(std::move(e));
                continue;
            }
        }
        if (haveStock) {
            Result<std::optional<Overlap>> r =
                overlapIndexed(*fixture, fixtureIndex, *in.stock, stock, stockTol, jobs, ctx, stats);
            if (!r) {
                return tl::unexpected(r.error());
            }
            if (*r) {
                SimEvent e;
                e.kind = EventKind::FixtureHitsStock;
                e.severity = Severity::Warning;
                e.operation = OperationId{};
                e.moveIndex = kSetupEvent;
                e.position = (*r)->point;
                e.amountMm = (*r)->depth;
                e.message = name + " overlaps the stock at " + at((*r)->point) +
                            " (setup coordinates), but not the part. Check the stock size and where the fixture sits: "
                            "rough stock is often drawn oversize. (Deeper than " +
                            mmText(stockTol) + " mm; touching is fine.)";
                out.push_back(std::move(e));
            }
        }
    }
    // (Nothing reported without fixtures, above: progress is rate-limited, and the moves' first report should not be
    // dropped.)
    ctx.progress.set(1.0, "Checking fixtures");
    return out;
}

} // namespace ray::sim::detail
