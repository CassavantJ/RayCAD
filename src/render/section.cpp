// The CPU half of the section view and of draft analysis (task T-023). See section.h and ray_render/viewport.h.
#include "section.h"

#include <ray_render/viewport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace ray::render {

namespace {

constexpr double kPi = 3.14159265358979323846;

double length(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

DraftClass draftClassOf(Vec3 normal, const DraftAnalysis& draft)
{
    const double ln = length(normal);
    const double lp = length(draft.pull);
    if (!(ln > 0) || !(lp > 0) || !std::isfinite(ln) || !std::isfinite(lp))
        return DraftClass::Vertical;
    const double s = (normal.x * draft.pull.x + normal.y * draft.pull.y + normal.z * draft.pull.z) / (ln * lp);
    const double t = std::sin(std::clamp(draft.thresholdDeg, 0.0, 89.0) * kPi / 180.0);
    if (s > t)
        return DraftClass::Positive;
    if (s < -t)
        return DraftClass::Negative;
    return DraftClass::Vertical;
}

Token draftToken(DraftClass c)
{
    switch (c) {
    case DraftClass::Positive:
        return Token::DraftPositive;
    case DraftClass::Negative:
        return Token::DraftNegative;
    case DraftClass::Vertical:
        return Token::DraftVertical;
    }
    return Token::DraftVertical;
}

namespace detail {

namespace {

struct Cell {
    std::int64_t x, y, z;
    bool operator==(const Cell&) const = default;
};
struct CellHash {
    std::size_t operator()(const Cell& c) const
    {
        std::uint64_t h = std::uint64_t(c.x) * 0x9E3779B97F4A7C15ull;
        h ^= std::uint64_t(c.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= std::uint64_t(c.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return std::size_t(h);
    }
};

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b)
{
    return a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a;
}

} // namespace

bool isClosedMesh(const kernel::Mesh& mesh)
{
    if (!mesh.failedFaces.empty() || mesh.indices.size() < 3 || mesh.positions.size() < 9)
        return false;
    const std::size_t vertexCount = mesh.positions.size() / 3;

    // 1. Triangle edges by vertex index. The kernel gives every face its own vertices, so edges used once are the
    //    faces' borders (and seams); an index edge used more than twice is a non-manifold mesh.
    std::vector<std::uint64_t> edges;
    edges.reserve(mesh.indices.size());
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = mesh.indices[t + std::size_t(k)];
            const std::uint32_t b = mesh.indices[t + std::size_t((k + 1) % 3)];
            if (a >= vertexCount || b >= vertexCount)
                return false;
            if (a != b)
                edges.push_back(edgeKey(a, b));
        }
    }
    std::sort(edges.begin(), edges.end());
    std::vector<std::uint64_t> border;
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i;
        while (j < edges.size() && edges[j] == edges[i])
            ++j;
        if (j - i == 1)
            border.push_back(edges[i]);
        else if (j - i > 2)
            return false;
        i = j;
    }
    if (border.empty())
        return true;

    // 2. Weld the border vertices by position (a grid of cells one tolerance wide; neighbours merge).
    float lo = std::numeric_limits<float>::infinity(), hi = -lo;
    for (const float c : mesh.positions) {
        lo = std::min(lo, c);
        hi = std::max(hi, c);
    }
    const double tol = std::max(1e-6, 1e-5 * double(hi - lo));
    std::vector<std::uint32_t> welded(vertexCount, std::numeric_limits<std::uint32_t>::max());
    std::unordered_map<Cell, std::uint32_t, CellHash> cells;
    std::uint32_t next = 0;
    const auto weld = [&](std::uint32_t v) {
        if (welded[v] != std::numeric_limits<std::uint32_t>::max())
            return welded[v];
        const float* p = &mesh.positions[std::size_t(v) * 3];
        const Cell c{std::int64_t(std::floor(p[0] / tol)), std::int64_t(std::floor(p[1] / tol)),
                     std::int64_t(std::floor(p[2] / tol))};
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto it = cells.find(Cell{c.x + dx, c.y + dy, c.z + dz});
                    if (it != cells.end())
                        return welded[v] = it->second;
                }
            }
        }
        cells.emplace(c, next);
        return welded[v] = next++;
    };

    // 3. Every welded border edge must be shared by an even number of face borders.
    std::vector<std::uint64_t> weldedBorder;
    weldedBorder.reserve(border.size());
    for (const std::uint64_t e : border) {
        const std::uint32_t a = weld(std::uint32_t(e >> 32));
        const std::uint32_t b = weld(std::uint32_t(e & 0xFFFFFFFFu));
        if (a != b) // an edge shorter than the tolerance collapses: it bounds nothing
            weldedBorder.push_back(edgeKey(a, b));
    }
    std::sort(weldedBorder.begin(), weldedBorder.end());
    for (std::size_t i = 0; i < weldedBorder.size();) {
        std::size_t j = i;
        while (j < weldedBorder.size() && weldedBorder[j] == weldedBorder[i])
            ++j;
        if ((j - i) % 2 != 0)
            return false;
        i = j;
    }
    return true;
}

std::optional<PlaneEquation> planeEquation(const SectionPlane& plane)
{
    const double len = length(plane.normal);
    if (!(len > 1e-12) || !std::isfinite(len))
        return std::nullopt;
    PlaneEquation eq;
    eq.nx = plane.normal.x / len;
    eq.ny = plane.normal.y / len;
    eq.nz = plane.normal.z / len;
    eq.d = -(eq.nx * plane.origin.x + eq.ny * plane.origin.y + eq.nz * plane.origin.z);
    if (!std::isfinite(eq.d))
        return std::nullopt;
    return eq;
}

std::optional<std::array<Vec3, 4>> capQuad(const PlaneEquation& plane, const Box3& bounds)
{
    const Vec3 c{(bounds.min.x + bounds.max.x) * 0.5, (bounds.min.y + bounds.max.y) * 0.5,
                 (bounds.min.z + bounds.max.z) * 0.5};
    const Vec3 half{(bounds.max.x - bounds.min.x) * 0.5, (bounds.max.y - bounds.min.y) * 0.5,
                    (bounds.max.z - bounds.min.z) * 0.5};
    const double radius = std::max(length(half), 1e-3);
    const double dist = plane.distance(c);
    if (!std::isfinite(dist) || std::abs(dist) > radius)
        return std::nullopt;
    const Vec3 n{plane.nx, plane.ny, plane.nz};
    const Vec3 p0{c.x - n.x * dist, c.y - n.y * dist, c.z - n.z * dist};
    // In-plane basis: the world axis least aligned with the normal, made perpendicular to it.
    const double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    const Vec3 axis = ax <= ay && ax <= az ? Vec3{1, 0, 0} : (ay <= az ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
    Vec3 u{n.y * axis.z - n.z * axis.y, n.z * axis.x - n.x * axis.z, n.x * axis.y - n.y * axis.x};
    const double lu = length(u);
    u = Vec3{u.x / lu, u.y / lu, u.z / lu};
    const Vec3 v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
    const double h = 2.0 * radius;
    const auto at = [&](double su, double sv) {
        return Vec3{p0.x + h * (su * u.x + sv * v.x), p0.y + h * (su * u.y + sv * v.y),
                    p0.z + h * (su * u.z + sv * v.z)};
    };
    return std::array<Vec3, 4>{at(-1, -1), at(1, -1), at(1, 1), at(-1, 1)};
}

} // namespace detail

} // namespace ray::render
