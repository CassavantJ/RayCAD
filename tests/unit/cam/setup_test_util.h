// Helpers of the setup resolution tests (task T-033): transforms applied by hand, frame checks, and resolveSetup that
// fails the test or must fail.
#pragma once

#include <ray/cam.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <sstream>
#include <string>

namespace ray::cam::test {

inline Vec3 apply(const Xform& xf, Vec3 p)
{
    const double* m = xf.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

// Row i of the rotation: WCS axis i (0 X, 1 Y, 2 Z) in model coordinates, for wcsFromModel.
inline Vec3 axisOf(const Xform& wcsFromModel, int i)
{
    return Vec3{wcsFromModel.m[i * 4], wcsFromModel.m[i * 4 + 1], wcsFromModel.m[i * 4 + 2]};
}

inline Vec3 crossOf(Vec3 a, Vec3 b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline double dotOf(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline std::string text(Vec3 v)
{
    std::ostringstream s;
    s.precision(17);
    s << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return s.str();
}

inline bool near3(Vec3 a, Vec3 b, double tol)
{
    return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.z - b.z) <= tol;
}

inline bool same3(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// The frame is a right-handed rigid move, and modelFromWcs is its inverse.
inline void checkFrame(const ResolvedSetup& rs)
{
    const Vec3 x = axisOf(rs.wcsFromModel, 0);
    const Vec3 y = axisOf(rs.wcsFromModel, 1);
    const Vec3 z = axisOf(rs.wcsFromModel, 2);
    INFO("X " << text(x) << " Y " << text(y) << " Z " << text(z));
    CHECK(std::abs(dotOf(x, x) - 1.0) < 1e-15);
    CHECK(std::abs(dotOf(y, y) - 1.0) < 1e-15);
    CHECK(std::abs(dotOf(z, z) - 1.0) < 1e-15);
    CHECK(std::abs(dotOf(x, y)) < 1e-15);
    CHECK(std::abs(dotOf(y, z)) < 1e-15);
    CHECK(std::abs(dotOf(z, x)) < 1e-15);
    CHECK(near3(crossOf(x, y), z, 1e-15)); // right-handed
    for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{100, -20, 7}, Vec3{-3, 55, 250}}) {
        CHECK(near3(apply(rs.modelFromWcs, apply(rs.wcsFromModel, p)), p, 1e-12));
    }
}

// Every value of the result, as bits: equal hashes mean bit-identical frames and bounds.
inline Hash256 hashOf(const ResolvedSetup& rs)
{
    Hasher h;
    for (const double v : rs.wcsFromModel.m) {
        h.f64(v);
    }
    for (const double v : rs.modelFromWcs.m) {
        h.f64(v);
    }
    for (const Box3& b : {rs.partBounds, rs.stockBounds}) {
        h.f64(b.min.x).f64(b.min.y).f64(b.min.z).f64(b.max.x).f64(b.max.y).f64(b.max.z);
    }
    return h.finish();
}

inline ResolvedSetup resolveOk(const CamSnapshot& snap, const Setup& setup, const naming::IResolver& resolver,
                               SetupResolveOptions options = {})
{
    JobContext ctx;
    auto rs = resolveSetup(snap, setup, resolver, ctx, options);
    if (!rs) {
        FAIL("resolveSetup failed: " << rs.error().message);
    }
    return *rs;
}

inline Error resolveFails(const CamSnapshot& snap, const Setup& setup, const naming::IResolver& resolver)
{
    JobContext ctx;
    auto rs = resolveSetup(snap, setup, resolver, ctx);
    if (rs) {
        FAIL("resolveSetup should have failed");
    }
    return rs.error();
}

// The eight corners and the top centre of the documented numbering (cam.h), of a box in the WCS orientation.
inline Vec3 cornerOf(const Box3& b, int k)
{
    const bool xMax = k == 1 || k == 2 || k == 5 || k == 6;
    const bool yMax = k == 2 || k == 3 || k == 6 || k == 7;
    return Vec3{xMax ? b.max.x : b.min.x, yMax ? b.max.y : b.min.y, k < 4 ? b.max.z : b.min.z};
}

inline Vec3 topCentreOf(const Box3& b)
{
    return Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.max.z};
}

// The box, in the orientation whose axes (in model coordinates) are x, y, z, of the given model points.
template <class Points>
Box3 boxIn(const Points& points, Vec3 x, Vec3 y, Vec3 z)
{
    Box3 b{Vec3{1e300, 1e300, 1e300}, Vec3{-1e300, -1e300, -1e300}};
    for (const Vec3& p : points) {
        const Vec3 q{dotOf(x, p), dotOf(y, p), dotOf(z, p)};
        b.min = Vec3{std::min(b.min.x, q.x), std::min(b.min.y, q.y), std::min(b.min.z, q.z)};
        b.max = Vec3{std::max(b.max.x, q.x), std::max(b.max.y, q.y), std::max(b.max.z, q.z)};
    }
    return b;
}

inline std::array<Vec3, 8> cornersOfModelBox(const Box3& b)
{
    std::array<Vec3, 8> out;
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] =
            Vec3{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
    }
    return out;
}

inline Box3 grown(const Box3& b, const BoxStock& s)
{
    return Box3{Vec3{b.min.x - s.xNeg, b.min.y - s.yNeg, b.min.z - s.zNeg},
                Vec3{b.max.x + s.xPos, b.max.y + s.yPos, b.max.z + s.zPos}};
}

inline Box3 shifted(const Box3& b, Vec3 by)
{
    return Box3{Vec3{b.min.x - by.x, b.min.y - by.y, b.min.z - by.z},
                Vec3{b.max.x - by.x, b.max.y - by.y, b.max.z - by.z}};
}

inline bool nearBox(const Box3& a, const Box3& b, double tol)
{
    return near3(a.min, b.min, tol) && near3(a.max, b.max, tol);
}

inline std::string text(const Box3& b)
{
    return text(b.min) + " - " + text(b.max);
}

// An element of `kind` of `shape` that passes `pred`, as a SubId (the test fails when there is none).
template <class Pred>
kernel::SubId findEdge(const kernel::Shape& shape, Pred pred)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId sub{kernel::SubKind::Edge, i};
        auto info = kernel::curveInfo(shape, sub);
        if (info && pred(*info)) {
            return sub;
        }
    }
    FAIL("no such edge");
    return kernel::SubId{};
}

template <class Pred>
kernel::SubId findVertex(const kernel::Shape& shape, Pred pred)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Vertex); ++i) {
        const kernel::SubId sub{kernel::SubKind::Vertex, i};
        auto p = kernel::vertexPoint(shape, sub);
        if (p && pred(*p)) {
            return sub;
        }
    }
    FAIL("no such vertex");
    return kernel::SubId{};
}

} // namespace ray::cam::test
