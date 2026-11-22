// The CPU half of picking (task T-015). See pick.h.
#include "pick.h"

#include <ray_render/viewport.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

namespace ray::render::detail {

using kernel::SubKind;

namespace {

// Kernel indices above this are not given a dense lookup table (a corrupt mesh, not a real part).
constexpr std::uint32_t kMaxDenseIndex = 1u << 24;
// A face under a 1 px edge line is taken from the nearest face pixel this close (pixels).
constexpr double kFaceUnderEdgePx = 1.5;
// A vertex counts as visible when an edge ending at it has an ID-buffer pixel this close to it (pixels).
constexpr double kVertexVisiblePx = 2.5;

std::vector<std::int32_t> denseLookup(std::size_t count, const auto& indexAt)
{
    std::uint32_t most = 0;
    for (std::size_t p = 0; p < count; ++p)
        most = std::max(most, indexAt(p));
    if (count == 0 || most >= kMaxDenseIndex)
        return {};
    std::vector<std::int32_t> out(std::size_t(most) + 1, -1);
    for (std::size_t p = 0; p < count; ++p) {
        std::int32_t& slot = out[indexAt(p)];
        if (slot < 0)
            slot = std::int32_t(p);
    }
    return out;
}

std::int32_t lookup(const std::vector<std::int32_t>& table, std::uint32_t index)
{
    return index < table.size() ? table[index] : -1;
}

std::uint32_t le24(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
}

void mul4(const double m[16], const double v[4], double out[4])
{
    for (int r = 0; r < 4; ++r)
        out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}

double dist2(double ax, double ay, double bx, double by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

kernel::SubKind subKindOf(IdKind kind)
{
    return kind == IdKind::Face ? kernel::SubKind::Face : kernel::SubKind::Edge;
}

struct Candidate {
    const IdPixel* pixel = nullptr;
    int x = 0, y = 0;
    double d2 = std::numeric_limits<double>::infinity();
};

// Nearer to the cursor first, then nearer to the eye; the scan order breaks the remaining ties.
bool better(const Candidate& c, double d2, const IdPixel& p)
{
    if (!c.pixel)
        return true;
    if (d2 != c.d2)
        return d2 < c.d2;
    return p.depth < c.pixel->depth;
}

struct VertexKey {
    ItemId item;
    std::int32_t vertex;
    auto operator<=>(const VertexKey&) const = default;
};

// True when an edge that ends at `v` shows in the ID buffer within kVertexVisiblePx of its screen position.
bool vertexVisible(const IdWindow& win, ItemId item, const MeshIndex::Vertex& v, Vec2 screen)
{
    const int r = int(std::ceil(kVertexVisiblePx));
    const int cx = int(std::floor(screen.x));
    const int cy = int(std::floor(screen.y));
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            const IdPixel* p = win.at(x, y);
            if (!p || p->kind != IdKind::Edge || p->item != item)
                continue;
            if (dist2(x + 0.5, y + 0.5, screen.x, screen.y) > kVertexVisiblePx * kVertexVisiblePx)
                continue;
            if (std::binary_search(v.edges.begin(), v.edges.end(), p->index))
                return true;
        }
    }
    return false;
}

struct VertexCandidate {
    ItemId item = 0;
    const MeshIndex::Vertex* vertex = nullptr;
    Vec3 world{};
    Vec2 screen{};
};

// The distinct visible vertices at the ends of the mesh edges in `edges`.
std::vector<VertexCandidate> vertexCandidates(const PickView& view, const IdWindow& win,
                                              const std::set<std::pair<ItemId, std::uint32_t>>& edges)
{
    std::vector<VertexCandidate> out;
    std::set<VertexKey> seen;
    for (const auto& [itemId, edgeIndex] : edges) {
        const PickItem* item = view.item(itemId);
        if (!item || item->lines || !item->index)
            continue;
        const MeshIndex& index = *item->index;
        const std::int32_t pos = lookup(index.edgeByKernel, edgeIndex);
        if (pos < 0)
            continue;
        for (int end = 0; end < 2; ++end) {
            const std::size_t slot = std::size_t(pos) * 2 + std::size_t(end);
            const std::int32_t v = slot < index.endVertex.size() ? index.endVertex[slot] : -1;
            if (v < 0 || !seen.insert(VertexKey{itemId, v}).second)
                continue;
            const MeshIndex::Vertex& vertex = index.vertices[std::size_t(v)];
            const Vec3 world = transformPoint(item->xform, vertex.pos);
            if (view.section && view.section->distance(world) > 0)
                continue; // cut away (T-023)
            const std::optional<Vec2> screen = view.project(world);
            if (!screen || !vertexVisible(win, itemId, vertex, *screen))
                continue;
            out.push_back(VertexCandidate{itemId, &vertex, world, *screen});
        }
    }
    return out;
}

bool wants(std::uint8_t mask, PickFilter f)
{
    return (mask & std::uint8_t(f)) != 0;
}

} // namespace

// ---------- MeshIndex ----------
const MeshIndex::Vertex* MeshIndex::vertexOfCode(std::uint32_t code) const
{
    const std::int32_t pos = lookup(edgeByKernel, vertexEdge(code));
    if (pos < 0)
        return nullptr;
    const std::size_t slot = std::size_t(pos) * 2 + (vertexIsLastEnd(code) ? 1 : 0);
    if (slot >= endVertex.size() || endVertex[slot] < 0)
        return nullptr;
    return &vertices[std::size_t(endVertex[slot])];
}

std::shared_ptr<const MeshIndex> buildMeshIndex(const kernel::Mesh& mesh)
{
    auto out = std::make_shared<MeshIndex>();
    out->faceByKernel = denseLookup(mesh.faces.size(), [&](std::size_t p) { return mesh.faces[p].face.index; });
    out->edgeByKernel = denseLookup(mesh.edges.size(), [&](std::size_t p) { return mesh.edges[p].edge.index; });

    out->edgeFirstSegment.resize(mesh.edges.size());
    out->edgeSegmentCount.resize(mesh.edges.size());
    std::uint32_t segments = 0;
    for (std::size_t p = 0; p < mesh.edges.size(); ++p) {
        const std::uint32_t n = mesh.edges[p].vertexCount > 1 ? mesh.edges[p].vertexCount - 1 : 0;
        out->edgeFirstSegment[p] = segments;
        out->edgeSegmentCount[p] = n;
        segments += n;
    }

    // Weld the end points of the edge polylines. The kernel puts both ends of adjacent edges on the same vertex
    // point; the tolerance only absorbs rounding.
    struct End {
        float x, y, z;
        std::uint32_t code;
        std::uint32_t slot; // position * 2 + end
    };
    std::vector<End> ends;
    ends.reserve(mesh.edges.size() * 2);
    float lo = std::numeric_limits<float>::infinity(), hi = -lo;
    for (std::size_t p = 0; p < mesh.edges.size(); ++p) {
        const kernel::EdgeRange& e = mesh.edges[p];
        if (e.vertexCount == 0)
            continue;
        for (std::uint32_t end = 0; end < 2; ++end) {
            const std::size_t at = std::size_t(e.firstVertex) + (end ? e.vertexCount - 1 : 0);
            const float* xyz = &mesh.edgePositions[at * 3];
            ends.push_back(End{xyz[0], xyz[1], xyz[2], vertexCode(e.edge.index, end == 1), std::uint32_t(p * 2 + end)});
            for (int c = 0; c < 3; ++c) {
                lo = std::min(lo, xyz[c]);
                hi = std::max(hi, xyz[c]);
            }
        }
    }
    out->endVertex.assign(mesh.edges.size() * 2, -1);
    if (ends.empty())
        return out;
    const double tol = std::max(1e-6, 1e-6 * double(hi - lo));
    std::sort(ends.begin(), ends.end(), [](const End& a, const End& b) {
        return std::tie(a.x, a.y, a.z, a.code) < std::tie(b.x, b.y, b.z, b.code);
    });
    std::vector<std::size_t> parent(ends.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&](std::size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    for (std::size_t i = 0; i < ends.size(); ++i) {
        for (std::size_t j = i + 1; j < ends.size() && double(ends[j].x) - double(ends[i].x) <= tol; ++j) {
            const double dy = double(ends[j].y) - double(ends[i].y);
            const double dz = double(ends[j].z) - double(ends[i].z);
            if (std::abs(dy) <= tol && std::abs(dz) <= tol) {
                const std::size_t a = root(i), b = root(j);
                if (a != b)
                    parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    // One vertex per cluster, named by its smallest code; vertices sorted by code.
    std::map<std::size_t, std::size_t> best; // root -> index into ends with the smallest code
    for (std::size_t i = 0; i < ends.size(); ++i) {
        auto [it, inserted] = best.emplace(root(i), i);
        if (!inserted && ends[i].code < ends[it->second].code)
            it->second = i;
    }
    std::vector<std::pair<std::uint32_t, std::size_t>> order; // code, root
    order.reserve(best.size());
    for (const auto& [r, i] : best)
        order.emplace_back(ends[i].code, r);
    std::sort(order.begin(), order.end());
    std::map<std::size_t, std::int32_t> vertexOfRoot;
    for (const auto& [code, r] : order) {
        MeshIndex::Vertex v;
        v.code = code;
        const End& e = ends[best[r]];
        v.pos[0] = e.x;
        v.pos[1] = e.y;
        v.pos[2] = e.z;
        vertexOfRoot[r] = std::int32_t(out->vertices.size());
        out->vertices.push_back(std::move(v));
    }
    for (std::size_t i = 0; i < ends.size(); ++i) {
        const std::int32_t v = vertexOfRoot[root(i)];
        out->endVertex[ends[i].slot] = v;
        out->vertices[std::size_t(v)].edges.push_back(vertexEdge(ends[i].code));
    }
    for (MeshIndex::Vertex& v : out->vertices) {
        std::sort(v.edges.begin(), v.edges.end());
        v.edges.erase(std::unique(v.edges.begin(), v.edges.end()), v.edges.end());
    }
    return out;
}

// ---------- ID window ----------
IdWindow decodeIdWindow(int x0, int y0, int width, int height, const std::uint8_t* ids, const std::uint8_t* items,
                        const std::uint8_t* depths, bool bottomUp)
{
    IdWindow win;
    win.x0 = x0;
    win.y0 = y0;
    win.width = std::max(0, width);
    win.height = std::max(0, height);
    win.pixels.resize(std::size_t(win.width) * std::size_t(win.height));
    for (int row = 0; row < win.height; ++row) {
        const int y = bottomUp ? win.height - 1 - row : row;
        for (int x = 0; x < win.width; ++x) {
            const std::size_t src = (std::size_t(row) * std::size_t(win.width) + std::size_t(x)) * 4;
            IdPixel& p = win.pixels[std::size_t(y) * std::size_t(win.width) + std::size_t(x)];
            const std::uint32_t idPlusOne = le24(ids + src);
            const std::uint8_t kind = ids[src + 3];
            if (kind == std::uint8_t(IdKind::Path)) {
                // T-038: the move index itself, its high byte in the item attachment's alpha.
                p.kind = IdKind::Path;
                p.index = idPlusOne | (std::uint32_t(items[src + 3]) << 24);
            } else {
                if (idPlusOne == 0 || kind == 0 || kind > std::uint8_t(IdKind::Cap))
                    continue;
                p.kind = IdKind(kind);
                p.index = idPlusOne - 1;
            }
            p.item = le24(items + src);
            p.depth = double(le24(depths + src)) / 16777215.0;
        }
    }
    return win;
}

// ---------- PickView ----------
const PickItem* PickView::item(ItemId id) const
{
    const auto it =
        std::lower_bound(items.begin(), items.end(), id, [](const PickItem& a, ItemId b) { return a.id < b; });
    return it != items.end() && it->id == id ? &*it : nullptr;
}

Vec3 PickView::unproject(int x, int y, double depth) const
{
    const double ndcX = 2.0 * (x + 0.5) / double(std::max(1, width)) - 1.0;
    const double t = 2.0 * (y + 0.5) / double(std::max(1, height));
    const double ndcY = yUpInNdc ? 1.0 - t : t - 1.0;
    const double clip[4] = {ndcX, ndcY, 2.0 * depth - 1.0, 1.0};
    double w[4];
    mul4(invViewProj, clip, w);
    const double inv = std::abs(w[3]) > 1e-300 ? 1.0 / w[3] : 0.0;
    return Vec3{w[0] * inv, w[1] * inv, w[2] * inv};
}

std::optional<Vec2> PickView::project(Vec3 p) const
{
    const double v[4] = {p.x, p.y, p.z, 1.0};
    double c[4];
    mul4(viewProj, v, c);
    if (c[3] <= 1e-12)
        return std::nullopt;
    const double ndcX = c[0] / c[3];
    const double ndcY = c[1] / c[3];
    const double sx = (ndcX + 1.0) * 0.5 * width;
    const double sy = (yUpInNdc ? (1.0 - ndcY) : (ndcY + 1.0)) * 0.5 * height;
    return Vec2{sx, sy};
}

bool invert4x4(const double m[16], double inv[16])
{
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::abs(det) > 0) || !std::isfinite(det))
        return false;
    const double k = 1.0 / det;
    for (int i = 0; i < 16; ++i)
        inv[i] *= k;
    return true;
}

Vec3 transformPoint(const Xform& xf, const float* p)
{
    const double* m = xf.m;
    const double x = p[0], y = p[1], z = p[2];
    return Vec3{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                m[8] * x + m[9] * y + m[10] * z + m[11]};
}

// ---------- Search ----------
std::optional<PickHit> resolvePick(const PickView& view, const IdWindow& win, int x, int y, double radius,
                                   std::uint8_t mask)
{
    if (x < 0 || y < 0 || x >= view.width || y >= view.height)
        return std::nullopt;
    const double r2 = radius * radius;
    const double reach = radius + kVertexVisiblePx;
    const int span = int(std::ceil(reach));
    const double cx = x + 0.5, cy = y + 0.5;

    Candidate edge, face, path;
    std::set<std::pair<ItemId, std::uint32_t>> nearEdges; // mesh edges for the vertex search
    for (int py = y - span; py <= y + span; ++py) {
        for (int px = x - span; px <= x + span; ++px) {
            const IdPixel* p = win.at(px, py);
            if (!p || p->kind == IdKind::None || p->kind == IdKind::Cap)
                continue; // a cap is only ever its body (below)
            const double d2 = double(px - x) * (px - x) + double(py - y) * (py - y);
            if (p->kind == IdKind::Face) {
                if (d2 <= kFaceUnderEdgePx * kFaceUnderEdgePx && better(face, d2, *p))
                    face = Candidate{p, px, py, d2};
                continue;
            }
            if (p->kind == IdKind::Path) { // T-038: toolpath segments, a kind of their own
                if (d2 <= r2 && better(path, d2, *p))
                    path = Candidate{p, px, py, d2};
                continue;
            }
            if (d2 <= r2 && better(edge, d2, *p))
                edge = Candidate{p, px, py, d2};
            if (p->kind == IdKind::Edge && d2 <= reach * reach)
                nearEdges.emplace(p->item, p->index);
        }
    }
    // A face needs the cursor pixel, unless that pixel is an edge line drawn over it.
    const IdPixel* centre = win.at(x, y);
    if (face.pixel && face.d2 > 0 && !(centre && centre->kind != IdKind::None && centre->kind != IdKind::Cap))
        face = Candidate{};

    std::optional<VertexCandidate> vertex;
    double vertexD2 = std::numeric_limits<double>::infinity();
    if (wants(mask, PickFilter::Vertices) || wants(mask, PickFilter::Bodies)) {
        for (const VertexCandidate& v : vertexCandidates(view, win, nearEdges)) {
            const double d2 = dist2(v.screen.x, v.screen.y, cx, cy);
            if (d2 > r2)
                continue;
            if (!vertex || d2 < vertexD2 ||
                (d2 == vertexD2 && std::tie(v.item, v.vertex->code) < std::tie(vertex->item, vertex->vertex->code))) {
                vertex = v;
                vertexD2 = d2;
            }
        }
    }

    const auto hitAt = [&](const Candidate& c, SubKind kind, std::uint32_t index) {
        return PickHit{c.pixel->item, kind, index, view.unproject(c.x, c.y, c.pixel->depth), 0};
    };
    if (wants(mask, PickFilter::Vertices) && vertex)
        return PickHit{vertex->item, SubKind::Vertex, vertex->vertex->code, vertex->world, 0};
    // T-038: a toolpath segment answers as an edge of its backplot, subIndex and moveIndex its move; with Edges
    // wanted as well, the nearer of the two (then the nearer to the eye) wins.
    const bool pathWins = wants(mask, PickFilter::Toolpath) && path.pixel &&
                          !(wants(mask, PickFilter::Edges) && edge.pixel && !better(edge, path.d2, *path.pixel));
    if (pathWins) {
        PickHit hit = hitAt(path, SubKind::Edge, path.pixel->index);
        hit.moveIndex = path.pixel->index;
        return hit;
    }
    if (wants(mask, PickFilter::Edges) && edge.pixel)
        return hitAt(edge, subKindOf(edge.pixel->kind), edge.pixel->index);
    if (wants(mask, PickFilter::Faces) && face.pixel)
        return hitAt(face, SubKind::Face, face.pixel->index);
    if (wants(mask, PickFilter::Bodies)) {
        // The body of whatever is under the cursor, else of the nearest element inside the tolerance. A toolpath is
        // no body (T-038).
        if (centre && centre->kind != IdKind::None && centre->kind != IdKind::Path)
            return PickHit{centre->item, SubKind::Solid, 0, view.unproject(x, y, centre->depth), 0};
        if (vertex)
            return PickHit{vertex->item, SubKind::Solid, 0, vertex->world, 0};
        if (edge.pixel)
            return hitAt(edge, SubKind::Solid, 0);
    }
    return std::nullopt;
}

std::vector<PickHit> resolvePickRect(const PickView& view, const IdWindow& win, int x0, int y0, int x1, int y1,
                                     std::uint8_t mask)
{
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, view.width - 1);
    y1 = std::min(y1, view.height - 1);
    std::vector<PickHit> out;
    if (x0 > x1 || y0 > y1)
        return out;
    const double cx = 0.5 * (x0 + x1 + 1), cy = 0.5 * (y0 + y1 + 1);

    // Per element, the pixel nearest the rectangle's centre gives its point.
    using Key = std::tuple<ItemId, SubKind, std::uint32_t>;
    std::map<Key, Candidate> found;
    std::set<std::pair<ItemId, std::uint32_t>> meshEdges;
    const auto keep = [&](const Key& key, const IdPixel* p, int px, int py) {
        const double d2 = dist2(px + 0.5, py + 0.5, cx, cy);
        Candidate& c = found[key];
        if (better(c, d2, *p))
            c = Candidate{p, px, py, d2};
    };
    // A hidden edge peeks out for a pixel or two at the end it shares with a visible vertex (the faces' depth bias
    // lets it through there). Such an edge counts only if it shows somewhere away from its end points, unless it is
    // so short on screen that it has nowhere else to show.
    struct EdgeEnds {
        std::optional<Vec2> a, b;
        bool tiny = false;
    };
    std::map<std::pair<ItemId, std::uint32_t>, EdgeEnds> ends;
    std::set<std::pair<ItemId, std::uint32_t>> shownAway;
    const auto endsOf = [&](ItemId itemId, std::uint32_t edgeIndex) -> const EdgeEnds& {
        auto [it, inserted] = ends.try_emplace({itemId, edgeIndex});
        if (!inserted)
            return it->second;
        const PickItem* item = view.item(itemId);
        if (!item || !item->index)
            return it->second;
        const std::int32_t pos = item->index->edgePosition(edgeIndex);
        if (pos < 0)
            return it->second;
        for (int end = 0; end < 2; ++end) {
            const std::int32_t v = item->index->endVertex[std::size_t(pos) * 2 + std::size_t(end)];
            if (v < 0)
                continue;
            const auto screen = view.project(transformPoint(item->xform, item->index->vertices[std::size_t(v)].pos));
            (end == 0 ? it->second.a : it->second.b) = screen;
        }
        EdgeEnds& e = it->second;
        constexpr double tinyPx = 2 * kVertexVisiblePx + 1;
        // A closed edge (both ends on one vertex) is not tiny; it shows away from its seam.
        if (e.a && e.b) {
            const double d2 = dist2(e.a->x, e.a->y, e.b->x, e.b->y);
            e.tiny = d2 > 0.25 && d2 <= tinyPx * tinyPx;
        }
        return e;
    };
    const auto nearEnd = [](const std::optional<Vec2>& end, int px, int py) {
        return end && dist2(px + 0.5, py + 0.5, end->x, end->y) <= kVertexVisiblePx * kVertexVisiblePx;
    };

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const IdPixel* p = win.at(px, py);
            if (!p || p->kind == IdKind::None)
                continue;
            if (p->kind == IdKind::Path) { // T-038: one hit per move, Toolpath only
                if (wants(mask, PickFilter::Toolpath))
                    keep(Key{p->item, SubKind::Edge, p->index}, p, px, py);
                continue;
            }
            if (p->kind == IdKind::Face && wants(mask, PickFilter::Faces))
                keep(Key{p->item, SubKind::Face, p->index}, p, px, py);
            if ((p->kind == IdKind::Edge || p->kind == IdKind::Segment) && wants(mask, PickFilter::Edges))
                keep(Key{p->item, SubKind::Edge, p->index}, p, px, py);
            if (wants(mask, PickFilter::Bodies))
                keep(Key{p->item, SubKind::Solid, 0}, p, px, py);
            if (p->kind == IdKind::Edge) {
                meshEdges.emplace(p->item, p->index);
                const EdgeEnds& e = endsOf(p->item, p->index);
                if (e.tiny || (!nearEnd(e.a, px, py) && !nearEnd(e.b, px, py)))
                    shownAway.emplace(p->item, p->index);
            }
        }
    }
    for (const auto& edge : meshEdges) {
        if (!shownAway.count(edge))
            found.erase(Key{edge.first, SubKind::Edge, edge.second});
    }
    for (const auto& [key, c] : found) {
        const std::uint32_t move = c.pixel->kind == IdKind::Path ? c.pixel->index : 0;
        out.push_back(PickHit{std::get<0>(key), std::get<1>(key), std::get<2>(key),
                              view.unproject(c.x, c.y, c.pixel->depth), move});
    }
    if (wants(mask, PickFilter::Vertices)) {
        for (const VertexCandidate& v : vertexCandidates(view, win, meshEdges)) {
            if (v.screen.x < x0 || v.screen.x >= x1 + 1 || v.screen.y < y0 || v.screen.y >= y1 + 1)
                continue;
            out.push_back(PickHit{v.item, SubKind::Vertex, v.vertex->code, v.world, 0});
        }
    }
    std::sort(out.begin(), out.end(), [](const PickHit& a, const PickHit& b) {
        return std::tie(a.item, a.kind, a.subIndex) < std::tie(b.item, b.kind, b.subIndex);
    });
    return out;
}

} // namespace ray::render::detail
