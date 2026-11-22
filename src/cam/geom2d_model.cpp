// Boundaries from the model (task T-050): slices, the silhouette, picked faces, the pocket above a floor and chains of
// picked edges, all through the kernel's public calls (src/cam may not read OCCT). geom2d.h states the rules.
#include "geom2d_internal.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <tuple>
#include <utility>

namespace ray::cam::geom2d {

using namespace detail;

namespace {

constexpr std::string_view kWhere = "cam::geom2d boundaries";

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {std::string(kWhere)}};
}

Vec3 apply(const Xform& f, const Vec3& p)
{
    return Vec3{f.m[0] * p.x + f.m[1] * p.y + f.m[2] * p.z + f.m[3], f.m[4] * p.x + f.m[5] * p.y + f.m[6] * p.z + f.m[7],
                f.m[8] * p.x + f.m[9] * p.y + f.m[10] * p.z + f.m[11]};
}

// An edge's projected curves as one path (open, or closed for a closed curve), in the curve's direction.
struct Piece {
    kernel::SubId edge;
    Path path;
    Coord zMin = 0, zMax = 0;
};
Result<std::optional<Piece>> pieceOf(const kernel::Shape& shape, kernel::SubId edge, const Xform& frame)
{
    using Kind = kernel::ProfileCurve::Kind;
    auto ec = kernel::edgeCurves(shape, edge, frame);
    if (!ec)
        return tl::unexpected(ec.error());
    if (ec->curves.empty())
        return std::optional<Piece>{};
    Piece piece;
    piece.edge = edge;
    piece.zMin = toCoord(ec->zMin);
    piece.zMax = toCoord(ec->zMax);
    if (ec->curves.size() == 1 && ec->curves[0].kind == Kind::Circle) {
        const kernel::ProfileCurve& c = ec->curves[0];
        piece.path = Path::circle(toPt(c.pts[0]), toCoord(c.radius), c.ccw);
        return std::optional<Piece>{std::move(piece)};
    }
    piece.path.start = toPt(ec->curves[0].pts[0]);
    for (const kernel::ProfileCurve& c : ec->curves) {
        if (c.kind == Kind::Line)
            piece.path.spans.push_back(Span{SpanKind::Line, toPt(c.pts[1]), {}, 0});
        else if (c.kind == Kind::Arc)
            piece.path.spans.push_back(Span{c.ccw ? SpanKind::ArcCCW : SpanKind::ArcCW, toPt(c.pts[1]),
                                            toPt(c.pts[2]), toCoord(c.radius)});
        else
            return tl::unexpected(invalid("An edge gave a curve the 2D core cannot read."));
    }
    piece.path = canonicalize(std::move(piece.path));
    if (piece.path.spans.empty())
        return std::optional<Piece>{};
    if (piece.path.spans.back().end == piece.path.start)
        piece.path.closed = true;
    return std::optional<Piece>{std::move(piece)};
}

Pt endOf(const Path& p)
{
    return p.spans.empty() ? p.start : p.spans.back().end;
}

bool near(Pt a, Pt b)
{
    return distance(d2(a), d2(b)) <= static_cast<double>(kJoinTolerance);
}

// Appends `next` (which starts within kJoinTolerance of where `chain` ends) to `chain`.
void append(Path& chain, const Path& next)
{
    for (const Span& s : next.spans)
        chain.spans.push_back(s);
}

D2 endTangent(const Path& p)
{
    const Vec2 t = tangentAt(p, length(p));
    return D2{t.x, t.y};
}
D2 startTangent(const Path& p)
{
    const Vec2 t = tangentAt(p, 0.0);
    return D2{t.x, t.y};
}

} // namespace

Result<Regions> sliceRegions(const kernel::Shape& shape, const Xform& frame, Coord z, JobContext& ctx)
{
    auto loops = kernel::sliceAt(shape, frame, toMm(z), ctx);
    if (!loops)
        return tl::unexpected(loops.error());
    return regionsFromLoops(*loops, ctx);
}

Result<Regions> silhouetteRegions(const kernel::Shape& shape, const Xform& frame, JobContext& ctx)
{
    auto loops = kernel::silhouette(shape, frame, ctx);
    if (!loops)
        return tl::unexpected(loops.error());
    return regionsFromLoops(*loops, ctx);
}

Result<FaceBoundary> faceBoundary(const kernel::Shape& shape, kernel::SubId face, const Xform& frame)
{
    auto fl = kernel::faceLoops(shape, face, frame);
    if (!fl)
        return tl::unexpected(fl.error());
    JobContext ctx;
    auto regions = regionsFromLoops(fl->loops, ctx);
    if (!regions)
        return tl::unexpected(regions.error());
    if (regions->size() != 1)
        return tl::unexpected(invalid("The face's boundary is not one region."));
    FaceBoundary out;
    out.region = std::move(regions->front());
    out.z = toCoord(fl->z);
    out.facesUp = fl->facesUp;
    return out;
}

Result<Pocket> pocketAboveFace(const kernel::Shape& shape, kernel::SubId floorFace, const Xform& frame, JobContext& ctx)
{
    auto floor = faceBoundary(shape, floorFace, frame);
    if (!floor)
        return tl::unexpected(floor.error());
    if (!floor->facesUp)
        return tl::unexpected(invalid("The pocket floor must be a face looking up the setup's Z axis."));
    const auto inside = interiorPoint(floor->region);
    if (!inside)
        return tl::unexpected(invalid("The pocket floor has no area."));
    auto slice = sliceRegions(shape, frame, floor->z, ctx);
    if (!slice)
        return tl::unexpected(slice.error());
    const Path* wall = nullptr;
    double wallArea = 0.0;
    for (const Region& r : *slice)
        for (const Path& h : r.holes)
            if (locate(*inside, h) == Where::Inside) {
                const double a = std::abs(area(h));
                if (wall == nullptr || a < wallArea) {
                    wall = &h;
                    wallArea = a;
                }
            }
    if (wall == nullptr)
        return tl::unexpected(invalid("The floor is not enclosed by walls (an open pocket); pick its boundary edges "
                                      "and mark the open ones instead."));
    Pocket out;
    out.z = floor->z;
    out.region.outer = reverse(*wall);
    for (const Region& r : *slice) {
        Where where = Where::OnBoundary;
        for (const Pt& q : vertices(r.outer)) {
            where = locate(q, *wall);
            if (where != Where::OnBoundary)
                break;
        }
        if (where == Where::Inside)
            out.region.holes.push_back(reverse(r.outer));
    }
    out.region = canonicalize(std::move(out.region));
    return out;
}

Result<std::vector<Chain>> chainEdges(const kernel::Shape& shape, std::span<const kernel::SubId> edges,
                                      const Xform& frame, const ChainOptions& options, JobContext& ctx)
{
    std::vector<kernel::SubId> picked(edges.begin(), edges.end());
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    std::vector<Piece> pieces;
    std::vector<Chain> chains;
    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (auto st = poll(ctx, i, 16); !st)
            return tl::unexpected(st.error());
        if (picked[i].kind != kernel::SubKind::Edge)
            return tl::unexpected(invalid("Only edges can be chained."));
        auto piece = pieceOf(shape, picked[i], frame);
        if (!piece)
            return tl::unexpected(piece.error());
        if (!*piece)
            continue; // along Z
        if ((*piece)->path.closed) {
            Chain c;
            c.path = (*piece)->path;
            c.zMin = (*piece)->zMin;
            c.zMax = (*piece)->zMax;
            c.edges.push_back(picked[i]);
            chains.push_back(std::move(c));
            continue;
        }
        pieces.push_back(std::move(**piece));
    }

    // Nodes: piece ends within kJoinTolerance of each other (union-find over ends sorted by x).
    const std::size_t ends = pieces.size() * 2;
    const auto endPoint = [&](std::size_t e) {
        const Piece& p = pieces[e / 2];
        return e % 2 == 0 ? p.path.start : endOf(p.path);
    };
    std::vector<std::size_t> parent(ends);
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto find = [&](std::size_t e) {
        while (parent[e] != e)
            e = parent[e] = parent[parent[e]];
        return e;
    };
    std::vector<std::size_t> byX(ends);
    std::iota(byX.begin(), byX.end(), std::size_t{0});
    std::sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        return std::make_pair(endPoint(a), a) < std::make_pair(endPoint(b), b);
    });
    for (std::size_t a = 0; a < byX.size(); ++a)
        for (std::size_t b = a + 1; b < byX.size() && endPoint(byX[b]).x - endPoint(byX[a]).x <= kJoinTolerance; ++b)
            if (near(endPoint(byX[a]), endPoint(byX[b]))) {
                const std::size_t ra = find(byX[a]), rb = find(byX[b]);
                if (ra != rb)
                    parent[std::max(ra, rb)] = std::min(ra, rb);
            }
    std::map<std::size_t, std::vector<std::size_t>> atNode; // node -> piece ends, ascending
    for (std::size_t e = 0; e < ends; ++e)
        atNode[find(e)].push_back(e);
    std::vector<bool> used(pieces.size(), false);

    const auto walk = [&](std::size_t startEnd) {
        Chain c;
        std::size_t e = startEnd;
        c.zMin = std::numeric_limits<Coord>::max();
        c.zMax = std::numeric_limits<Coord>::min();
        bool first = true;
        for (;;) {
            const std::size_t pi = e / 2;
            used[pi] = true;
            const Piece& p = pieces[pi];
            const Path oriented = e % 2 == 0 ? p.path : reverse(p.path);
            if (first) {
                c.path = oriented;
                c.path.closed = false;
                first = false;
            } else {
                append(c.path, oriented);
            }
            c.edges.push_back(p.edge);
            c.zMin = std::min(c.zMin, p.zMin);
            c.zMax = std::max(c.zMax, p.zMax);
            const std::size_t other = e ^ 1u;
            const std::vector<std::size_t>& node = atNode[find(other)];
            if (node.size() != 2)
                break;
            const std::size_t next = node[0] == other ? node[1] : node[0];
            if (used[next / 2]) {
                if (next == startEnd) {
                    c.path.closed = true;
                    c.path.spans.back().end = c.path.start;
                }
                break;
            }
            e = next;
        }
        return c;
    };
    // Open chains from nodes that are not a simple joint, in node order; then the cycles.
    for (const auto& [node, list] : atNode) {
        if (list.size() == 2)
            continue;
        for (const std::size_t e : list)
            if (!used[e / 2])
                chains.push_back(walk(e));
    }
    for (std::size_t pi = 0; pi < pieces.size(); ++pi)
        if (!used[pi])
            chains.push_back(walk(pi * 2));

    // Tangent extension of open ends through the shape's other edges.
    if (options.tangentExtend) {
        std::vector<kernel::SubId> taken = picked;
        for (const Chain& c : chains)
            taken.insert(taken.end(), c.edges.begin(), c.edges.end());
        std::sort(taken.begin(), taken.end());
        const double cosLimit = dmath::cos(options.tangentAngleDeg * kPi / 180.0);
        for (Chain& c : chains) {
            for (int end = 0; end < 2 && !c.path.closed; ++end) {
                if (end == 1) {
                    c.path = reverse(c.path);
                    std::reverse(c.edges.begin(), c.edges.end());
                }
                for (std::size_t guard = 0; guard < shape.count(kernel::SubKind::Edge) && !c.path.closed; ++guard) {
                    if (auto st = ctx.cancel.check(); !st)
                        return tl::unexpected(st.error());
                    const Pt tip = endOf(c.path);
                    // The vertex of the last edge at the chain's end.
                    std::optional<kernel::SubId> vertex;
                    double best = 0.0;
                    for (const kernel::SubId& v : kernel::adjacent(shape, c.edges.back(), kernel::SubKind::Vertex)) {
                        auto pt = kernel::vertexPoint(shape, v);
                        if (!pt)
                            continue;
                        const Vec3 q = apply(frame, *pt);
                        const double d = distance(d2(toPt(Vec2{q.x, q.y})), d2(tip));
                        if (!vertex || d < best) {
                            vertex = v;
                            best = d;
                        }
                    }
                    if (!vertex || best > static_cast<double>(kJoinTolerance))
                        break;
                    const D2 t = endTangent(c.path);
                    std::vector<Path> candidates;
                    std::vector<kernel::SubId> candidateEdges;
                    std::vector<Piece> candidatePieces;
                    for (const kernel::SubId& e : kernel::adjacent(shape, *vertex, kernel::SubKind::Edge)) {
                        if (std::binary_search(taken.begin(), taken.end(), e))
                            continue;
                        auto piece = pieceOf(shape, e, frame);
                        if (!piece)
                            return tl::unexpected(piece.error());
                        if (!*piece || (*piece)->path.closed)
                            continue;
                        Path oriented;
                        if (near((*piece)->path.start, tip))
                            oriented = (*piece)->path;
                        else if (near(endOf((*piece)->path), tip))
                            oriented = reverse((*piece)->path);
                        else
                            continue;
                        if (dot(t, startTangent(oriented)) < cosLimit)
                            continue;
                        candidates.push_back(std::move(oriented));
                        candidateEdges.push_back(e);
                        candidatePieces.push_back(std::move(**piece));
                    }
                    if (candidates.size() != 1)
                        break; // none, or a branch: the extension stops
                    append(c.path, candidates[0]);
                    c.edges.push_back(candidateEdges[0]);
                    c.zMin = std::min(c.zMin, candidatePieces[0].zMin);
                    c.zMax = std::max(c.zMax, candidatePieces[0].zMax);
                    taken.insert(std::upper_bound(taken.begin(), taken.end(), candidateEdges[0]), candidateEdges[0]);
                    if (near(endOf(c.path), c.path.start)) {
                        c.path.closed = true;
                        c.path.spans.back().end = c.path.start;
                    }
                }
            }
        }
    }

    // Direction and order.
    for (Chain& c : chains) {
        if (c.path.closed) {
            if (area(c.path) < 0.0) {
                c.path = reverse(c.path);
                std::reverse(c.edges.begin(), c.edges.end());
            }
            // Start at the lowest vertex; the edge list is rotated with it where the start moves to an edge's end.
            c.path = canonicalize(std::move(c.path));
        } else if (endOf(c.path) < c.path.start) {
            c.path = reverse(c.path);
            std::reverse(c.edges.begin(), c.edges.end());
        }
    }
    std::stable_sort(chains.begin(), chains.end(), [](const Chain& a, const Chain& b) {
        return std::make_tuple(a.path.start, endOf(a.path), a.path.spans.size()) <
               std::make_tuple(b.path.start, endOf(b.path), b.path.spans.size());
    });
    return chains;
}

} // namespace ray::cam::geom2d
