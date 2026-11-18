// The naming oracle (task T-081a). See oracle.h. Ported from spikes/s4-naming/truth.cpp: OCCT calls replaced by
// the public kernel queries, owners for imported faces added.
#include "oracle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numeric>
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
        return t == static_cast<int>(T::Plane) && (!c.oriented || dot(es.nrm, c.a) > 0.99);
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

struct Tol {
    double node, sample, margin, thr;
};
Tol tolFor(SubKind k)
{
    if (k == SubKind::Face)
        return {1e-4, 0.06, 1e-4, 0.02};
    if (k == SubKind::Edge)
        return {2e-3, 2e-3, 2e-3, 0.05};
    return {2e-3, 2e-3, 2e-3, 0.5};
}
// Do the nodes of an element lie on the carrier? Every node within 10 x tol and at least 95 % within tol: nodes on
// the boundary of a face are computed on its edges, and the edges of a boolean result may lie off the surfaces by
// their (larger) tolerance.
bool nodesOn(const Carrier& c, const std::vector<Vec3>& nodes, double tol)
{
    std::size_t close = 0;
    for (const Vec3& p : nodes) {
        if (!c.on(p, 10.0 * tol))
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
    return s.in == nullptr || !*s.in || (*s.in)(projectOnto(*s.c, p), kBandMargin);
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

// ---------------------------------------------------------------- carriers
double FaceMesh::distanceWithin(const Vec3& p, double limit) const
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
            if (best <= limit * 0.25)
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
        if (info->type == kernel::SurfaceType::Plane)
            es.nrm = averageNormal(*mesh, range);
        if (info->type == kernel::SurfaceType::Cylinder)
            es.sigma = cylinderSide(*mesh, range, info->origin, unit(info->axis));
    }

    for (std::uint32_t i = 0; i < edges; ++i) {
        ElemSamples& es = out->e[kEdge][i];
        const SubId edge{SubKind::Edge, i};
        for (const SubId& f : kernel::adjacent(shape, edge, SubKind::Face))
            out->facesOf[1][i].push_back(f.index);
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
                        // (A face the mesher left without triangles cannot be checked, and is taken as fitting.)
                        const ElemSamples& face = smp.of(SubKind::Face)[f];
                        found = found || face.nodes.empty() ||
                                (typeMatches(o->c, face) && nodesOn(o->c, face.nodes, tolFor(SubKind::Face).node));
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
            if (frac < 0.98)
                ev.merged = true;
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
            d.why = "not unique at baseline";
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

Verdict judge(Answer answer, std::vector<std::uint32_t> X, const TruthEval& te, bool weak, long cong)
{
    std::sort(X.begin(), X.end());
    X.erase(std::unique(X.begin(), X.end()), X.end());
    const std::vector<std::uint32_t>& E = te.elems;
    if (answer == Answer::Broken)
        return E.empty() ? Verdict::BrokenGone : Verdict::Missed;
    if (weak) {
        if (E.empty())
            return Verdict::WrongGhost;
        if (!std::includes(E.begin(), E.end(), X.begin(), X.end()))
            return Verdict::WrongOther;
        if (cong >= 0 && (answer == Answer::Exact || answer == Answer::Repaired) &&
            !(X.size() == 1 && static_cast<long>(X[0]) == cong))
            return Verdict::WrongOther;
        return Verdict::Correct;
    }
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

// Is p within tol of element es (its triangles, polyline or point)?
bool nearElement(const Vec3& p, const ElemSamples& es, SubKind kind, double tol)
{
    if (es.nodes.empty() || outsideBox(p, es.boxMin, es.boxMax, tol))
        return false;
    if (kind == SubKind::Face)
        return es.mesh && es.mesh->distanceWithin(p, tol) <= tol;
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

bool overlap(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    return shareOn(a, b, kind) >= 0.1 || shareOn(b, a, kind) >= 0.1;
}

// Could a point of a face of the new shape lie ON baseline face b? Faces that meet at an angle share only their
// boundary; only faces on the same plane (or curved faces) can hold the same area.
bool sameSheet(const ElemSamples& a, const ElemSamples& b, SubKind kind)
{
    const int plane = static_cast<int>(kernel::SurfaceType::Plane);
    return kind != SubKind::Face || a.type != plane || b.type != plane || std::abs(dot(a.nrm, b.nrm)) > 0.99;
}

// Does element `e` of the new shape overlap a baseline element of its kind, other than the picked one, that still
// exists after the edit?
bool overlapsOtherBaseline(const ElemSamples& e, SubKind kind, const Samples& baseline, std::uint32_t picked,
                           const std::function<bool(SubId)>& survives)
{
    const auto& B = baseline.of(kind);
    for (std::uint32_t i = 0; i < B.size(); ++i)
        if (i != picked && sameSheet(e, B[i], kind) && overlap(e, B[i], kind) && survives(SubId{kind, i}))
            return true;
    return false;
}

} // namespace

Verdict reconsider(Verdict v, Answer answer, const std::vector<std::uint32_t>& elements, const TruthDesc& d,
                   const TruthEval& expected, const Owners& ow, const Samples& now, const Samples& baseline,
                   SubId picked, const std::function<bool(SubId)>& survives, bool& newTerritory)
{
    newTerritory = false;
    if (d.weak || answer == Answer::Broken || expected.elems.empty() || elements.empty() ||
        !(v == Verdict::WrongIncomplete || v == Verdict::WrongOther || v == Verdict::WrongSilentMerge))
        return v;
    const SubKind kind = d.kind;
    const auto& N = now.of(kind);
    const ElemSamples& P = baseline.of(kind)[picked.index];
    // The description without separators: the picked element's own logical face or edge, every piece of it.
    TruthDesc logical = d;
    logical.seps.clear();
    for (Constraint& c : logical.cons)
        c.seps.clear();
    const TruthEval L = evalTruth(logical, ow, now);
    auto inL = [&](std::uint32_t e) {
        return std::binary_search(L.elems.begin(), L.elems.end(), e);
    };
    auto isNewTerritory = [&](std::uint32_t e) {
        return inL(e) && !overlap(N[e], P, kind) &&
               !overlapsOtherBaseline(N[e], kind, baseline, picked.index, survives);
    };
    std::vector<std::uint32_t> X(elements);
    std::sort(X.begin(), X.end());
    X.erase(std::unique(X.begin(), X.end()), X.end());
    const std::vector<std::uint32_t>& E = expected.elems;
    // Every expected element that overlaps what was picked must be in the answer (all of E if none overlaps:
    // the element moved).
    std::vector<std::uint32_t> required;
    for (const std::uint32_t e : E)
        if (overlap(N[e], P, kind))
            required.push_back(e);
    if (required.empty())
        required = E;
    if (!std::includes(X.begin(), X.end(), required.begin(), required.end()))
        return v;
    // An answered element that was not expected must be new territory. An expected element that was not answered
    // and does not overlap what was picked is either new territory or the continuation of another baseline
    // element that the separators failed to exclude (a separator's band can miss it after an edit); leaving it out
    // is not wrong.
    std::vector<std::uint32_t> extra;
    std::set_difference(X.begin(), X.end(), E.begin(), E.end(), std::back_inserter(extra));
    for (const std::uint32_t e : extra)
        if (!isNewTerritory(e))
            return v;
    // An Exact answer on an element larger than the description: the part outside it must be new territory too,
    // and inside the logical element.
    if (answer == Answer::Exact && X.size() == 1 && (v == Verdict::WrongSilentMerge || expected.merged)) {
        // The element is on the description's carriers (it matched); what lies outside the description must
        // overlap no other baseline element: a region that was another element when the reference was picked
        // makes this a merge, and Exact is wrong.
        const std::uint32_t e = X.front();
        FitMasks masks;
        evalTruthImpl(d, ow, now, &masks);
        const std::vector<char>* mask = nullptr;
        for (const auto& [x, m] : masks)
            if (x == e)
                mask = &m;
        if (mask == nullptr)
            return v;
        const auto& B = baseline.of(kind);
        double outside = 0, onOther = 0;
        for (std::size_t j = 0; j < N[e].pts.size(); ++j) {
            if ((*mask)[j] != 0)
                continue;
            outside += N[e].w[j];
            for (std::uint32_t i = 0; i < B.size(); ++i)
                if (i != picked.index && sameSheet(N[e], B[i], kind) &&
                    nearElement(N[e].pts[j], B[i], kind, overlapTolerance(kind)) && survives(SubId{kind, i})) {
                    onOther += N[e].w[j];
                    break;
                }
        }
        if (outside > 0 && onOther >= 0.1 * outside)
            return v;
    }
    newTerritory = true;
    return Verdict::Correct;
}

} // namespace ray::naming_oracle
