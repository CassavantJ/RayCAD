// The naming oracle (task T-081a). See oracle.h. Ported from spikes/s4-naming/truth.cpp: OCCT calls replaced by
// the public kernel queries, owners for imported faces added.
#include "oracle.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
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
            // The chamfer face's outward normal lies between its parents' outward normals, convex or concave (the
            // material it removes or adds is on the side both parents' material is). With it, a face back to back
            // with the chamfer on the same plane (a boss's start cap left on it) is told apart by its material side.
            if (A.c.oriented && B.c.oriented) {
                if (dot(out.a, add(nA, nB)) < 0)
                    out.a = mul(out.a, -1.0);
                out.oriented = true;
            }
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
namespace {

// A direct edit's change to the owners (oracle.h EditSpec).
void applyEdit(Owners& O, const Feature& f)
{
    const EditSpec& e = f.edit;
    std::set<std::string> touched;
    auto touch = [&](const std::string& key) {
        touched.insert(key);
        O.touchedByEdit.insert(key);
        if (const Owner* o = O.get(key); o != nullptr && o->cat == "imported face")
            O.importEdited = true;
    };
    auto reindex = [&] {
        O.ix.clear();
        for (std::size_t i = 0; i < O.v.size(); ++i)
            O.ix[O.v[i].key] = static_cast<int>(i);
    };
    switch (e.kind) {
    case EKind::Delete: {
        const std::vector<std::string>& gone = e.removed.empty() ? e.targets : e.removed;
        const std::set<std::string> drop(gone.begin(), gone.end());
        for (const std::string& key : drop)
            touch(key);
        O.removedByEdit.insert(drop.begin(), drop.end());
        std::vector<Owner> kept;
        for (Owner& o : O.v)
            if (drop.count(o.key) == 0)
                kept.push_back(std::move(o));
        O.v = std::move(kept);
        reindex();
        break;
    }
    case EKind::Offset:
        for (const std::string& key : e.targets) {
            const auto it = O.ix.find(key);
            if (it == O.ix.end())
                continue;
            Owner& o = O.v[static_cast<std::size_t>(it->second)];
            touch(key);
            if (o.c.k != Carrier::K::Plane || !o.c.oriented) {
                O.undescribable.insert(key);
                continue;
            }
            const Vec3 t = mul(o.c.a, e.amount);
            o.c.p = add(o.c.p, t);
            const auto old = o.in;
            o.in = [old, t](const Vec3& p, double m) {
                return old(sub(p, t), m);
            };
        }
        break;
    case EKind::Resize:
        for (const std::string& key : e.resized.empty() ? e.targets : e.resized) {
            const auto it = O.ix.find(key);
            if (it == O.ix.end())
                continue;
            Owner& o = O.v[static_cast<std::size_t>(it->second)];
            touch(key);
            if (o.c.k != Carrier::K::Cyl || !(e.amount > 0)) {
                O.undescribable.insert(key);
                continue;
            }
            const Carrier c0 = o.c;
            o.c.r = e.amount;
            const auto old = o.in;
            // The extent carried radially: a point at the new radius is inside where the point at the old radius was.
            o.in = [old, c0](const Vec3& p, double m) {
                const Vec3 d = sub(p, c0.p);
                const double z = dot(d, c0.a);
                const Vec3 radial = sub(d, mul(c0.a, z));
                const double rho = norm(radial);
                const Vec3 q = rho > 1e-12 ? add(add(c0.p, mul(c0.a, z)), mul(radial, c0.r / rho)) : p;
                return old(q, m);
            };
        }
        break;
    case EKind::Unify:
        break;
    }
    // Delete, Resize: the faces that bordered the removed faces or the bore run on over the ground they left.
    if (!e.fill.empty()) {
        const auto fill = e.fill;
        const auto reach = e.fillReach;
        const std::set<std::string> grown(e.grown.begin(), e.grown.end());
        for (Owner& o : O.v) {
            if (grown.count(o.key) == 0)
                continue;
            O.grownByEdit.insert(o.key);
            const auto old = o.in;
            o.in = [old, fill, reach](const Vec3& p, double m) {
                if (old(p, m))
                    return true;
                for (std::size_t i = 0; i < fill.size() && i < reach.size(); ++i) {
                    const double limit = reach[i] + m;
                    if (fill[i] && fill[i]->distanceWithin(p, limit) <= limit)
                        return true;
                }
                return false;
            };
        }
    }
    // A sweep with an edited face is no longer the sweep the script describes: its volume is not what it moved or
    // kept (mapElement).
    for (const std::string& key : touched)
        O.sweeps.erase(key.substr(0, key.find('/')));
}

} // namespace

Owners buildOwners(const Script& script)
{
    static std::atomic<std::uint64_t> serials{0};
    Owners O;
    O.serial = ++serials;
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
            O.import = f.base;
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
        } else if (f.type == FType::Edit) {
            applyEdit(O, f);
        } else {
            const bool fillet = f.type == FType::Fillet;
            for (std::size_t k = 0; k < f.targets.size(); ++k) {
                const Owner *A = O.get(f.targets[k].a), *B = O.get(f.targets[k].b);
                const std::string key = f.id + "/b" + std::to_string(k);
                if (!A || !B || !A->c.analytic() || !B->c.analytic()) {
                    O.undescribable.insert(key);
                    continue;
                }
                Owner o;
                o.key = key;
                o.cat = fillet ? "fillet face" : "chamfer face";
                if (!blendCarrier(*A, *B, fillet, f.targets[k].convex, f.size, o.c)) {
                    O.undescribable.insert(key);
                    continue;
                }
                // A torus whose tube is as wide as its ring (a fillet at least as large as the corner arc it rounds)
                // may or may not be what the kernel builds (Owners::degenerate).
                if (o.c.k == Carrier::K::Torus && !(o.c.r > o.c.r2))
                    O.degenerate.insert(key);
                const Owner a = *A, b = *B;
                const double rho = f.size;
                // Within the blend size of both carriers, over both extents. The extents get a little more room
                // (5 % of the size): where a blend runs onto another blend, the parents' own extents are
                // approximations at their ends. Along a parent, a blend reaches past the parent's own face by as much
                // as the largest other blend on that parent cut its end back (a fillet that runs into a larger
                // chamfer at a corner, random 16:15); it stays within its own size of both carriers in any case.
                const double slack = 0.01 + 0.05 * rho;
                double reach = rho;
                for (const Feature& g : script)
                    if (!g.suppressed && &g != &f && (g.type == FType::Fillet || g.type == FType::Chamfer))
                        for (const Target& t : g.targets)
                            if (t.a == a.key || t.b == a.key || t.a == b.key || t.b == b.key)
                                reach = std::max(reach, g.size);
                o.in = [=](const Vec3& p, double m) {
                    return std::abs(a.c.dist(p)) <= rho + m + 1e-3 && std::abs(b.c.dist(p)) <= rho + m + 1e-3 &&
                           a.in(p, m + reach + slack) && b.in(p, m + reach + slack);
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
TruthEval evalTruthImpl(const TruthDesc& d, const Owners& ow, const Samples& smp, FitMasks* masks,
                        double minFraction = -1)
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
            // Each face of the description needs a face of its OWN: an edge described as the meeting of two faces on
            // one owner (the line where a face is split into two coplanar pieces) is not any edge with one face on that
            // owner (T-081a random 5:23).
            const auto& own = smp.facesOf[d.kind == SubKind::Edge ? 1 : 0][x];
            std::vector<std::vector<std::size_t>> fits(cons.size());
            for (std::size_t ci = 0; ci < cons.size(); ++ci)
                for (std::size_t k = 0; k < own.size(); ++k)
                    for (const Owner* o : cons[ci].alts) {
                        // (A face the mesher left without triangles is checked on its boundary samples.)
                        const ElemSamples& face = smp.of(SubKind::Face)[own[k]];
                        if (typeMatches(o->c, face) && nodesOn(o->c, face.nodes, tolFor(SubKind::Face).node)) {
                            fits[ci].push_back(k);
                            break;
                        }
                    }
            std::vector<char> used(own.size(), 0);
            const std::function<bool(std::size_t)> assign = [&](std::size_t ci) {
                if (ci == fits.size())
                    return true;
                for (const std::size_t k : fits[ci]) {
                    if (used[k] != 0)
                        continue;
                    used[k] = 1;
                    if (assign(ci + 1))
                        return true;
                    used[k] = 0;
                }
                return false;
            };
            if (!assign(0))
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
        if (frac >= (minFraction >= 0 ? minFraction : T.thr) && frac > 0) {
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
    // An edge or vertex: which owners of the description each of its own faces lies on (each constraint needs a face).
    if (d.kind != SubKind::Face) {
        out += "; its faces:";
        for (const std::uint32_t f : smp.facesOf[d.kind == SubKind::Edge ? 1 : 0][static_cast<std::size_t>(x)]) {
            out += " " + std::to_string(f) + "(";
            const ElemSamples& face = smp.of(SubKind::Face)[f];
            for (const Owner& o : ow.v)
                if (typeMatches(o.c, face) && nodesOn(o.c, face.nodes, tolFor(SubKind::Face).node))
                    out += o.key + " ";
            out += ")";
        }
    }
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

const std::optional<ElemSamples>& TruthBuilder::mappedTo(SubId s, const Owners& after) const
{
    const auto key = std::make_tuple(after.serial, static_cast<int>(s.kind), s.index);
    {
        const std::lock_guard<std::mutex> guard(lock_);
        const auto it = mapped_.find(key);
        if (it != mapped_.end())
            return it->second;
    }
    std::optional<ElemSamples> out;
    const TruthDesc d = describe(s);
    // An element on an owner a direct edit removed is gone: it lies nowhere now, rather than somewhere it cannot be
    // mapped to (no samples: where it was is still checked as baseline territory).
    bool removed = false;
    for (const Constraint& c : d.cons)
        for (const std::string& alt : c.alts)
            removed = removed || (after.removedByEdit.count(alt) != 0 && after.get(alt) == nullptr);
    if (d.ok && removed) {
        out = ElemSamples{};
    } else if (d.ok) {
        bool moved = false;
        out = mapElement(samples_, s, d, owners_, after, moved);
        if (!moved)
            out = ElemSamples{};
        else if (out->pts.empty())
            out.reset();
    }
    const std::lock_guard<std::mutex> guard(lock_);
    return mapped_.emplace(key, std::move(out)).first->second;
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

std::vector<std::uint32_t> facesOn(const std::string& key, const Owners& ow, const Samples& smp)
{
    if (ow.get(key) == nullptr)
        return {};
    TruthDesc d;
    d.kind = SubKind::Face;
    d.cons.push_back(Constraint{{key}, {}});
    d.ok = true;
    return evalTruth(d, ow, smp).elems;
}

std::string measureEdit(Feature& f, const Script& built, const Samples& before, const Samples& after)
{
    EditSpec& e = f.edit;
    const Owners O0 = buildOwners(built);
    auto withEdit = [&] {
        Script s = built;
        s.push_back(f);
        return buildOwners(s);
    };
    // The owners, other than `exclude`, with a face that shares an edge with one of `faces` (of the shape before).
    auto bordering = [&](const std::set<std::uint32_t>& faces, const std::vector<std::string>& exclude) {
        std::vector<std::string> out;
        for (const Owner& o : O0.v) {
            if (std::find(exclude.begin(), exclude.end(), o.key) != exclude.end())
                continue;
            bool borders = false;
            for (const std::uint32_t x : facesOn(o.key, O0, before))
                for (std::uint32_t edge = 0; !borders && edge < before.facesOf[1].size(); ++edge) {
                    const auto& fs = before.facesOf[1][edge];
                    if (std::find(fs.begin(), fs.end(), x) == fs.end())
                        continue;
                    for (const std::uint32_t y : fs)
                        borders = borders || faces.count(y) != 0;
                }
            if (borders)
                out.push_back(o.key);
        }
        return out;
    };
    switch (e.kind) {
    case EKind::Delete: {
        e.removed.clear();
        e.fill.clear();
        e.fillReach.clear();
        std::set<std::uint32_t> gone, start;
        std::map<std::uint32_t, std::string> ownerOf;
        for (const Owner& o : O0.v) {
            const std::vector<std::uint32_t> was = facesOn(o.key, O0, before);
            if (was.empty() || !facesOn(o.key, O0, after).empty())
                continue;
            e.removed.push_back(o.key);
            const bool target = std::find(e.targets.begin(), e.targets.end(), o.key) != e.targets.end();
            for (const std::uint32_t x : was) {
                gone.insert(x);
                ownerOf.emplace(x, o.key);
                if (target)
                    start.insert(x);
            }
        }
        for (const std::string& t : e.targets)
            if (std::find(e.removed.begin(), e.removed.end(), t) == e.removed.end())
                return "a picked face is still there: " + t;
        // Every face that went must be one of the picked ones or reach them through faces that went (the rest of a
        // hole's stack): a face that disappeared elsewhere is not something the model describes.
        std::set<std::uint32_t> reached = start;
        std::vector<std::uint32_t> todo(start.begin(), start.end());
        while (!todo.empty()) {
            const std::uint32_t x = todo.back();
            todo.pop_back();
            for (std::uint32_t edge = 0; edge < before.facesOf[1].size(); ++edge) {
                const auto& fs = before.facesOf[1][edge];
                if (std::find(fs.begin(), fs.end(), x) == fs.end())
                    continue;
                for (const std::uint32_t y : fs)
                    if (gone.count(y) != 0 && reached.insert(y).second)
                        todo.push_back(y);
            }
        }
        for (const std::uint32_t x : gone)
            if (reached.count(x) == 0)
                return "a face away from the picked ones is gone: face " + std::to_string(x) + " of " + ownerOf[x];
        // A face of the result on a removed owner's surface (half its nodes) and over what that owner held (half of
        // either one on the other): a remnant of the face, or the face run on over the hole it bounded (a hole's mouth
        // chamfer extended into a cone where the bore was). Which logical face it continues is not something the model
        // of a removal says.
        const auto& A = after.of(SubKind::Face);
        for (const std::string& key : e.removed) {
            const Owner* o = O0.get(key);
            for (std::uint32_t y = 0; o != nullptr && y < A.size(); ++y) {
                if (A[y].nodes.empty() || !typeMatches(o->c, A[y]))
                    continue;
                std::size_t on = 0;
                for (const Vec3& p : A[y].nodes)
                    on += o->c.on(p, tolFor(SubKind::Face).node) ? 1 : 0;
                if (2 * on < A[y].nodes.size())
                    continue;
                for (const std::uint32_t x : facesOn(key, O0, before)) {
                    const ElemSamples& B = before.of(SubKind::Face)[x];
                    if (shareOnElement(A[y], B, SubKind::Face) >= 0.5 || shareOnElement(B, A[y], SubKind::Face) >= 0.5)
                        return "a removed face is still there in part: face " + std::to_string(y) + " on " + key;
                }
            }
        }
        // The owners whose faces bordered a removed face (shared an edge with one): only those run on over the ground.
        e.grown = bordering(gone, e.removed);
        // How far a face around may run on over each removed face: a curved face's smallest box dimension (a hole's
        // diameter, a fillet's size; a ring-shaped blend is as thick as its size, not as wide as its ring), a flat
        // face's 0.1 mm (a face running on over a removed pocket or flat chamfer runs over ground nobody held: new
        // territory, expect's merged rule).
        for (const std::uint32_t x : gone) {
            const ElemSamples& es = before.of(SubKind::Face)[x];
            if (!es.mesh)
                continue;
            std::array<double, 3> dims{es.boxMax.x - es.boxMin.x, es.boxMax.y - es.boxMin.y, es.boxMax.z - es.boxMin.z};
            std::sort(dims.begin(), dims.end());
            const bool flat = es.type == static_cast<int>(kernel::SurfaceType::Plane);
            e.fill.push_back(es.mesh);
            e.fillReach.push_back((flat ? 0.0 : dims[0]) + 0.1);
        }
        return {};
    }
    case EKind::Offset: {
        // Another owner on the moved plane (same side) with no face of its own: its ground may be part of the moved
        // face (a boss's start cap that the boss's join unified with the face it stands flush on, 6:47 of the hidden
        // block), and then it moves with it, which the model of an offset (only the targets move) does not say. A
        // cut's start cap is never on the result with that side, and is left out.
        for (const std::string& t : e.targets) {
            const Owner* T = O0.get(t);
            if (T == nullptr || T->c.k != Carrier::K::Plane)
                continue;
            for (const Owner& o : O0.v) {
                if (o.key == t || o.c.k != Carrier::K::Plane || !o.c.oriented || dot(o.c.a, T->c.a) < 1 - 1e-9 ||
                    std::abs(T->c.dist(o.c.p)) > 1e-4)
                    continue;
                const auto cut = O0.sweeps.find(o.key.substr(0, o.key.find('/')));
                const bool cutStart =
                    o.key.find("/c0/") != std::string::npos && cut != O0.sweeps.end() && cut->second.op == BOp::Cut;
                if (!cutStart && facesOn(o.key, O0, before).empty())
                    return "another face's ground may be on the moved plane: " + o.key;
            }
        }
        // The faces around a moved face run on over the slab it swept (pushed out) when they are square to it: as far
        // as the face moved from the moved face as it was (an edge where such a face meets the moved one lies there:
        // T-081b hidden block 18:20, a boss's start cap and its side pushed out).
        e.fill.clear();
        e.fillReach.clear();
        std::set<std::uint32_t> moved;
        for (const std::string& t : e.targets)
            for (const std::uint32_t x : facesOn(t, O0, before))
                if (moved.insert(x).second && before.of(SubKind::Face)[x].mesh) {
                    e.fill.push_back(before.of(SubKind::Face)[x].mesh);
                    e.fillReach.push_back(std::abs(e.amount) + 0.1);
                }
        e.grown = bordering(moved, e.targets);
        const Owners O1 = withEdit();
        for (const std::string& t : e.targets) {
            if (O1.undescribable.count(t) != 0)
                return "the moved face is not a plane with a known outward normal: " + t;
            if (facesOn(t, O1, after).empty())
                return "the moved face is not where it was moved to: " + t;
        }
        return {};
    }
    case EKind::Resize: {
        e.resized.clear();
        const Owner* T = e.targets.empty() ? nullptr : O0.get(e.targets.front());
        if (T == nullptr || T->c.k != Carrier::K::Cyl)
            return "the picked face is not a cylinder: " + (e.targets.empty() ? std::string() : e.targets.front());
        for (const Owner& o : O0.v) {
            if (o.c.k != Carrier::K::Cyl || o.c.sigma != T->c.sigma ||
                std::abs(o.c.r - T->c.r) > 1e-5 * std::max(1.0, T->c.r) || norm(cross(o.c.a, T->c.a)) > 1e-7)
                continue;
            const Vec3 d = sub(o.c.p, T->c.p);
            if (norm(sub(d, mul(T->c.a, dot(d, T->c.a)))) > 1e-5)
                continue;
            if (facesOn(o.key, O0, before).empty() || !facesOn(o.key, O0, after).empty())
                continue;
            e.resized.push_back(o.key);
        }
        if (std::find(e.resized.begin(), e.resized.end(), T->key) == e.resized.end())
            return "the picked bore face is still at its old radius: " + T->key;
        // The faces around the bore run on over the ring between the old and the new radius when it closes down: as far
        // as the radius changed from the bore's faces as they were.
        e.fill.clear();
        e.fillReach.clear();
        std::set<std::uint32_t> bore;
        for (const std::string& r : e.resized)
            for (const std::uint32_t x : facesOn(r, O0, before))
                if (e.amount < T->c.r && bore.insert(x).second && before.of(SubKind::Face)[x].mesh) {
                    e.fill.push_back(before.of(SubKind::Face)[x].mesh);
                    e.fillReach.push_back(std::abs(T->c.r - e.amount) + 0.1);
                }
        e.grown = bordering(bore, e.resized);
        const Owners O1 = withEdit();
        for (const std::string& r : e.resized)
            if (facesOn(r, O1, after).empty())
                return "a bore face is not at the new radius: " + r;
        return {};
    }
    case EKind::Unify:
        return {};
    }
    return {};
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

double interiorShare(const ElemSamples& a, const ElemSamples& b, SubKind kind, double tol = kAnchorTolerance)
{
    const double margin = besideMargin(b, kind);
    double on = 0, all = 0;
    for (std::size_t i = 0; i < a.pts.size(); ++i) {
        all += a.w[i];
        if (nearInterior(a.pts[i], b, kind, tol, margin))
            on += a.w[i];
    }
    return all > 0 ? on / all : 0;
}

// Do two elements share territory (10 % of either one's samples inside the other)? `tol`: how far apart two points
// of shared territory may be (the anchor tolerance for an element mapped through an edit).
bool overlap(const ElemSamples& a, const ElemSamples& b, SubKind kind, double tol = kAnchorTolerance)
{
    return interiorShare(a, b, kind, tol) >= 0.1 || interiorShare(b, a, kind, tol) >= 0.1;
}

// A vertex that the edit map left where it was is compared exactly: its mapped point is its own point, not a
// projection, and a different vertex 0.03 mm away (a boolean leaves such near-coincident vertices) is not it.
constexpr double kExactVertexTolerance = 1e-3;

// Did mapElement leave every sample of `e` where it was (the identity map)?
bool unmovedMap(const ElemSamples& e, const ElemSamples& mapped)
{
    if (mapped.pts.size() != e.pts.size() || mapped.nodes.size() != e.nodes.size())
        return false;
    for (std::size_t i = 0; i < e.pts.size(); ++i)
        if (norm(sub(e.pts[i], mapped.pts[i])) > 1e-9)
            return false;
    for (std::size_t i = 0; i < e.nodes.size(); ++i)
        if (norm(sub(e.nodes[i], mapped.nodes[i])) > 1e-9)
            return false;
    return true;
}

// The anchor tolerance for an element mapped through an edit: exact for a vertex the map left in place, and for an
// edge left in place, the chord slack of the sample mesh (its samples lie on the curve, the other edge's polyline
// within a chord of it): a short edge 0.03 mm beside the picked one's end is not the picked edge.
double anchorTolerance(SubKind kind, bool identity)
{
    if (!identity || kind == SubKind::Face)
        return kAnchorTolerance;
    return kind == SubKind::Vertex ? kExactVertexTolerance : kSampleMesh.chordTol + kExactVertexTolerance;
}

// Could a point of a face of the new shape lie ON baseline face b? Faces that meet at an angle share only their
// boundary; only faces on the same plane (or curved faces) can hold the same area.
bool sameSheet(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    const int plane = static_cast<int>(kernel::SurfaceType::Plane);
    return kind != SubKind::Face || a.type != plane || b.type != plane || std::abs(dot(a.nrm, b.nrm)) > 0.99;
}

// The largest distance any owner's reference point moved between two scripts (owners in both), mm.
double largestOwnerShift(const Owners& before, const Owners& after)
{
    double out = 0;
    for (const Owner& o : before.v)
        if (const Owner* a = after.get(o.key))
            out = std::max(out, norm(sub(a->c.p, o.c.p)));
    return out;
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

// A sweep edited only by a similarity of its profile: the same operation, direction and sketch axes, every profile
// coordinate multiplied by one factor k (the random test's "resize"), the sketch origin possibly moved, the depth
// possibly changed. Returns k. The sweep's own faces then follow the map p -> o' + k (u x + v y) + h n, (u, v, h) the
// point in the old sketch frame: each point of a wall stays at the same place along the wall, scaled.
std::optional<double> profileScale(const Feature& a, const Feature& b)
{
    auto same = [](const Vec3& x, const Vec3& y) {
        return norm(sub(x, y)) <= 1e-12;
    };
    if (a.type != FType::Sweep || b.type != FType::Sweep || a.op != b.op || !same(a.frame.n, b.frame.n) ||
        !same(a.frame.xd, b.frame.xd) || a.loops.size() != b.loops.size())
        return std::nullopt;
    double k = 0;
    auto check = [&](double x, double y) {
        if (std::abs(x) <= 1e-12)
            return std::abs(y) <= 1e-12;
        if (k == 0)
            k = y / x;
        return k > 0 && std::abs(y - k * x) <= 1e-9 * std::max(1.0, std::abs(y));
    };
    for (std::size_t i = 0; i < a.loops.size(); ++i) {
        if (a.loops[i].ents.size() != b.loops[i].ents.size())
            return std::nullopt;
        for (std::size_t j = 0; j < a.loops[i].ents.size(); ++j) {
            const Ent &x = a.loops[i].ents[j], &y = b.loops[i].ents[j];
            if (x.k != y.k || x.tag != y.tag || !check(x.x0, y.x0) || !check(x.y0, y.y0) || !check(x.x1, y.x1) ||
                !check(x.y1, y.y1) || !check(x.cx, y.cx) || !check(x.cy, y.cy) || !check(x.r, y.r))
                return std::nullopt;
        }
    }
    if (k == 0)
        return std::nullopt;
    return k;
}

// The point p of sweep a's sketch frame, scaled by k and carried to sweep b's sketch frame.
Vec3 similarMap(const Feature& a, const Feature& b, double k, const Vec3& p)
{
    const Vec3 d = sub(p, a.frame.o);
    const double u = dot(d, a.frame.xd), v = dot(d, a.frame.yd()), h = dot(d, a.frame.n);
    return add(add(add(b.frame.o, mul(b.frame.xd, k * u)), mul(b.frame.yd(), k * v)), mul(b.frame.n, h));
}

// The feature id of an owner key ("<feature>/s<tag>", "<feature>/c<cap>/<loop>", ...).
std::string featureOfOwner(const std::string& key)
{
    return key.substr(0, key.find('/'));
}

// similarMap backwards: the point of sweep a's frame that similarMap(a, b, k, .) carries to p.
Vec3 similarUnmap(const Feature& a, const Feature& b, double k, const Vec3& p)
{
    const Vec3 d = sub(p, b.frame.o);
    const double u = dot(d, b.frame.xd), v = dot(d, b.frame.yd()), h = dot(d, b.frame.n);
    return add(add(add(a.frame.o, mul(a.frame.xd, u / k)), mul(a.frame.yd(), v / k)), mul(a.frame.n, h));
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

// The same sweep in both scripts (nothing about it edited).
bool sameSweep(const Feature& a, const Feature& b)
{
    auto same = [](const Vec3& x, const Vec3& y) {
        return norm(sub(x, y)) <= 1e-12;
    };
    if (a.op != b.op || a.dist != b.dist || !same(a.frame.o, b.frame.o) || !same(a.frame.n, b.frame.n) ||
        !same(a.frame.xd, b.frame.xd) || a.loops.size() != b.loops.size())
        return false;
    for (std::size_t i = 0; i < a.loops.size(); ++i) {
        if (a.loops[i].ents.size() != b.loops[i].ents.size())
            return false;
        for (std::size_t k = 0; k < a.loops[i].ents.size(); ++k) {
            const Ent &x = a.loops[i].ents[k], &y = b.loops[i].ents[k];
            if (x.k != y.k || x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1 || x.cx != y.cx ||
                x.cy != y.cy || x.r != y.r)
                return false;
        }
    }
    return true;
}

// The triangles of an imported base's face meshes, bucketed by (y, z) for rays along +x (insideImport). Made once per
// base, thread-safe.
struct RayGrid {
    std::vector<std::array<Vec3, 3>> tris;
    double y0 = 0, z0 = 0, dy = 1, dz = 1;
    static constexpr int kCells = 64;
    std::vector<std::vector<std::uint32_t>> cells; // kCells x kCells
};

const RayGrid& rayGridOf(const ImportBase& base)
{
    static std::mutex lock;
    static std::map<const ImportBase*, std::unique_ptr<RayGrid>> grids;
    const std::lock_guard<std::mutex> guard(lock);
    auto& slot = grids[&base];
    if (slot)
        return *slot;
    auto g = std::make_unique<RayGrid>();
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (const auto& mesh : base.meshes)
        if (mesh)
            for (const auto& t : mesh->triangles) {
                g->tris.push_back({mesh->nodes[t[0]], mesh->nodes[t[1]], mesh->nodes[t[2]]});
                for (const Vec3& v : g->tris.back()) {
                    lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                    hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
                }
            }
    g->y0 = lo.y;
    g->z0 = lo.z;
    g->dy = std::max(1e-9, (hi.y - lo.y) / RayGrid::kCells);
    g->dz = std::max(1e-9, (hi.z - lo.z) / RayGrid::kCells);
    g->cells.resize(static_cast<std::size_t>(RayGrid::kCells * RayGrid::kCells));
    auto cell = [&](double v, double v0, double d) {
        return std::clamp(static_cast<int>((v - v0) / d), 0, RayGrid::kCells - 1);
    };
    for (std::uint32_t i = 0; i < g->tris.size(); ++i) {
        const auto& t = g->tris[i];
        const int a0 = cell(std::min({t[0].y, t[1].y, t[2].y}), g->y0, g->dy);
        const int a1 = cell(std::max({t[0].y, t[1].y, t[2].y}), g->y0, g->dy);
        const int b0 = cell(std::min({t[0].z, t[1].z, t[2].z}), g->z0, g->dz);
        const int b1 = cell(std::max({t[0].z, t[1].z, t[2].z}), g->z0, g->dz);
        for (int a = a0; a <= a1; ++a)
            for (int b = b0; b <= b1; ++b)
                g->cells[static_cast<std::size_t>(a * RayGrid::kCells + b)].push_back(i);
    }
    slot = std::move(g);
    return *slot;
}

// Is p inside the imported base solid, at least m from its faces? Its faces' fine meshes for both: the margin, and the
// parity of the crossings of a ray from p along +x with their triangles (the meshes are closed).
bool insideImport(const ImportBase& base, const Vec3& p, double m)
{
    for (const auto& mesh : base.meshes)
        if (mesh && mesh->distanceWithin(p, m) <= m)
            return false;
    const RayGrid& g = rayGridOf(base);
    const int a = static_cast<int>((p.y - g.y0) / g.dy), b = static_cast<int>((p.z - g.z0) / g.dz);
    if (a < 0 || b < 0 || a >= RayGrid::kCells || b >= RayGrid::kCells)
        return false;
    int crossings = 0;
    for (const std::uint32_t i : g.cells[static_cast<std::size_t>(a * RayGrid::kCells + b)]) {
        const auto& t = g.tris[i];
        // The triangle's projection on the (y, z) plane holds (p.y, p.z)? (Half-open edges: a ray through an edge
        // counts once.)
        const double y = p.y, z = p.z;
        auto side = [&](const Vec3& u, const Vec3& v) {
            return (v.y - u.y) * (z - u.z) - (v.z - u.z) * (y - u.y);
        };
        const double s0 = side(t[0], t[1]), s1 = side(t[1], t[2]), s2 = side(t[2], t[0]);
        const bool in = (s0 > 0 && s1 > 0 && s2 > 0) || (s0 < 0 && s1 < 0 && s2 < 0);
        if (!in)
            continue;
        // Where the ray meets the triangle's plane.
        const Vec3 n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
        if (std::abs(n.x) < 1e-15)
            continue;
        const double x = t[0].x - (n.y * (y - t[0].y) + n.z * (z - t[0].z)) / n.x;
        if (x > p.x)
            ++crossings;
    }
    return crossings % 2 == 1;
}

TruthDesc withoutSeparators(const TruthDesc& d)
{
    TruthDesc out = d;
    out.seps.clear();
    for (Constraint& c : out.cons)
        c.seps.clear();
    return out;
}

// The description without the separators whose owner the edited script no longer has (Sep::frozen): the feature
// that told the pieces apart is gone (suppressed, or it failed to build where an edit moved it), so where it was no
// longer divides anything.
TruthDesc withoutFrozenSeparators(const TruthDesc& d, const Owners& now)
{
    TruthDesc out = d;
    auto live = [&](std::vector<Sep>& seps) {
        seps.erase(std::remove_if(seps.begin(), seps.end(), [&](const Sep& s) { return now.get(s.owner) == nullptr; }),
                   seps.end());
    };
    live(out.seps);
    for (Constraint& c : out.cons)
        live(c.seps);
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

bool coincidesWithAnother(const TruthDesc& d, SubId s, const Owners& owners, const Samples& smp)
{
    const auto& E = smp.of(s.kind);
    const ElemSamples& self = E[s.index];
    // Both elements are of the same shape, so they are compared exactly (vertices, edges) or within the sample mesh's
    // chord slack (faces, sampled on triangles).
    const double tol = s.kind == SubKind::Face ? kAnchorTolerance : kExactVertexTolerance;
    for (const std::uint32_t other : evalTruth(withoutSeparators(d), owners, smp).elems)
        if (other != s.index && !E[other].pts.empty() && sameSheet(self, E[other], s.kind) &&
            overlap(self, E[other], s.kind, tol))
            return true;
    return false;
}

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
    bool boundByJoin = false; // one of them is a face of a join (it bounds the element by the material it adds)
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
                if (const auto f = before.sweeps.find(featureOfOwner(o.key));
                    f != before.sweeps.end() && f->second.op != BOp::Cut)
                    boundByJoin = true;
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
    std::vector<const Feature*> keptMaterial; // joins the edit left as they were
    for (const auto& [id, f] : before.sweeps) {
        const auto it = after.sweeps.find(id);
        if (it == after.sweeps.end())
            continue;
        if (const auto t = pureMove(f, it->second))
            movedSweeps.push_back({&f, *t});
        else if (f.op != BOp::Cut && sameSweep(f, it->second))
            keptMaterial.push_back(&f);
    }
    // But not what lies in material that stayed where it was: inside the imported base (unchanged by every edit) or a
    // join the edit did not touch. A point there does not depend on the moved sweep (T-081a holdout3 18:7: a slot's
    // edge running through a boss and the imported part it was joined into; where the slot crosses the part, the
    // edge stays when the boss moves).
    // (Not after a direct edit of an imported face: the imported solid is then not the material that is there.)
    const bool keptImport =
        before.import != nullptr && before.import == after.import && !before.importEdited && !after.importEdited;
    auto inKeptMaterial = [&](const Vec3& p) {
        for (const Feature* g : keptMaterial)
            if (insideSweep(*g, p, kAnchorTolerance))
                return true;
        return keptImport && insideImport(*before.import, p, kAnchorTolerance);
    };
    // A sweep edited by a similarity of its profile (a resize, with or without a move) carries the points of its own
    // walls exactly (profileScale): a point where two of its walls meet (the tangent line between a straight wall and
    // a corner arc) cannot be found again by projecting onto the walls, which barely cross there. Used when every
    // carrier of the element that changed is one of that sweep's.
    const Feature* simA = nullptr;
    const Feature* simB = nullptr;
    double simK = 1;
    auto useSimilarity = [&](const std::string& feature, bool requireChange) {
        const auto a = before.sweeps.find(feature);
        const auto b = after.sweeps.find(feature);
        if (a == before.sweeps.end() || b == after.sweeps.end())
            return;
        const auto k = profileScale(a->second, b->second);
        if (!k || (requireChange && *k == 1.0 && norm(sub(a->second.frame.o, b->second.frame.o)) <= 1e-12))
            return;
        simA = &a->second;
        simB = &b->second;
        simK = *k;
    };
    if (ownChanged) {
        std::set<std::string> changedFeatures;
        for (const Constraint& c : d.cons)
            for (const auto& alt : c.alts) {
                const Owner* o0 = before.get(alt);
                const Owner* o1 = after.get(alt);
                if (o0 != nullptr && o1 != nullptr && carrierChanged(o0->c, o1->c))
                    changedFeatures.insert(featureOfOwner(alt));
            }
        if (changedFeatures.size() == 1)
            useSimilarity(*changedFeatures.begin(), false);
    } else {
        // Every face of the element is a face of one sweep that moved or was scaled, though none of their carriers
        // changed (a start cap on the face the sweep was sketched on, a wall along the direction of the move): the
        // element is part of that sweep and moves with it (random 14:46: a boss's start face, the boss moved by its
        // width along the face).
        std::set<std::string> features;
        for (const Constraint& c : d.cons)
            for (const auto& alt : c.alts)
                features.insert(featureOfOwner(alt));
        if (features.size() == 1) {
            useSimilarity(*features.begin(), true);
            if (simA != nullptr) {
                ownChanged = true;
                moved = true;
            }
        }
    }
    std::size_t keptPoints = 0, shiftedPoints = 0; // points kept where they were by inKeptMaterial; points moved
    auto mapPoint = [&](const Vec3& p, Vec3& q) {
        Vec3 shift{0, 0, 0};
        double best = 0;
        const Vec3* withSweep = nullptr;
        for (const auto& [f, t] : movedSweeps)
            if (insideSweep(*f, p, kAnchorTolerance)) {
                withSweep = &t;
                break;
            }
        // A point in material that stayed does not move with a moved join, as what lies in its volume (withSweep) nor
        // as what its faces bound (bound): it does not depend on it. (Cuts that bound a piece move it.)
        const bool kept =
            !ownChanged && (withSweep != nullptr || (boundByJoin && norm(bound) > 1e-9)) && inKeptMaterial(p);
        if (kept)
            withSweep = nullptr;
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
        // Nothing the point depends on changed: it stays exactly where it is (projecting it onto carriers that meet
        // at a small angle would slide it along them, onto a neighbouring vertex 0.04 mm away).
        if (withSweep == nullptr && !ownChanged && (kept || !(norm(bound) > 1e-9))) {
            q = p;
            keptPoints += kept ? 1 : 0;
            return true;
        }
        ++shiftedPoints;
        if (withSweep == nullptr && simA != nullptr)
            q = similarMap(*simA, *simB, simK, p);
        else
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
    // An edge some of whose points stayed in material that did not move while the rest moved is no longer the polyline
    // of its mapped ends: its mapped samples are.
    if (out.nodes.empty() || (kind == SubKind::Edge && keptPoints > 0 && shiftedPoints > 0))
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

namespace {

// ---------------------------------------------------------------- sliding crossings (oracle.h CONTINUATION)
// How much nearer the mapped pick must be to the element it picks out than to any other candidate, and than any other
// baseline piece's mapped place is to that element.
constexpr double kCrossingMargin = 2.0;

// Mean distance from the samples of `a` (its points; a vertex's point) to element `b` (its polyline or point), mm.
double meanDistanceTo(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    const std::vector<Vec3>& from = a.pts.empty() ? a.nodes : a.pts;
    if (from.empty() || b.nodes.empty())
        return 1e300;
    double sum = 0;
    for (const Vec3& p : from) {
        double d = norm(sub(p, b.nodes.front()));
        if (kind == SubKind::Edge)
            for (std::size_t k = 1; k < b.nodes.size(); ++k)
                d = std::min(d, segmentDistance(p, b.nodes[k - 1], b.nodes[k]));
        sum += d;
    }
    return sum / static_cast<double>(from.size());
}

// Is vertex v of `smp` a seam vertex: on exactly two faces? No third face meets there; the vertex only splits (or
// closes) the curve where the two meet, at the seam of the periodic one (a hole's wall), whether or not a seam edge
// of that face starts there (the face may only touch its seam line at that point).
bool seamVertex(const Samples& smp, std::uint32_t v)
{
    std::vector<std::uint32_t> faces = smp.facesOf[0][v];
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    return faces.size() == 2;
}

} // namespace

Expectation expect(const PickRecord& pick, const Owners& now, const Samples& smp)
{
    Expectation x;
    const TruthDesc& d = pick.truth;
    x.kind = d.kind;
    x.weak = d.weak;
    // A logical face of the description that the edited script still has but the oracle cannot describe (Owners::
    // undescribable): where its continuation is cannot be told.
    for (const Constraint& c : d.cons)
        for (const std::string& alt : c.alts)
            if (now.undescribable.count(alt) != 0) {
                x.noAnchor = "the edited script's " + alt + " has no surface the oracle can describe";
                return x;
            }
    x.strict = evalTruth(d, now, smp);
    x.mapped = mapElement(*pick.baseline, pick.base, d, *pick.owners, now, x.moved);
    const auto& N = smp.of(x.kind);
    const double pickTol = anchorTolerance(x.kind, unmovedMap(pick.picked(), x.mapped));
    // An element that holds the picked element (mapped through the edit) continues it, however small a part of it the
    // picked element is: evalTruth's floor (2 % of a face's area on the description) is for telling overlaps from
    // noise, not for containment (a boss's start face exposed in a corner, now flush with and unified into a large
    // face; a strip 0.03 mm wide now part of the face beside it). Such an element runs past the description: merged.
    // It must still lie on every carrier of the description with the right type and material side, and on the right
    // side of every separator whose owner still exists: a separator whose owner is gone (Sep::frozen) divides nothing
    // any more, and the element holding the mapped pick is its continuation whichever side of it the rest lies (T-081a
    // holdout3 16:1: the stub of an import edge below a cut that an edit moved off the part; the edge is whole again).
    std::set<std::uint32_t> containers;
    if (!x.mapped.pts.empty()) {
        std::vector<std::uint32_t> candidates =
            evalTruthImpl(withoutFrozenSeparators(d, now), now, smp, nullptr, 0.0).elems;
        // A face on the description's carrier none of whose samples falls inside the extent (a 2 mm wide strip sampled
        // every 2.5 mm, over a 4.7 mm long pick: T-081b hidden block 19:48), when the description has no separator:
        // whether it holds the mapped pick is told by the pick's samples, not by its own.
        if (x.kind == SubKind::Face && d.seps.empty() && d.cons.size() == 1 && d.cons[0].seps.empty())
            for (std::uint32_t e = 0; e < N.size(); ++e) {
                if (std::binary_search(candidates.begin(), candidates.end(), e) || N[e].nodes.empty())
                    continue;
                for (const std::string& alt : d.cons[0].alts)
                    if (const Owner* o = now.get(alt);
                        o != nullptr && typeMatches(o->c, N[e]) && nodesOn(o->c, N[e].nodes, tolFor(x.kind).node)) {
                        candidates.insert(std::lower_bound(candidates.begin(), candidates.end(), e), e);
                        break;
                    }
            }
        for (const std::uint32_t e : candidates) {
            if (interiorShare(x.mapped, N[e], x.kind, pickTol) < 0.9)
                continue;
            containers.insert(e);
            if (std::binary_search(x.strict.elems.begin(), x.strict.elems.end(), e))
                continue;
            x.strict.elems.insert(std::lower_bound(x.strict.elems.begin(), x.strict.elems.end(), e), e);
            x.strict.partial.insert(std::lower_bound(x.strict.partial.begin(), x.strict.partial.end(), e), e);
            x.strict.merged = true;
        }
    }
    const TruthEval L = evalTruth(withoutSeparators(d), now, smp);
    // Every carrier of the description as it was at pick time: the picked element did not move, only what bounds it.
    x.inPlace = true;
    for (const Constraint& c : d.cons)
        for (const std::string& alt : c.alts) {
            const Owner* o0 = pick.owners->get(alt);
            const Owner* o1 = now.get(alt);
            x.inPlace = x.inPlace && o0 != nullptr && o1 != nullptr && !carrierChanged(o0->c, o1->c);
        }
    // REMNANTS. Moved only because a separator or a bounding feature moved, its own carriers as they were: an element on
    // those carriers lying all but 10 % on the picked element where it was is ground the pick had and kept, whichever
    // side of a moved separator it now lies (T-081b holdout1 15:12: a push/pull pulled the separating face away but
    // left two strips of the pick, where it did not extend an angled neighbour; 17:10: the start cap of a boss a moved
    // cut had bounded). The map, which moves such an element with what bounds it, would place it elsewhere.
    std::set<std::uint32_t> remnants;
    if (x.moved && x.inPlace && x.kind != SubKind::Vertex)
        for (const std::uint32_t e : L.elems) {
            if (N[e].pts.empty() || sliver(N[e], x.kind) || shareOn(N[e], pick.picked(), x.kind) < 0.9)
                continue;
            remnants.insert(e);
            if (!std::binary_search(x.strict.elems.begin(), x.strict.elems.end(), e))
                x.strict.elems.insert(std::lower_bound(x.strict.elems.begin(), x.strict.elems.end(), e), e);
        }
    std::set_union(L.elems.begin(), L.elems.end(), x.strict.elems.begin(), x.strict.elems.end(),
                   std::back_inserter(x.logical));
    // The logical element is gone with its owner (a feature suppressed, a face a direct edit removed), yet an element
    // of the shape still holds all the ground the picked one held: whose element that is now cannot be told (the
    // description named one of two coincident owners: T-081b directedit 17:20, a cut's start cap on the plane of a
    // boss's start cap, the boss suppressed). Only an answer that is not Broken is affected (not judged instead of a
    // ghost).
    if (x.logical.empty()) {
        bool ownerGone = false;
        for (const Constraint& c : d.cons) {
            bool any = false;
            for (const std::string& alt : c.alts)
                any = any || now.get(alt) != nullptr;
            ownerGone = ownerGone || !any;
        }
        if (ownerGone)
            for (std::uint32_t e = 0; e < N.size(); ++e)
                if (!N[e].pts.empty() && sameSheet(pick.picked(), N[e], x.kind) &&
                    (x.kind == SubKind::Vertex || N[e].type == pick.picked().type) &&
                    shareOn(pick.picked(), N[e], x.kind) >= 0.9) {
                    x.noAnchor = "the picked element's owner is gone, but element " + std::to_string(e) +
                                 " holds all of its ground";
                    return x;
                }
    }
    // A description using a degenerate blend surface that nothing of the edited shape fits: the kernel built another
    // surface, and where the blend went cannot be told (Owners::degenerate).
    if (x.logical.empty())
        for (const Constraint& c : d.cons)
            for (const std::string& alt : c.alts)
                if (now.degenerate.count(alt) != 0) {
                    x.noAnchor = "the edited script's " + alt + " is a degenerate blend that no element fits";
                    return x;
                }
    // The same when the kernel built a blend of the description as a B-spline near its analytic surface (within 0.1 mm,
    // inside its extent), not as that surface: what lies on the blend cannot be told from it (T-081b hidden block
    // 18:35: a fillet between a bore and a face, made larger, built as a B-spline up to 0.062 mm off the torus).
    if (x.logical.empty())
        for (const Constraint& c : d.cons)
            for (const std::string& alt : c.alts) {
                const Owner* o = now.get(alt);
                if (o == nullptr || (o->cat != "fillet face" && o->cat != "chamfer face"))
                    continue;
                for (const ElemSamples& f : smp.of(SubKind::Face)) {
                    if (f.type != static_cast<int>(kernel::SurfaceType::Bspline) &&
                        f.type != static_cast<int>(kernel::SurfaceType::Other))
                        continue;
                    std::size_t on = 0, in = 0;
                    for (const Vec3& q : f.nodes)
                        on += o->c.on(q, 0.1) ? 1 : 0;
                    for (const Vec3& q : f.pts)
                        in += o->in(q, 0.1) ? 1 : 0;
                    if (!f.nodes.empty() && 10 * on >= 9 * f.nodes.size() && 2 * in >= f.pts.size()) {
                        x.noAnchor = "the kernel built the blend " + alt + " as another surface";
                        return x;
                    }
                }
            }
    // An owner of the description held the picked element only by ground a direct edit's fill gave it (Owners::
    // grownByEdit at pick time), the edited script has no such edit of it, and an element still lies where the picked
    // one was, on every carrier of the description: that ground was nobody's the oracle could name before the edit
    // took it over (T-081b directedit 14:0: the triangle a chamfer at a rib's foot adds to the rib's end face, which
    // the end face's extent gained only when a push/pull of the rib wall grew it; that push/pull suppressed, the
    // chamfer's corner on the end face is still there). Whether it continues the pick cannot be told from the extents.
    if (x.logical.empty()) {
        bool grownOnly = false;
        for (const Constraint& c : d.cons)
            for (const std::string& alt : c.alts)
                grownOnly = grownOnly || (pick.owners->grownByEdit.count(alt) != 0 && now.grownByEdit.count(alt) == 0 &&
                                          now.get(alt) != nullptr);
        if (grownOnly)
            for (std::uint32_t e = 0; e < N.size(); ++e) {
                if (N[e].pts.empty() || !sameSheet(pick.picked(), N[e], x.kind) ||
                    (x.kind != SubKind::Vertex && N[e].type != pick.picked().type) ||
                    shareOn(pick.picked(), N[e], x.kind) < 0.9 || shareOn(N[e], pick.picked(), x.kind) < 0.9)
                    continue;
                bool onAll = true;
                for (const Constraint& c : d.cons) {
                    bool on = false;
                    for (const std::string& alt : c.alts)
                        if (const Owner* o = now.get(alt))
                            on = on || nodesOn(o->c, N[e].nodes, tolFor(x.kind).node);
                    onAll = onAll && on;
                }
                if (onAll) {
                    x.noAnchor = "an owner of the description held the picked element only by an edit's fill, which "
                                 "the edited script does not have, and element " +
                                 std::to_string(e) + " is still where it was";
                    return x;
                }
            }
    }
    x.component = componentsOf(x.logical, x.kind, smp);
    x.anchored.resize(x.logical.size(), 0);
    for (std::size_t i = 0; i < x.logical.size(); ++i)
        x.anchored[i] = (remnants.count(x.logical[i]) != 0 ||
                         (!x.mapped.pts.empty() && (containers.count(x.logical[i]) != 0 ||
                                                    overlap(N[x.logical[i]], x.mapped, x.kind, pickTol))))
                            ? 1
                            : 0;
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
            const double tol = anchorTolerance(x.kind, unmovedMap(pick.baseline->of(x.kind)[s], sibling));
            for (std::size_t i = 0; i < x.logical.size(); ++i)
                if (x.claimed[i] == 0 && overlap(N[x.logical[i]], sibling, x.kind, tol))
                    x.claimed[i] = 1;
        }
    }
    const auto cong = congruent(pick.picked(), x.kind, smp, overlapTolerance(x.kind));
    x.congruentIndex = cong.size() == 1 ? static_cast<long>(cong[0]) : -1;
    // Nothing the picked edge or vertex depends on changed, and one element of the logical element lies exactly where
    // it was with the same extent: that element is its continuation, all of it and nothing else. (Overlap alone would
    // also take in a short edge that leaves the picked one's end at a small angle, sharing a stretch within the
    // tolerance.) Not for faces: congruence of faces is judged within the sample mesh's slack, and a strip narrower
    // than that, split off the picked face, would pass for nothing.
    // "Moved" only because a direct edit moved, resized or removed a separator, while every carrier of the element
    // itself is as it was: the element did not move (T-081b, hidden block 13:31: a slot wall pulled away from an edge
    // piece the slot had cut off; 17:27). The separator's verdict is about pieces, and the congruent element is
    // untouched.
    bool ownSame = true, sepsByEdit = true;
    for (const Constraint& c : d.cons)
        for (const std::string& alt : c.alts) {
            const Owner* o0 = pick.owners->get(alt);
            const Owner* o1 = now.get(alt);
            ownSame = ownSame && o0 != nullptr && o1 != nullptr && !carrierChanged(o0->c, o1->c);
        }
    auto sepCheck = [&](const std::vector<Sep>& seps) {
        for (const Sep& s : seps) {
            const Owner* o0 = pick.owners->get(s.owner);
            const Owner* o1 = now.get(s.owner);
            if (o0 == nullptr || o1 == nullptr || carrierChanged(o0->c, o1->c))
                sepsByEdit = sepsByEdit && now.touchedByEdit.count(s.owner) != 0;
        }
    };
    sepCheck(d.seps);
    for (const Constraint& c : d.cons)
        sepCheck(c.seps);
    const bool stillByEdit = x.moved && ownSame && sepsByEdit && !now.touchedByEdit.empty();
    if (x.kind != SubKind::Face && (!x.moved || stillByEdit) && x.congruentIndex >= 0 &&
        x.inLogical(static_cast<std::uint32_t>(x.congruentIndex)) &&
        !sliver(N[static_cast<std::size_t>(x.congruentIndex)], x.kind)) {
        x.expected = TruthEval{};
        x.expected.elems = {static_cast<std::uint32_t>(x.congruentIndex)};
        return x;
    }
    // A vertex where two pieces of one logical face met another face, the pieces since joined into one face (a later
    // boolean's unification, a direct edit's: T-081b directedit 7:0, 4:1), nothing it depends on moved, and the one
    // vertex congruent in place with it lies on every logical face of its description: it is still where those faces
    // meet, and continues the pick. (Each face of a description needs a face of its own, so the description itself no
    // longer fits it: the two pieces are one face now. A point holds nothing more than it did: not merged.)
    // The same for an edge between two pieces of one logical face, the pieces joined: the edge is now that face's seam
    // (T-081b directedit, a holdout of the generator sorted by geometry: 827-9999-905 #41). Every face of the element
    // must lie on an owner of the description (an edge where another face now meets the logical face is another
    // element, though it lies where the picked one did).
    // (In place: the one congruent element, or, when the mesher put another node on it, the one element that lies on
    // the pick and the pick on it, each all but 10 %: T-081b hidden block 18:41, a torus's split line now its seam.)
    long same = x.congruentIndex;
    if (same < 0 && x.kind != SubKind::Face && !x.moved) {
        for (std::uint32_t e = 0; e < N.size(); ++e)
            if (!N[e].pts.empty() && shareOn(pick.picked(), N[e], x.kind) >= 0.9 &&
                shareOn(N[e], pick.picked(), x.kind) >= 0.9)
                same = same == -1 ? static_cast<long>(e) : -2;
        if (same < 0)
            same = -1;
    }
    if (x.kind != SubKind::Face && !x.moved && same >= 0 && !x.inLogical(static_cast<std::uint32_t>(same))) {
        TruthDesc joined = withoutSeparators(d);
        std::vector<Constraint> distinct;
        for (const Constraint& c : joined.cons) {
            std::vector<std::string> alts = c.alts;
            std::sort(alts.begin(), alts.end());
            bool seen = false;
            for (const Constraint& k : distinct)
                seen = seen || k.alts == alts;
            if (!seen)
                distinct.push_back(Constraint{alts, {}});
        }
        bool ownFaces = true;
        for (const std::uint32_t f : smp.facesOf[x.kind == SubKind::Edge ? 1 : 0][static_cast<std::size_t>(same)]) {
            bool on = false;
            for (const Constraint& c : distinct)
                for (const std::string& alt : c.alts)
                    if (const Owner* o = now.get(alt))
                        on = on || (typeMatches(o->c, smp.of(SubKind::Face)[f]) &&
                                    nodesOn(o->c, smp.of(SubKind::Face)[f].nodes, tolFor(SubKind::Face).node));
            ownFaces = ownFaces && on;
        }
        if (distinct.size() < joined.cons.size() && ownFaces) {
            joined.cons = distinct;
            const TruthEval fits = evalTruth(joined, now, smp);
            if (std::binary_search(fits.elems.begin(), fits.elems.end(), static_cast<std::uint32_t>(same))) {
                x.expected = TruthEval{};
                x.expected.elems = {static_cast<std::uint32_t>(same)};
                return x;
            }
        }
    }
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
    // A piece of the logical element that overlaps no element of the baseline (ground nobody held when the reference
    // was made; overlap as for anchoring: 10 % of either one on the other, away from its boundary) and not the mapped
    // pick, beside a piece that continues the pick, when nothing moved: a new face next to the picked one on its
    // surface (the side of a push/pull slab flush with it, left a face of its own where the boolean could not merge the
    // two; T-081b directedit 0:19, 17:10). It is new territory, not the picked element's continuation; an answer that
    // has it too is judged by the new-territory rule (judgeAnswer).
    auto withoutNewPieces = [&](TruthEval& te) {
        if ((x.moved && !stillByEdit) || te.elems.size() < 2)
            return;
        bool anchored = false;
        for (const std::uint32_t e : te.elems)
            anchored = anchored || x.isAnchored(e);
        if (!anchored)
            return;
        const auto& B = pick.baseline->of(x.kind);
        TruthEval kept;
        for (const std::uint32_t e : te.elems) {
            bool fresh = !x.isAnchored(e) && !N[e].pts.empty();
            for (std::uint32_t i = 0; fresh && i < B.size(); ++i)
                if (!B[i].pts.empty() &&
                    !(N[e].boxMax.x + 0.05 < B[i].boxMin.x || B[i].boxMax.x + 0.05 < N[e].boxMin.x ||
                      N[e].boxMax.y + 0.05 < B[i].boxMin.y || B[i].boxMax.y + 0.05 < N[e].boxMin.y ||
                      N[e].boxMax.z + 0.05 < B[i].boxMin.z || B[i].boxMax.z + 0.05 < N[e].boxMin.z))
                    fresh = !overlap(N[e], B[i], x.kind);
            if (!fresh)
                kept.elems.push_back(e);
        }
        if (kept.elems.size() == te.elems.size())
            return;
        for (const std::uint32_t e : te.partial)
            if (std::binary_search(kept.elems.begin(), kept.elems.end(), e))
                kept.partial.push_back(e);
        kept.merged = !kept.partial.empty();
        te = kept;
    };
    // An edge between two pieces of one logical face is described by that face twice, which any other such edge fits
    // too: a vertical split line of a bore fits the description of its horizontal split line and meets it (T-081b
    // hidden block 6:7). Only the pieces that overlap the mapped pick continue it.
    auto withoutOtherSplitLines = [&](TruthEval& te) {
        if (x.kind != SubKind::Edge || d.cons.size() < 2)
            return;
        for (const Constraint& c : d.cons)
            if (c.alts != d.cons[0].alts)
                return;
        TruthEval kept;
        for (const std::uint32_t e : te.elems)
            if (x.isAnchored(e))
                kept.elems.push_back(e);
        if (kept.elems.empty() || kept.elems.size() == te.elems.size())
            return;
        for (const std::uint32_t e : te.partial)
            if (std::binary_search(kept.elems.begin(), kept.elems.end(), e))
                kept.partial.push_back(e);
        kept.merged = !kept.partial.empty();
        te = kept;
    };
    // An expected element that runs past the description is merged only if the part outside the description lies on
    // territory that something else had at pick time. A part on the picked element itself (mapped through the edit)
    // only shows that the owners' extents are approximations (an imported face's extent is its own mesh, a blend's is
    // built from its parents' extents): the picked edge of an unchanged part is not "merged" with itself. And when
    // nothing the element depends on moved, a part on no element of the baseline is new territory (the face that a
    // suppressed hole no longer interrupts, which was empty space when the face was picked). A part on another
    // baseline element, or one that cannot be placed because something moved, keeps the element merged. A weak
    // reference's "claimed" elements (they continue another baseline piece) stay merged in any case.
    auto ownTerritory = [&](TruthEval& te) {
        if (te.partial.empty())
            return;
        FitMasks masks;
        evalTruthImpl(d, now, smp, &masks);
        const auto& B = pick.baseline->of(x.kind);
        // Territory counts only on the description's own surfaces: an element on another surface that comes within the
        // tolerance of this one only where the two surfaces meet (a wall standing on a floor, a hole wall crossing
        // another, an arc tangent to a line where it starts) held none of its ground. So the other element must lie on
        // the description's carriers itself (half its nodes on a carrier of each constraint, with its type and material
        // side). (T-081b directedit 7:47, 8:6, 15:25, 11:40: push/pull and hole edits extend a wall or an edge along
        // such a neighbour.)
        // (Against the description's carriers where the element is now, except for a point carried back to pick time
        // with its sweep: there against those of pick time.)
        std::map<std::pair<const ElemSamples*, const Owners*>, bool> sameSurface;
        auto onDescribedSurfaces = [&](const ElemSamples& b, const Owners& owners) {
            const auto it = sameSurface.find({&b, &owners});
            if (it != sameSurface.end())
                return it->second;
            bool all = !b.nodes.empty();
            for (std::size_t ci = 0; all && ci < d.cons.size(); ++ci) {
                bool any = false;
                for (const std::string& alt : d.cons[ci].alts) {
                    const Owner* o = owners.get(alt);
                    if (o == nullptr || (x.kind == SubKind::Face && !typeMatches(o->c, b)))
                        continue;
                    std::size_t on = 0;
                    for (const Vec3& q : b.nodes)
                        on += o->c.on(q, 10.0 * tolFor(x.kind).node) ? 1 : 0;
                    if (2 * on >= b.nodes.size()) {
                        any = true;
                        break;
                    }
                }
                all = any;
            }
            sameSurface.emplace(std::make_pair(&b, &owners), all);
            return all;
        };
        // Is p of element `of` on territory of element b (inside it, not beside it)? `owners`: whose carriers b must
        // lie on (today's, or the baseline's for a point carried back to pick time).
        auto onTerritoryOf = [&](const Vec3& p, const ElemSamples& of, const ElemSamples& b, const Owners& owners) {
            return !b.pts.empty() && sameSheet(of, b, x.kind) &&
                   nearInterior(p, b, x.kind, kAnchorTolerance, besideMargin(b, x.kind)) &&
                   onDescribedSurfaces(b, owners);
        };
        // (On a baseline element other than the picked one: where the picked element itself was is not something else's
        // territory, also when the picked element moved away from it.)
        // The baseline elements whose box lies within `reach` of element `of`'s box (made once per element and reach).
        std::map<std::pair<const ElemSamples*, double>, std::vector<std::uint32_t>> nearby;
        auto near = [&](const ElemSamples& of, double reach) -> const std::vector<std::uint32_t>& {
            auto it = nearby.find({&of, reach});
            if (it != nearby.end())
                return it->second;
            std::vector<std::uint32_t> out;
            const Vec3 lo = sub(of.boxMin, Vec3{reach, reach, reach}), hi = add(of.boxMax, Vec3{reach, reach, reach});
            for (std::uint32_t i = 0; i < B.size(); ++i)
                if (i != pick.base.index && !B[i].pts.empty() && B[i].boxMin.x <= hi.x && B[i].boxMax.x >= lo.x &&
                    B[i].boxMin.y <= hi.y && B[i].boxMax.y >= lo.y && B[i].boxMin.z <= hi.z && B[i].boxMax.z >= lo.z)
                    out.push_back(i);
            return nearby.emplace(std::make_pair(&of, reach), std::move(out)).first->second;
        };
        // (p a point of `of`, so only the elements near `of` need looking at.)
        auto onBaseline = [&](const Vec3& p, const ElemSamples& of) {
            for (const std::uint32_t i : near(of, kAnchorTolerance))
                if (onTerritoryOf(p, of, B[i], now))
                    return true;
            return false;
        };
        // (p anywhere.)
        auto onBaselineAnywhere = [&](const Vec3& p, const ElemSamples& of) {
            for (std::uint32_t i = 0; i < B.size(); ++i)
                if (i != pick.base.index && onTerritoryOf(p, of, B[i], *pick.owners))
                    return true;
            return false;
        };
        // The baseline elements that the edit may have moved to p, mapped through it: where their territory is now.
        // Only elements within reach of p are looked at (their baseline box within twice the largest shift of any
        // owner, plus 1 mm: an element moves with its owners), each mapped once. One that cannot be described or mapped
        // counts as lying at p (it might have moved there).
        const double reach = 2.0 * largestOwnerShift(*pick.owners, now) + 1.0;
        // (`unknown` is set when the only reason is an element that cannot be mapped.)
        bool unknown = false;
        auto onMovedBaseline = [&](const Vec3& p, const ElemSamples& of) {
            unknown = false;
            if (!pick.builder) {
                unknown = true;
                return true; // nothing can be mapped: as before, the element stays merged
            }
            bool unmapped = false;
            for (const std::uint32_t i : near(of, reach)) {
                if (outsideBox(p, B[i].boxMin, B[i].boxMax, reach))
                    continue;
                // (Mapped once per element and edited script, for every reference of the baseline: TruthBuilder.)
                const std::optional<ElemSamples>& m = pick.builder->mappedTo(SubId{x.kind, i}, now);
                if (!m) {
                    unmapped = true;
                    continue;
                }
                if (!m->pts.empty() && onTerritoryOf(p, of, *m, now)) // no samples: where it was, onBaseline's business
                    return true;
            }
            unknown = unmapped;
            return unmapped;
        };
        // When every face of the description is one sweep's and that sweep moved or was scaled (profileScale), the
        // picked element moved with it, and so did everything else of that sweep: a point is checked where it was
        // (elements that did not move) and where the sweep's map brings it from (elements that moved with it). A part
        // on neither is new territory (the floor of a moved pocket where a suppressed cut had taken a corner of it).
        const Feature* fa = nullptr;
        const Feature* fb = nullptr;
        double fk = 1;
        {
            std::set<std::string> features;
            for (const Constraint& c : d.cons)
                for (const auto& alt : c.alts)
                    features.insert(featureOfOwner(alt));
            if (features.size() == 1) {
                const auto a = pick.owners->sweeps.find(*features.begin());
                const auto b = now.sweeps.find(*features.begin());
                if (a != pick.owners->sweeps.end() && b != now.sweeps.end())
                    if (const auto k = profileScale(a->second, b->second)) {
                        fa = &a->second;
                        fb = &b->second;
                        fk = *k;
                    }
            }
        }
        const bool movedWithSweep = x.moved && fa != nullptr;
        std::vector<std::uint32_t> still;
        bool allUnknown = true; // every merged element is merged only for an element that cannot be mapped
        for (const std::uint32_t e : te.partial) {
            const std::vector<char>* mask = nullptr;
            for (const auto& [m, bits] : masks)
                if (m == e)
                    mask = &bits;
            bool merged = mask == nullptr || (x.weak && x.isClaimed(e));
            bool onlyUnknown = !merged;
            for (std::size_t j = 0; (!merged || onlyUnknown) && j < N[e].pts.size(); ++j) {
                if ((*mask)[j] != 0)
                    continue;
                const Vec3& p = N[e].pts[j];
                // (On, not inside: the samples of an element that kept part of the picked one's border lie on that
                // border; a sibling's territory beyond the border has samples further than the tolerance from it.)
                if (!x.mapped.pts.empty() && nearElement(p, x.mapped, x.kind, pickTol))
                    continue; // on what was picked
                if (!x.moved && !onBaseline(p, N[e]))
                    continue; // new territory
                if (movedWithSweep && !onBaseline(p, N[e]) && !onBaselineAnywhere(similarUnmap(*fa, *fb, fk, p), N[e]))
                    continue; // new territory of an element that moved with its sweep
                if (x.moved && !movedWithSweep && !onBaseline(p, N[e]) && !onMovedBaseline(p, N[e]))
                    continue; // new territory of an element that moved otherwise: nothing had it, here or moved here
                const bool concrete = !(x.moved && !movedWithSweep && !onBaseline(p, N[e]) && unknown);
                merged = true;
                onlyUnknown = onlyUnknown && !concrete;
                if (std::getenv("RAYCAD_ORACLE_DEBUG_MERGED") != nullptr) {
                    std::string on;
                    for (std::uint32_t i = 0; i < B.size(); ++i)
                        if (i != pick.base.index && onTerritoryOf(p, N[e], B[i], *pick.owners))
                            on += " b" + std::to_string(i) + "(t" + std::to_string(B[i].type) + ")";
                    std::printf("merged: element %u (t%d) sample (%.3f %.3f %.3f) moved %d on%s\n", e, N[e].type, p.x,
                                p.y, p.z, x.moved ? 1 : 0, on.c_str());
                }
            }
            if (merged) {
                still.push_back(e);
                allUnknown = allUnknown && onlyUnknown;
            }
        }
        te.partial = still;
        te.merged = !still.empty();
        // Merged only because a baseline element that cannot be mapped through the edit might have moved there (a
        // direct edit's faces among them: T-081b directedit 17:29, 11:17, 6:38): whether the element took in another's
        // ground cannot be told, so an Exact answer is not judged (a Merged one of exactly E is right either way).
        if (te.merged && allUnknown)
            x.mergeUnknown = true;
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
        withoutNewPieces(x.expected);
        withoutOtherSplitLines(x.expected);
        ownTerritory(x.expected);
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
    withoutNewPieces(x.expected);
    withoutOtherSplitLines(x.expected);
    ownTerritory(x.expected);
    // A weak reference that moved, whose mapped pick and the mapped position of another piece of its logical element
    // both fall on one element: the map is only good to the anchor tolerance, and a vertex where a moved wall meets
    // an unmoved curved face slides along it, so whether that element continues both (a merge) or only one cannot be
    // told.
    // A Merged answer with exactly E is right either way; only an Exact one cannot be judged (Expectation::
    // mergeUnknown).
    if (x.weak && x.moved)
        for (const std::uint32_t e : x.expected.elems)
            if (x.isAnchored(e) && x.isClaimed(e)) {
                x.mergeUnknown = true;
                x.sharedAnchor = true;
            }
    // Two vertices of the logical element both near the mapped pick (the ends of an edge a few micrometres long, both
    // on the description's faces: T-081b holdout1 17:7), and the mapped pick on exactly one of them: a vertex continues
    // as one vertex, the one it lies on.
    if (x.kind == SubKind::Vertex && x.expected.elems.size() >= 2 && !x.mapped.pts.empty()) {
        std::vector<std::uint32_t> on;
        bool othersApart = true;
        for (const std::uint32_t e : x.expected.elems) {
            const double dist = N[e].nodes.empty() ? 1e300 : norm(sub(N[e].nodes.front(), x.mapped.pts.front()));
            if (dist <= kExactVertexTolerance)
                on.push_back(e);
            else
                othersApart = othersApart && dist > 2 * kExactVertexTolerance;
        }
        if (on.size() == 1 && othersApart) {
            x.expected = TruthEval{};
            x.expected.elems = on;
        }
    }
    if (x.kind == SubKind::Vertex && x.expected.elems.size() >= 2) {
        x.twins = true;
        const Vec3 p = N[x.expected.elems.front()].nodes.front();
        for (const std::uint32_t e : x.expected.elems)
            x.twins = x.twins && norm(sub(N[e].nodes.front(), p)) <= 1e-4;
    }
    // Edges that a boolean left lying on each other (one along a stretch of the other): position cannot tell them
    // apart either. Only an answer congruent in place with the picked element, when nothing moved, is accepted as the
    // twin (judgeAnswer); the edges must each share territory with every other one, exactly.
    if (x.kind == SubKind::Edge && x.expected.elems.size() >= 2 && !x.moved && x.congruentIndex >= 0) {
        x.twins = true;
        for (const std::uint32_t a : x.expected.elems)
            for (const std::uint32_t b : x.expected.elems)
                x.twins = x.twins && (a == b || overlap(N[a], N[b], x.kind, kExactVertexTolerance));
    }
    // SLIDING CROSSINGS (vertices and edges only). A vertex or edge where a moved or resized feature's face crosses a
    // face (or edge) that stayed, slides along the one that stayed: the map moves each point with its owners and then
    // projects it onto the edited carriers, which lands on the edited crossing curve but not where on it the crossing
    // now is. So no part of the logical element overlaps the mapped pick, and the continuation is told here instead,
    // from first principles, or not at all:
    //  (a) a seam vertex (on only its two faces, seamVertex): no third face makes it; it only splits or closes the
    //  curve
    //      where the two faces meet, at the seam of the periodic one (a hole's wall), whose parametrisation moves with
    //      that face. Its identity is "the seam vertex of where these two faces meet", as a described vertex's is
    //      "where these faces meet". When the logical element had exactly one seam vertex at pick time (the pick) and
    //      exactly one element fitting the description is one now, that one continues it. (Two periodic faces would
    //      give two seam vertices on their curve: then neither count is one, and (b) decides);
    //  (b) otherwise, by position, with a clear margin: the parts fitting the description (without the elements a
    //      mapped sibling claims), each scored by how far the mapped pick lies from it. Every other piece of the
    //      logical element at pick time is mapped through the same edit, and slides along the same surfaces by an error
    //      of the same kind, so the arrangement survives up to that error: the part nearest the mapped pick continues
    //      it when every other part is at least kCrossingMargin times as far from the mapped pick, and every other
    //      mapped piece at least kCrossingMargin times as far from that part as the mapped pick is. A piece that cannot
    //      be mapped could be anywhere: not judged.
    // Both need the part within reach of the mapped pick (twice the largest owner shift plus 1 mm: a crossing does not
    // slide further than the edit moved its surfaces, with room for shallow crossing angles). Both can say WRONG: any
    // other answer than the part chosen is judged against it as usual.
    if (x.expected.elems.empty() && anchoredParts.empty() && x.moved && !x.mapped.pts.empty() &&
        x.kind != SubKind::Face) {
        const double reach = 2.0 * largestOwnerShift(*pick.owners, now) + 1.0;
        const std::vector<std::uint32_t> before = evalTruth(withoutSeparators(d), *pick.owners, *pick.baseline).elems;
        std::vector<std::uint32_t> chosen;
        char why[200];
        if (x.kind == SubKind::Vertex && seamVertex(*pick.baseline, pick.base.index)) {
            std::size_t then = 0;
            for (const std::uint32_t b : before)
                if (seamVertex(*pick.baseline, b))
                    ++then;
            std::vector<std::uint32_t> seamNow;
            for (const std::uint32_t e : x.strict.elems)
                if (seamVertex(smp, e))
                    seamNow.push_back(e);
            const double dist = seamNow.size() == 1 ? meanDistanceTo(x.mapped, N[seamNow.front()], x.kind) : 1e300;
            if (then == 1 && seamNow.size() == 1 && dist <= reach) {
                chosen = seamNow;
                std::snprintf(why, sizeof why,
                              "seam: the one vertex on only its two faces, then and now, %.3f mm from the mapped pick",
                              dist);
                x.crossing = why;
            }
        }
        if (chosen.empty()) {
            std::map<int, std::vector<std::uint32_t>> parts;
            for (const std::uint32_t e : x.strict.elems)
                if (!sliver(N[e], x.kind) && !(x.weak && x.isClaimed(e)))
                    parts[x.componentOf(e)].push_back(e);
            std::vector<ElemSamples> others;
            bool placed = true;
            for (const std::uint32_t b : before) {
                if (b == pick.base.index)
                    continue;
                bool unused = false;
                ElemSamples m = mapElement(*pick.baseline, SubId{x.kind, b}, d, *pick.owners, now, unused);
                placed = placed && !m.pts.empty();
                others.push_back(std::move(m));
            }
            auto distance = [&](const ElemSamples& from, const std::vector<std::uint32_t>& part) {
                double best = 1e300;
                for (const std::uint32_t e : part)
                    best = std::min(best, meanDistanceTo(from, N[e], x.kind));
                return best;
            };
            const std::vector<std::uint32_t>* best = nullptr;
            double dBest = 1e300, dNext = 1e300;
            for (const auto& [component, part] : parts) {
                const double dist = distance(x.mapped, part);
                if (dist < dBest) {
                    dNext = dBest;
                    dBest = dist;
                    best = &part;
                } else {
                    dNext = std::min(dNext, dist);
                }
            }
            double dOther = 1e300;
            if (best != nullptr)
                for (const ElemSamples& o : others)
                    dOther = std::min(dOther, distance(o, *best));
            if (placed && best != nullptr && dBest <= reach && dNext >= kCrossingMargin * dBest &&
                dOther >= kCrossingMargin * dBest) {
                chosen = *best;
                std::snprintf(why, sizeof why,
                              "nearest: %.3f mm from the mapped pick; next part %.3f mm, nearest other mapped piece "
                              "%.3f mm",
                              dBest, dNext >= 1e299 ? -1.0 : dNext, dOther >= 1e299 ? -1.0 : dOther);
                x.crossing = why;
            }
        }
        if (!chosen.empty()) {
            x.expected.elems = chosen;
            for (const std::uint32_t e : chosen)
                if (std::binary_search(x.strict.partial.begin(), x.strict.partial.end(), e))
                    x.expected.partial.push_back(e);
            x.expected.merged = !x.expected.partial.empty();
            ownTerritory(x.expected);
        }
    }
    // A piece of a face that did not move, where no face of the shape holds any of its ground any more: the piece is
    // gone (cut away by a moved feature: T-081b sorted block 5:15, a sliver of a boss's side beside a cut that moved
    // over it), whatever is left of the rest of its logical face. Only other pieces' continuations are left.
    if (x.expected.elems.empty() && x.weak && x.kind == SubKind::Face && !x.moved && !x.mapped.pts.empty()) {
        bool held = false;
        for (std::uint32_t e = 0; e < N.size() && !held; ++e)
            held = !N[e].pts.empty() && shareOn(x.mapped, N[e], x.kind) > 0.0;
        if (!held)
            return x;
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

namespace {

// NEW GROUND (T-081b round 2). Territory that did not exist at pick time and lies on the picked element's own carriers
// (an element of its logical element) is neither required nor forbidden in a flagged answer (Split or Merged): a
// person reviews every flagged answer before a toolpath can be posted, and sees an extra sliver of the same face.
// Accepted when (a) the answer holds all the picked ground that survives (the mapped pick, or the pick in place when
// its carriers did not move: Expectation::inPlace), and some of it; (b) every element of the answer that is not
// expected is on the picked element's own carriers and on no ground another element held at pick time; (c) the
// answer is flagged. Never for an Exact answer. Returns why, or nothing.
std::string newGroundRule(const Expectation& x, const PickRecord& pick, const Samples& smp, Answer answer,
                          const std::vector<std::uint32_t>& X)
{
    if ((answer != Answer::Split && answer != Answer::Merged) || X.empty() || !x.noAnchor.empty())
        return {};
    const SubKind kind = x.kind;
    const auto& N = smp.of(kind);
    const auto& B = pick.baseline->of(kind);
    const double tol = overlapTolerance(kind);
    const std::vector<std::uint32_t>& E = x.expected.elems;
    auto covers = [&](const ElemSamples& P) {
        std::size_t onX = 0, onAny = 0;
        for (const Vec3& p : P.pts) {
            bool inX = false;
            for (const std::uint32_t e : X)
                inX = inX || nearElement(p, N[e], kind, tol);
            bool inAny = inX;
            for (std::uint32_t e = 0; e < N.size() && !inAny; ++e)
                inAny = !N[e].pts.empty() && nearElement(p, N[e], kind, tol);
            onX += inX ? 1 : 0;
            onAny += inAny ? 1 : 0;
        }
        return onX > 0 && static_cast<double>(onX) >= 0.98 * static_cast<double>(onAny);
    };
    // The pick in place when only what bounds it moved (the map moves such a pick with what bounds it), else mapped.
    if (!covers(x.inPlace && x.moved ? pick.picked() : x.mapped))
        return {};
    std::size_t extras = 0;
    for (const std::uint32_t e : X) {
        if (std::binary_search(E.begin(), E.end(), e))
            continue;
        if (!x.inLogical(e))
            return {};
        for (const Vec3& p : N[e].pts)
            for (std::uint32_t i = 0; i < B.size(); ++i)
                if (i != pick.base.index && sameSheet(N[e], B[i], kind) && nearElement(p, B[i], kind, tol))
                    return {};
        ++extras;
    }
    std::size_t missing = 0;
    for (const std::uint32_t e : E)
        missing += std::binary_search(X.begin(), X.end(), e) ? 0 : 1;
    return std::string(answerName(answer)) + " " + listOf(X) + " where the oracle expects " + listOf(E) + ": " +
           std::to_string(extras) + " more on new ground of the picked element's carriers, " + std::to_string(missing) +
           " expected left out that hold none of the surviving picked ground";
}

Judgement judgeCore(const Expectation& x, const PickRecord& pick, const Owners& now, const Samples& smp,
                    Answer answer, std::vector<std::uint32_t> X);

} // namespace

Judgement judgeAnswer(const Expectation& x, const PickRecord& pick, const Owners& now, const Samples& smp,
                      Answer answer, std::vector<std::uint32_t> X)
{
    Judgement j = judgeCore(x, pick, now, smp, answer, X);
    if (isWrong(j.verdict)) {
        std::sort(X.begin(), X.end());
        X.erase(std::unique(X.begin(), X.end()), X.end());
        bool valid = true;
        for (const std::uint32_t e : X)
            valid = valid && e < smp.of(x.kind).size();
        if (valid)
            if (std::string why = newGroundRule(x, pick, smp, answer, X); !why.empty()) {
                j.verdict = Verdict::Correct;
                j.newGround = std::move(why);
            }
    }
    return j;
}

namespace {

Judgement judgeCore(const Expectation& x, const PickRecord& pick, const Owners& now, const Samples& smp,
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
        std::binary_search(E.begin(), E.end(), X[0]) &&
        (x.kind != SubKind::Edge || static_cast<long>(X[0]) == x.congruentIndex)) {
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
        else if (x.sharedAnchor && !X.empty() && X != E &&
                 std::all_of(X.begin(), X.end(), [&](std::uint32_t e) { return x.inLogical(e); })) {
            j.verdict = Verdict::Unjudged; // Expectation::sharedAnchor
            j.unjudged = true;
        } else if (X.empty() || !std::includes(E.begin(), E.end(), X.begin(), X.end()))
            j.verdict = Verdict::WrongOther;
        else if (answer == Answer::Exact || answer == Answer::Repaired) {
            // The one piece that continues the pick; an element that also continues another baseline piece is a
            // merge, and with two or more pieces the pick came apart.
            if (X.size() != 1) {
                j.verdict = Verdict::WrongOther;
            } else if (X != E) {
                j.verdict = Verdict::WrongIncomplete;
            } else if (x.mergeUnknown) {
                j.verdict = Verdict::Unjudged;
                j.unjudged = true;
            } else {
                j.verdict = x.expected.merged ? Verdict::WrongSilentMerge : Verdict::Correct;
            }
        } else
            j.verdict = X == E ? Verdict::Correct : Verdict::WrongIncomplete;
        return j;
    }
    // An Exact answer of exactly E, where whether E took in another element's ground cannot be told (ownTerritory).
    if (x.mergeUnknown && x.expected.merged && (answer == Answer::Exact || answer == Answer::Repaired)) {
        std::vector<std::uint32_t> sorted = X;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (sorted == E) {
            j.verdict = Verdict::Unjudged;
            j.unjudged = true;
            return j;
        }
    }
    j.verdict = judge(answer, X, x.expected);
    if (isWrong(j.verdict) && answer != Answer::Broken &&
        newTerritory(x, pick, now, smp, answer, X, j.verdict, j.accepted))
        j.verdict = Verdict::Correct;
    return j;
}

} // namespace

} // namespace ray::naming_oracle
