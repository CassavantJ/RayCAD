// The rematch oracle (task T-084); the rules are in rematch_oracle.h. Kernel queries and plain geometry only.
#include "rematch_oracle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace ray::rematch_oracle {

namespace {

Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 mul(const Vec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 d = sub(b, a);
    const double l2 = dot(d, d);
    double t = l2 > 0 ? dot(sub(p, a), d) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return norm(sub(p, add(a, mul(d, t))));
}

// Distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
double triangleDistance(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return norm(ap);
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return norm(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return norm(sub(p, add(a, mul(ab, d1 / (d1 - d3)))));
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return norm(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return norm(sub(p, add(a, mul(ac, d2 / (d2 - d6)))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return norm(sub(p, add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))))));
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    return norm(sub(p, add(a, add(mul(ab, v), mul(ac, w)))));
}

struct Box {
    Vec3 lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max()};
    Vec3 hi{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
            -std::numeric_limits<double>::max()};
    void grow(const Vec3& p)
    {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    bool near(const Vec3& p, double tol) const
    {
        return p.x >= lo.x - tol && p.x <= hi.x + tol && p.y >= lo.y - tol && p.y <= hi.y + tol && p.z >= lo.z - tol &&
               p.z <= hi.z + tol;
    }
    bool meets(const Box& o, double tol) const
    {
        return lo.x <= o.hi.x + tol && o.lo.x <= hi.x + tol && lo.y <= o.hi.y + tol && o.lo.y <= hi.y + tol &&
               lo.z <= o.hi.z + tol && o.lo.z <= hi.z + tol;
    }
};

struct Element {
    Box box;
    std::vector<Vec3> nodes;                        // faces: mesh nodes; edges: polyline; vertices: the point
    std::vector<std::array<Vec3, 3>> triangles;     // faces
    std::vector<Box> triangleBoxes;                 // faces
    std::vector<Vec3> interior;                     // samples away from the boundary
    int surface = -1;                               // faces: kernel::SurfaceType; edges: kernel::CurveType
};

// At most n points, spread.
std::vector<Vec3> thin(const std::vector<Vec3>& points, std::size_t n)
{
    if (points.size() <= n)
        return points;
    std::vector<Vec3> out;
    const std::size_t step = (points.size() + n - 1) / n;
    for (std::size_t i = 0; i < points.size(); i += step)
        out.push_back(points[i]);
    return out;
}

} // namespace

struct Samples::Impl {
    kernel::Shape shape;
    bool ok = false;
    std::array<std::vector<Element>, 3> elements; // vertex, edge, face

    const Element& at(SubId s) const { return elements[static_cast<std::size_t>(s.kind)][s.index]; }

    bool on(SubId s, const Vec3& p, double tol) const
    {
        const Element& e = at(s);
        if (!e.box.near(p, tol))
            return false;
        switch (s.kind) {
        case SubKind::Vertex:
            return !e.nodes.empty() && norm(sub(p, e.nodes.front())) <= tol;
        case SubKind::Edge:
            if (e.nodes.size() == 1)
                return norm(sub(p, e.nodes.front())) <= tol;
            for (std::size_t i = 1; i < e.nodes.size(); ++i)
                if (segmentDistance(p, e.nodes[i - 1], e.nodes[i]) <= tol)
                    return true;
            return false;
        case SubKind::Face:
            for (std::size_t i = 0; i < e.triangles.size(); ++i)
                if (e.triangleBoxes[i].near(p, tol) &&
                    triangleDistance(p, e.triangles[i][0], e.triangles[i][1], e.triangles[i][2]) <= tol)
                    return true;
            return false;
        default:
            return false;
        }
    }
};

Samples::Samples(const kernel::Shape& shape) : impl_(std::make_unique<Impl>())
{
    Impl& m = *impl_;
    m.shape = shape;
    const std::uint32_t nv = shape.count(SubKind::Vertex), ne = shape.count(SubKind::Edge),
                        nf = shape.count(SubKind::Face);
    m.elements[0].resize(nv);
    m.elements[1].resize(ne);
    m.elements[2].resize(nf);
    for (std::uint32_t i = 0; i < nv; ++i)
        if (const auto p = kernel::vertexPoint(shape, {SubKind::Vertex, i})) {
            m.elements[0][i].nodes = {*p};
            m.elements[0][i].interior = {*p};
            m.elements[0][i].box.grow(*p);
        }
    JobContext ctx;
    const auto mesh = kernel::tessellate(shape, kernel::MeshOptions{kChord, 0.1}, ctx);
    if (!mesh)
        return;
    m.ok = true;
    for (const kernel::EdgeRange& r : mesh->edges) {
        if (r.edge.kind != SubKind::Edge || r.edge.index >= ne)
            continue;
        Element& e = m.elements[1][r.edge.index];
        for (std::uint32_t k = 0; k < r.vertexCount; ++k) {
            const std::size_t j = 3 * (static_cast<std::size_t>(r.firstVertex) + k);
            const Vec3 p{mesh->edgePositions[j], mesh->edgePositions[j + 1], mesh->edgePositions[j + 2]};
            e.nodes.push_back(p);
            e.box.grow(p);
        }
    }
    for (std::uint32_t i = 0; i < ne; ++i) {
        Element& e = m.elements[1][i];
        const auto info = kernel::curveInfo(shape, {SubKind::Edge, i});
        if (info)
            e.surface = static_cast<int>(info->type);
        // An edge the mesher drew no polyline for (a degenerate edge at a pole or apex): its end points.
        if (e.nodes.empty() && info) {
            e.nodes = {info->start, info->end};
            e.box.grow(info->start);
            e.box.grow(info->end);
        }
        // Points along every segment (a straight edge's polyline is only its two ends), at most 0.5 mm apart.
        std::vector<Vec3> dense;
        for (std::size_t k = 0; k + 1 < e.nodes.size(); ++k) {
            const double l = norm(sub(e.nodes[k + 1], e.nodes[k]));
            const int n = std::clamp(static_cast<int>(std::ceil(l / 0.5)), 1, 200);
            for (int j = 0; j < n; ++j)
                dense.push_back(add(e.nodes[k], mul(sub(e.nodes[k + 1], e.nodes[k]), static_cast<double>(j) / n)));
        }
        if (!e.nodes.empty())
            dense.push_back(e.nodes.back());
        e.nodes = std::move(dense);
        double length = 0;
        std::vector<double> along{0};
        for (std::size_t k = 1; k < e.nodes.size(); ++k) {
            length += norm(sub(e.nodes[k], e.nodes[k - 1]));
            along.push_back(length);
        }
        for (std::size_t k = 0; k < e.nodes.size(); ++k)
            if (along[k] >= kMargin && length - along[k] >= kMargin)
                e.interior.push_back(e.nodes[k]);
        e.interior = thin(e.interior, 64);
    }
    auto at = [&](std::uint32_t n) {
        return Vec3{mesh->positions[3 * n], mesh->positions[3 * n + 1], mesh->positions[3 * n + 2]};
    };
    for (const kernel::FaceRange& r : mesh->faces) {
        if (r.face.kind != SubKind::Face || r.face.index >= nf)
            continue;
        Element& f = m.elements[2][r.face.index];
        std::set<std::uint32_t> seen;
        for (std::uint32_t t = 0; t + 2 < r.indexCount; t += 3) {
            const std::size_t base = static_cast<std::size_t>(r.firstIndex) + t;
            const std::array<std::uint32_t, 3> idx{mesh->indices[base], mesh->indices[base + 1],
                                                   mesh->indices[base + 2]};
            std::array<Vec3, 3> tri{at(idx[0]), at(idx[1]), at(idx[2])};
            Box b;
            for (const Vec3& p : tri) {
                b.grow(p);
                f.box.grow(p);
            }
            f.triangles.push_back(tri);
            f.triangleBoxes.push_back(b);
            for (const std::uint32_t n : idx)
                if (seen.insert(n).second)
                    f.nodes.push_back(at(n));
            f.interior.push_back(mul(add(add(tri[0], tri[1]), tri[2]), 1.0 / 3.0));
        }
    }
    for (std::uint32_t i = 0; i < nf; ++i) {
        Element& f = m.elements[2][i];
        if (const auto info = kernel::surfaceInfo(shape, {SubKind::Face, i}))
            f.surface = static_cast<int>(info->type);
        // Interior: nodes and triangle centres at least kMargin from every edge of the face.
        std::vector<const Element*> boundary;
        for (const SubId& e : kernel::adjacent(shape, {SubKind::Face, i}, SubKind::Edge))
            boundary.push_back(&m.elements[1][e.index]);
        std::vector<Vec3> candidates = f.interior;
        candidates.insert(candidates.end(), f.nodes.begin(), f.nodes.end());
        candidates = thin(candidates, 400);
        f.interior.clear();
        for (const Vec3& p : candidates) {
            bool inside = true;
            for (const Element* e : boundary) {
                for (std::size_t k = 1; k < e->nodes.size() && inside; ++k)
                    inside = segmentDistance(p, e->nodes[k - 1], e->nodes[k]) >= kMargin;
                if (!inside)
                    break;
            }
            if (inside)
                f.interior.push_back(p);
        }
        f.interior = thin(f.interior, 200);
        f.nodes = thin(f.nodes, 300);
    }
}

Samples::~Samples() = default;

const kernel::Shape& Samples::shape() const
{
    return impl_->shape;
}
bool Samples::ok() const
{
    return impl_->ok;
}
std::uint32_t Samples::count(SubKind kind) const
{
    return static_cast<std::uint32_t>(impl_->elements[static_cast<std::size_t>(kind)].size());
}

bool Samples::on(SubId element, const Vec3& p, double tol) const
{
    return impl_->on(element, p, tol);
}

bool Samples::identical(SubId mine, const Samples& other, SubId theirs) const
{
    if (mine.kind != theirs.kind)
        return false;
    const Element& a = impl_->at(mine);
    const Element& b = other.impl_->at(theirs);
    if (a.nodes.empty() || b.nodes.empty() || a.surface != b.surface)
        return false;
    if (mine.kind == SubKind::Face && (a.triangles.empty() || b.triangles.empty()))
        return false;
    if (!a.box.meets(b.box, kTolerance))
        return false;
    for (const Vec3& p : a.nodes)
        if (!other.on(theirs, p))
            return false;
    for (const Vec3& p : b.nodes)
        if (!on(mine, p))
            return false;
    // A flat face's mesh has nodes on its boundary only: a hole that came or went in its middle shows in its interior
    // samples (triangle centres), each of which must lie on the other face as well.
    for (const Vec3& p : a.interior)
        if (!other.on(theirs, p))
            return false;
    for (const Vec3& p : b.interior)
        if (!on(mine, p))
            return false;
    return true;
}

double Samples::shareOn(SubId mine, const Samples& other, SubId theirs) const
{
    const Element& a = impl_->at(mine);
    if (a.interior.empty() || mine.kind != theirs.kind)
        return 0;
    std::size_t hits = 0;
    for (const Vec3& p : a.interior)
        if (other.on(theirs, p))
            ++hits;
    return static_cast<double>(hits) / static_cast<double>(a.interior.size());
}

bool Samples::sharesGround(SubId mine, const Samples& other, SubId theirs) const
{
    if (mine.kind != theirs.kind)
        return false;
    if (!impl_->at(mine).box.meets(other.impl_->at(theirs).box, kTolerance))
        return false;
    return shareOn(mine, other, theirs) > 0 || other.shareOn(theirs, *this, mine) > 0;
}

std::vector<SubId> Samples::identicalIn(SubId mine, const Samples& other) const
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < other.count(mine.kind); ++i)
        if (identical(mine, other, {mine.kind, i}))
            out.push_back({mine.kind, i});
    return out;
}

std::vector<SubId> Samples::sharingIn(SubId mine, const Samples& other) const
{
    std::vector<SubId> out;
    for (std::uint32_t i = 0; i < other.count(mine.kind); ++i)
        if (sharesGround(mine, other, {mine.kind, i}))
            out.push_back({mine.kind, i});
    return out;
}

std::optional<Vec3> Samples::interiorPoint(SubId face, double clearance) const
{
    const Element& f = impl_->at(face);
    std::vector<const Element*> boundary;
    for (const SubId& e : kernel::adjacent(impl_->shape, face, SubKind::Edge))
        boundary.push_back(&impl_->at(e));
    std::optional<Vec3> best;
    double bestDistance = clearance;
    for (const Vec3& p : f.interior) {
        double d = std::numeric_limits<double>::max();
        for (const Element* e : boundary)
            for (std::size_t k = 1; k < e->nodes.size(); ++k)
                d = std::min(d, segmentDistance(p, e->nodes[k - 1], e->nodes[k]));
        if (d >= bestDistance) {
            bestDistance = d;
            best = p;
        }
    }
    return best;
}

Judgement judge(const Samples& before, SubId pick, const Samples& after, State state,
                const std::vector<SubId>& answer)
{
    auto name = [](SubId s) {
        const char* k = s.kind == SubKind::Face ? "face " : (s.kind == SubKind::Edge ? "edge " : "vertex ");
        return std::string(k) + std::to_string(s.index);
    };
    switch (state) {
    case State::Exact:
    case State::Repaired:
        if (answer.size() == 1 && before.identical(pick, after, answer.front()))
            return {state == State::Exact ? Verdict::Matched : Verdict::Flagged, {}};
        return {Verdict::Wrong, std::string(state == State::Exact ? "Exact" : "Repaired") + " to " +
                                    (answer.empty() ? std::string("nothing") : name(answer.front())) +
                                    ", which is not the same geometry as the picked " + name(pick)};
    case State::Split:
    case State::Merged: {
        for (const SubId& a : answer)
            if (!before.sharesGround(pick, after, a))
                return {Verdict::Wrong, "answer " + name(a) + " shares no ground with the picked " + name(pick)};
        for (const SubId& e : before.sharingIn(pick, after))
            if (std::find(answer.begin(), answer.end(), e) == answer.end() &&
                before.shareOn(pick, after, e) >= kMissingShare)
                return {Verdict::Wrong, "the answer leaves out " + name(e) + ", which holds part of the picked " +
                                            name(pick)};
        return {Verdict::Flagged, {}};
    }
    case State::Broken:
        if (!before.identicalIn(pick, after).empty())
            return {Verdict::Lost, {}};
        if (!before.sharingIn(pick, after).empty())
            return {Verdict::Flagged, {}};
        return {Verdict::Removed, {}};
    }
    return {Verdict::Wrong, "unknown state"};
}

} // namespace ray::rematch_oracle
