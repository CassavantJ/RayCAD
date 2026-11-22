// The CPU side of toolpath backplots and of the tool glyph (task T-038). See lines.h.
#include "lines.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace ray::render::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

Status invalid(std::string what)
{
    return Status(tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(what), {}}));
}

// cam::MoveKind::Rapid and the cam::MoveRole values, as numbers (the shader sees bytes).
constexpr std::uint8_t kRapid = 0;
constexpr std::uint8_t kRoleCut = 0, kRolePlunge = 1, kRoleRamp = 2, kRoleLeadOut = 4;
static_assert(std::uint8_t(cam::MoveKind::Rapid) == kRapid);
static_assert(std::uint8_t(cam::MoveRole::Cut) == kRoleCut && std::uint8_t(cam::MoveRole::Plunge) == kRolePlunge &&
                  std::uint8_t(cam::MoveRole::Ramp) == kRoleRamp && std::uint8_t(cam::MoveRole::LeadIn) == 3 &&
                  std::uint8_t(cam::MoveRole::LeadOut) == kRoleLeadOut && std::uint8_t(cam::MoveRole::Link) == 5 &&
                  std::uint8_t(cam::MoveRole::Retract) == 6 && std::uint8_t(cam::MoveRole::Approach) == 7,
              "path.vert decodes the roles by number");

} // namespace

PathSlot pathSlot(std::uint8_t role, std::uint8_t kind)
{
    if (kind == kRapid)
        return PathSlot::Rapid;
    switch (role) {
    case kRoleCut:
        return PathSlot::Cut;
    case kRolePlunge:
        return PathSlot::Plunge;
    case kRoleRamp:
        return PathSlot::Ramp;
    case 3:
    case kRoleLeadOut:
        return PathSlot::Lead;
    default:
        return PathSlot::Link; // Link, Retract, Approach (no tokens of their own), and anything unknown
    }
}

Token pathToken(PathSlot slot)
{
    switch (slot) {
    case PathSlot::Cut:
        return Token::PathCut;
    case PathSlot::Rapid:
        return Token::PathRapid;
    case PathSlot::Plunge:
        return Token::PathPlunge;
    case PathSlot::Ramp:
        return Token::PathRamp;
    case PathSlot::Lead:
        return Token::PathLead;
    case PathSlot::Link:
    case PathSlot::Count:
        break;
    }
    return Token::PathLink;
}

bool pathDashed(std::uint8_t kind)
{
    return kind == kRapid;
}

Status validateBackplot(const sim::Backplot& bp)
{
    const std::vector<std::uint32_t>& mfv = bp.moveFirstVertex;
    if (mfv.empty())
        return invalid("backplot cannot be displayed: moveFirstVertex is empty");
    if (mfv.back() != bp.lineVertices.size())
        return invalid("backplot cannot be displayed: the last moveFirstVertex entry is not the vertex count");
    if (mfv.size() > 1 && mfv.front() < 1)
        return invalid("backplot cannot be displayed: vertex 0 belongs to a move");
    for (std::size_t m = 1; m < mfv.size(); ++m) {
        if (mfv[m] < mfv[m - 1])
            return invalid("backplot cannot be displayed: moveFirstVertex decreases");
    }
    for (const sim::BackplotVertex& v : bp.lineVertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return invalid("backplot cannot be displayed: a vertex is not finite");
    }
    return {};
}

std::uint64_t backplotSegmentCount(const sim::Backplot& bp)
{
    return bp.lineVertices.size() > 1 ? bp.lineVertices.size() - 1 : 0;
}

std::optional<Box3> backplotBounds(const sim::Backplot& bp)
{
    if (bp.lineVertices.empty())
        return std::nullopt;
    const Box3& b = bp.bounds;
    if (b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z)
        return b;
    constexpr double inf = std::numeric_limits<double>::infinity();
    Box3 box{Vec3{inf, inf, inf}, Vec3{-inf, -inf, -inf}};
    for (const sim::BackplotVertex& v : bp.lineVertices) {
        box.min = {std::min(box.min.x, double(v.x)), std::min(box.min.y, double(v.y)),
                   std::min(box.min.z, double(v.z))};
        box.max = {std::max(box.max.x, double(v.x)), std::max(box.max.y, double(v.y)),
                   std::max(box.max.z, double(v.z))};
    }
    return box;
}

SegmentRange backplotMoveSegments(const sim::Backplot& bp, std::uint32_t firstMove, std::uint32_t lastMove)
{
    const std::vector<std::uint32_t>& mfv = bp.moveFirstVertex;
    const std::uint64_t segments = backplotSegmentCount(bp);
    if (mfv.size() < 2 || segments == 0)
        return {};
    const std::uint64_t moves = mfv.size() - 1;
    if (firstMove >= moves || firstMove > lastMove)
        return {};
    const std::uint64_t last = std::min<std::uint64_t>(lastMove, moves - 1);
    SegmentRange r;
    r.first = std::min<std::uint64_t>(mfv[firstMove] > 0 ? mfv[firstMove] - 1 : 0, segments);
    r.end = std::min<std::uint64_t>(mfv[last + 1] > 0 ? mfv[last + 1] - 1 : 0, segments);
    return r;
}

std::uint32_t backplotStride(std::uint64_t segments, int maxSegments)
{
    if (maxSegments <= 0 || segments <= std::uint64_t(maxSegments))
        return 1;
    const std::uint64_t k = (segments + std::uint64_t(maxSegments) - 1) / std::uint64_t(maxSegments);
    return std::uint32_t(std::min<std::uint64_t>(k, std::numeric_limits<std::uint32_t>::max()));
}

std::uint64_t strideSegmentCount(std::uint64_t segments, std::uint32_t stride)
{
    const std::uint64_t k = std::max<std::uint32_t>(stride, 1);
    return (segments + k - 1) / k;
}

SegmentRange strideRange(SegmentRange original, std::uint32_t stride, std::uint64_t strideSegments)
{
    if (original.empty())
        return {};
    const std::uint64_t k = std::max<std::uint32_t>(stride, 1);
    SegmentRange r;
    r.first = std::min(original.first / k, strideSegments);
    r.end = std::min((original.end + k - 1) / k, strideSegments);
    return r;
}

std::vector<sim::BackplotVertex> decimateBackplot(const sim::Backplot& bp, std::uint32_t stride)
{
    const std::vector<sim::BackplotVertex>& v = bp.lineVertices;
    if (stride <= 1 || v.size() < 2)
        return v;
    const std::uint64_t segments = strideSegmentCount(v.size() - 1, stride);
    std::vector<sim::BackplotVertex> out;
    out.reserve(std::size_t(segments) + 1);
    for (std::uint64_t j = 0; j < segments; ++j)
        out.push_back(v[std::size_t(j * stride)]);
    out.push_back(v.back());
    return out;
}

namespace {

// Squared distance of p from segment a-b.
double distance2(const sim::BackplotVertex& p, const sim::BackplotVertex& a, const sim::BackplotVertex& b)
{
    const double abx = double(b.x) - a.x, aby = double(b.y) - a.y, abz = double(b.z) - a.z;
    const double apx = double(p.x) - a.x, apy = double(p.y) - a.y, apz = double(p.z) - a.z;
    const double len2 = abx * abx + aby * aby + abz * abz;
    double t = len2 > 0 ? (apx * abx + apy * aby + apz * abz) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = apx - t * abx, dy = apy - t * aby, dz = apz - t * abz;
    return dx * dx + dy * dy + dz * dz;
}

PathSlot slotOf(const sim::BackplotVertex& v)
{
    return pathSlot(v.role, v.kind);
}

// One level from the strip `in` (whose vertices sit at `inSource` in the full path) at tolerance `tol`.
PathLod simplify(const std::vector<sim::BackplotVertex>& in, const std::vector<std::uint32_t>& inSource, double tol)
{
    PathLod out;
    const std::size_t n = in.size();
    out.vertices.reserve(n / 2 + 2);
    out.source.reserve(n / 2 + 2);
    const double tol2 = tol * tol;
    std::size_t anchor = 0;
    out.vertices.push_back(in[0]);
    out.source.push_back(inSource[0]);
    while (anchor + 1 < n) {
        // Extend the chord anchor -> end while every vertex between stays within tol and the slot stays the same.
        std::size_t end = anchor + 1;
        const PathSlot slot = slotOf(in[end]);
        while (end + 1 < n && end + 1 - anchor <= kLodMaxRun && slotOf(in[end + 1]) == slot) {
            const std::size_t candidate = end + 1;
            bool fits = true;
            for (std::size_t i = anchor + 1; i < candidate && fits; ++i)
                fits = distance2(in[i], in[anchor], in[candidate]) <= tol2;
            if (!fits)
                break;
            end = candidate;
        }
        out.vertices.push_back(in[end]);
        out.source.push_back(inSource[end]);
        anchor = end;
    }
    return out;
}

} // namespace

std::vector<PathLod> buildPathLods(const sim::Backplot& bp)
{
    std::vector<PathLod> levels;
    const std::size_t n = bp.lineVertices.size();
    if (n < kLodMinVertices)
        return levels;
    // Tolerances from 2 um up, doubling, to 1 % of the path's size; each level is built from the one below, so its
    // distance to the full path is at most the sum of the tolerances on the way. A level is kept only when it has at
    // most 80 % of the vertices of the last one kept, and building stops below kLodMinVertices / 4 vertices.
    const std::optional<Box3> box = backplotBounds(bp);
    const double dx = box->max.x - box->min.x, dy = box->max.y - box->min.y, dz = box->max.z - box->min.z;
    const double largest = 0.01 * std::sqrt(dx * dx + dy * dy + dz * dz);
    std::vector<std::uint32_t> identity(n);
    for (std::size_t i = 0; i < n; ++i)
        identity[i] = std::uint32_t(i);
    const std::vector<sim::BackplotVertex>* in = &bp.lineVertices;
    const std::vector<std::uint32_t>* inSource = &identity;
    PathLod current;
    double sum = 0;
    std::size_t lastKept = n;
    for (double tol = 0.002; tol <= largest && in->size() >= kLodMinVertices / 4; tol *= 2) {
        PathLod next = simplify(*in, *inSource, tol);
        sum += tol;
        next.toleranceMm = sum;
        current = std::move(next);
        in = &current.vertices;
        inSource = &current.source;
        if (current.vertices.size() * 5 <= lastKept * 4) {
            lastKept = current.vertices.size();
            levels.push_back(current);
        }
    }
    return levels;
}

int pathLodLevel(const std::vector<PathLod>& lods, double pxPerMm)
{
    int level = 0;
    if (!(pxPerMm > 0) || !std::isfinite(pxPerMm))
        return 0;
    for (std::size_t j = 0; j < lods.size(); ++j) {
        if (lods[j].toleranceMm * pxPerMm <= kLodErrorPx)
            level = int(j) + 1;
    }
    return level;
}

LodPieces lodPieces(const PathLod& lod, SegmentRange original)
{
    LodPieces p;
    if (original.empty())
        return p;
    const std::vector<std::uint32_t>& src = lod.source;
    // First kept vertex at or after the range's first vertex, last kept vertex at or before its last one.
    const auto q0 = std::lower_bound(src.begin(), src.end(), original.first);
    const auto after = std::upper_bound(src.begin(), src.end(), original.end);
    if (q0 == src.end() || after == src.begin() || std::prev(after) <= q0) {
        p.head = original; // no whole level segment inside: all of it from the full path
        return p;
    }
    const auto q1 = std::prev(after);
    p.head = SegmentRange{original.first, *q0};
    p.level = SegmentRange{std::uint64_t(q0 - src.begin()), std::uint64_t(q1 - src.begin())};
    p.tail = SegmentRange{*q1, original.end};
    return p;
}

Result<kernel::Mesh> revolveProfile(std::span<const Vec2> profile, int sides)
{
    sides = std::max(sides, 3);
    std::vector<Vec2> p;
    p.reserve(profile.size());
    for (const Vec2& q : profile) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y))
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "tool glyph: a profile point is not finite", {}});
        if (q.x < 0)
            return tl::unexpected(Error{ErrorCode::InvalidArgument, "tool glyph: a profile radius is negative", {}});
        if (p.empty() || p.back().x != q.x || p.back().y != q.y)
            p.push_back(q);
    }
    if (p.size() < 2)
        return tl::unexpected(
            Error{ErrorCode::InvalidArgument, "tool glyph: the profile needs two distinct points", {}});

    // Outward normal of each segment in the (radius, height) plane: right of the direction of travel, for a profile
    // that runs from the tip up with the material towards the axis.
    const std::size_t segs = p.size() - 1;
    std::vector<Vec2> n(segs);
    for (std::size_t i = 0; i < segs; ++i) {
        const double dr = p[i + 1].x - p[i].x, dz = p[i + 1].y - p[i].y;
        const double len = std::sqrt(dr * dr + dz * dz);
        n[i] = Vec2{dz / len, -dr / len};
    }
    // Normal at both ends of every band: shared with the neighbour where the profile bends less than the crease angle.
    const double creaseCos = std::cos(kGlyphCreaseDeg * kPi / 180.0);
    const auto blend = [&](std::size_t a, std::size_t b) {
        if (n[a].x * n[b].x + n[a].y * n[b].y < creaseCos)
            return std::pair<Vec2, Vec2>{n[a], n[b]};
        const double x = n[a].x + n[b].x, y = n[a].y + n[b].y;
        const double len = std::sqrt(x * x + y * y);
        const Vec2 m{x / len, y / len};
        return std::pair<Vec2, Vec2>{m, m};
    };
    std::vector<Vec2> startNormal(n), endNormal(n);
    for (std::size_t i = 0; i + 1 < segs; ++i) {
        const auto [endOfA, startOfB] = blend(i, i + 1);
        endNormal[i] = endOfA;
        startNormal[i + 1] = startOfB;
    }

    kernel::Mesh mesh;
    const std::size_t sideCount = static_cast<std::size_t>(sides);
    std::vector<double> cosA(sideCount), sinA(sideCount);
    for (int j = 0; j < sides; ++j) {
        const double a = 2.0 * kPi * j / sides;
        cosA[std::size_t(j)] = std::cos(a);
        sinA[std::size_t(j)] = std::sin(a);
    }
    const auto vertex = [&](double r, double z, Vec2 normal, int j) {
        const std::size_t k = std::size_t(j % sides);
        mesh.positions.push_back(float(r * cosA[k]));
        mesh.positions.push_back(float(r * sinA[k]));
        mesh.positions.push_back(float(z));
        mesh.normals.push_back(float(normal.x * cosA[k]));
        mesh.normals.push_back(float(normal.x * sinA[k]));
        mesh.normals.push_back(float(normal.y));
        return std::uint32_t(mesh.positions.size() / 3 - 1);
    };
    for (std::size_t i = 0; i < segs; ++i) {
        const std::uint32_t base = std::uint32_t(mesh.positions.size() / 3);
        for (int j = 0; j < sides; ++j) {
            vertex(p[i].x, p[i].y, startNormal[i], j);
            vertex(p[i + 1].x, p[i + 1].y, endNormal[i], j);
        }
        for (int j = 0; j < sides; ++j) {
            const std::uint32_t a0 = base + std::uint32_t(2 * j), a1 = a0 + 1;
            const std::uint32_t b0 = base + std::uint32_t(2 * ((j + 1) % sides)), b1 = b0 + 1;
            if (p[i].x > 0) // the triangle with an edge on the start ring; degenerate on the axis
                mesh.indices.insert(mesh.indices.end(), {a0, b0, a1});
            if (p[i + 1].x > 0)
                mesh.indices.insert(mesh.indices.end(), {a1, b0, b1});
        }
    }
    // Flat discs on open ends.
    const auto disc = [&](const Vec2& at, double nz) {
        if (at.x <= 0)
            return;
        const std::uint32_t centre = vertex(0, at.y, Vec2{0, nz}, 0);
        const std::uint32_t first = std::uint32_t(mesh.positions.size() / 3);
        for (int j = 0; j < sides; ++j)
            vertex(at.x, at.y, Vec2{0, nz}, j);
        for (int j = 0; j < sides; ++j) {
            const std::uint32_t a = first + std::uint32_t(j), b = first + std::uint32_t((j + 1) % sides);
            if (nz > 0)
                mesh.indices.insert(mesh.indices.end(), {centre, a, b});
            else
                mesh.indices.insert(mesh.indices.end(), {centre, b, a});
        }
    };
    disc(p.front(), -1.0);
    disc(p.back(), 1.0);
    mesh.faces.push_back(
        kernel::FaceRange{kernel::SubId{kernel::SubKind::Face, 0}, 0, std::uint32_t(mesh.indices.size())});
    return mesh;
}

} // namespace ray::render::detail
