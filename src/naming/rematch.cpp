// rematchImported (task T-084): the names of an imported body carried over to a new revision of it. The rules are in
// rematch.h. Deterministic: every loop runs in SubId order, every choice is made by sorting on indices, and no <cmath>
// trig is used (PLAN.md section 8).
#include "rematch.h"

#include "import_naming.h"
#include "territory.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace ray::naming {

namespace detail {

namespace {

using kernel::SubId;
using kernel::SubKind;

constexpr std::size_t kVertex = 0, kEdge = 1, kFace = 2;
constexpr std::array<SubKind, 3> kKinds{SubKind::Vertex, SubKind::Edge, SubKind::Face};

// ---------------------------------------------------------------- small geometry
double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(const Vec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}
bool unitOf(const Vec3& v, Vec3& out)
{
    const double n = norm(v);
    if (!(n > 0.0))
        return false;
    out = mul(v, 1.0 / n);
    return true;
}
// Distance of p from the line through o along unit u.
double offLine(const Vec3& p, const Vec3& o, const Vec3& u)
{
    const Vec3 w = sub(p, o);
    return norm(sub(w, mul(u, dot(w, u))));
}
double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 d = sub(b, a);
    const double l2 = dot(d, d);
    double t = l2 > 0.0 ? dot(sub(p, a), d) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return norm(sub(p, add(a, mul(d, t))));
}
bool parallel(const Vec3& a, const Vec3& b)
{
    return std::abs(dot(a, b)) >= 1.0 - kRematchParallel;
}
// Boxes that agree within `tol` on every side.
bool sameBox(const Box3& a, const Box3& b, double tol)
{
    return std::abs(a.min.x - b.min.x) <= tol && std::abs(a.min.y - b.min.y) <= tol &&
           std::abs(a.min.z - b.min.z) <= tol && std::abs(a.max.x - b.max.x) <= tol &&
           std::abs(a.max.y - b.max.y) <= tol && std::abs(a.max.z - b.max.z) <= tol;
}
// Boxes that touch or overlap, each grown by `tol`.
bool boxesMeet(const Box3& a, const Box3& b, double tol)
{
    return a.min.x <= b.max.x + tol && b.min.x <= a.max.x + tol && a.min.y <= b.max.y + tol &&
           b.min.y <= a.max.y + tol && a.min.z <= b.max.z + tol && b.min.z <= a.max.z + tol;
}

std::vector<std::uint32_t> indicesOf(const std::vector<SubId>& subs)
{
    std::vector<std::uint32_t> out;
    out.reserve(subs.size());
    for (const SubId& s : subs)
        out.push_back(s.index);
    return out;
}

// ---------------------------------------------------------------- what is known of one shape
struct FaceGeo {
    bool ok = false;
    kernel::SurfaceInfo info{};
    int carrier = -1; // 0 plane, 1 cylinder, -1 any other surface (compared by its points)
    Vec3 axis{};      // plane: outward normal; cylinder: unit axis
    int side = 0;     // cylinder: +1 material inside the radius (a boss), -1 outside it (a hole); 0 unknown
    std::vector<std::uint32_t> edges;
};
struct EdgeGeo {
    bool ok = false;
    kernel::CurveInfo info{};
    int carrier = -1; // 0 line, 1 circle, -1 any other curve
    Vec3 axis{};      // line: unit direction; circle: unit axis
    std::vector<std::uint32_t> vertices, faces;
    std::optional<Box3> box;
};
struct VertexGeo {
    bool ok = false;
    Vec3 p{};
    std::vector<std::uint32_t> faces;
};

class Geo {
public:
    explicit Geo(const kernel::Shape& shape) : shape_(shape) {}

    Status read()
    {
        const std::uint32_t nv = shape_.count(SubKind::Vertex), ne = shape_.count(SubKind::Edge),
                            nf = shape_.count(SubKind::Face);
        vertices.resize(nv);
        edges.resize(ne);
        faces.resize(nf);
        for (std::uint32_t i = 0; i < nv; ++i) {
            const SubId v{SubKind::Vertex, i};
            if (const auto p = kernel::vertexPoint(shape_, v)) {
                vertices[i].ok = true;
                vertices[i].p = *p;
            }
            vertices[i].faces = indicesOf(kernel::adjacent(shape_, v, SubKind::Face));
        }
        for (std::uint32_t i = 0; i < ne; ++i) {
            const SubId e{SubKind::Edge, i};
            EdgeGeo& g = edges[i];
            g.vertices = indicesOf(kernel::adjacent(shape_, e, SubKind::Vertex));
            g.faces = indicesOf(kernel::adjacent(shape_, e, SubKind::Face));
            g.box = elementBox(shape_, e);
            if (const auto info = kernel::curveInfo(shape_, e)) {
                g.ok = true;
                g.info = *info;
                if (info->type == kernel::CurveType::Line && unitOf(sub(info->end, info->start), g.axis))
                    g.carrier = 0;
                else if (info->type == kernel::CurveType::Circle && unitOf(info->axis, g.axis) && info->radius > 0.0)
                    g.carrier = 1;
            }
        }
        for (std::uint32_t i = 0; i < nf; ++i) {
            const SubId f{SubKind::Face, i};
            FaceGeo& g = faces[i];
            g.edges = indicesOf(kernel::adjacent(shape_, f, SubKind::Edge));
            const auto info = kernel::surfaceInfo(shape_, f);
            if (!info)
                continue;
            g.ok = true;
            g.info = *info;
            // The side of the material from the kernel's outward normal (faceSample): a surface's own frame may be
            // left-handed, so neither its axis nor `reversed` says it.
            const auto sample = kernel::faceSample(shape_, f);
            if (info->type == kernel::SurfaceType::Plane && sample && unitOf(sample->normal, g.axis)) {
                g.carrier = 0;
            } else if (info->type == kernel::SurfaceType::Cylinder && unitOf(info->axis, g.axis) &&
                       info->radius > 0.0) {
                g.carrier = 1;
                if (sample) {
                    const Vec3 w = sub(sample->point, info->origin);
                    const Vec3 radial = sub(w, mul(g.axis, dot(w, g.axis)));
                    const double s = dot(radial, sample->normal);
                    g.side = s > 0.0 ? 1 : (s < 0.0 ? -1 : 0);
                }
            }
        }
        return {};
    }

    // Samples from a display mesh: per face, mesh nodes (on the surface) and interior points (triangle centres at least
    // kRematchMargin from the face's boundary); per edge, interior polyline points. Coarse: a sample is a witness, not
    // a measure. Without a mesh (it failed), there are none, and the territory points stand alone.
    void sample()
    {
        faceNodes.assign(faces.size(), {});
        faceInner.assign(faces.size(), {});
        edgeInner.assign(edges.size(), {});
        const Box3 box = kernel::bounds(shape_);
        const double diagonal = norm(sub(box.max, box.min));
        chord_ = std::clamp(diagonal * 1e-4, 2e-3, 0.02);
        JobContext ctx;
        const auto mesh = kernel::tessellate(shape_, kernel::MeshOptions{chord_, 0.3}, ctx);
        if (!mesh)
            return;
        auto at = [&](std::uint32_t i) {
            return Vec3{mesh->positions[3 * i], mesh->positions[3 * i + 1], mesh->positions[3 * i + 2]};
        };
        // Edge polylines.
        std::vector<std::vector<Vec3>> polylines(edges.size());
        for (const kernel::EdgeRange& r : mesh->edges) {
            if (r.edge.kind != SubKind::Edge || r.edge.index >= edges.size())
                continue;
            std::vector<Vec3>& line = polylines[r.edge.index];
            for (std::uint32_t k = 0; k < r.vertexCount; ++k) {
                const std::size_t j = 3 * (static_cast<std::size_t>(r.firstVertex) + k);
                if (j + 2 < mesh->edgePositions.size())
                    line.push_back({mesh->edgePositions[j], mesh->edgePositions[j + 1], mesh->edgePositions[j + 2]});
            }
        }
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const std::vector<Vec3>& line = polylines[e];
            if (line.size() < 2)
                continue;
            double length = 0;
            std::vector<double> along{0.0};
            for (std::size_t k = 1; k < line.size(); ++k) {
                length += norm(sub(line[k], line[k - 1]));
                along.push_back(length);
            }
            for (std::size_t k = 0; k < line.size(); ++k)
                if (along[k] >= kRematchMargin && length - along[k] >= kRematchMargin)
                    edgeInner[e].push_back(line[k]);
            thin(edgeInner[e], 16);
        }
        for (const kernel::FaceRange& r : mesh->faces) {
            if (r.face.kind != SubKind::Face || r.face.index >= faces.size())
                continue;
            const std::uint32_t f = r.face.index;
            std::vector<Vec3> nodes, centres;
            std::set<std::uint32_t> seen;
            for (std::uint32_t t = 0; t + 2 < r.indexCount; t += 3) {
                const std::size_t base = static_cast<std::size_t>(r.firstIndex) + t;
                if (base + 2 >= mesh->indices.size())
                    break;
                const std::uint32_t a = mesh->indices[base], b = mesh->indices[base + 1], c = mesh->indices[base + 2];
                for (const std::uint32_t n : {a, b, c})
                    if (seen.insert(n).second)
                        nodes.push_back(at(n));
                centres.push_back(mul(add(add(at(a), at(b)), at(c)), 1.0 / 3.0));
            }
            // Interior: away from every edge of the face by the margin (and by the chord, for curved faces).
            const double margin = kRematchMargin + 2.0 * chord_;
            for (const Vec3& p : centres) {
                bool inside = true;
                for (const std::uint32_t e : faces[f].edges) {
                    const std::vector<Vec3>& line = polylines[e];
                    for (std::size_t k = 1; k < line.size() && inside; ++k)
                        inside = segmentDistance(p, line[k - 1], line[k]) >= margin;
                    if (!inside)
                        break;
                }
                if (inside)
                    faceInner[f].push_back(p);
            }
            thin(nodes, 24);
            thin(faceInner[f], 24);
            faceNodes[f] = std::move(nodes);
        }
    }

    const Territory& territory(SubId s) const
    {
        auto& slot = s.kind == SubKind::Face ? faceTerritory_ : edgeTerritory_;
        if (slot.empty())
            slot.resize(s.kind == SubKind::Face ? faces.size() : edges.size());
        if (!slot[s.index])
            slot[s.index] = std::make_unique<Territory>(territoryOf(shape_, s));
        return *slot[s.index];
    }
    const Probe& probe(SubId s) const
    {
        auto& slot = s.kind == SubKind::Face ? faceProbe_ : edgeProbe_;
        if (slot.empty())
            slot.resize(s.kind == SubKind::Face ? faces.size() : edges.size());
        if (!slot[s.index])
            slot[s.index] = std::make_unique<Probe>(shape_, s);
        return *slot[s.index];
    }
    // The probe tolerance for points of a coarse mesh: a triangle centre lies off a curved surface by up to the chord.
    double sampleTolerance() const { return kRematchTolerance + chord_; }

    std::vector<VertexGeo> vertices;
    std::vector<EdgeGeo> edges;
    std::vector<FaceGeo> faces;
    std::vector<std::vector<Vec3>> faceNodes, faceInner, edgeInner;

private:
    // At most `n` points, spread: every k-th, in order.
    static void thin(std::vector<Vec3>& points, std::size_t n)
    {
        if (points.size() <= n)
            return;
        std::vector<Vec3> out;
        const std::size_t step = (points.size() + n - 1) / n;
        for (std::size_t k = 0; k < points.size(); k += step)
            out.push_back(points[k]);
        points = std::move(out);
    }

    const kernel::Shape& shape_;
    double chord_ = 0.01;
    mutable std::vector<std::unique_ptr<Territory>> faceTerritory_, edgeTerritory_;
    mutable std::vector<std::unique_ptr<Probe>> faceProbe_, edgeProbe_;
};

// Do all the points lie on the element?
bool allOn(const Probe& probe, const std::vector<Vec3>& points, double tol)
{
    return std::all_of(points.begin(), points.end(), [&](const Vec3& p) { return probe.on(p, tol); });
}
bool allOn(const Probe& probe, const Territory& t)
{
    return probe.count(t) == t.points.size();
}
std::size_t countOn(const Probe& probe, const std::vector<Vec3>& points, double tol)
{
    return static_cast<std::size_t>(
        std::count_if(points.begin(), points.end(), [&](const Vec3& p) { return probe.on(p, tol); }));
}
std::size_t countOn(const Probe& probe, const Territory& t)
{
    return probe.count(t);
}

// One-to-one matches from candidate pairs: a pair is kept when neither side has another candidate.
std::vector<std::int64_t> oneToOne(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs,
                                   std::size_t oldCount, std::size_t newCount)
{
    std::vector<int> perOld(oldCount, 0), perNew(newCount, 0);
    for (const auto& [o, n] : pairs) {
        ++perOld[o];
        ++perNew[n];
    }
    std::vector<std::int64_t> same(oldCount, -1);
    for (const auto& [o, n] : pairs)
        if (perOld[o] == 1 && perNew[n] == 1)
            same[o] = n;
    return same;
}

// Connected components of the bipartite graph `links` (old -> new), skipping elements already matched. Groups of one
// old and any number of new elements, or of any number of old and one new, are kept; the others are ambiguous.
std::vector<KindPlan::Group> groupsOf(const std::vector<std::vector<std::uint32_t>>& links, std::size_t newCount)
{
    const std::size_t oldCount = links.size();
    // Union-find over old (0..oldCount-1) and new (oldCount..) nodes.
    std::vector<std::size_t> parent(oldCount + newCount);
    for (std::size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    auto find = [&](std::size_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (std::size_t o = 0; o < oldCount; ++o)
        for (const std::uint32_t n : links[o]) {
            const std::size_t a = find(o), b = find(oldCount + n);
            if (a != b)
                parent[std::max(a, b)] = std::min(a, b);
        }
    std::map<std::size_t, KindPlan::Group> byRoot;
    for (std::size_t o = 0; o < oldCount; ++o)
        if (!links[o].empty())
            byRoot[find(o)].olds.push_back(static_cast<std::uint32_t>(o));
    std::set<std::uint32_t> linkedNew;
    for (const auto& l : links)
        linkedNew.insert(l.begin(), l.end());
    for (const std::uint32_t n : linkedNew)
        byRoot[find(oldCount + n)].news.push_back(n);
    std::vector<KindPlan::Group> out;
    for (auto& [root, g] : byRoot) {
        (void)root;
        if (g.olds.empty() || g.news.empty())
            continue;
        if (g.olds.size() == 1 || g.news.size() == 1)
            out.push_back(std::move(g));
    }
    return out;
}

// ---------------------------------------------------------------- the geometric plan
class Planner {
public:
    Planner(const Geo& a, const Geo& b) : a_(a), b_(b) {}

    RematchPlan run()
    {
        RematchPlan plan;
        const std::vector<std::int64_t> vertexSame = matchVertices();
        const std::vector<std::int64_t> edgeSame = matchEdges(vertexSame);
        // Faces.
        KindPlan& faces = plan.kinds[kFace];
        faces.same = matchFaces(edgeSame);
        faces.groups = groupsOf(faceOverlaps(faces.same), b_.faces.size());
        // Where each old face went (unchanged, or the new faces of a group that carries it).
        std::vector<std::set<std::uint32_t>> faceImage(a_.faces.size());
        for (std::size_t o = 0; o < faces.same.size(); ++o)
            if (faces.same[o] >= 0)
                faceImage[o].insert(static_cast<std::uint32_t>(faces.same[o]));
        for (const KindPlan::Group& g : faces.groups)
            for (const std::uint32_t o : g.olds)
                faceImage[o].insert(g.news.begin(), g.news.end());
        auto facesCarried = [&](const std::vector<std::uint32_t>& oldFaces, const std::vector<std::uint32_t>& newFaces) {
            if (oldFaces.size() != newFaces.size())
                return false;
            for (const std::uint32_t f : oldFaces) {
                const auto& image = faceImage[f];
                if (std::none_of(newFaces.begin(), newFaces.end(), [&](std::uint32_t n) { return image.count(n); }))
                    return false;
            }
            return true;
        };
        // Edges: unchanged when their faces carried over; else changed (the same geometry with other faces around it).
        KindPlan& edges = plan.kinds[kEdge];
        edges.same.assign(a_.edges.size(), -1);
        std::vector<std::vector<std::uint32_t>> edgeLinks(a_.edges.size());
        for (std::size_t o = 0; o < edgeSame.size(); ++o) {
            if (edgeSame[o] < 0)
                continue;
            const auto n = static_cast<std::uint32_t>(edgeSame[o]);
            if (facesCarried(a_.edges[o].faces, b_.edges[n].faces))
                edges.same[o] = n;
            else
                edgeLinks[o].push_back(n);
        }
        const auto overlaps = edgeOverlaps(edgeSame);
        for (std::size_t o = 0; o < overlaps.size(); ++o)
            if (edgeSame[o] < 0)
                edgeLinks[o] = overlaps[o];
        edges.groups = groupsOf(edgeLinks, b_.edges.size());
        // Vertices: unchanged when their faces carried over; else changed.
        KindPlan& vertices = plan.kinds[kVertex];
        vertices.same.assign(a_.vertices.size(), -1);
        std::vector<std::vector<std::uint32_t>> vertexLinks(a_.vertices.size());
        for (std::size_t o = 0; o < vertexSame.size(); ++o) {
            if (vertexSame[o] < 0)
                continue;
            const auto n = static_cast<std::uint32_t>(vertexSame[o]);
            if (facesCarried(a_.vertices[o].faces, b_.vertices[n].faces))
                vertices.same[o] = n;
            else
                vertexLinks[o].push_back(n);
        }
        vertices.groups = groupsOf(vertexLinks, b_.vertices.size());
        return plan;
    }

private:
    std::vector<std::int64_t> matchVertices() const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        // Sorted by x so only near ones are compared.
        std::vector<std::uint32_t> order;
        for (std::uint32_t n = 0; n < b_.vertices.size(); ++n)
            if (b_.vertices[n].ok)
                order.push_back(n);
        std::sort(order.begin(), order.end(), [&](std::uint32_t l, std::uint32_t r) {
            return b_.vertices[l].p.x != b_.vertices[r].p.x ? b_.vertices[l].p.x < b_.vertices[r].p.x : l < r;
        });
        for (std::uint32_t o = 0; o < a_.vertices.size(); ++o) {
            if (!a_.vertices[o].ok)
                continue;
            const Vec3& p = a_.vertices[o].p;
            auto it = std::lower_bound(order.begin(), order.end(), p.x - kRematchTolerance,
                                       [&](std::uint32_t n, double x) { return b_.vertices[n].p.x < x; });
            for (; it != order.end() && b_.vertices[*it].p.x <= p.x + kRematchTolerance; ++it)
                if (norm(sub(b_.vertices[*it].p, p)) <= kRematchTolerance)
                    pairs.emplace_back(o, *it);
        }
        std::sort(pairs.begin(), pairs.end());
        return oneToOne(pairs, a_.vertices.size(), b_.vertices.size());
    }

    bool sameCurve(const EdgeGeo& x, const EdgeGeo& y) const
    {
        if (!x.ok || !y.ok || x.info.type != y.info.type || x.carrier != y.carrier)
            return false;
        if (std::abs(x.info.length - y.info.length) > kRematchTolerance)
            return false;
        switch (x.carrier) {
        case 0:
            return parallel(x.axis, y.axis) && offLine(y.info.start, x.info.start, x.axis) <= kRematchTolerance;
        case 1:
            return parallel(x.axis, y.axis) && std::abs(x.info.radius - y.info.radius) <= kRematchTolerance &&
                   norm(sub(x.info.center, y.info.center)) <= kRematchTolerance;
        default:
            return true; // compared by its points
        }
    }

    std::vector<std::int64_t> matchEdges(const std::vector<std::int64_t>& vertexSame) const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        for (std::uint32_t o = 0; o < a_.edges.size(); ++o) {
            const EdgeGeo& x = a_.edges[o];
            if (!x.ok || !x.box)
                continue;
            // The new vertices the old end vertices became; all must have matched.
            std::vector<std::uint32_t> ends;
            bool endsKnown = true;
            for (const std::uint32_t v : x.vertices) {
                if (vertexSame[v] < 0)
                    endsKnown = false;
                else
                    ends.push_back(static_cast<std::uint32_t>(vertexSame[v]));
            }
            if (!endsKnown)
                continue;
            std::sort(ends.begin(), ends.end());
            for (std::uint32_t n = 0; n < b_.edges.size(); ++n) {
                const EdgeGeo& y = b_.edges[n];
                if (!y.box || !sameBox(*x.box, *y.box, 10.0 * kRematchTolerance) || !sameCurve(x, y))
                    continue;
                std::vector<std::uint32_t> theirs = y.vertices;
                std::sort(theirs.begin(), theirs.end());
                if (theirs != ends)
                    continue;
                const SubId so{SubKind::Edge, o}, sn{SubKind::Edge, n};
                if (!allOn(b_.probe(sn), a_.territory(so)) || !allOn(a_.probe(so), b_.territory(sn)))
                    continue;
                if (!allOn(b_.probe(sn), a_.edgeInner[o], b_.sampleTolerance()) ||
                    !allOn(a_.probe(so), b_.edgeInner[n], a_.sampleTolerance()))
                    continue;
                pairs.emplace_back(o, n);
            }
        }
        return oneToOne(pairs, a_.edges.size(), b_.edges.size());
    }

    bool sameSurface(std::uint32_t o, std::uint32_t n) const
    {
        const FaceGeo& x = a_.faces[o];
        const FaceGeo& y = b_.faces[n];
        if (!x.ok || !y.ok || x.info.type != y.info.type || x.carrier != y.carrier)
            return false;
        if (!sameBox(x.info.bounds, y.info.bounds, 0.05))
            return false;
        // Area and centroid: a sanity check for every face, the only global one for a surface compared by its points.
        const double area = std::max(x.info.area, y.info.area);
        if (std::abs(x.info.area - y.info.area) > 1e-5 * area + 1e-3)
            return false;
        if (norm(sub(x.info.centroid, y.info.centroid)) > 2.0 * kRematchTolerance)
            return false;
        switch (x.carrier) {
        case 0:
            return dot(x.axis, y.axis) >= 1.0 - kRematchParallel &&
                   std::abs(dot(sub(y.info.origin, x.info.origin), x.axis)) <= kRematchTolerance;
        case 1:
            return parallel(x.axis, y.axis) && std::abs(x.info.radius - y.info.radius) <= kRematchTolerance &&
                   offLine(y.info.origin, x.info.origin, x.axis) <= kRematchTolerance && x.side == y.side;
        default:
            if (std::abs(x.info.radius - y.info.radius) > kRematchTolerance)
                return false;
            // Mesh nodes lie on the surface: each face's on the other.
            return allOn(b_.probe({SubKind::Face, n}), a_.faceNodes[o], 2.0 * kRematchTolerance) &&
                   allOn(a_.probe({SubKind::Face, o}), b_.faceNodes[n], 2.0 * kRematchTolerance);
        }
    }

    // A seam: an edge that lies on its face only (the line where a closed surface meets itself). Not a boundary.
    static bool seam(const Geo& g, std::uint32_t face, std::uint32_t edge)
    {
        const auto& faces = g.edges[edge].faces;
        return faces.size() == 1 && faces.front() == face;
    }

    // Does every edge of face `f` of `x` (seams aside) lie on edges of face `h` of `y`: its territory points, its interior
    // samples and its end points, each on one of them? Two faces bounded alike, however the bounds are cut into edges (one
    // file splits a hole's rim into two arcs, another writes one circle).
    static bool boundaryOn(const Geo& x, std::uint32_t f, const Geo& y, std::uint32_t h)
    {
        std::vector<const Probe*> edges;
        for (const std::uint32_t e : y.faces[h].edges)
            if (!seam(y, h, e))
                edges.push_back(&y.probe({SubKind::Edge, e}));
        auto onSome = [&](const Vec3& p, double tol) {
            return std::any_of(edges.begin(), edges.end(), [&](const Probe* probe) { return probe->on(p, tol); });
        };
        for (const std::uint32_t e : x.faces[f].edges) {
            if (seam(x, f, e))
                continue;
            const EdgeGeo& g = x.edges[e];
            if (!g.ok)
                return false;
            for (const Vec3& p : {g.info.start, g.info.end})
                if (!onSome(p, kRematchTolerance))
                    return false;
            for (const auto& q : x.territory({SubKind::Edge, e}).points)
                if (!onSome(fromQuanta(q), kRematchTolerance))
                    return false;
            for (const Vec3& p : x.edgeInner[e])
                if (!onSome(p, y.sampleTolerance()))
                    return false;
        }
        return true;
    }

    std::vector<std::int64_t> matchFaces(const std::vector<std::int64_t>& edgeSame) const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        for (std::uint32_t o = 0; o < a_.faces.size(); ++o) {
            const FaceGeo& x = a_.faces[o];
            if (!x.ok)
                continue;
            // The new edges the old boundary became, when every one of them is unchanged.
            std::vector<std::uint32_t> boundary;
            bool known = true;
            for (const std::uint32_t e : x.edges) {
                if (edgeSame[e] < 0)
                    known = false;
                else
                    boundary.push_back(static_cast<std::uint32_t>(edgeSame[e]));
            }
            std::sort(boundary.begin(), boundary.end());
            for (std::uint32_t n = 0; n < b_.faces.size(); ++n) {
                if (!sameSurface(o, n))
                    continue;
                // Bounded by the same edges, or (edges cut otherwise) by edges on the same curves.
                if (!(known && b_.faces[n].edges == boundary) &&
                    !(boundaryOn(a_, o, b_, n) && boundaryOn(b_, n, a_, o)))
                    continue;
                const SubId so{SubKind::Face, o}, sn{SubKind::Face, n};
                if (!allOn(b_.probe(sn), a_.territory(so)) || !allOn(a_.probe(so), b_.territory(sn)))
                    continue;
                if (!allOn(b_.probe(sn), a_.faceInner[o], b_.sampleTolerance()) ||
                    !allOn(a_.probe(so), b_.faceInner[n], a_.sampleTolerance()))
                    continue;
                pairs.emplace_back(o, n);
            }
        }
        return oneToOne(pairs, a_.faces.size(), b_.faces.size());
    }

    // Same plane (outward side) or cylinder (material side), position aside.
    bool sameCarrier(const FaceGeo& x, const FaceGeo& y) const
    {
        if (!x.ok || !y.ok || x.carrier < 0 || x.carrier != y.carrier)
            return false;
        if (x.carrier == 0)
            return dot(x.axis, y.axis) >= 1.0 - kRematchParallel &&
                   std::abs(dot(sub(y.info.origin, x.info.origin), x.axis)) <= kRematchTolerance;
        return parallel(x.axis, y.axis) && std::abs(x.info.radius - y.info.radius) <= kRematchTolerance &&
               offLine(y.info.origin, x.info.origin, x.axis) <= kRematchTolerance && x.side == y.side && x.side != 0;
    }

    // Old faces not matched, each with the new faces not matched that share ground with it on the same carrier.
    std::vector<std::vector<std::uint32_t>> faceOverlaps(const std::vector<std::int64_t>& same) const
    {
        std::vector<bool> newMatched(b_.faces.size(), false);
        for (const std::int64_t n : same)
            if (n >= 0)
                newMatched[static_cast<std::size_t>(n)] = true;
        std::vector<std::vector<std::uint32_t>> out(a_.faces.size());
        for (std::uint32_t o = 0; o < a_.faces.size(); ++o) {
            if (same[o] >= 0)
                continue;
            const FaceGeo& x = a_.faces[o];
            for (std::uint32_t n = 0; n < b_.faces.size(); ++n) {
                const FaceGeo& y = b_.faces[n];
                if (newMatched[n] || !boxesMeet(x.info.bounds, y.info.bounds, kRematchTolerance) ||
                    !sameCarrier(x, y))
                    continue;
                const SubId so{SubKind::Face, o}, sn{SubKind::Face, n};
                const std::size_t hits = countOn(b_.probe(sn), a_.territory(so)) +
                                         countOn(a_.probe(so), b_.territory(sn)) +
                                         countOn(b_.probe(sn), a_.faceInner[o], b_.sampleTolerance()) +
                                         countOn(a_.probe(so), b_.faceInner[n], a_.sampleTolerance());
                if (hits > 0)
                    out[o].push_back(n);
            }
        }
        return out;
    }

    // Edges not matched by geometry, each with the new edges not matched that share ground with it on the same line or
    // circle.
    std::vector<std::vector<std::uint32_t>> edgeOverlaps(const std::vector<std::int64_t>& same) const
    {
        std::vector<bool> newMatched(b_.edges.size(), false);
        for (const std::int64_t n : same)
            if (n >= 0)
                newMatched[static_cast<std::size_t>(n)] = true;
        std::vector<std::vector<std::uint32_t>> out(a_.edges.size());
        for (std::uint32_t o = 0; o < a_.edges.size(); ++o) {
            const EdgeGeo& x = a_.edges[o];
            if (same[o] >= 0 || !x.ok || x.carrier < 0 || !x.box)
                continue;
            for (std::uint32_t n = 0; n < b_.edges.size(); ++n) {
                const EdgeGeo& y = b_.edges[n];
                if (newMatched[n] || !y.ok || y.carrier != x.carrier || !y.box ||
                    !boxesMeet(*x.box, *y.box, kRematchTolerance) || !parallel(x.axis, y.axis))
                    continue;
                const bool carrier =
                    x.carrier == 0 ? offLine(y.info.start, x.info.start, x.axis) <= kRematchTolerance
                                   : std::abs(x.info.radius - y.info.radius) <= kRematchTolerance &&
                                         norm(sub(x.info.center, y.info.center)) <= kRematchTolerance;
                if (!carrier)
                    continue;
                const SubId so{SubKind::Edge, o}, sn{SubKind::Edge, n};
                const std::size_t hits = countOn(b_.probe(sn), a_.territory(so)) +
                                         countOn(a_.probe(so), b_.territory(sn)) +
                                         countOn(b_.probe(sn), a_.edgeInner[o], b_.sampleTolerance()) +
                                         countOn(a_.probe(so), b_.edgeInner[n], a_.sampleTolerance());
                if (hits > 0)
                    out[o].push_back(n);
            }
        }
        return out;
    }

    const Geo& a_;
    const Geo& b_;
};

// ---------------------------------------------------------------- the fingerprint plan
// Old-shape facts read from a map: the elements of each kind by index, and the faces of each edge and vertex from the
// parents of their imported names (an imported edge or vertex is named from the faces that contain it).
struct MapFacts {
    std::array<std::vector<std::optional<Fingerprint>>, 3> fingerprints;
    std::array<std::vector<std::vector<std::uint32_t>>, 3> faces; // edge, vertex: the faces they lie on
    std::vector<std::set<std::uint32_t>> faceNeighbours;          // faces sharing an edge
};

MapFacts factsOf(const ElementMap& map)
{
    MapFacts out;
    if (!map.impl)
        return out;
    std::array<std::uint32_t, 3> count{};
    for (const ElementMap::Impl::Element& e : map.impl->elements)
        if (e.sub.kind != SubKind::Solid)
            count[static_cast<std::size_t>(e.sub.kind)] =
                std::max(count[static_cast<std::size_t>(e.sub.kind)], e.sub.index + 1);
    for (std::size_t k = 0; k < 3; ++k) {
        out.fingerprints[k].resize(count[k]);
        out.faces[k].resize(count[k]);
    }
    out.faceNeighbours.resize(count[kFace]);
    for (const ElementMap::Impl::Element& e : map.impl->elements)
        if (e.sub.kind != SubKind::Solid)
            out.fingerprints[static_cast<std::size_t>(e.sub.kind)][e.sub.index] = e.fingerprint;
    for (const ElementMap::Impl::Element& e : map.impl->elements) {
        if (e.sub.kind != SubKind::Edge && e.sub.kind != SubKind::Vertex)
            continue;
        std::set<std::uint32_t> faces;
        for (const Hash256& name : e.names) {
            const TopoName* record = map.record(name);
            if (record == nullptr || record->role.rfind("import.", 0) != 0)
                continue;
            for (const Hash256& parent : record->parents)
                if (const auto f = map.find(parent); f && f->kind == SubKind::Face)
                    faces.insert(f->index);
        }
        out.faces[static_cast<std::size_t>(e.sub.kind)][e.sub.index].assign(faces.begin(), faces.end());
        if (e.sub.kind == SubKind::Edge)
            for (const std::uint32_t a : faces)
                for (const std::uint32_t b : faces)
                    if (a != b)
                        out.faceNeighbours[a].insert(b);
    }
    return out;
}

std::vector<std::int64_t> fingerprintPairs(const std::vector<std::optional<Fingerprint>>& a,
                                           const std::vector<std::optional<Fingerprint>>& b,
                                           const std::function<bool(std::uint32_t, std::uint32_t)>& also)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (std::uint32_t o = 0; o < a.size(); ++o)
        for (std::uint32_t n = 0; n < b.size(); ++n)
            if (a[o] && b[n] && a[o]->matches(*b[n]) && (!also || also(o, n)))
                pairs.emplace_back(o, n);
    return oneToOne(pairs, a.size(), b.size());
}

} // namespace

Hash256 rematchMark()
{
    Hasher hasher;
    hasher.str("ray.naming.rematch.own/1");
    return hasher.finish();
}

Result<RematchPlan> planWithShape(const kernel::Shape& oldShape, const kernel::Shape& replacement)
{
    Geo a(oldShape), b(replacement);
    if (auto st = a.read(); !st)
        return tl::unexpected(st.error());
    if (auto st = b.read(); !st)
        return tl::unexpected(st.error());
    a.sample();
    b.sample();
    return Planner(a, b).run();
}

Result<RematchPlan> planByFingerprint(const ElementMap& old, const ElementMap& natural)
{
    const MapFacts a = factsOf(old), b = factsOf(natural);
    RematchPlan plan;
    // Faces: fingerprint, then neighbours (repeated until nothing changes: a face is kept while every neighbour of it
    // has a candidate among the neighbours of its candidate).
    std::vector<std::set<std::uint32_t>> candidates(a.fingerprints[kFace].size());
    for (std::uint32_t o = 0; o < a.fingerprints[kFace].size(); ++o)
        for (std::uint32_t n = 0; n < b.fingerprints[kFace].size(); ++n)
            if (a.fingerprints[kFace][o] && b.fingerprints[kFace][n] &&
                a.fingerprints[kFace][o]->matches(*b.fingerprints[kFace][n]))
                candidates[o].insert(n);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t o = 0; o < candidates.size(); ++o)
            for (auto it = candidates[o].begin(); it != candidates[o].end();) {
                const std::uint32_t n = *it;
                const auto& mine = a.faceNeighbours[o];
                const auto& theirs = b.faceNeighbours[n];
                bool fits = mine.size() == theirs.size();
                for (const std::uint32_t m : mine) {
                    if (!fits)
                        break;
                    fits = std::any_of(theirs.begin(), theirs.end(),
                                       [&](std::uint32_t t) { return candidates[m].count(t) != 0; });
                }
                if (fits) {
                    ++it;
                } else {
                    it = candidates[o].erase(it);
                    changed = true;
                }
            }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> facePairs;
    for (std::uint32_t o = 0; o < candidates.size(); ++o)
        for (const std::uint32_t n : candidates[o])
            facePairs.emplace_back(o, n);
    plan.kinds[kFace].same = oneToOne(facePairs, a.fingerprints[kFace].size(), b.fingerprints[kFace].size());
    const std::vector<std::int64_t>& faceSame = plan.kinds[kFace].same;
    // Edges and vertices: fingerprint, and their faces map onto theirs.
    for (const std::size_t k : {kEdge, kVertex}) {
        plan.kinds[k].same = fingerprintPairs(a.fingerprints[k], b.fingerprints[k], [&](std::uint32_t o, std::uint32_t n) {
            const auto& mine = a.faces[k][o];
            const auto& theirs = b.faces[k][n];
            if (mine.empty() || mine.size() != theirs.size())
                return false;
            std::vector<std::uint32_t> mapped;
            for (const std::uint32_t f : mine) {
                if (faceSame[f] < 0)
                    return false;
                mapped.push_back(static_cast<std::uint32_t>(faceSame[f]));
            }
            std::sort(mapped.begin(), mapped.end());
            return mapped == theirs;
        });
    }
    return plan;
}

Result<RematchResult> assembleRematch(const ElementMap& old, const ElementMap& natural, const RematchPlan& plan,
                                      const FeatureId& feature)
{
    if (!natural.impl)
        return tl::unexpected(Error{ErrorCode::KernelFailure, "The new shape has no names.", {"naming::rematch"}});
    const Hash256 mark = rematchMark();
    // Every old name, and per old name how many old elements hold it.
    std::map<Hash256, std::vector<SubId>> holders;
    if (old.impl)
        for (const ElementMap::Impl::Element& e : old.impl->elements)
            for (const Hash256& name : e.names)
                holders[name].push_back(e.sub);
    auto sameOf = [&](const SubId& s) -> std::int64_t {
        if (s.kind == SubKind::Solid)
            return -1;
        const auto& same = plan.kinds[static_cast<std::size_t>(s.kind)].same;
        return s.index < same.size() ? same[s.index] : -1;
    };
    // A name that several old elements held is carried only if all of them are unchanged.
    auto carriable = [&](const Hash256& name) {
        const auto it = holders.find(name);
        if (it == holders.end())
            return false;
        if (it->second.size() == 1)
            return true;
        return std::all_of(it->second.begin(), it->second.end(), [&](const SubId& s) { return sameOf(s) >= 0; });
    };
    auto oldNames = [&](SubId s) {
        std::vector<Hash256> out;
        for (const Hash256& name : old.namesOf(s))
            if (carriable(name))
                out.push_back(name);
        return out;
    };

    std::map<SubId, std::vector<Hash256>> names; // new element -> names
    std::map<Hash256, TopoName> records;
    std::set<Hash256> carried;      // old names that went anywhere
    std::set<Hash256> carriedExact; // old names on unchanged elements
    RematchResult out;

    auto keepOld = [&](const Hash256& name) {
        if (const TopoName* record = old.record(name))
            records.emplace(name, *record);
        carried.insert(name);
    };
    // A new element's own name, distinct from every old name.
    auto ownName = [&](const SubId& s) -> std::optional<Hash256> {
        const std::span<const Hash256> own = natural.namesOf(s);
        if (own.empty())
            return std::nullopt;
        const TopoName* record = natural.record(own.front());
        if (record == nullptr)
            return std::nullopt;
        TopoName name = *record;
        if (holders.count(name.id) != 0) {
            name.discriminator.push_back(mark);
            std::sort(name.discriminator.begin(), name.discriminator.end());
            name.id = nameId(name);
        }
        records.emplace(name.id, name);
        return name.id;
    };

    for (const std::size_t k : {kFace, kEdge, kVertex}) {
        const KindPlan& kp = plan.kinds[k];
        const SubKind kind = kKinds[k];
        std::set<std::uint32_t> placed;
        for (std::uint32_t o = 0; o < kp.same.size(); ++o) {
            if (kp.same[o] < 0)
                continue;
            const SubId n{kind, static_cast<std::uint32_t>(kp.same[o])};
            for (const Hash256& name : oldNames({kind, o})) {
                keepOld(name);
                carriedExact.insert(name);
                names[n].push_back(name);
            }
            if (!names[n].empty())
                placed.insert(n.index);
        }
        for (const KindPlan::Group& g : kp.groups) {
            std::vector<Hash256> from;
            for (const std::uint32_t o : g.olds)
                for (const Hash256& name : oldNames({kind, o}))
                    from.push_back(name);
            std::sort(from.begin(), from.end());
            from.erase(std::unique(from.begin(), from.end()), from.end());
            if (from.empty())
                continue;
            if (g.news.size() == 1) {
                // Grew, shrank, or several became one: the new element keeps its own name and takes the old ones.
                const SubId n{kind, g.news.front()};
                if (const auto own = ownName(n))
                    names[n].push_back(*own);
                for (const Hash256& name : from) {
                    keepOld(name);
                    names[n].push_back(name);
                }
                placed.insert(n.index);
                std::vector<Hash256> to = names[n];
                std::sort(to.begin(), to.end());
                for (const Hash256& name : from)
                    out.lineage.push_back(LineageEvent{LineageEvent::Kind::Merged, name, to, feature});
                continue;
            }
            // Cut in pieces: each piece is named "split" from every old name, told apart by fingerprint.
            std::vector<std::pair<Fingerprint, std::uint32_t>> pieces;
            for (const std::uint32_t n : g.news)
                pieces.emplace_back(natural.fingerprintOf({kind, n}), n);
            std::sort(pieces.begin(), pieces.end(), [](const auto& l, const auto& r) {
                if (const auto c = ordinalOrder(l.first, r.first); c != 0)
                    return c < 0;
                return l.second < r.second;
            });
            std::vector<Hash256> pieceNames;
            std::uint32_t ordinal = 0;
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                if (i == 0 || ordinalOrder(pieces[i - 1].first, pieces[i].first) != 0)
                    ++ordinal;
                TopoName piece{};
                piece.kind = kind;
                piece.origin = feature;
                piece.role = "split";
                piece.parents = from;
                piece.ordinal = ordinal;
                piece.siblingCount = static_cast<std::uint32_t>(pieces.size());
                piece.family = familyId(kind, feature, piece.role, piece.parents);
                piece.id = nameId(piece);
                records.emplace(piece.id, piece);
                const SubId n{kind, pieces[i].second};
                names[n].push_back(piece.id);
                if (const auto own = ownName(n))
                    names[n].push_back(*own);
                pieceNames.push_back(piece.id);
                placed.insert(n.index);
            }
            std::sort(pieceNames.begin(), pieceNames.end());
            pieceNames.erase(std::unique(pieceNames.begin(), pieceNames.end()), pieceNames.end());
            for (const Hash256& name : from) {
                keepOld(name);
                out.lineage.push_back(LineageEvent{LineageEvent::Kind::Split, name, pieceNames, feature});
            }
        }
        // Everything else: its own name.
        const std::uint32_t count =
            static_cast<std::uint32_t>(std::count_if(natural.impl->elements.begin(), natural.impl->elements.end(),
                                                     [&](const ElementMap::Impl::Element& e) { return e.sub.kind == kind; }));
        for (std::uint32_t n = 0; n < count; ++n) {
            if (placed.count(n) != 0)
                continue;
            if (const auto own = ownName({kind, n}))
                names[{kind, n}].push_back(*own);
        }
    }

    // Deleted: old names that went nowhere.
    for (const auto& [name, subs] : holders) {
        (void)subs;
        if (carried.count(name) == 0)
            out.lineage.push_back(LineageEvent{LineageEvent::Kind::Deleted, name, {}, feature});
        if (carriedExact.count(name) == 0)
            out.unmatched.push_back(name);
    }
    std::sort(out.lineage.begin(), out.lineage.end(), [](const LineageEvent& l, const LineageEvent& r) {
        return l.from != r.from ? l.from < r.from : static_cast<int>(l.kind) < static_cast<int>(r.kind);
    });

    // The table. Every element of the new shape, in SubId order, with its fingerprint; names sorted, smallest first.
    ElementMap::Impl table;
    for (const ElementMap::Impl::Element& e : natural.impl->elements) {
        ElementMap::Impl::Element element{e.sub, e.fingerprint, {}};
        if (const auto it = names.find(e.sub); it != names.end())
            element.names = it->second;
        std::sort(element.names.begin(), element.names.end());
        element.names.erase(std::unique(element.names.begin(), element.names.end()), element.names.end());
        if (element.names.empty())
            return tl::unexpected(
                Error{ErrorCode::Internal, "An element of the new shape got no name.", {"naming::rematch"}});
        table.elements.push_back(std::move(element));
    }
    std::set<Hash256> held;
    for (const ElementMap::Impl::Element& e : table.elements)
        held.insert(e.names.begin(), e.names.end());
    for (auto& [id, record] : records)
        if (held.count(id) != 0)
            table.records.push_back(std::move(record));
    auto map = finishMap(std::move(table), false);
    if (!map)
        return tl::unexpected(map.error());
    out.map = std::move(*map);
    return out;
}

} // namespace detail

namespace {

RematchResult failedRematch(const ElementMap& old, const FeatureId& feature, const ElementMap& natural,
                            const std::string& why)
{
    try {
        ray::log(LogLevel::Warn, "naming", "rematchImported carried no names over: " + why);
    } catch (...) {
    }
    RematchResult out;
    out.map = natural;
    if (old.impl) {
        std::set<Hash256> all;
        for (const ElementMap::Impl::Element& e : old.impl->elements)
            all.insert(e.names.begin(), e.names.end());
        for (const Hash256& name : all) {
            out.lineage.push_back(LineageEvent{LineageEvent::Kind::Deleted, name, {}, feature});
            out.unmatched.push_back(name);
        }
    }
    return out;
}

RematchResult rematch(const ElementMap& old, const kernel::Shape* oldShape, const FeatureId& feature,
                      const kernel::Shape& replacement)
{
    ElementMap natural;
    try {
        natural = detail::nameImported(feature, replacement, detail::VisitOrder{});
        if (!natural.impl)
            return failedRematch(old, feature, natural, "the new shape could not be named");
        auto plan = oldShape != nullptr ? detail::planWithShape(*oldShape, replacement)
                                        : detail::planByFingerprint(old, natural);
        if (!plan)
            return failedRematch(old, feature, natural, plan.error().message);
        auto result = detail::assembleRematch(old, natural, *plan, feature);
        if (!result)
            return failedRematch(old, feature, natural, result.error().message);
        return std::move(*result);
    } catch (const std::bad_alloc&) {
        return failedRematch(old, feature, natural, "out of memory");
    } catch (...) {
        return failedRematch(old, feature, natural, "unexpected failure");
    }
}

} // namespace

RematchResult rematchImported(const ElementMap& old, FeatureId importFeature, const kernel::Shape& replacement)
{
    return rematch(old, nullptr, importFeature, replacement);
}

RematchResult rematchImported(const ElementMap& old, const kernel::Shape& oldShape, FeatureId importFeature,
                              const kernel::Shape& replacement)
{
    return rematch(old, &oldShape, importFeature, replacement);
}

} // namespace ray::naming
