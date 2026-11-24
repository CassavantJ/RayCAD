// Exact geometry of the safety checks (task T-071): verify_geom.h says what each function does and why it is exact.
#include "verify_geom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ray::sim::detail {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint32_t kLeafSize = 4;

Box3 emptyBox()
{
    return {{kInf, kInf, kInf}, {-kInf, -kInf, -kInf}};
}
void addPoint(Box3& b, const Vec3& p)
{
    b.min.x = std::min(b.min.x, p.x);
    b.min.y = std::min(b.min.y, p.y);
    b.min.z = std::min(b.min.z, p.z);
    b.max.x = std::max(b.max.x, p.x);
    b.max.y = std::max(b.max.y, p.y);
    b.max.z = std::max(b.max.z, p.z);
}
void addBox(Box3& b, const Box3& o)
{
    addPoint(b, o.min);
    addPoint(b, o.max);
}
double centre(const Box3& b, int axis)
{
    return axis == 0 ? b.min.x + b.max.x : axis == 1 ? b.min.y + b.max.y : b.min.z + b.max.z;
}
int longestAxis(const Box3& b)
{
    const double dx = b.max.x - b.min.x, dy = b.max.y - b.min.y, dz = b.max.z - b.min.z;
    return dx >= dy && dx >= dz ? 0 : dy >= dz ? 1 : 2;
}

struct P2 {
    double x, y;
};

// The distance from the origin to the convex hull of pts[0, n) (n <= 48); +inf for none. Reorders pts.
double hullDistance(P2* pts, int n)
{
    if (n <= 0) {
        return kInf;
    }
    std::sort(pts, pts + n, [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (m == 0 || pts[i].x != pts[m - 1].x || pts[i].y != pts[m - 1].y) {
            pts[m++] = pts[i];
        }
    }
    const auto segDist = [](const P2& p, const P2& q) {
        const double dx = q.x - p.x, dy = q.y - p.y;
        const double len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? -(p.x * dx + p.y * dy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double x = p.x + t * dx, y = p.y + t * dy;
        return std::sqrt(x * x + y * y);
    };
    if (m == 1) {
        return std::sqrt(pts[0].x * pts[0].x + pts[0].y * pts[0].y);
    }
    const auto cross = [](const P2& o, const P2& a, const P2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    // Andrew's monotone chain, counter-clockwise, collinear points dropped.
    P2 hull[2 * 48 + 2];
    int h = 0;
    for (int i = 0; i < m; ++i) {
        while (h >= 2 && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0) {
            --h;
        }
        hull[h++] = pts[i];
    }
    for (int i = m - 2, lower = h + 1; i >= 0; --i) {
        while (h >= lower && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0) {
            --h;
        }
        hull[h++] = pts[i];
    }
    --h; // the last point is the first
    if (h <= 2) {
        return segDist(hull[0], hull[h > 1 ? 1 : 0]);
    }
    const P2 o{0, 0};
    bool inside = true;
    for (int i = 0; i < h && inside; ++i) {
        inside = cross(hull[i], hull[(i + 1) % h], o) >= 0;
    }
    if (inside) {
        return 0.0;
    }
    double d = kInf;
    for (int i = 0; i < h; ++i) {
        d = std::min(d, segDist(hull[i], hull[(i + 1) % h]));
    }
    return d;
}

// The points of conv(w[0, n)) between heights lo and hi (lo <= hi) that span its slab, projected to XY: the vertices
// inside the slab and the crossings of every segment between two vertices with the planes z = lo and z = hi.
int slabPoints(const Vec3* w, int n, double lo, double hi, P2* out)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (w[i].z >= lo && w[i].z <= hi) {
            out[k++] = {w[i].x, w[i].y};
        }
    }
    const double planes[2] = {lo, hi};
    const int np = lo == hi ? 1 : 2;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const double zi = w[i].z, zj = w[j].z;
            for (int q = 0; q < np; ++q) {
                const double c = planes[q];
                if ((zi < c && zj > c) || (zi > c && zj < c)) {
                    const double t = (c - zi) / (zj - zi);
                    out[k++] = {w[i].x + t * (w[j].x - w[i].x), w[i].y + t * (w[j].y - w[i].y)};
                }
            }
        }
    }
    return k;
}

// The sign of orient(a, b, p) with p moved by (eps, eps^2) when it is exactly on the line (simulation of
// simplicity), so a point on an edge shared by two triangles is inside exactly one of them.
bool leftOf(double ax, double ay, double bx, double by, double px, double py)
{
    const double o = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    if (o != 0) {
        return o > 0;
    }
    if (by != ay) {
        return by < ay;
    }
    return bx > ax;
}

} // namespace

// ---------- SoupIndex ----------

SoupIndex::SoupIndex(const kernel::TriSoup& soup)
{
    const std::size_t nv = soup.positions.size() / 3;
    const auto vertex = [&](std::uint32_t i) {
        return Vec3{soup.positions[3 * std::size_t(i)], soup.positions[3 * std::size_t(i) + 1],
                    soup.positions[3 * std::size_t(i) + 2]};
    };
    bounds_ = emptyBox();
    std::vector<Tri> all;
    all.reserve(soup.indices.size() / 3);
    for (std::size_t t = 0; t + 2 < soup.indices.size(); t += 3) {
        const std::uint32_t i0 = soup.indices[t], i1 = soup.indices[t + 1], i2 = soup.indices[t + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) {
            continue;
        }
        Tri tri{{vertex(i0), vertex(i1), vertex(i2)}, emptyBox()};
        bool finite = true;
        for (const Vec3& v : tri.v) {
            finite = finite && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            addPoint(tri.box, v);
        }
        if (!finite) {
            continue;
        }
        addBox(bounds_, tri.box);
        all.push_back(tri);
    }
    if (all.empty()) {
        bounds_ = {};
        return;
    }
    std::vector<std::uint32_t> order(all.size());
    std::iota(order.begin(), order.end(), 0u);
    nodes_.reserve(2 * all.size() / kLeafSize + 2);
    tris_ = std::move(all);
    build(order, 0, static_cast<std::uint32_t>(order.size()));
    std::vector<Tri> sorted;
    sorted.reserve(tris_.size());
    for (std::uint32_t i : order) {
        sorted.push_back(tris_[i]);
    }
    tris_ = std::move(sorted);
}

std::uint32_t SoupIndex::build(std::vector<std::uint32_t>& order, std::uint32_t first, std::uint32_t last)
{
    const std::uint32_t self = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Box3 box = emptyBox(), cbox = emptyBox();
    for (std::uint32_t i = first; i < last; ++i) {
        const Box3& b = tris_[order[i]].box;
        addBox(box, b);
        addPoint(cbox, {centre(b, 0), centre(b, 1), centre(b, 2)});
    }
    nodes_[self].box = box;
    if (last - first <= kLeafSize) {
        nodes_[self].leaf = true;
        nodes_[self].a = first;
        nodes_[self].b = last;
        return self;
    }
    const int axis = longestAxis(cbox);
    std::sort(order.begin() + first, order.begin() + last, [&](std::uint32_t x, std::uint32_t y) {
        const double cx = centre(tris_[x].box, axis), cy = centre(tris_[y].box, axis);
        return cx < cy || (cx == cy && x < y);
    });
    const std::uint32_t mid = first + (last - first) / 2;
    const std::uint32_t l = build(order, first, mid);
    const std::uint32_t r = build(order, mid, last);
    nodes_[self].a = l;
    nodes_[self].b = r;
    return self;
}

bool SoupIndex::contains(const Vec3& p) const
{
    if (tris_.empty() || p.x < bounds_.min.x || p.x > bounds_.max.x || p.y < bounds_.min.y || p.y > bounds_.max.y ||
        p.z > bounds_.max.z) {
        return false;
    }
    const Box3 ray{{p.x, p.y, p.z}, {p.x, p.y, bounds_.max.z}};
    int winding = 0;
    bool on = false;
    any(ray, [&](std::uint32_t, const Vec3* v) {
        const double area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (area == 0) {
            return false; // vertical: the ray along Z meets it only on edges, which its neighbours count
        }
        const bool ccw = area > 0;
        for (int e = 0; e < 3; ++e) {
            const Vec3& a = v[e];
            const Vec3& b = v[(e + 1) % 3];
            if (leftOf(a.x, a.y, b.x, b.y, p.x, p.y) != ccw) {
                return false;
            }
        }
        // The triangle's height over p (barycentric in XY).
        const double w1 = ((p.x - v[0].x) * (v[2].y - v[0].y) - (p.y - v[0].y) * (v[2].x - v[0].x)) / area;
        const double w2 = ((v[1].x - v[0].x) * (p.y - v[0].y) - (v[1].y - v[0].y) * (p.x - v[0].x)) / area;
        const double z = v[0].z + w1 * (v[1].z - v[0].z) + w2 * (v[2].z - v[0].z);
        if (z == p.z) {
            on = true;
            return true;
        }
        if (z > p.z) {
            winding += ccw ? 1 : -1;
        }
        return false;
    });
    return on || winding != 0;
}

// ---------- OrderedBoxes ----------

OrderedBoxes::OrderedBoxes(std::vector<Box3> boxes) : boxes_(std::move(boxes))
{
    if (boxes_.empty()) {
        return;
    }
    order_.resize(boxes_.size());
    std::iota(order_.begin(), order_.end(), 0u);
    nodes_.reserve(2 * boxes_.size() / kLeafSize + 2);
    build(0, static_cast<std::uint32_t>(boxes_.size()));
}

std::uint32_t OrderedBoxes::build(std::uint32_t first, std::uint32_t last)
{
    const std::uint32_t self = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Box3 box = emptyBox(), cbox = emptyBox();
    std::uint32_t minIndex = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = first; i < last; ++i) {
        const Box3& b = boxes_[order_[i]];
        addBox(box, b);
        addPoint(cbox, {centre(b, 0), centre(b, 1), centre(b, 2)});
        minIndex = std::min(minIndex, order_[i]);
    }
    nodes_[self].box = box;
    nodes_[self].minIndex = minIndex;
    if (last - first <= kLeafSize) {
        nodes_[self].leaf = true;
        nodes_[self].a = first;
        nodes_[self].b = last;
        return self;
    }
    const int axis = longestAxis(cbox);
    std::sort(order_.begin() + first, order_.begin() + last, [&](std::uint32_t x, std::uint32_t y) {
        const double cx = centre(boxes_[x], axis), cy = centre(boxes_[y], axis);
        return cx < cy || (cx == cy && x < y);
    });
    const std::uint32_t mid = first + (last - first) / 2;
    const std::uint32_t l = build(first, mid);
    const std::uint32_t r = build(mid, last);
    nodes_[self].a = l;
    nodes_[self].b = r;
    return self;
}

bool OrderedBoxes::anyBefore(const Box3& box, std::uint32_t limit) const
{
    if (nodes_.empty()) {
        return false;
    }
    std::uint32_t stack[96];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[stack[--top]];
        if (n.minIndex >= limit || !boxesOverlap(box, n.box)) {
            continue;
        }
        if (n.leaf) {
            for (std::uint32_t i = n.a; i < n.b; ++i) {
                if (order_[i] < limit && boxesOverlap(box, boxes_[order_[i]])) {
                    return true;
                }
            }
            continue;
        }
        stack[top++] = n.b;
        stack[top++] = n.a;
    }
    return false;
}

// ---------- Sweeps ----------

Box3 sweepBox(const CutterShape& zone, const Vec3& a, const Vec3& b)
{
    const double r = zone.rmax;
    return {{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, std::min(a.z, b.z) + zone.hmin},
            {std::max(a.x, b.x) + r, std::max(a.y, b.y) + r, std::max(a.z, b.z) + zone.hmax}};
}

bool pieceHitsTriangle(const ConvexPiece& pc, const Vec3& a, const Vec3& b, const Vec3 tri[3])
{
    Vec3 w[6];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        w[n++] = {tri[i].x - a.x, tri[i].y - a.y, tri[i].z - a.z};
    }
    if (a.x != b.x || a.y != b.y || a.z != b.z) {
        for (int i = 0; i < 3; ++i) {
            w[n++] = {tri[i].x - b.x, tri[i].y - b.y, tri[i].z - b.z};
        }
    }
    double zl = w[0].z, zh = w[0].z;
    for (int i = 1; i < n; ++i) {
        zl = std::min(zl, w[i].z);
        zh = std::max(zh, w[i].z);
    }
    const double lo = std::max(zl, pc.h0), hi = std::min(zh, pc.h1);
    if (!(lo <= hi)) {
        return false;
    }
    P2 pts[48];
    const double d = hullDistance(pts, slabPoints(w, n, lo, hi, pts));
    if (d > pc.rmax) {
        return false;
    }
    if (pc.endless || (lo >= pc.plateauLo && hi <= pc.plateauHi)) {
        return true; // the piece is a cylinder of rmax over these heights
    }
    const double rlo = pc.radiusAt(lo), rhi = pc.radiusAt(hi);
    if (d <= std::min(rlo, rhi)) {
        return true; // r is concave: never below its value at an end
    }
    const auto g = [&](double h) {
        P2 q[48];
        return hullDistance(q, slabPoints(w, n, h, h, q)) - pc.radiusAt(h);
    };
    double best = std::min(g(lo), g(hi));
    if (best <= 0) {
        return true;
    }
    // Golden-section search for the minimum of the convex g on [lo, hi].
    constexpr double kInvPhi = 0.6180339887498949;
    double x0 = lo, x1 = hi;
    double c = x1 - kInvPhi * (x1 - x0), e = x0 + kInvPhi * (x1 - x0);
    double gc = g(c), ge = g(e);
    const double stop = 1e-12 * (1.0 + std::abs(lo) + std::abs(hi));
    for (int it = 0; it < 200; ++it) {
        best = std::min(best, std::min(gc, ge));
        if (best <= 0) {
            return true;
        }
        if (x1 - x0 <= stop) {
            break;
        }
        if (gc <= ge) {
            x1 = e;
            e = c;
            ge = gc;
            c = x1 - kInvPhi * (x1 - x0);
            gc = g(c);
        } else {
            x0 = c;
            c = e;
            gc = ge;
            e = x0 + kInvPhi * (x1 - x0);
            ge = g(e);
        }
    }
    return best <= 0;
}

bool sweepHitsSoup(const CutterShape& zone, const Vec3& a, const Vec3& b, const SoupIndex& soup)
{
    if (zone.empty() || soup.empty()) {
        return false;
    }
    const Box3 box = sweepBox(zone, a, b);
    if (!boxesOverlap(box, soup.bounds())) {
        return false;
    }
    const bool hit = soup.any(box, [&](std::uint32_t, const Vec3* tri) {
        Box3 tb = emptyBox();
        addPoint(tb, tri[0]);
        addPoint(tb, tri[1]);
        addPoint(tb, tri[2]);
        for (const ConvexPiece& pc : zone.pieces) {
            const Box3 pb{{std::min(a.x, b.x) - pc.rmax, std::min(a.y, b.y) - pc.rmax, std::min(a.z, b.z) + pc.h0},
                          {std::max(a.x, b.x) + pc.rmax, std::max(a.y, b.y) + pc.rmax, std::max(a.z, b.z) + pc.h1}};
            if (boxesOverlap(pb, tb) && pieceHitsTriangle(pc, a, b, tri)) {
                return true;
            }
        }
        return false;
    });
    if (hit) {
        return true;
    }
    // No triangle met: each piece's sweep is wholly inside or wholly outside. One that goes on upward without end
    // leaves every bounded solid, so it would have crossed a triangle.
    for (const ConvexPiece& pc : zone.pieces) {
        if (!pc.endless && soup.contains({a.x, a.y, a.z + pc.h0})) {
            return true;
        }
    }
    return false;
}

CutterShape grownShape(const CutterShape& shape, double e)
{
    CutterShape out;
    const auto append = [&](const CutterShape& c) {
        for (const ConvexPiece& p : c.pieces) {
            out.pieces.push_back(p);
        }
    };
    for (const ConvexPiece& pc : shape.pieces) {
        if (pc.endless) {
            append(compileProfile({{pc.rmax + e, pc.h0 - e}, {pc.rmax + e, pc.h0 - e + 1.0}}, true));
            continue;
        }
        const std::size_t m = pc.ph.size();
        std::size_t first = 0, last = m - 1;
        while (first + 1 < m && pc.pr[first] < pc.rmax) {
            ++first;
        }
        while (last > 0 && pc.pr[last] < pc.rmax) {
            --last;
        }
        std::vector<Vec2> p;
        for (std::size_t i = 0; i < m; ++i) {
            const double r = pc.pr[i] + e;
            if (i < first) {
                p.push_back({r, pc.ph[i] - e});
            } else if (i > last) {
                p.push_back({r, pc.ph[i] + e});
            } else {
                if (i == first) {
                    p.push_back({r, pc.ph[i] - e});
                }
                if (i == last) {
                    p.push_back({r, pc.ph[i] + e});
                }
                if (i != first && i != last) {
                    p.push_back({r, pc.ph[i]});
                }
            }
        }
        append(compileProfile(p, false));
    }
    if (!out.pieces.empty()) {
        out.hmin = out.pieces.front().h0;
        out.hmax = out.pieces.front().h1;
        for (const ConvexPiece& p : out.pieces) {
            out.rmax = std::max(out.rmax, p.rmax);
            out.hmin = std::min(out.hmin, p.h0);
            out.hmax = std::max(out.hmax, p.h1);
        }
    }
    return out;
}

std::vector<Vec2> shrunkProfile(const std::vector<Vec2>& profile, double s)
{
    std::vector<Vec2> pts;
    for (const Vec2& v : profile) {
        if (std::isfinite(v.x) && std::isfinite(v.y)) {
            Vec2 q{std::max(0.0, v.x), v.y};
            if (!pts.empty()) {
                q.y = std::max(q.y, pts.back().y);
            }
            pts.push_back(q);
        }
    }
    if (pts.size() < 2 || !(s > 0)) {
        return pts;
    }
    const double lo = pts.front().y + s, hi = pts.back().y - s;
    if (!(hi > lo)) {
        return {};
    }
    std::vector<Vec2> out;
    for (const Vec2& v : pts) {
        out.push_back({std::max(0.0, v.x - s), std::clamp(v.y, lo, hi)});
    }
    return out;
}

} // namespace ray::sim::detail
