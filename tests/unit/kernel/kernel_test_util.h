// Helpers shared by the kernel tests. No OCCT header here (PLAN.md 0.4 rule 1).
#pragma once
#include <catch2/catch_test_macros.hpp>

#include <ray/kernel.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ray::kernel::test {

// The value of a Result, failing the test with the error message if there is none.
template <class T>
T take(Result<T> result)
{
    INFO("unexpected error: " << (result.has_value() ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return std::move(*result);
}

inline Shape box(Vec3 min, Vec3 max)
{
    return take(makeBox(min, max));
}

inline double length(const Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// |actual - expected| <= rel * max(1, |expected|)
inline bool close(double actual, double expected, double rel)
{
    return std::abs(actual - expected) <= rel * std::max(1.0, std::abs(expected));
}
inline bool close(const Vec3& actual, const Vec3& expected, double rel)
{
    return length(sub(actual, expected)) <= rel * std::max(1.0, length(expected));
}

// Exact bit pattern of every double, so two fingerprints compare equal only if bit-identical.
inline void appendBits(std::vector<unsigned char>& out, double value)
{
    unsigned char bytes[sizeof(double)];
    std::memcpy(bytes, &value, sizeof(double));
    out.insert(out.end(), bytes, bytes + sizeof(double));
}
inline void appendBits(std::vector<unsigned char>& out, const Vec3& v)
{
    appendBits(out, v.x);
    appendBits(out, v.y);
    appendBits(out, v.z);
}

// What each SubId of the shape is, in SubId order: vertex points, edge end points and lengths, face centroids
// and areas, solid volumes. Two shapes with equal fingerprints have the same canonical order.
inline std::vector<unsigned char> orderFingerprint(const Shape& shape)
{
    std::vector<unsigned char> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Vertex); ++i)
        appendBits(out, take(vertexPoint(shape, SubId{SubKind::Vertex, i})));
    for (std::uint32_t i = 0; i < shape.count(SubKind::Edge); ++i) {
        const CurveInfo info = take(curveInfo(shape, SubId{SubKind::Edge, i}));
        appendBits(out, info.start);
        appendBits(out, info.end);
        appendBits(out, info.length);
    }
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const SurfaceInfo info = take(surfaceInfo(shape, SubId{SubKind::Face, i}));
        appendBits(out, info.centroid);
        appendBits(out, info.area);
    }
    appendBits(out, static_cast<double>(shape.count(SubKind::Solid)));
    return out;
}

} // namespace ray::kernel::test
