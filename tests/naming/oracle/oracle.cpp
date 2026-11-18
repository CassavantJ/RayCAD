// The naming oracle (task T-081a). See oracle.h. Ported from spikes/s4-naming/truth.cpp: OCCT calls replaced by
// the public kernel queries, owners for imported faces added.
#include "oracle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <optional>
#include <set>

namespace ray::naming_oracle {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kVertex = 0, kEdge = 1, kFace = 2;

// Meshes: the imported base finely (it is the carrier of B-spline faces), built shapes for sampling.
constexpr kernel::MeshOptions kBaseMesh{0.005, 0.1};
constexpr kernel::MeshOptions kSampleMesh{0.02, 0.2};
// Interior samples of a face: triangles are subdivided to about this edge length, at most kMaxSplit times.
constexpr double kSampleSpacing = 2.5;
constexpr int kMaxSplit = 12;
// How far an interior sample of a curved face may be off its surface (it lies on a chord of the sample mesh).
constexpr double kSampleChordSlack = 2.0 * kSampleMesh.chordTol;
// Edge samples: about this spacing, at least kMinEdgeSamples per edge.
constexpr double kEdgeSpacing = 1.0;
constexpr int kMinEdgeSamples = 16;

std::size_t slot(SubKind k)
{
    return static_cast<std::size_t>(k);
}

Vec3 fromFloats(const std::vector<float>& v, std::size_t i)
{
    return {static_cast<double>(v[3 * i]), static_cast<double>(v[3 * i + 1]), static_cast<double>(v[3 * i + 2])};
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return a;
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return add(a, mul(ab, d1 / (d1 - d3)));
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return add(a, mul(ac, d2 / (d2 - d6)));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    const double denom = 1.0 / (va + vb + vc);
    return add(a, add(mul(ab, vb * denom), mul(ac, vc * denom)));
}

double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = sub(b, a);
    const double len2 = dot(ab, ab);
    double t = len2 > 0 ? dot(sub(p, a), ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return norm(sub(p, add(a, mul(ab, t))));
}

bool outsideBox(const Vec3& p, const Vec3& lo, const Vec3& hi, double margin)
{
    return p.x < lo.x - margin || p.y < lo.y - margin || p.z < lo.z - margin || p.x > hi.x + margin ||
           p.y > hi.y + margin || p.z > hi.z + margin;
}

void growBox(Vec3& lo, Vec3& hi, const Vec3& p, bool first)
{
    if (first) {
        lo = hi = p;
        return;
    }
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
}

std::shared_ptr<FaceMesh> faceMesh(const kernel::Mesh& mesh, const kernel::FaceRange& range)
{
    auto out = std::make_shared<FaceMesh>();
    std::map<std::uint32_t, std::uint32_t> local;
    for (std::uint32_t i = 0; i < range.indexCount; i += 3) {
        std::array<std::uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t g = mesh.indices[range.firstIndex + i + static_cast<std::uint32_t>(k)];
            auto [it, fresh] = local.try_emplace(g, static_cast<std::uint32_t>(out->nodes.size()));
            if (fresh)
                out->nodes.push_back(fromFloats(mesh.positions, g));
            tri[static_cast<std::size_t>(k)] = it->second;
        }
        out->triangles.push_back(tri);
        Vec3 lo{}, hi{};
        for (int k = 0; k < 3; ++k)
            growBox(lo, hi, out->nodes[tri[static_cast<std::size_t>(k)]], k == 0);
        out->triangleBoxes.push_back({lo, hi});
    }
    for (std::size_t i = 0; i < out->nodes.size(); ++i)
        growBox(out->boxMin, out->boxMax, out->nodes[i], i == 0);
    out->findBoundary();
    return out;
}

// Average outward normal of a face from the mesh normals (area weighted by triangle).
Vec3 averageNormal(const kernel::Mesh& mesh, const kernel::FaceRange& range)
{
    Vec3 sum{0, 0, 0};
    for (std::uint32_t i = 0; i < range.indexCount; ++i)
        sum = add(sum, fromFloats(mesh.normals, mesh.indices[range.firstIndex + i]));
    return unit(sum);
}

int cylinderSide(const kernel::Mesh& mesh, const kernel::FaceRange& range, const Vec3& p, const Vec3& a)
{
    int out = 0, in = 0;
    for (std::uint32_t i = 0; i < range.indexCount; ++i) {
        const std::uint32_t g = mesh.indices[range.firstIndex + i];
        const Vec3 x = fromFloats(mesh.positions, g);
        const Vec3 d = sub(x, p);
        const Vec3 radial = sub(d, mul(a, dot(d, a)));
        (dot(fromFloats(mesh.normals, g), radial) > 0 ? out : in)++;
    }
    if (out > 9 * in)
        return +1;
    if (in > 9 * out)
        return -1;
    return 0;
}

// ---------------------------------------------------------------- 2D profile helpers (spike truth.cpp)
double normAng(double x)
{
    while (x < 0)
        x += 2 * kPi;
    while (x >= 2 * kPi)
        x -= 2 * kPi;
    return x;
}
bool insideLoop(const Loop& l, double u, double v)
{
    if (l.ents.size() == 1 && l.ents[0].k == 'C')
        return std::hypot(u - l.ents[0].cx, v - l.ents[0].cy) <= l.ents[0].r;
    bool in = false; // polygon of the chords
    for (const Ent& e : l.ents) {
        if ((e.y0 > v) != (e.y1 > v)) {
            const double x = e.x0 + (v - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0);
            if (u < x)
                in = !in;
        }
    }
    if (in)
        return true;
    for (const Ent& e : l.ents) { // circular segments of ccw arcs bulge outwards of a ccw loop
        if (e.k != 'A')
            continue;
        const double cr = (e.x1 - e.x0) * (v - e.y0) - (e.y1 - e.y0) * (u - e.x0);
        if (cr <= 0 && std::hypot(u - e.cx, v - e.cy) <= e.r)
            return true;
    }
    return false;
}
double distEnt(const Ent& e, double u, double v)
{
    if (e.k == 'L') {
        const double dx = e.x1 - e.x0, dy = e.y1 - e.y0, len2 = dx * dx + dy * dy;
        const double t = std::clamp(((u - e.x0) * dx + (v - e.y0) * dy) / len2, 0.0, 1.0);
        return std::hypot(u - (e.x0 + t * dx), v - (e.y0 + t * dy));
    }
    const double rr = std::hypot(u - e.cx, v - e.cy);
    if (e.k == 'C')
        return std::abs(rr - e.r);
    const double a0 = std::atan2(e.y0 - e.cy, e.x0 - e.cx), a1 = std::atan2(e.y1 - e.cy, e.x1 - e.cx);
    const double a = std::atan2(v - e.cy, u - e.cx);
    if (normAng(a - a0) <= normAng(a1 - a0))
        return std::abs(rr - e.r);
    return std::min(std::hypot(u - e.x0, v - e.y0), std::hypot(u - e.x1, v - e.y1));
}
double distLoop(const Loop& l, double u, double v)
{
    double d = 1e9;
    for (const Ent& e : l.ents)
        d = std::min(d, distEnt(e, u, v));
    return d;
}

// ---------------------------------------------------------------- blends (spike truth.cpp)
bool blendCarrier(const Owner& A, const Owner& B, bool fillet, bool convex, double size, Carrier& out)
{
    using K = Carrier::K;
    if (A.c.k == K::Plane && B.c.k == K::Plane) {
        const Vec3 nA = A.c.a, nB = B.c.a;
        const double c = dot(nA, nB);
        if (std::abs(c) > 0.999)
            return false;
        const Vec3 dir = cross(nA, nB);
        const double dA = dot(nA, A.c.p), dB = dot(nB, B.c.p);
        const Vec3 e0 = mul(add(mul(cross(nB, dir), dA), mul(cross(dir, nA), dB)), 1.0 / dot(dir, dir));
        const double s = convex ? -1.0 : 1.0;
        if (fillet) {
            out.k = K::Cyl;
            out.p = add(e0, mul(add(nA, nB), s * size / (1 + c)));
            out.a = unit(dir);
            out.r = size;
            out.sigma = convex ? +1 : -1;
            out.oriented = true;
        } else {
            const double sn = std::sqrt(1 - c * c);
            const Vec3 pA = add(e0, mul(sub(nB, mul(nA, c)), s * size / sn));
            const Vec3 pB = add(e0, mul(sub(nA, mul(nB, c)), s * size / sn));
            out.k = K::Plane;
            out.p = pA;
            out.a = unit(cross(dir, sub(pA, pB)));
        }
        return true;
    }
    const Carrier *P = nullptr, *C = nullptr;
    if (A.c.k == K::Plane && B.c.k == K::Cyl) {
        P = &A.c;
        C = &B.c;
    } else if (A.c.k == K::Cyl && B.c.k == K::Plane) {
        P = &B.c;
        C = &A.c;
    } else {
        return false;
    }
    if (!C->oriented || !P->oriented)
        return false;
    const double al = dot(P->a, C->a);
    if (std::abs(al) < 0.999)
        return false;
    const double sP = al > 0 ? 1.0 : -1.0;
    const double z0 = dot(sub(P->p, C->p), C->a);
    const double R = C->r, sc = C->sigma;
    out.p = C->p;
    out.a = C->a;
    if (fillet) {
        const double zc = convex ? z0 - sP * size : z0 + sP * size;
        const double rho0 = convex ? R - sc * size : R + sc * size;
        out.k = K::Torus;
        out.p = add(C->p, mul(C->a, zc));
        out.r = rho0;
        out.r2 = size;
    } else {
        out.k = K::Cone;
        if (convex) {
            out.q[0] = R;
            out.q[1] = z0 - sP * size;
            out.q[2] = R - sc * size;
            out.q[3] = z0;
        } else {
            out.q[0] = R;
            out.q[1] = z0 + sP * size;
            out.q[2] = R + sc * size;
            out.q[3] = z0;
        }
    }
    return true;
}

// Surface type must agree, and where known, so must the side the material is on.
bool typeMatches(const Carrier& c, const ElemSamples& es)
{
    using K = Carrier::K;
    using T = kernel::SurfaceType;
    const int t = es.type;
    switch (c.k) {
    case K::Plane:
        return t == static_cast<int>(T::Plane) &&
               (!c.oriented || (es.sided ? dot(es.nrm, c.a) : std::abs(dot(es.nrm, c.a))) > 0.99);
    case K::Cyl:
        return t == static_cast<int>(T::Cylinder) && (!c.oriented || es.sigma == 0 || es.sigma == c.sigma);
    case K::Torus:
        return t == static_cast<int>(T::Torus);
    case K::Cone:
        return t == static_cast<int>(T::Cone);
    case K::Sphere:
        return t == static_cast<int>(T::Sphere);
    case K::Mesh:
        return t == static_cast<int>(c.surface);
    case K::None:
        return false;
    }
    return false;
}

// RAYCAD_ORACLE_STRICT (oracle.h, STRICT MODE).
struct Strictness {
    bool nodes = false, band = false;
};
const Strictness& strictness()
{
    static const Strictness s = [] {
        Strictness out;
        if (const char* v = std::getenv("RAYCAD_ORACLE_STRICT")) {
            const std::string list(v);
            out.nodes = list.find("nodes") != std::string::npos;
            out.band = list.find("band") != std::string::npos;
        }
        return out;
    }();
    return s;
}

struct Tol {
    double node, sample, margin, thr;
};
Tol tolFor(SubKind k)
{
    if (k == SubKind::Face)
        return {1e-4, 0.06, 1e-4, 0.02};
    // The spike's 1e-3 (a 2e-3 loosening changed no verdict in S4 or the random test, so it was withdrawn).
    const double t = 1e-3;
    if (k == SubKind::Edge)
        return {t, t, t, 0.05};
    return {t, t, t, 0.5};
}
// Do the nodes of an element lie on the carrier? Every node within 10 x tol and at least 95 % within tol: nodes on
// the boundary of a face are computed on its edges, and the edges of a boolean result may lie off the surfaces by
// their (larger) tolerance. Strict mode: every node within tol.
bool nodesOn(const Carrier& c, const std::vector<Vec3>& nodes, double tol)
{
    const bool strict = strictness().nodes;
    std::size_t close = 0;
    for (const Vec3& p : nodes) {
        if (!c.on(p, strict ? tol : 10.0 * tol))
            return false;
        if (c.on(p, tol))
            ++close;
    }
    return !nodes.empty() && 20 * close >= 19 * nodes.size();
}

struct SepRef {
    const Carrier* c;
    int side;
    const std::function<bool(const Vec3&, double)>* in;
};
using SepList = std::vector<SepRef>;

// How far outside its extent a separator still applies, mm.
constexpr double kBandMargin = 0.05;

Vec3 projectOnto(const Carrier& c, const Vec3& p)
{
    if (c.k == Carrier::K::Plane)
        return sub(p, mul(c.a, c.dist(p)));
    if (c.k == Carrier::K::Cyl) {
        const Vec3 d = sub(p, c.p);
        const double z = dot(d, c.a);
        const Vec3 radial = sub(d, mul(c.a, z));
        const double rho = norm(radial);
        if (!(rho > 0))
            return p;
        return add(add(c.p, mul(c.a, z)), mul(radial, c.r / rho));
    }
    return p;
}

bool inBand(const SepRef& s, const Vec3& p)
{
    return strictness().band || s.in == nullptr || !*s.in || (*s.in)(projectOnto(*s.c, p), kBandMargin);
}

// Nearest point of any analytic carrier (projectOnto covers the separator kinds only).
Vec3 projectAny(const Carrier& c, const Vec3& p)
{
    using K = Carrier::K;
    if (c.k == K::Plane || c.k == K::Cyl)
        return projectOnto(c, p);
    const Vec3 d = sub(p, c.p);
    if (c.k == K::Sphere)
        return norm(d) > 0 ? add(c.p, mul(unit(d), c.r)) : p;
    if (c.k != K::Torus && c.k != K::Cone)
        return p;
    const double z = dot(d, c.a);
    const Vec3 radial = sub(d, mul(c.a, z));
    const double rho = norm(radial);
    if (!(rho > 0))
        return p;
    const Vec3 u = mul(radial, 1.0 / rho);
    if (c.k == K::Torus) {
        const Vec3 ring = add(c.p, mul(u, c.r));
        const Vec3 off = sub(p, ring);
        return norm(off) > 0 ? add(ring, mul(unit(off), c.r2)) : p;
    }
    // Cone: the nearest point of the generator line through (q0, q1) and (q2, q3) in the (rho, z) half-plane.
    const double dr = c.q[2] - c.q[0], dz = c.q[3] - c.q[1], len = std::hypot(dr, dz);
    if (!(len > 0))
        return p;
    const double t = ((rho - c.q[0]) * dr + (z - c.q[1]) * dz) / (len * len);
    const double r1 = c.q[0] + t * dr, z1 = c.q[1] + t * dz;
    return add(add(c.p, mul(c.a, z1)), mul(u, r1));
}

bool sepsOk(const SepList& seps, const Vec3& p)
{
    for (const SepRef& s : seps)
        if (inBand(s, p) && s.side * s.c->dist(p) < -1e-3)
            return false;
    return true;
}

SepRef sepRef(const Sep& s, const Owners& ow)
{
    const Owner* o = ow.get(s.owner);
    return o != nullptr ? SepRef{&o->c, s.side, &o->in} : SepRef{&s.frozen, s.side, &s.frozenIn};
}

// Carriers of an imported face: the analytic surface if every mesh node lies on it, else the mesh.
Carrier importCarrier(const kernel::SurfaceInfo& info, const kernel::Mesh& mesh, const kernel::FaceRange& range,
                      const std::shared_ptr<const FaceMesh>& fm, double meshTol)
{
    using K = Carrier::K;
    using T = kernel::SurfaceType;
    Carrier c;
    const Vec3 axis = unit(info.axis);
    switch (info.type) {
    case T::Plane: {
        c.k = K::Plane;
        c.p = info.origin;
        const Vec3 n = averageNormal(mesh, range);
        c.a = dot(n, axis) >= 0 ? axis : mul(axis, -1.0);
        c.oriented = std::abs(dot(n, axis)) > 0.999;
        break;
    }
    case T::Cylinder:
        c.k = K::Cyl;
        c.p = info.origin;
        c.a = axis;
        c.r = info.radius;
        c.sigma = cylinderSide(mesh, range, c.p, c.a);
        c.oriented = c.sigma != 0;
        break;
    case T::Cone: {
        c.k = K::Cone;
        c.p = info.origin;
        c.a = axis;
        c.q[0] = info.radius;
        c.q[1] = 0;
        c.q[2] = info.radius + std::tan(info.semiAngle);
        c.q[3] = 1;
        break;
    }
    case T::Sphere:
        c.k = K::Sphere;
        c.p = info.origin;
        c.r = info.radius;
        break;
    case T::Torus: {
        c.k = K::Torus;
        c.p = info.origin;
        c.a = axis;
        c.r = info.radius;
        std::vector<double> minor;
        for (const Vec3& x : fm->nodes) {
            const Vec3 d = sub(x, c.p);
            const double z = dot(d, c.a);
            minor.push_back(std::hypot(norm(sub(d, mul(c.a, z))) - c.r, z));
        }
        if (!minor.empty()) {
            std::nth_element(minor.begin(), minor.begin() + static_cast<std::ptrdiff_t>(minor.size() / 2), minor.end());
            c.r2 = minor[minor.size() / 2];
        }
        break;
    }
    default:
        break;
    }
    bool fits = c.analytic() && !fm->nodes.empty();
    for (const Vec3& x : fm->nodes)
        if (fits && std::abs(c.dist(x)) > 1e-4)
            fits = false;
    if (fits)
        return c;
    Carrier m;
    m.k = K::Mesh;
    m.surface = info.type;
    m.mesh = fm;
    m.meshTolerance = meshTol;
    return m;
}

} // namespace

double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

Vec3 unit(const Vec3& a)
{
    const double n = norm(a);
    return n > 0 ? mul(a, 1.0 / n) : Vec3{0, 0, 1};
}

std::string strictnessName()
{
    const Strictness& s = strictness();
    std::string out;
    if (s.nodes)
        out += " nodes";
    if (s.band)
        out += " band";
    return out.empty() ? "default (95 % nodes / 10x, band rule)" : "strict:" + out;
}

// ---------------------------------------------------------------- carriers
void FaceMesh::findBoundary()
{
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    for (const auto& tri : triangles)
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = tri[static_cast<std::size_t>(k)], b = tri[static_cast<std::size_t>((k + 1) % 3)];
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    boundary.clear();
    for (const auto& [seg, n] : uses)
        if (n == 1)
            boundary.push_back({nodes[seg.first], nodes[seg.second]});
}

double FaceMesh::distanceWithin(const Vec3& p, double limit, bool exact) const
{
    if (triangles.empty() || outsideBox(p, boxMin, boxMax, limit))
        return limit + 1.0;
    double best = limit + 1.0;
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        if (outsideBox(p, triangleBoxes[t][0], triangleBoxes[t][1], std::min(best, limit)))
            continue;
        const auto& tri = triangles[t];
        const double d = norm(sub(p, closestOnTriangle(p, nodes[tri[0]], nodes[tri[1]], nodes[tri[2]])));
        if (d < best) {
            best = d;
            if (!exact && best <= limit * 0.25)
                return best;
        }
    }
    return best;
}

double Carrier::dist(const Vec3& x) const
{
    const Vec3 d = sub(x, p);
    const double z = dot(d, a);
    switch (k) {
    case K::Plane:
        return z;
    case K::Cyl:
        return norm(sub(d, mul(a, z))) - r;
    case K::Torus: {
        const double rho = norm(sub(d, mul(a, z)));
        return std::hypot(rho - r, z) - r2;
    }
    case K::Cone: {
        const double rho = norm(sub(d, mul(a, z)));
        const double dr = q[2] - q[0], dz = q[3] - q[1], len = std::hypot(dr, dz);
        return ((rho - q[0]) * dz - (z - q[1]) * dr) / len;
    }
    case K::Sphere:
        return norm(d) - r;
    case K::Mesh:
        return mesh ? mesh->distanceWithin(x, 1e9) : 1e9;
    case K::None:
        return 1e9;
    }
    return 1e9;
}

bool Carrier::on(const Vec3& x, double tol) const
{
    if (k == K::Mesh)
        return mesh && mesh->distanceWithin(x, tol + meshTolerance) <= tol + meshTolerance;
    return std::abs(dist(x)) <= tol;
}

Result<std::shared_ptr<const ImportBase>> analyseImport(const kernel::Shape& shape)
{
    JobContext ctx;
    auto mesh = kernel::tessellate(shape, kBaseMesh, ctx);
    if (!mesh)
        return tl::unexpected(mesh.error());
    auto base = std::make_shared<ImportBase>();
    base->shape = shape;
    base->meshTolerance = 1.5 * kBaseMesh.chordTol + 1e-4;
    const std::uint32_t faces = shape.count(SubKind::Face);
    for (std::uint32_t i = 0; i < faces; ++i) {
        const SubId face{SubKind::Face, i};
        auto info = kernel::surfaceInfo(shape, face);
        if (!info)
            return tl::unexpected(info.error());
        const kernel::FaceRange& range = mesh->faces[i];
        std::shared_ptr<const FaceMesh> fm = faceMesh(*mesh, range);
        base->carriers.push_back(importCarrier(*info, *mesh, range, fm, base->meshTolerance));
        base->meshes.push_back(fm);
    }
    return std::shared_ptr<const ImportBase>(base);
}

// ---------------------------------------------------------------- owners from the script
Owners buildOwners(const Script& script)
{
    Owners O;
    auto addOwner = [&](Owner o) {
        O.ix[o.key] = static_cast<int>(O.v.size());
        O.v.push_back(std::move(o));
    };
    for (const Feature& f : script) {
        if (f.suppressed)
            continue;
        if (f.type == FType::Import) {
            if (!f.base)
                continue;
            for (std::size_t i = 0; i < f.base->carriers.size(); ++i) {
                Owner o;
                o.key = f.id + "/i" + std::to_string(i);
                o.c = f.base->carriers[i];
                o.cat = "imported face";
                const std::shared_ptr<const FaceMesh> fm = f.base->meshes[i];
                const double tol = f.base->meshTolerance;
                // Inside the extent = near the face's own mesh. The distance to the mesh also counts how far the
                // point is off the surface (a sample of a curved face lies on a chord); that part is allowed: it
                // is the carrier test's business, not the extent's.
                const Carrier c = o.c;
                // The sample mesh's own chords also reach past the face's boundary (at a hole) by up to its chord
                // tolerance.
                o.in = [fm, tol, c](const Vec3& p, double m) {
                    const double off = c.analytic() ? std::abs(c.dist(p)) : kSampleChordSlack;
                    const double limit = m + tol + off + kSampleChordSlack;
                    return fm->distanceWithin(p, limit) <= limit;
                };
                addOwner(std::move(o));
            }
        } else if (f.type == FType::Sweep) {
            O.sweeps[f.id] = f;
            const Frame fr = f.frame;
            const double sg = f.op == BOp::Cut ? -1.0 : 1.0, D = f.dist;
            const bool cut = f.op == BOp::Cut, hole = f.role == "hole";
            auto local = [fr, sg](const Vec3& p, double& u, double& v, double& h) {
                const Vec3 d = sub(p, fr.o);
                u = dot(d, fr.xd);
                v = dot(d, fr.yd());
                h = sg * dot(d, fr.n);
            };
            for (const Loop& l : f.loops) {
                for (const Ent& e : l.ents) {
                    Owner o;
                    o.key = f.id + "/s" + std::to_string(e.tag);
                    o.cat = hole ? "hole wall" : cut ? "cut wall" : "boss/base side";
                    if (e.k == 'L') {
                        const double len = std::hypot(e.x1 - e.x0, e.y1 - e.y0);
                        const double tx = (e.x1 - e.x0) / len, ty = (e.y1 - e.y0) / len;
                        const double ox = cut ? -ty : ty, oy = cut ? tx : -tx;
                        o.c.oriented = true;
                        o.c.k = Carrier::K::Plane;
                        o.c.p = fr.at(e.x0, e.y0);
                        o.c.a = unit(add(mul(fr.xd, ox), mul(fr.yd(), oy)));
                        o.in = [=](const Vec3& p, double m) {
                            double u = 0, v = 0, h = 0;
                            local(p, u, v, h);
                            const double t = (u - e.x0) * tx + (v - e.y0) * ty;
                            return t >= -m && t <= len + m && h >= -m && h <= D + m;
                        };
                    } else {
                        o.c.oriented = true;
                        o.c.k = Carrier::K::Cyl;
                        o.c.p = fr.at(e.cx, e.cy);
                        o.c.a = fr.n;
                        o.c.r = e.r;
                        o.c.sigma = cut ? -1 : +1;
                        const double a0 = std::atan2(e.y0 - e.cy, e.x0 - e.cx);
                        const double span = e.k == 'C' ? 2 * kPi : normAng(std::atan2(e.y1 - e.cy, e.x1 - e.cx) - a0);
                        o.in = [=](const Vec3& p, double m) {
                            double u = 0, v = 0, h = 0;
                            local(p, u, v, h);
                            if (h < -m || h > D + m)
                                return false;
                            if (e.k == 'C')
                                return true;
                            const double a = normAng(std::atan2(v - e.cy, u - e.cx) - a0), ma = m / e.r;
                            return a <= span + ma || a >= 2 * kPi - ma;
                        };
                    }
                    addOwner(std::move(o));
                }
                for (int cap = 0; cap < 2; ++cap) {
                    Owner o;
                    o.key = f.id + "/c" + std::to_string(cap) + "/" + std::to_string(l.id);
                    o.cat = cap == 0 ? "bottom face" : hole ? "hole floor" : cut ? "cut floor" : "top face";
                    o.c.k = Carrier::K::Plane;
                    o.c.oriented = true;
                    o.c.p = fr.at(0, 0, cap == 0 ? 0.0 : sg * D);
                    const Vec3 dir = cut ? mul(fr.n, -1.0) : fr.n;
                    o.c.a = (cap == 0 || cut) ? mul(dir, -1.0) : dir;
                    const Loop lp = l;
                    o.in = [=](const Vec3& p, double m) {
                        double u = 0, v = 0, h = 0;
                        local(p, u, v, h);
                        return insideLoop(lp, u, v) || distLoop(lp, u, v) <= m;
                    };
                    addOwner(std::move(o));
                }
            }
        } else {
            const bool fillet = f.type == FType::Fillet;
            for (std::size_t k = 0; k < f.targets.size(); ++k) {
                const Owner *A = O.get(f.targets[k].a), *B = O.get(f.targets[k].b);
                if (!A || !B || !A->c.analytic() || !B->c.analytic())
                    continue;
                Owner o;
                o.key = f.id + "/b" + std::to_string(k);
                o.cat = fillet ? "fillet face" : "chamfer face";
                if (!blendCarrier(*A, *B, fillet, f.targets[k].convex, f.size, o.c))
                    continue;
                const Owner a = *A, b = *B;
                const double rho = f.size;
                // Within the blend size of both carriers, over both extents. The extents get a little more room
                // (5 % of the size): where a blend runs onto another blend, the parents' own extents are
                // approximations at their ends.
                const double slack = 0.01 + 0.05 * rho;
                o.in = [=](const Vec3& p, double m) {
                    return std::abs(a.c.dist(p)) <= rho + m + 1e-3 && std::abs(b.c.dist(p)) <= rho + m + 1e-3 &&
                           a.in(p, m + rho + slack) && b.in(p, m + rho + slack);
                };
                addOwner(std::move(o));
            }
        }
    }
    return O;
}

bool canBlend(const Owners& owners, const Target& target, bool fillet)
{
    const Owner *A = owners.get(target.a), *B = owners.get(target.b);
    if (!A || !B || !A->c.analytic() || !B->c.analytic())
        return false;
    Carrier c;
    return blendCarrier(*A, *B, fillet, target.convex, 1.0, c);
}

// ---------------------------------------------------------------- samples
Result<std::shared_ptr<const Samples>> sampleShape(const kernel::Shape& shape)
{
    JobContext ctx;
    auto mesh = kernel::tessellate(shape, kSampleMesh, ctx);
    if (!mesh)
        return tl::unexpected(mesh.error());
    auto out = std::make_shared<Samples>();
    const std::uint32_t faces = shape.count(SubKind::Face);
    const std::uint32_t edges = shape.count(SubKind::Edge);
    const std::uint32_t vertices = shape.count(SubKind::Vertex);
    out->e[kFace].resize(faces);
    out->e[kEdge].resize(edges);
    out->e[kVertex].resize(vertices);
    out->facesOf[0].resize(vertices);
    out->facesOf[1].resize(edges);
    out->verticesOf.resize(edges);

    for (std::uint32_t i = 0; i < faces; ++i) {
        ElemSamples& es = out->e[kFace][i];
        const SubId face{SubKind::Face, i};
        auto info = kernel::surfaceInfo(shape, face);
        if (!info)
            return tl::unexpected(info.error());
        es.type = static_cast<int>(info->type);
        const kernel::FaceRange& range = mesh->faces[i];
        const auto fm = faceMesh(*mesh, range);
        es.nodes = fm->nodes;
        es.boxMin = fm->boxMin;
        es.boxMax = fm->boxMax;
        es.mesh = fm;
        for (const auto& tri : fm->triangles) {
            const Vec3 a = fm->nodes[tri[0]], b = fm->nodes[tri[1]], c = fm->nodes[tri[2]];
            const double longest = std::max({norm(sub(b, a)), norm(sub(c, b)), norm(sub(a, c))});
            const int n = std::clamp(static_cast<int>(std::ceil(longest / kSampleSpacing)), 1, kMaxSplit);
            const double area = 0.5 * norm(cross(sub(b, a), sub(c, a)));
            if (!(area > 0))
                continue;
            // Barycentric grid of n*n sub-triangles; each contributes its centroid.
            for (int s = 0; s < n; ++s)
                for (int t = 0; t < n - s; ++t) {
                    for (int up = 0; up < 2; ++up) {
                        if (up == 1 && s + t + 1 >= n)
                            continue;
                        const double u = (s + (up == 0 ? 1.0 / 3.0 : 2.0 / 3.0)) / n;
                        const double v = (t + (up == 0 ? 1.0 / 3.0 : 2.0 / 3.0)) / n;
                        es.pts.push_back(add(a, add(mul(sub(b, a), u), mul(sub(c, a), v))));
                        es.w.push_back(area / (n * n));
                    }
                }
        }
        // The material side comes from the mesh normals. A face without triangles has none (the kernel's face
        // query does not say on which side its surface's normal points): its plane normal is known up to sign
        // (sided = false) and its cylinder side is unknown (sigma 0).
        const bool meshed = !fm->triangles.empty();
        if (info->type == kernel::SurfaceType::Plane) {
            es.nrm = meshed ? averageNormal(*mesh, range) : unit(info->axis);
            es.sided = meshed;
        }
        if (info->type == kernel::SurfaceType::Cylinder)
            es.sigma = meshed ? cylinderSide(*mesh, range, info->origin, unit(info->axis)) : 0;
    }

    for (std::uint32_t i = 0; i < edges; ++i) {
        ElemSamples& es = out->e[kEdge][i];
        const SubId edge{SubKind::Edge, i};
        for (const SubId& f : kernel::adjacent(shape, edge, SubKind::Face))
            out->facesOf[1][i].push_back(f.index);
        for (const SubId& v : kernel::adjacent(shape, edge, SubKind::Vertex))
            out->verticesOf[i].push_back(v.index);
        const kernel::EdgeRange& range = mesh->edges[i];
        for (std::uint32_t k = 0; k < range.vertexCount; ++k)
            es.nodes.push_back(fromFloats(mesh->edgePositions, range.firstVertex + k));
        auto info = kernel::curveInfo(shape, edge);
        if (!info || es.nodes.size() < 2) {
            es.nodes.clear();
            continue;
        }
        es.type = static_cast<int>(info->type);
        double total = 0;
        for (std::size_t k = 1; k < es.nodes.size(); ++k)
            total += norm(sub(es.nodes[k], es.nodes[k - 1]));
        const int wanted = std::max(kMinEdgeSamples, static_cast<int>(std::ceil(total / kEdgeSpacing)));
        const Vec3 axis = unit(info->axis);
        auto project = [&](const Vec3& x) {
            if (info->type == kernel::CurveType::Circle) {
                const Vec3 d = sub(x, info->center);
                const Vec3 radial = sub(d, mul(axis, dot(d, axis)));
                return add(info->center, mul(unit(radial), info->radius));
            }
            return x; // a line is exact
        };
        const bool exact = info->type == kernel::CurveType::Line || info->type == kernel::CurveType::Circle;
        if (!exact) {
            // No closed form to project onto: the polyline nodes themselves (they lie on the curve), each weighted
            // by half of its two segments.
            for (std::size_t k = 0; k < es.nodes.size(); ++k) {
                double w = 0;
                if (k > 0)
                    w += 0.5 * norm(sub(es.nodes[k], es.nodes[k - 1]));
                if (k + 1 < es.nodes.size())
                    w += 0.5 * norm(sub(es.nodes[k + 1], es.nodes[k]));
                es.pts.push_back(es.nodes[k]);
                es.w.push_back(w);
            }
        }
        for (std::size_t k = 1; exact && k < es.nodes.size(); ++k) {
            const Vec3 a = es.nodes[k - 1], b = es.nodes[k];
            const double len = norm(sub(b, a));
            const int n = std::max(1, static_cast<int>(std::ceil(wanted * len / std::max(total, 1e-12))));
            for (int s = 0; s < n; ++s) {
                es.pts.push_back(project(add(a, mul(sub(b, a), (s + 0.5) / n))));
                es.w.push_back(len / n);
            }
        }
        for (std::size_t k = 0; k < es.nodes.size(); ++k)
            growBox(es.boxMin, es.boxMax, es.nodes[k], k == 0);
    }

    for (std::uint32_t i = 0; i < vertices; ++i) {
        ElemSamples& es = out->e[kVertex][i];
        const SubId vertex{SubKind::Vertex, i};
        for (const SubId& f : kernel::adjacent(shape, vertex, SubKind::Face))
            out->facesOf[0][i].push_back(f.index);
        auto p = kernel::vertexPoint(shape, vertex);
        if (!p)
            return tl::unexpected(p.error());
        es.nodes = {*p};
        es.pts = {*p};
        es.w = {1.0};
        es.boxMin = es.boxMax = *p;
    }

    // A face the mesher left without triangles: sampled on its boundary (the samples of its edges and its vertices).
    // It has no mesh; nearness to it is nearness to these points (nearElement).
    for (std::uint32_t i = 0; i < faces; ++i) {
        ElemSamples& es = out->e[kFace][i];
        if (!es.pts.empty())
            continue;
        ++out->unmeshedFaces;
        es.nodes.clear();
        es.mesh.reset();
        for (const SubId& e : kernel::adjacent(shape, SubId{SubKind::Face, i}, SubKind::Edge)) {
            const ElemSamples& edge = out->e[kEdge][e.index];
            es.pts.insert(es.pts.end(), edge.pts.begin(), edge.pts.end());
            es.w.insert(es.w.end(), edge.w.begin(), edge.w.end());
            es.nodes.insert(es.nodes.end(), edge.nodes.begin(), edge.nodes.end());
        }
        for (const SubId& v : kernel::adjacent(shape, SubId{SubKind::Face, i}, SubKind::Vertex))
            es.nodes.push_back(out->e[kVertex][v.index].nodes.front());
        for (std::size_t k = 0; k < es.nodes.size(); ++k)
            growBox(es.boxMin, es.boxMax, es.nodes[k], k == 0);
    }
    return std::shared_ptr<const Samples>(out);
}

// ---------------------------------------------------------------- evaluation
namespace {
// evalTruth, also giving for each matching element which of its samples fit the description.
using FitMasks = std::vector<std::pair<std::uint32_t, std::vector<char>>>;
TruthEval evalTruthImpl(const TruthDesc& d, const Owners& ow, const Samples& smp, FitMasks* masks)
{
    TruthEval ev;
    const Tol T = tolFor(d.kind);
    struct C {
        std::vector<const Owner*> alts;
        SepList seps;
    };
    std::vector<C> cons;
    // A separator follows its owner while the owner exists; if it is gone (feature deleted or suppressed) the
    // baseline geometry is used, so "which piece" stays a geometric question.
    auto conv = [&](const std::vector<Sep>& in, SepList& out) {
        for (const Sep& s : in)
            out.push_back(sepRef(s, ow));
    };
    for (const Constraint& c : d.cons) {
        C x;
        for (const auto& a : c.alts)
            if (const Owner* o = ow.get(a))
                x.alts.push_back(o);
        if (x.alts.empty())
            return ev; // the logical face no longer exists in the script
        conv(c.seps, x.seps);
        cons.push_back(std::move(x));
    }
    SepList dseps;
    conv(d.seps, dseps);
    const auto& E = smp.of(d.kind);
    for (std::uint32_t x = 0; x < E.size(); ++x) {
        const ElemSamples& es = E[x];
        if (es.nodes.empty() || es.pts.empty())
            continue;
        bool carr = true;
        for (const C& c : cons) {
            bool anyAlt = false;
            for (const Owner* o : c.alts) {
                if (d.kind == SubKind::Face && !typeMatches(o->c, es))
                    continue;
                if (nodesOn(o->c, es.nodes, T.node)) {
                    anyAlt = true;
                    break;
                }
            }
            if (!anyAlt) {
                carr = false;
                break;
            }
        }
        if (!carr)
            continue;
        // An edge or vertex must also have, for each face of the description, a face of its own with that face's
        // surface type and material side: an edge on the same two carriers between other faces (a feature's own
        // corner showing through a gap, back to back with the described face) is a different edge.
        if (d.kind != SubKind::Face) {
            // The face must lie on that carrier too (not just near it where the edge runs): an edge can run within
            // tolerance of a third carrier where two features nearly coincide.
            const auto& own = smp.facesOf[d.kind == SubKind::Edge ? 1 : 0][x];
            for (const C& c : cons) {
                bool found = false;
                for (const std::uint32_t f : own)
                    for (const Owner* o : c.alts) {
                        // (A face the mesher left without triangles is checked on its boundary samples.)
                        const ElemSamples& face = smp.of(SubKind::Face)[f];
                        found =
                            found || (typeMatches(o->c, face) && nodesOn(o->c, face.nodes, tolFor(SubKind::Face).node));
                    }
                if (!found) {
                    carr = false;
                    break;
                }
            }
            if (!carr)
                continue;
        }
        double okw = 0, W = 0;
        std::vector<char> mask(es.pts.size(), 0);
        for (std::size_t j = 0; j < es.pts.size(); ++j) {
            const Vec3& p = es.pts[j];
            W += es.w[j];
            bool ok = sepsOk(dseps, p);
            for (std::size_t ci = 0; ok && ci < cons.size(); ++ci) {
                bool any = false;
                if (sepsOk(cons[ci].seps, p))
                    for (const Owner* o : cons[ci].alts)
                        if (o->c.on(p, T.sample) && o->in(p, T.margin)) {
                            any = true;
                            break;
                        }
                ok = any;
            }
            if (ok) {
                okw += es.w[j];
                mask[j] = 1;
            }
        }
        const double frac = W > 0 ? okw / W : 0;
        if (frac >= T.thr) {
            ev.elems.push_back(x);
            if (frac < 0.98) {
                ev.merged = true;
                ev.partial.push_back(x);
            }
            if (masks != nullptr)
                masks->emplace_back(x, std::move(mask));
        }
    }
    return ev;
}
} // namespace

TruthEval evalTruth(const TruthDesc& d, const Owners& ow, const Samples& smp)
{
    return evalTruthImpl(d, ow, smp, nullptr);
}

std::string explain(const TruthDesc& d, const Owners& ow, const Samples& smp, long x)
{
    std::string out = "desc{";
    auto seps = [&](const std::vector<Sep>& v) {
        std::string s;
        for (const Sep& sp : v)
            s += " sep " + sp.owner + (sp.side > 0 ? "+" : "-") + (ow.get(sp.owner) ? "" : "(frozen)");
        return s;
    };
    for (const Constraint& c : d.cons) {
        out += " [";
        for (const auto& a : c.alts)
            out += a + (ow.get(a) ? " " : "(gone) ");
        out += seps(c.seps) + "]";
    }
    out += seps(d.seps) + "}";
    if (x < 0 || static_cast<std::size_t>(x) >= smp.of(d.kind).size())
        return out;
    const ElemSamples& es = smp.of(d.kind)[static_cast<std::size_t>(x)];
    const Tol T = tolFor(d.kind);
    out += " element " + std::to_string(x) + ":";
    for (const Constraint& c : d.cons) {
        for (const auto& a : c.alts) {
            const Owner* o = ow.get(a);
            if (!o)
                continue;
            std::size_t on = 0;
            double worst = 0;
            for (const Vec3& p : es.nodes) {
                if (o->c.on(p, T.node))
                    ++on;
                if (o->c.analytic())
                    worst = std::max(worst, std::abs(o->c.dist(p)));
            }
            std::size_t inExt = 0;
            for (const Vec3& p : es.pts)
                if (o->in(p, T.margin))
                    ++inExt;
            char buf[200];
            std::snprintf(buf, sizeof buf, " %s: nodes on %zu/%zu (worst %.2g), type %s, samples in extent %zu/%zu;",
                          a.c_str(), on, es.nodes.size(), worst,
                          d.kind != SubKind::Face || typeMatches(o->c, es) ? "ok" : "BAD", inExt, es.pts.size());
            out += buf;
        }
    }
    SepList all;
    for (const Sep& s : d.seps)
        all.push_back(sepRef(s, ow));
    for (const Constraint& c : d.cons)
        for (const Sep& s : c.seps)
            all.push_back(sepRef(s, ow));
    std::size_t sepOk = 0;
    for (const Vec3& p : es.pts)
        if (sepsOk(all, p))
            ++sepOk;
    out += " all seps ok " + std::to_string(sepOk) + "/" + std::to_string(es.pts.size());
    return out;
}

namespace {

// Carriers that separate element `self` from the other elements that satisfy `d`.
bool findSeps(TruthDesc& d, std::uint32_t self, std::vector<Sep>& where, const Owners& ow, const Samples& smp)
{
    for (int iter = 0; iter < 6; ++iter) {
        const TruthEval ev = evalTruth(d, ow, smp);
        if (ev.elems.size() == 1 && ev.elems[0] == self)
            return true;
        if (std::find(ev.elems.begin(), ev.elems.end(), self) == ev.elems.end())
            return false;
        std::uint32_t other = 0;
        for (const std::uint32_t x : ev.elems)
            if (x != self) {
                other = x;
                break;
            }
        const ElemSamples &S = smp.of(d.kind)[self], &Oth = smp.of(d.kind)[other];
        bool added = false;
        for (const Owner& o : ow.v) {
            if (o.c.k != Carrier::K::Plane && o.c.k != Carrier::K::Cyl)
                continue;
            double smin = 1e9, smax = -1e9, omin = 1e9, omax = -1e9, touch = 1e9;
            const SepRef band{&o.c, 0, &o.in};
            // The separator must bound the piece: a boundary point of the piece lies on the carrier AND inside
            // the owner's extent (else a coincident but unrelated carrier could be chosen). Only points in the
            // separator's band count (see Sep).
            for (const Vec3& p : S.nodes) {
                const double x = o.c.dist(p);
                if (std::abs(x) < touch && o.in(p, 1e-2))
                    touch = std::abs(x);
                if (!inBand(band, p))
                    continue;
                smin = std::min(smin, x);
                smax = std::max(smax, x);
            }
            for (const Vec3& p : Oth.nodes) {
                if (!inBand(band, p))
                    continue;
                const double x = o.c.dist(p);
                omin = std::min(omin, x);
                omax = std::max(omax, x);
            }
            if (touch > 1e-3 || smin > smax || omin > omax)
                continue;
            int side = 0;
            if (smin >= -1e-3 && smax > 0.05 && omax <= 1e-3 && omin < -0.05)
                side = +1;
            if (smax <= 1e-3 && smin < -0.05 && omin >= -1e-3 && omax > 0.05)
                side = -1;
            if (side == 0)
                continue;
            where.push_back({o.key, side, o.c, o.in});
            added = true;
            break;
        }
        if (!added)
            return false;
    }
    return false;
}

} // namespace

TruthBuilder::TruthBuilder(const Owners& o, const Samples& s) : owners_(o), samples_(s)
{
    const std::size_t faces = samples_.of(SubKind::Face).size();
    faceDesc_.resize(faces);
    for (std::uint32_t f = 0; f < faces; ++f)
        faceDesc_[f] = describe({SubKind::Face, f});
}

TruthDesc TruthBuilder::describe(SubId s) const
{
    TruthDesc d;
    d.kind = s.kind;
    if (s.kind == SubKind::Face) {
        if (!faceDesc_.empty() && s.index < faceDesc_.size() && faceDesc_[s.index].kind == SubKind::Face &&
            (faceDesc_[s.index].ok || !faceDesc_[s.index].why.empty()))
            return faceDesc_[s.index];
        const ElemSamples& es = samples_.of(SubKind::Face)[s.index];
        if (es.nodes.empty() || es.pts.empty()) {
            d.why = "no mesh";
            return d;
        }
        Constraint c;
        double total = 0, W = 0;
        for (const double w : es.w)
            W += w;
        for (const Owner& o : owners_.v) {
            if (!typeMatches(o.c, es) || !nodesOn(o.c, es.nodes, tolFor(SubKind::Face).node))
                continue;
            double okw = 0;
            for (std::size_t j = 0; j < es.pts.size(); ++j)
                if (o.in(es.pts[j], 1e-4))
                    okw += es.w[j];
            if (okw / W >= 0.02) {
                c.alts.push_back(o.key);
                total += okw / W;
                if (d.cat.empty())
                    d.cat = o.cat;
            }
        }
        if (c.alts.empty()) {
            d.why = "no owner";
            return d;
        }
        if (total < 0.98) {
            d.why = "owners cover only part of the face";
            return d;
        }
        d.cons.push_back(c);
        if (!findSeps(d, s.index, d.cons[0].seps, owners_, samples_)) {
            // No carrier separates it from the other pieces of its logical face (two pieces either side of a
            // breakout): a weak description, judged by position (oracle.h).
            d.cons[0].seps.clear();
            const TruthEval ev = evalTruth(d, owners_, samples_);
            if (std::find(ev.elems.begin(), ev.elems.end(), s.index) == ev.elems.end()) {
                d.why = "not found at baseline";
                return d;
            }
            d.weak = true;
            d.cat += " (weak oracle)";
            d.ok = true;
            return d;
        }
        const TruthEval ev = evalTruth(d, owners_, samples_);
        if (ev.merged) {
            d.why = "baseline self-check not full";
            return d;
        }
        d.ok = true;
        return d;
    }
    const auto& fs = samples_.facesOf[s.kind == SubKind::Edge ? 1 : 0][s.index];
    if (fs.size() < 2) {
        d.why = "fewer than two faces (seam)";
        return d;
    }
    if (samples_.of(s.kind)[s.index].nodes.empty()) {
        d.why = "no samples";
        return d;
    }
    // The owners of its faces, without the separators that tell pieces of those faces apart: an edge is the meeting
    // of two logical faces wherever it runs (the top rim of a hole is still that rim when a groove later cuts the
    // top face in two under it). Where pieces of the edge itself must be told apart, it gets its own separators.
    for (const std::uint32_t f : fs) {
        if (!faceDesc_[f].ok) {
            d.why = "adjacent face has no truth";
            return d;
        }
        d.cons.push_back(Constraint{faceDesc_[f].cons[0].alts, {}});
    }
    d.cat = s.kind == SubKind::Edge ? "edge" : "vertex";
    if (!findSeps(d, s.index, d.seps, owners_, samples_)) {
        d.seps.clear();
        const TruthEval ev = evalTruth(d, owners_, samples_);
        if (std::find(ev.elems.begin(), ev.elems.end(), s.index) == ev.elems.end()) {
            d.why = "not found at baseline";
            return d;
        }
        d.weak = true;
        d.cat += " (weak oracle)";
    } else if (evalTruth(d, owners_, samples_).merged) {
        d.why = "baseline self-check not full";
        return d;
    } else if (s.kind == SubKind::Vertex && fs.size() < 3) {
        // A vertex on only two faces (the seam vertex of a hole rim) is just "some point of that curve".
        d.weak = true;
        d.cat += " (weak oracle)";
    }
    d.ok = true;
    return d;
}

std::vector<std::pair<std::uint32_t, TruthDesc>> pickTarget(const Target& t, const Owners& ow, const Samples& smp)
{
    std::vector<std::pair<std::uint32_t, TruthDesc>> out;
    if (!ow.get(t.a) || !ow.get(t.b))
        return out;
    TruthDesc d;
    d.kind = SubKind::Edge;
    d.cat = "feature pick";
    d.cons.push_back(Constraint{{t.a}, {}});
    d.cons.push_back(Constraint{{t.b}, {}});
    const TruthEval ev = evalTruth(d, ow, smp);
    for (const std::uint32_t e : ev.elems) {
        TruthDesc x = d;
        x.ok = findSeps(x, e, x.seps, ow, smp);
        if (!x.ok) {
            x.why = "pick not unique";
        } else if (evalTruth(x, ow, smp).merged) {
            x.ok = false;
            x.why = "pick self-check not full";
        }
        out.push_back({e, x});
    }
    return out;
}

const char* verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Correct:
        return "correct";
    case Verdict::BrokenGone:
        return "broken and really gone";
    case Verdict::Missed:
        return "missed (broken but exists)";
    case Verdict::Unjudged:
        return "NOT JUDGED (continuation unknown)";
    case Verdict::WrongOther:
        return "WRONG different element";
    case Verdict::WrongIncomplete:
        return "WRONG incomplete set";
    case Verdict::WrongGhost:
        return "WRONG ghost (should be gone)";
    case Verdict::WrongSilentMerge:
        return "WRONG unflagged merge";
    }
    return "?";
}

Verdict judge(Answer answer, std::vector<std::uint32_t> X, const TruthEval& te)
{
    std::sort(X.begin(), X.end());
    X.erase(std::unique(X.begin(), X.end()), X.end());
    const std::vector<std::uint32_t>& E = te.elems;
    if (answer == Answer::Broken)
        return E.empty() ? Verdict::BrokenGone : Verdict::Missed;
    if (X.empty())
        return E.empty() ? Verdict::WrongGhost : Verdict::WrongOther;
    if (X == E) {
        if (answer == Answer::Exact && te.merged)
            return Verdict::WrongSilentMerge;
        return Verdict::Correct;
    }
    if (E.empty())
        return Verdict::WrongGhost;
    if (std::includes(E.begin(), E.end(), X.begin(), X.end()))
        return Verdict::WrongIncomplete;
    return Verdict::WrongOther;
}

std::vector<std::uint32_t> congruent(const ElemSamples& picked, SubKind kind, const Samples& smp, double tol)
{
    std::vector<std::uint32_t> out;
    if (picked.nodes.empty())
        return out;
    const auto& E = smp.of(kind);
    // Point-to-element distance: to the triangles of a face, the polyline of an edge, the point of a vertex.
    auto near = [&](const Vec3& p, const ElemSamples& es) {
        if (outsideBox(p, es.boxMin, es.boxMax, tol))
            return false;
        if (kind == SubKind::Face)
            return es.mesh && es.mesh->distanceWithin(p, tol) <= tol;
        if (kind == SubKind::Edge) {
            for (std::size_t k = 1; k < es.nodes.size(); ++k)
                if (segmentDistance(p, es.nodes[k - 1], es.nodes[k]) <= tol)
                    return true;
            return false;
        }
        return norm(sub(p, es.nodes.front())) <= tol;
    };
    for (std::uint32_t x = 0; x < E.size(); ++x) {
        const ElemSamples& es = E[x];
        if (es.nodes.empty() || es.type != picked.type)
            continue;
        bool ok = true;
        for (const Vec3& p : picked.nodes)
            if (!near(p, es)) {
                ok = false;
                break;
            }
        for (std::size_t k = 0; ok && k < es.nodes.size(); ++k)
            if (!near(es.nodes[k], picked))
                ok = false;
        if (ok)
            out.push_back(x);
    }
    return out;
}

namespace {

// Is p within tol of element es (its triangles, polyline or point; for a face without triangles, its boundary
// samples)?
bool nearElement(const Vec3& p, const ElemSamples& es, SubKind kind, double tol)
{
    if (es.nodes.empty() || outsideBox(p, es.boxMin, es.boxMax, tol))
        return false;
    if (kind == SubKind::Face) {
        if (es.mesh && !es.mesh->triangles.empty())
            return es.mesh->distanceWithin(p, tol) <= tol;
        for (const Vec3& q : es.pts)
            if (norm(sub(p, q)) <= tol)
                return true;
        return false;
    }
    if (kind == SubKind::Edge) {
        for (std::size_t k = 1; k < es.nodes.size(); ++k)
            if (segmentDistance(p, es.nodes[k - 1], es.nodes[k]) <= tol)
                return true;
        return es.nodes.size() == 1 && norm(sub(p, es.nodes.front())) <= tol;
    }
    return norm(sub(p, es.nodes.front())) <= tol;
}

double overlapTolerance(SubKind kind)
{
    return kind == SubKind::Vertex ? 1e-3 : 0.05;
}

// Share of a's samples (by weight) that lie on b.
double shareOn(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    double on = 0, all = 0;
    for (std::size_t i = 0; i < a.pts.size(); ++i) {
        all += a.w[i];
        if (nearElement(a.pts[i], b, kind, overlapTolerance(kind)))
            on += a.w[i];
    }
    return all > 0 ? on / all : 0;
}

// How near a point of one element must be to another, for the mapped pick and siblings (all kinds: a mapped vertex
// is a projection, not an exact point).
constexpr double kAnchorTolerance = 0.05;

// A mapped point may be off by up to a fifth of the anchor tolerance (mapElement): within this much of a boundary,
// a point counts as beside the element.
constexpr double kBesideMargin = 0.2 * kAnchorTolerance;

// Is p within tol of element es, and not merely beside it? A point beyond an edge's end or beyond a face's boundary
// is as near to that boundary as to the element; a point on the element is nearer to the element than to its
// boundary. So a short piece next to the picked one (sharing a vertex or an edge) does not overlap it, however small
// either is.
// How much nearer p is to element es than to its boundary (large where es has no boundary). tol: how far p may be.
double depthIn(const Vec3& p, const ElemSamples& es, SubKind kind, double tol)
{
    if (kind == SubKind::Edge && es.nodes.size() >= 2) {
        if (norm(sub(es.nodes.front(), es.nodes.back())) <= 1e-9)
            return 1e300; // closed: no boundary
        double toEdge = 1e300;
        for (std::size_t k = 1; k < es.nodes.size(); ++k)
            toEdge = std::min(toEdge, segmentDistance(p, es.nodes[k - 1], es.nodes[k]));
        return std::min(norm(sub(p, es.nodes.front())), norm(sub(p, es.nodes.back()))) - toEdge;
    }
    if (kind == SubKind::Face && es.mesh && !es.mesh->triangles.empty()) {
        const double toFace = es.mesh->distanceWithin(p, tol, true);
        double toBoundary = 1e300;
        for (const auto& s : es.mesh->boundary)
            toBoundary = std::min(toBoundary, segmentDistance(p, s[0], s[1]));
        return toBoundary - toFace;
    }
    return 1e300;
}

// Within this much of a boundary a point counts as beside the element: a mapped point may be off by up to a fifth
// of the anchor tolerance (mapElement), but never more than half the depth of the element's own deepest sample (a
// sliver has hardly any interior).
double besideMargin(const ElemSamples& es, SubKind kind)
{
    double deepest = 0;
    for (const Vec3& p : es.pts) {
        deepest = std::max(deepest, depthIn(p, es, kind, kAnchorTolerance));
        if (deepest >= 2.0 * kBesideMargin)
            break;
    }
    return std::min(kBesideMargin, 0.5 * deepest);
}

// No sample more than 2 x kBesideMargin inside it (a face 0.04 mm wide, an edge 0.04 mm long).
bool sliver(const ElemSamples& es, SubKind kind)
{
    if (kind == SubKind::Vertex)
        return false;
    if (norm(sub(es.boxMax, es.boxMin)) <= 4.0 * kBesideMargin)
        return true; // also a face without triangles that small
    return besideMargin(es, kind) < kBesideMargin;
}

bool nearInterior(const Vec3& p, const ElemSamples& es, SubKind kind, double tol, double margin)
{
    return nearElement(p, es, kind, tol) && depthIn(p, es, kind, tol) > margin;
}

double interiorShare(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    const double margin = besideMargin(b, kind);
    double on = 0, all = 0;
    for (std::size_t i = 0; i < a.pts.size(); ++i) {
        all += a.w[i];
        if (nearInterior(a.pts[i], b, kind, kAnchorTolerance, margin))
            on += a.w[i];
    }
    return all > 0 ? on / all : 0;
}

// Do two elements share territory (10 % of either one's samples inside the other)?
bool overlap(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    return interiorShare(a, b, kind) >= 0.1 || interiorShare(b, a, kind) >= 0.1;
}

// Could a point of a face of the new shape lie ON baseline face b? Faces that meet at an angle share only their
// boundary; only faces on the same plane (or curved faces) can hold the same area.
bool sameSheet(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    const int plane = static_cast<int>(kernel::SurfaceType::Plane);
    return kind != SubKind::Face || a.type != plane || b.type != plane || std::abs(dot(a.nrm, b.nrm)) > 0.99;
}

// Is b another surface than a (not merely another reference point on the same surface)?
bool carrierChanged(const Carrier& a, const Carrier& b)
{
    using K = Carrier::K;
    constexpr double eps = 1e-9;
    if (a.k != b.k)
        return true;
    if (!a.analytic())
        return false; // an imported face's mesh carrier never moves
    if (norm(sub(a.a, b.a)) > eps || a.sigma != b.sigma || std::abs(a.r - b.r) > eps || std::abs(a.r2 - b.r2) > eps)
        return true;
    const Vec3 d = sub(b.p, a.p);
    switch (a.k) {
    case K::Plane:
        return std::abs(dot(d, a.a)) > eps;
    case K::Cyl:
        return norm(sub(d, mul(a.a, dot(d, a.a)))) > eps;
    default:
        break;
    }
    if (norm(d) > eps)
        return true;
    for (int i = 0; i < 4; ++i)
        if (std::abs(a.q[i] - b.q[i]) > eps)
            return true;
    return false;
}

// How a changed carrier moved: the shift of its reference point (which moves with its feature, sideways too), less
// the part along a cylinder's axis (which changes nothing). Zero for an unchanged carrier.
Vec3 carrierShift(const Carrier& a, const Carrier& b)
{
    if (!a.analytic() || !b.analytic() || !carrierChanged(a, b))
        return Vec3{0, 0, 0};
    const Vec3 d = sub(b.p, a.p);
    if (a.k == Carrier::K::Cyl)
        return sub(d, mul(a.a, dot(d, a.a)));
    return d;
}

// A sweep that only moved (same profile, depth and orientation, another origin): its shift, else nullopt.
std::optional<Vec3> pureMove(const Feature& a, const Feature& b)
{
    auto same = [](const Vec3& x, const Vec3& y) {
        return norm(sub(x, y)) <= 1e-12;
    };
    if (a.op != b.op || a.dist != b.dist || !same(a.frame.n, b.frame.n) || !same(a.frame.xd, b.frame.xd) ||
        a.loops.size() != b.loops.size() || same(a.frame.o, b.frame.o))
        return std::nullopt;
    for (std::size_t i = 0; i < a.loops.size(); ++i) {
        if (a.loops[i].ents.size() != b.loops[i].ents.size())
            return std::nullopt;
        for (std::size_t k = 0; k < a.loops[i].ents.size(); ++k) {
            const Ent &x = a.loops[i].ents[k], &y = b.loops[i].ents[k];
            if (x.k != y.k || x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1 || x.cx != y.cx ||
                x.cy != y.cy || x.r != y.r)
                return std::nullopt;
        }
    }
    return sub(b.frame.o, a.frame.o);
}

// Is p inside the volume sweep f swept, at least m from its boundary? (A point on the boundary lies on the sweep's
// own faces, which move by their own carriers.)
bool insideSweep(const Feature& f, const Vec3& p, double m)
{
    const Vec3 d = sub(p, f.frame.o);
    const double u = dot(d, f.frame.xd), v = dot(d, f.frame.yd());
    const double h = (f.op == BOp::Cut ? -1.0 : 1.0) * dot(d, f.frame.n);
    if (h < m || h > f.dist - m)
        return false;
    for (const Loop& l : f.loops)
        if (insideLoop(l, u, v) && distLoop(l, u, v) > m)
            return true;
    return false;
}

TruthDesc withoutSeparators(const TruthDesc& d)
{
    TruthDesc out = d;
    out.seps.clear();
    for (Constraint& c : out.cons)
        c.seps.clear();
    return out;
}

const char* answerName(Answer a)
{
    switch (a) {
    case Answer::Exact:
        return "Exact";
    case Answer::Split:
        return "Split";
    case Answer::Merged:
        return "Merged";
    case Answer::Repaired:
        return "Repaired";
    case Answer::Broken:
        return "Broken";
    }
    return "?";
}

std::string listOf(const std::vector<std::uint32_t>& v)
{
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i)
        s += (i ? " " : "") + std::to_string(v[i]);
    return s + "}";
}

} // namespace

ElemSamples mapElement(const Samples& baseline, SubId element, const TruthDesc& d, const Owners& before,
                       const Owners& after, bool& moved)
{
    const SubKind kind = element.kind;
    const ElemSamples& es = baseline.of(kind)[element.index];
    moved = false;
    ElemSamples out;
    // Own carriers changed?
    bool ownChanged = false;
    for (const Constraint& c : d.cons)
        for (const auto& alt : c.alts) {
            const Owner* o0 = before.get(alt);
            const Owner* o1 = after.get(alt);
            ownChanged = ownChanged || o0 == nullptr || o1 == nullptr || carrierChanged(o0->c, o1->c);
        }
    // The common shift of the bounding features (owners its vertices lie on, other than its own).
    Vec3 bound{0, 0, 0};
    if (!ownChanged && kind != SubKind::Vertex) {
        std::set<std::string> own;
        for (const Constraint& c : d.cons)
            own.insert(c.alts.begin(), c.alts.end());
        std::vector<std::uint32_t> vertices;
        if (kind == SubKind::Edge) {
            vertices = baseline.verticesOf[element.index];
        } else {
            for (std::uint32_t v = 0; v < baseline.facesOf[0].size(); ++v) {
                const auto& fs = baseline.facesOf[0][v];
                if (std::find(fs.begin(), fs.end(), element.index) != fs.end())
                    vertices.push_back(v);
            }
        }
        // Each vertex moves with the changed carriers it lies on (if they agree), or stays; the element moves only
        // if all its vertices move alike (a piece between two cuts that both moved).
        bool first = true, agree = true;
        for (const std::uint32_t v : vertices) {
            const Vec3 p = baseline.of(SubKind::Vertex)[v].nodes.front();
            Vec3 at{0, 0, 0};
            bool any = false;
            for (const Owner& o : before.v) {
                if (own.count(o.key) != 0 || !o.c.analytic() || !o.c.on(p, 1e-3) || !o.in(p, 1e-2))
                    continue;
                const Owner* o1 = after.get(o.key);
                if (o1 == nullptr || !carrierChanged(o.c, o1->c))
                    continue;
                const Vec3 t = carrierShift(o.c, o1->c);
                if (any && norm(sub(t, at)) > 1e-6)
                    agree = false;
                at = t;
                any = true;
            }
            if (first)
                bound = at;
            else if (norm(sub(at, bound)) > 1e-6)
                agree = false;
            first = false;
        }
        if (!agree)
            bound = Vec3{0, 0, 0};
        if (norm(bound) > 1e-9)
            moved = true;
    }
    out.type = es.type;
    out.nrm = es.nrm;
    out.sided = es.sided;
    out.sigma = es.sigma;
    auto check = [&](const std::string& key) {
        const Owner* o0 = before.get(key);
        const Owner* o1 = after.get(key);
        if (o0 == nullptr || o1 == nullptr || carrierChanged(o0->c, o1->c))
            moved = true;
    };
    for (const Sep& s : d.seps)
        check(s.owner);
    for (const Constraint& c : d.cons) {
        for (const auto& alt : c.alts)
            check(alt);
        for (const Sep& s : c.seps)
            check(s.owner);
    }
    // Sweeps that only moved: what lies in the volume one swept (the part of a hole wall inside a moved boss) moves
    // with it.
    std::vector<std::pair<const Feature*, Vec3>> movedSweeps;
    for (const auto& [id, f] : before.sweeps) {
        const auto it = after.sweeps.find(id);
        if (it == after.sweeps.end())
            continue;
        if (const auto t = pureMove(f, it->second))
            movedSweeps.push_back({&f, *t});
    }
    auto mapPoint = [&](const Vec3& p, Vec3& q) {
        Vec3 shift{0, 0, 0};
        double best = 0;
        const Vec3* withSweep = nullptr;
        for (const auto& [f, t] : movedSweeps)
            if (insideSweep(*f, p, kAnchorTolerance)) {
                withSweep = &t;
                break;
            }
        std::vector<const Carrier*> onto;
        for (const Constraint& c : d.cons) {
            const Owner* o0 = nullptr;
            const Owner* o1 = nullptr;
            for (const auto& alt : c.alts) {
                const Owner* b = before.get(alt);
                if (b == nullptr || (c.alts.size() > 1 && !b->c.on(p, 0.1)))
                    continue;
                o0 = b;
                o1 = after.get(alt);
                break;
            }
            if (o0 == nullptr || o1 == nullptr)
                return false;
            if (o0->c.analytic() && o1->c.analytic()) {
                const Vec3 t = carrierShift(o0->c, o1->c);
                if (norm(t) > best) {
                    best = norm(t);
                    shift = t;
                }
                onto.push_back(&o1->c);
            }
        }
        if (withSweep != nullptr)
            moved = true;
        q = add(p, withSweep != nullptr ? *withSweep : ownChanged ? shift : bound);
        double worst = 0;
        // Alternating projections; carriers that meet at a small angle (a blend and its tangent face) converge
        // slowly. A point that settles within a fifth of the anchor tolerance of every carrier is used.
        for (int it = 0; it < 200; ++it) {
            for (const Carrier* c : onto)
                q = projectAny(*c, q);
            worst = 0;
            for (const Carrier* c : onto)
                worst = std::max(worst, std::abs(c->dist(q)));
            if (worst <= 1e-7)
                break;
        }
        return worst <= 0.2 * kAnchorTolerance;
    };
    double all = 0, mappedWeight = 0;
    for (std::size_t i = 0; i < es.pts.size(); ++i) {
        all += es.w[i];
        Vec3 q{};
        if (mapPoint(es.pts[i], q)) {
            out.pts.push_back(q);
            out.w.push_back(es.w[i]);
            mappedWeight += es.w[i];
        }
    }
    if (!(all > 0) || mappedWeight < 0.5 * all) {
        out.pts.clear();
        out.w.clear();
        return out;
    }
    bool everyNode = true;
    for (const Vec3& p : es.nodes) {
        Vec3 q{};
        if (mapPoint(p, q))
            out.nodes.push_back(q);
        else
            everyNode = false;
    }
    if (kind == SubKind::Face && everyNode && es.mesh && !es.mesh->triangles.empty() &&
        es.mesh->nodes.size() == out.nodes.size()) {
        auto fm = std::make_shared<FaceMesh>();
        fm->nodes = out.nodes;
        fm->triangles = es.mesh->triangles;
        for (const auto& tri : fm->triangles) {
            Vec3 lo{}, hi{};
            for (int k = 0; k < 3; ++k)
                growBox(lo, hi, fm->nodes[tri[static_cast<std::size_t>(k)]], k == 0);
            fm->triangleBoxes.push_back({lo, hi});
        }
        for (std::size_t i = 0; i < fm->nodes.size(); ++i)
            growBox(fm->boxMin, fm->boxMax, fm->nodes[i], i == 0);
        fm->findBoundary();
        out.mesh = fm;
    }
    if (out.nodes.empty())
        out.nodes = out.pts;
    bool first = true;
    for (const auto* v : {&out.nodes, &out.pts})
        for (const Vec3& p : *v) {
            growBox(out.boxMin, out.boxMax, p, first);
            first = false;
        }
    return out;
}

double shareOnElement(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    return shareOn(a, b, kind);
}

bool isSliver(const ElemSamples& es, SubKind kind)
{
    return sliver(es, kind);
}

std::vector<int> componentsOf(const std::vector<std::uint32_t>& elems, SubKind kind, const Samples& smp)
{
    std::vector<std::size_t> parent(elems.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    auto find = [&](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    auto unite = [&](std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    };
    auto indexOf = [&](std::uint32_t e) -> long {
        const auto it = std::lower_bound(elems.begin(), elems.end(), e);
        return it != elems.end() && *it == e ? static_cast<long>(it - elems.begin()) : -1;
    };
    if (kind == SubKind::Face) {
        for (const auto& faces : smp.facesOf[1]) {
            long firstIn = -1;
            for (const std::uint32_t f : faces) {
                const long i = indexOf(f);
                if (i < 0)
                    continue;
                if (firstIn < 0)
                    firstIn = i;
                else
                    unite(static_cast<std::size_t>(firstIn), static_cast<std::size_t>(i));
            }
        }
    } else if (kind == SubKind::Edge) {
        std::map<std::uint32_t, std::size_t> firstAt;
        for (std::size_t i = 0; i < elems.size(); ++i)
            for (const std::uint32_t v : smp.verticesOf[elems[i]]) {
                const auto [it, fresh] = firstAt.try_emplace(v, i);
                if (!fresh)
                    unite(it->second, i);
            }
    }
    std::vector<int> out(elems.size(), -1);
    std::map<std::size_t, int> ids;
    for (std::size_t i = 0; i < elems.size(); ++i) {
        const auto [it, fresh] = ids.try_emplace(find(i), static_cast<int>(ids.size()));
        out[i] = it->second;
    }
    return out;
}

bool Expectation::inLogical(std::uint32_t e) const
{
    return std::binary_search(logical.begin(), logical.end(), e);
}

int Expectation::componentOf(std::uint32_t e) const
{
    const auto it = std::lower_bound(logical.begin(), logical.end(), e);
    return it != logical.end() && *it == e ? component[static_cast<std::size_t>(it - logical.begin())] : -1;
}

bool Expectation::isAnchored(std::uint32_t e) const
{
    const auto it = std::lower_bound(logical.begin(), logical.end(), e);
    return it != logical.end() && *it == e && anchored[static_cast<std::size_t>(it - logical.begin())] != 0;
}

bool Expectation::isClaimed(std::uint32_t e) const
{
    const auto it = std::lower_bound(logical.begin(), logical.end(), e);
    return it != logical.end() && *it == e && !claimed.empty() &&
           claimed[static_cast<std::size_t>(it - logical.begin())] != 0;
}

Expectation expect(const PickRecord& pick, const Owners& now, const Samples& smp)
{
    Expectation x;
    const TruthDesc& d = pick.truth;
    x.kind = d.kind;
    x.weak = d.weak;
    x.strict = evalTruth(d, now, smp);
    const TruthEval L = evalTruth(withoutSeparators(d), now, smp);
    std::set_union(L.elems.begin(), L.elems.end(), x.strict.elems.begin(), x.strict.elems.end(),
                   std::back_inserter(x.logical));
    x.component = componentsOf(x.logical, x.kind, smp);
    x.mapped = mapElement(*pick.baseline, pick.base, d, *pick.owners, now, x.moved);
    const auto& N = smp.of(x.kind);
    x.anchored.resize(x.logical.size(), 0);
    for (std::size_t i = 0; i < x.logical.size(); ++i)
        x.anchored[i] = !x.mapped.pts.empty() && overlap(N[x.logical[i]], x.mapped, x.kind) ? 1 : 0;
    if (x.weak) {
        // The other pieces of the logical element at pick time, mapped through the edit: what overlaps one of
        // them is that piece's continuation.
        x.claimed.resize(x.logical.size(), 0);
        for (const std::uint32_t s : evalTruth(withoutSeparators(d), *pick.owners, *pick.baseline).elems) {
            if (s == pick.base.index)
                continue;
            ++x.siblings;
            bool unused = false;
            const ElemSamples sibling = mapElement(*pick.baseline, SubId{x.kind, s}, d, *pick.owners, now, unused);
            if (sibling.pts.empty())
                continue;
            for (std::size_t i = 0; i < x.logical.size(); ++i)
                if (x.claimed[i] == 0 && overlap(N[x.logical[i]], sibling, x.kind))
                    x.claimed[i] = 1;
        }
    }
    const auto cong = congruent(pick.picked(), x.kind, smp, overlapTolerance(x.kind));
    x.congruentIndex = cong.size() == 1 ? static_cast<long>(cong[0]) : -1;
    if (x.strict.elems.empty())
        return x;
    // Slivers are left out of the expectation (isSliver), unless nothing else is left.
    auto withoutSlivers = [&](TruthEval& te) {
        TruthEval kept;
        for (const std::uint32_t e : te.elems)
            if (!sliver(N[e], x.kind))
                kept.elems.push_back(e);
        if (kept.elems.empty() || kept.elems.size() == te.elems.size())
            return;
        for (const std::uint32_t e : te.partial)
            if (std::binary_search(kept.elems.begin(), kept.elems.end(), e))
                kept.partial.push_back(e);
        kept.merged = !kept.partial.empty();
        te = kept;
    };
    std::set<int> strictParts, anchoredParts;
    for (const std::uint32_t e : x.strict.elems)
        if (!sliver(N[e], x.kind))
            strictParts.insert(x.componentOf(e));
    for (std::size_t i = 0; i < x.logical.size(); ++i)
        if (x.anchored[i] != 0)
            anchoredParts.insert(x.component[i]);
    if ((!x.weak || x.siblings == 0) && strictParts.size() <= 1) {
        // The description (with its separators) picks out one connected part: that is the continuation. So does a
        // weak one when the picked element was the only piece of its logical element.
        x.expected = x.strict;
        withoutSlivers(x.expected);
        return x;
    }
    // A weak reference keeps, of those parts, the pieces over the mapped pick and the pieces over no other baseline
    // piece (another piece's continuation is not its own).
    for (const std::uint32_t e : x.strict.elems)
        if (anchoredParts.count(x.componentOf(e)) != 0 && (!x.weak || x.isAnchored(e) || !x.isClaimed(e)))
            x.expected.elems.push_back(e);
    if (x.weak && x.expected.elems.empty() && !anchoredParts.empty())
        return x; // only other pieces' continuations are left: the picked piece is gone
    for (const std::uint32_t e : x.expected.elems)
        if (std::binary_search(x.strict.partial.begin(), x.strict.partial.end(), e) || (x.weak && x.isClaimed(e)))
            x.expected.partial.push_back(e);
    x.expected.merged = !x.expected.partial.empty();
    withoutSlivers(x.expected);
    if (x.kind == SubKind::Vertex && x.expected.elems.size() >= 2) {
        x.twins = true;
        const Vec3 p = N[x.expected.elems.front()].nodes.front();
        for (const std::uint32_t e : x.expected.elems)
            x.twins = x.twins && norm(sub(N[e].nodes.front(), p)) <= 1e-4;
    }
    if (x.expected.elems.empty())
        x.noAnchor = x.mapped.pts.empty() ? "the picked element cannot be mapped through the edit"
                                          : "no part of the logical element overlaps the picked element mapped "
                                            "through the edit";
    return x;
}

namespace {

// The new-territory rule (oracle.h). `v` is the set verdict.
bool newTerritory(const Expectation& x, const PickRecord& pick, const Owners& now, const Samples& smp, Answer answer,
                  const std::vector<std::uint32_t>& X, Verdict v, std::string& why)
{
    const std::vector<std::uint32_t>& E = x.expected.elems;
    if (x.moved || E.empty() || x.mapped.pts.empty() || answer == Answer::Merged)
        return false;
    if (!std::includes(X.begin(), X.end(), E.begin(), E.end()))
        return false; // never drop an expected element
    const SubKind kind = x.kind;
    const auto& N = smp.of(kind);
    const auto& B = pick.baseline->of(kind);
    const ElemSamples& P = pick.picked();
    // Does point p (of element `of` of the new shape) lie on a baseline element? `skipPicked`: not counting the
    // picked element itself.
    auto onBaseline = [&](const Vec3& p, const ElemSamples& of, bool skipPicked) {
        for (std::uint32_t i = 0; i < B.size(); ++i) {
            if (skipPicked && i == pick.base.index)
                continue;
            if (sameSheet(of, B[i], kind) && nearElement(p, B[i], kind, overlapTolerance(kind)))
                return true;
        }
        return false;
    };
    std::vector<std::uint32_t> extra;
    std::set_difference(X.begin(), X.end(), E.begin(), E.end(), std::back_inserter(extra));
    for (const std::uint32_t e : extra) {
        if (!x.inLogical(e))
            return false;
        bool linked = false;
        for (const std::uint32_t a : X)
            linked = linked || (x.isAnchored(a) && x.componentOf(a) == x.componentOf(e));
        if (!linked)
            return false;
        for (const Vec3& p : N[e].pts)
            if (onBaseline(p, N[e], false))
                return false;
    }
    // An expected element larger than the description, answered without Merged: the part outside the description
    // must be new too.
    std::vector<std::uint32_t> partial;
    for (const std::uint32_t e : x.expected.partial)
        partial.push_back(e);
    if (!partial.empty()) {
        std::vector<std::pair<std::uint32_t, std::vector<char>>> masks;
        evalTruthImpl(pick.truth, now, smp, &masks);
        for (const std::uint32_t e : partial) {
            const std::vector<char>* mask = nullptr;
            for (const auto& [m, bits] : masks)
                if (m == e)
                    mask = &bits;
            if (mask == nullptr)
                return false;
            for (std::size_t j = 0; j < N[e].pts.size(); ++j) {
                if ((*mask)[j] != 0)
                    continue;
                const Vec3& p = N[e].pts[j];
                if (nearElement(p, P, kind, overlapTolerance(kind)))
                    continue; // on what was picked
                if (onBaseline(p, N[e], true))
                    return false;
            }
        }
    }
    why = std::string(answerName(answer)) + " " + listOf(X) + " where the oracle expects " + listOf(E) +
          (x.expected.merged ? " (larger than described)" : "") + " [" + verdictName(v) +
          "]: the difference lies on no element of the baseline and is connected to the picked element";
    return true;
}

} // namespace

Judgement judgeAnswer(const Expectation& x, const PickRecord& pick, const Owners& now, const Samples& smp,
                      Answer answer, std::vector<std::uint32_t> X)
{
    Judgement j;
    std::sort(X.begin(), X.end());
    X.erase(std::unique(X.begin(), X.end()), X.end());
    const auto& N = smp.of(x.kind);
    for (const std::uint32_t e : X)
        if (e >= N.size()) {
            j.verdict = Verdict::WrongOther; // not an element of the shape at all
            return j;
        }
    // Slivers are left out of the answer as out of the expectation; an answer of slivers alone is judged as it is.
    {
        std::vector<std::uint32_t> kept;
        for (const std::uint32_t e : X)
            if (!sliver(N[e], x.kind))
                kept.push_back(e);
        if (!kept.empty())
            X = std::move(kept);
    }
    const std::vector<std::uint32_t>& E = x.expected.elems;
    if (x.twins && (answer == Answer::Exact || answer == Answer::Repaired) && X.size() == 1 &&
        std::binary_search(E.begin(), E.end(), X[0])) {
        j.verdict = Verdict::Correct;
        j.twin = true;
        return j;
    }
    if (!x.noAnchor.empty()) {
        if (answer == Answer::Broken) {
            j.verdict = Verdict::Missed;
        } else {
            j.verdict = Verdict::Unjudged;
            j.unjudged = true;
        }
        return j;
    }
    if (x.weak) {
        if (answer == Answer::Broken)
            j.verdict = E.empty() ? Verdict::BrokenGone : Verdict::Missed;
        else if (E.empty())
            j.verdict = Verdict::WrongGhost;
        else if (X.empty() || !std::includes(E.begin(), E.end(), X.begin(), X.end()))
            j.verdict = Verdict::WrongOther;
        else if (answer == Answer::Exact || answer == Answer::Repaired) {
            // The one piece that continues the pick; an element that also continues another baseline piece is a
            // merge, and with two or more pieces the pick came apart.
            if (X.size() != 1)
                j.verdict = Verdict::WrongOther;
            else if (X != E)
                j.verdict = Verdict::WrongIncomplete;
            else
                j.verdict = x.expected.merged ? Verdict::WrongSilentMerge : Verdict::Correct;
        } else
            j.verdict = X == E ? Verdict::Correct : Verdict::WrongIncomplete;
        return j;
    }
    j.verdict = judge(answer, X, x.expected);
    if (isWrong(j.verdict) && answer != Answer::Broken &&
        newTerritory(x, pick, now, smp, answer, X, j.verdict, j.accepted))
        j.verdict = Verdict::Correct;
    return j;
}

} // namespace ray::naming_oracle
