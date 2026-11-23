// The part as the invariant checker sees it (part_model.h).
#include "part_model.h"

#include <ray/kernel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace ray::cam::invariants {

namespace {

// Keeps grid centres off round coordinates (part faces at whole or half millimetres), so a cell centre never lies
// exactly on a mesh edge of an axis-aligned part.
constexpr double kGridShift = 0.0137137137;

struct Poly {
    Vec3 v[12];
    int n = 0;
};

double coord(const Vec3& p, int axis)
{
    return axis == 0 ? p.x : p.y;
}

// Sutherland-Hodgman against the closed half space coord(axis) >= bound (keepAbove) or <= bound.
void clip(Poly& p, int axis, double bound, bool keepAbove)
{
    Poly out;
    for (int k = 0; k < p.n; ++k) {
        const Vec3& a = p.v[k];
        const Vec3& b = p.v[(k + 1) % p.n];
        const double ca = coord(a, axis) - bound, cb = coord(b, axis) - bound;
        const bool ina = keepAbove ? ca >= 0 : ca <= 0;
        const bool inb = keepAbove ? cb >= 0 : cb <= 0;
        if (ina) {
            out.v[out.n++] = a;
        }
        if (ina != inb) {
            const double t = ca / (ca - cb);
            out.v[out.n++] = Vec3{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
        }
        if (out.n >= 10) {
            break;
        }
    }
    p = out;
}

// The Z range of triangle t clipped to the column [x0, x1] x [y0, y1]; false when it does not meet the column.
bool clippedRange(const Tri& t, double x0, double y0, double x1, double y1, double& zlo, double& zhi)
{
    Poly p;
    p.v[0] = t.a;
    p.v[1] = t.b;
    p.v[2] = t.c;
    p.n = 3;
    clip(p, 0, x0, true);
    if (p.n == 0) {
        return false;
    }
    clip(p, 0, x1, false);
    if (p.n == 0) {
        return false;
    }
    clip(p, 1, y0, true);
    if (p.n == 0) {
        return false;
    }
    clip(p, 1, y1, false);
    if (p.n == 0) {
        return false;
    }
    zlo = zhi = p.v[0].z;
    for (int k = 1; k < p.n; ++k) {
        zlo = std::min(zlo, p.v[k].z);
        zhi = std::max(zhi, p.v[k].z);
    }
    return true;
}

double orient(double ax, double ay, double bx, double by, double qx, double qy)
{
    return (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
}

// clippedRange with two shortcuts: a column wholly inside the triangle's projection (the plane's Z at the column's
// corners: a plane's extremes over a rectangle are at its corners), and one wholly outside an edge of it (no meeting).
// Only columns an edge crosses are clipped; vertical triangles always are.
bool columnRange(const Tri& t, double x0, double y0, double x1, double y1, double& zlo, double& zhi)
{
    Vec3 a = t.a, b = t.b, c = t.c;
    double area = orient(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) {
        return clippedRange(t, x0, y0, x1, y1, zlo, zhi);
    }
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    const double cx[4] = {x0, x1, x1, x0}, cy[4] = {y0, y0, y1, y1};
    const Vec3* edges[3][2] = {{&b, &c}, {&c, &a}, {&a, &b}};
    double w[3][4];
    bool inside = true;
    for (int e = 0; e < 3; ++e) {
        int out = 0;
        for (int k = 0; k < 4; ++k) {
            w[e][k] = orient(edges[e][0]->x, edges[e][0]->y, edges[e][1]->x, edges[e][1]->y, cx[k], cy[k]);
            out += w[e][k] < 0 ? 1 : 0;
        }
        if (out == 4) {
            return false;
        }
        inside = inside && out == 0;
    }
    if (!inside) {
        return clippedRange(t, x0, y0, x1, y1, zlo, zhi);
    }
    for (int k = 0; k < 4; ++k) {
        const double z = (w[0][k] * a.z + w[1][k] * b.z + w[2][k] * c.z) / area;
        zlo = k == 0 ? z : std::min(zlo, z);
        zhi = k == 0 ? z : std::max(zhi, z);
    }
    return true;
}

// Whether a point exactly on edge p0 -> p1 (of a counter-clockwise triangle) belongs to that triangle: of the two
// triangles sharing an edge, which run it in opposite directions, exactly one owns it.
bool owns(const Vec3& p0, const Vec3& p1)
{
    const double dy = p1.y - p0.y, dx = p1.x - p0.x;
    return dy < 0 || (dy == 0 && dx < 0);
}

// The Z where the vertical line through (qx, qy) meets triangle t, if it does (by the ownership rule on edges).
bool hit(const Tri& t, double qx, double qy, double& z)
{
    Vec3 a = t.a, b = t.b, c = t.c;
    double area = orient(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) {
        return false; // vertical: the line runs along it, never through it
    }
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    const double wa = orient(b.x, b.y, c.x, c.y, qx, qy); // opposite a
    const double wb = orient(c.x, c.y, a.x, a.y, qx, qy); // opposite b
    const double wc = orient(a.x, a.y, b.x, b.y, qx, qy); // opposite c
    if (wa < 0 || wb < 0 || wc < 0) {
        return false;
    }
    if ((wa == 0 && !owns(b, c)) || (wb == 0 && !owns(c, a)) || (wc == 0 && !owns(a, b))) {
        return false;
    }
    z = (wa * a.z + wb * b.z + wc * c.z) / area;
    return true;
}

} // namespace

void mergeIntervals(std::vector<Iv>& v)
{
    if (v.size() < 2) {
        return;
    }
    std::sort(v.begin(), v.end(),
              [](const Iv& a, const Iv& b) { return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi); });
    std::size_t k = 0;
    for (std::size_t n = 1; n < v.size(); ++n) {
        if (v[n].lo <= v[k].hi) {
            v[k].hi = std::max(v[k].hi, v[n].hi);
        } else {
            v[++k] = v[n];
        }
    }
    v.resize(k + 1);
}

void subtractInterval(std::vector<Iv>& v, double lo, double hi)
{
    if (!(hi > lo)) {
        return;
    }
    std::vector<Iv> out;
    for (const Iv& iv : v) {
        if (iv.hi <= lo || iv.lo >= hi) {
            out.push_back(iv);
            continue;
        }
        if (iv.lo < lo) {
            out.push_back(Iv{iv.lo, lo});
        }
        if (iv.hi > hi) {
            out.push_back(Iv{hi, iv.hi});
        }
    }
    v = std::move(out);
}

Result<std::shared_ptr<const PartModel>> PartModel::build(const ResolvedGeometry& geom, double cell, double chord)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<Tri> tris;
    const double* m = geom.wcsFromModel.m;
    const auto wcs = [&](double x, double y, double z) {
        return Vec3{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                    m[8] * x + m[9] * y + m[10] * z + m[11]};
    };
    for (const auto* list : {&geom.partShapes, &geom.fixtureShapes}) {
        for (const kernel::Shape& s : *list) {
            if (s.isNull()) {
                continue;
            }
            JobContext ctx;
            auto soup = kernel::triangulateExact(s, chord, ctx);
            if (!soup) {
                return tl::unexpected(soup.error());
            }
            const std::vector<double>& p = soup->positions;
            for (std::size_t k = 0; k + 2 < soup->indices.size(); k += 3) {
                const auto v = [&](std::uint32_t i) {
                    return wcs(p[3 * i], p[3 * i + 1], p[3 * i + 2]);
                };
                tris.push_back(Tri{v(soup->indices[k]), v(soup->indices[k + 1]), v(soup->indices[k + 2])});
            }
        }
    }
    const auto meshed = std::chrono::steady_clock::now();
    auto model = fromTriangles(std::move(tris), cell, chord);
    if (std::getenv("RAY_INVARIANTS_TIMING") != nullptr) {
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "invariants timing: part mesh %.3f s\ninvariants timing: part grid %.3f s (%zu triangles)\n",
                     std::chrono::duration<double>(meshed - start).count(),
                     std::chrono::duration<double>(now - meshed).count(), model->triangleCount());
    }
    return model;
}

std::shared_ptr<const PartModel> PartModel::fromTriangles(std::vector<Tri> tris, double cell, double chord)
{
    auto pm = std::make_shared<PartModel>();
    pm->tris_ = std::move(tris);
    pm->h_ = cell;
    pm->chord_ = chord;
    pm->index();
    return pm;
}

void PartModel::index()
{
    maxTop_ = -std::numeric_limits<double>::infinity();
    if (tris_.empty()) {
        return;
    }
    double x0 = tris_[0].a.x, x1 = x0, y0 = tris_[0].a.y, y1 = y0;
    for (const Tri& t : tris_) {
        for (const Vec3* v : {&t.a, &t.b, &t.c}) {
            x0 = std::min(x0, v->x);
            x1 = std::max(x1, v->x);
            y0 = std::min(y0, v->y);
            y1 = std::max(y1, v->y);
        }
    }
    x0_ = x0 - h_ + kGridShift * h_;
    y0_ = y0 - h_ + kGridShift * h_;
    nx_ = static_cast<int>(std::ceil((x1 - x0_) / h_)) + 1;
    ny_ = static_cast<int>(std::ceil((y1 - y0_) / h_)) + 1;
    const std::size_t cells = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
    // Triangles per cell, with the Z range of each over the cell's column.
    std::vector<std::vector<std::uint32_t>> lists(cells);
    std::vector<std::vector<Iv>> ranges(cells);
    for (std::uint32_t k = 0; k < tris_.size(); ++k) {
        const Tri& t = tris_[k];
        const double tx0 = std::min({t.a.x, t.b.x, t.c.x}), tx1 = std::max({t.a.x, t.b.x, t.c.x});
        const double ty0 = std::min({t.a.y, t.b.y, t.c.y}), ty1 = std::max({t.a.y, t.b.y, t.c.y});
        const int i0 = std::max(0, static_cast<int>(std::floor((tx0 - x0_) / h_)));
        const int i1 = std::min(nx_ - 1, static_cast<int>(std::floor((tx1 - x0_) / h_)));
        const int j0 = std::max(0, static_cast<int>(std::floor((ty0 - y0_) / h_)));
        const int j1 = std::min(ny_ - 1, static_cast<int>(std::floor((ty1 - y0_) / h_)));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                double zlo = 0, zhi = 0;
                const double cx0 = x0_ + i * h_, cy0 = y0_ + j * h_;
                if (columnRange(t, cx0, cy0, cx0 + h_, cy0 + h_, zlo, zhi)) {
                    lists[static_cast<std::size_t>(j) * nx_ + i].push_back(k);
                    ranges[static_cast<std::size_t>(j) * nx_ + i].push_back(Iv{zlo, zhi});
                }
            }
        }
    }
    triStart_.assign(cells + 1, 0);
    for (std::size_t c = 0; c < cells; ++c) {
        triStart_[c + 1] = triStart_[c] + static_cast<std::uint32_t>(lists[c].size());
    }
    triOf_.reserve(triStart_[cells]);
    for (const auto& l : lists) {
        triOf_.insert(triOf_.end(), l.begin(), l.end());
    }
    lists.clear();
    // Each cell's Z-set, and the block tops.
    bnx_ = (nx_ + kBlock - 1) / kBlock;
    bny_ = (ny_ + kBlock - 1) / kBlock;
    blockTop_.assign(static_cast<std::size_t>(bnx_) * bny_, -std::numeric_limits<double>::infinity());
    zStart_.assign(cells + 1, 0);
    std::vector<Iv> zs;
    for (int j = 0; j < ny_; ++j) {
        for (int i = 0; i < nx_; ++i) {
            const std::size_t c = static_cast<std::size_t>(j) * nx_ + i;
            bool odd = false;
            rayCell(i, j, centreX(i), centreY(j), zs, odd);
            oddRays_ += odd ? 1 : 0;
            zs.insert(zs.end(), ranges[c].begin(), ranges[c].end());
            std::vector<Iv>().swap(ranges[c]);
            mergeIntervals(zs);
            z_.insert(z_.end(), zs.begin(), zs.end());
            zStart_[c + 1] = static_cast<std::uint32_t>(z_.size());
            if (!zs.empty()) {
                double& top = blockTop_[static_cast<std::size_t>(j / kBlock) * bnx_ + i / kBlock];
                top = std::max(top, zs.back().hi);
                maxTop_ = std::max(maxTop_, zs.back().hi);
            }
        }
    }
}

void PartModel::rayCell(int i, int j, double x, double y, std::vector<Iv>& out, bool& odd) const
{
    out.clear();
    odd = false;
    if (i < 0 || j < 0 || i >= nx_ || j >= ny_) {
        return;
    }
    const std::size_t c = static_cast<std::size_t>(j) * nx_ + i;
    std::vector<double> zs;
    for (std::uint32_t n = triStart_[c]; n < triStart_[c + 1]; ++n) {
        double z = 0;
        if (hit(tris_[triOf_[n]], x, y, z)) {
            zs.push_back(z);
        }
    }
    std::sort(zs.begin(), zs.end());
    odd = zs.size() % 2 != 0;
    for (std::size_t k = 0; k + 1 < zs.size(); k += 2) {
        out.push_back(Iv{zs[k], zs[k + 1]});
    }
    if (odd) {
        out.push_back(Iv{zs.back(), zs.back()});
    }
}

void PartModel::ray(double x, double y, std::vector<Iv>& out) const
{
    out.clear();
    if (tris_.empty()) {
        return;
    }
    const int i = static_cast<int>(std::floor((x - x0_) / h_));
    const int j = static_cast<int>(std::floor((y - y0_) / h_));
    bool odd = false;
    rayCell(i, j, x, y, out, odd);
}

void PartModel::zset(int i, int j, double qx, double qy, double s, std::span<const AllowedCut> allowed,
                     std::vector<Iv>& out, double* wholeBelow) const
{
    out.clear();
    if (wholeBelow != nullptr) {
        *wholeBelow = std::numeric_limits<double>::infinity();
        for (const AllowedCut& a : allowed) {
            // The nearest point of the square to the cut's axis or path.
            const double near = std::max(0.0, allowedDistance(a, qx, qy) - s * std::sqrt(2.0));
            *wholeBelow = std::min(*wholeBelow, allowedFrom(a, near, 0));
        }
    }
    if (i < 0 || j < 0 || i >= nx_ || j >= ny_) {
        return;
    }
    const std::size_t c = static_cast<std::size_t>(j) * nx_ + i;
    if (s * 2 >= h_ && qx == centreX(i) && qy == centreY(j)) {
        out.assign(z_.begin() + zStart_[c], z_.begin() + zStart_[c + 1]);
    } else {
        bool odd = false;
        rayCell(i, j, qx, qy, out, odd);
        for (std::uint32_t n = triStart_[c]; n < triStart_[c + 1]; ++n) {
            double zlo = 0, zhi = 0;
            if (columnRange(tris_[triOf_[n]], qx - s, qy - s, qx + s, qy + s, zlo, zhi)) {
                out.push_back(Iv{zlo, zhi});
            }
        }
        mergeIntervals(out);
    }
    for (const AllowedCut& a : allowed) {
        const double from = allowedFrom(a, allowedDistance(a, qx, qy), s * std::sqrt(2.0));
        if (from < a.zTop) {
            subtractInterval(out, from, a.zTop);
        }
    }
}

double allowedDistance(const AllowedCut& a, double px, double py)
{
    if (a.path.size() < 2) {
        return std::hypot(px - a.x, py - a.y);
    }
    if (a.pathLo.x <= a.pathHi.x) {
        // Beyond the widest the cut gets (at zTop) from the path's box: that distance is a lower bound, enough to say
        // no.
        const double bx = std::max({a.pathLo.x - px, 0.0, px - a.pathHi.x});
        const double by = std::max({a.pathLo.y - py, 0.0, py - a.pathHi.y});
        const double box = std::hypot(bx, by);
        if (box > a.radius + std::max(0.0, a.slope) * std::max(0.0, a.zTop - a.zBottom) + 1) {
            return box;
        }
    }
    double best = std::numeric_limits<double>::infinity();
    const std::size_t n = a.path.size();
    for (std::size_t k = 0; k + 1 < n + (a.closed ? 1 : 0); ++k) {
        const Vec2 p = a.path[k], q = a.path[(k + 1) % n];
        const double dx = q.x - p.x, dy = q.y - p.y;
        const double len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? ((px - p.x) * dx + (py - p.y) * dy) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(px - (p.x + t * dx), py - (p.y + t * dy)));
    }
    return best;
}

double allowedFrom(const AllowedCut& a, double d, double spread)
{
    const double reach = d + spread; // the farthest point of the disc
    if (reach <= a.radius) {
        return a.zBottom;
    }
    if (!(a.slope > 0)) {
        return std::numeric_limits<double>::infinity();
    }
    return a.zBottom + (reach - a.radius) / a.slope;
}

double PartModel::blockTop(int bi, int bj) const
{
    if (bi < 0 || bj < 0 || bi >= bnx_ || bj >= bny_) {
        return -std::numeric_limits<double>::infinity();
    }
    return blockTop_[static_cast<std::size_t>(bj) * bnx_ + bi];
}

} // namespace ray::cam::invariants
