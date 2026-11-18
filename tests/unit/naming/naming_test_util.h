// Helpers shared by the naming tests. No OCCT header here (PLAN.md 0.4 rule 1).
#pragma once
#include <catch2/catch_test_macros.hpp>

#include <ray/naming.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ray::naming::test {

// The value of a Result, failing the test with the error message if there is none.
template <class T>
T take(Result<T> result)
{
    INFO("unexpected error: " << (result.has_value() ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    return std::move(*result);
}

inline FeatureId featureId(std::uint8_t seed)
{
    FeatureId id;
    for (std::size_t i = 0; i < id.value.bytes.size(); ++i)
        id.value.bytes[i] = static_cast<std::uint8_t>(seed * 31u + i);
    return id;
}

inline BodyId bodyId(std::uint8_t seed)
{
    BodyId id;
    for (std::size_t i = 0; i < id.value.bytes.size(); ++i)
        id.value.bytes[i] = static_cast<std::uint8_t>(seed * 17u + 3u * i);
    return id;
}

inline std::vector<kernel::SubId> allElements(const kernel::Shape& shape)
{
    std::vector<kernel::SubId> out;
    for (const kernel::SubKind kind : {kernel::SubKind::Vertex, kernel::SubKind::Edge, kernel::SubKind::Face})
        for (std::uint32_t i = 0; i < shape.count(kind); ++i)
            out.push_back(kernel::SubId{kind, i});
    return out;
}

// What an element is, straight from the kernel (independent of the naming code): a vertex's point; an edge's
// start, end and length; a face's centroid and area.
inline std::vector<double> geometryOf(const kernel::Shape& shape, kernel::SubId sub)
{
    switch (sub.kind) {
    case kernel::SubKind::Vertex: {
        const Vec3 p = take(kernel::vertexPoint(shape, sub));
        return {p.x, p.y, p.z};
    }
    case kernel::SubKind::Edge: {
        const kernel::CurveInfo c = take(kernel::curveInfo(shape, sub));
        return {c.start.x, c.start.y, c.start.z, c.end.x, c.end.y, c.end.z, c.length};
    }
    case kernel::SubKind::Face: {
        const kernel::SurfaceInfo s = take(kernel::surfaceInfo(shape, sub));
        return {s.centroid.x, s.centroid.y, s.centroid.z, s.area};
    }
    default:
        return {};
    }
}

inline bool sameGeometry(const std::vector<double>& a, const std::vector<double>& b, double tolerance)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > tolerance)
            return false;
    return true;
}

// Every name of the map with the geometry of the element(s) holding it (sorted), so two maps of differently
// ordered shapes can be compared.
inline std::map<Hash256, std::vector<std::vector<double>>> nameGeometry(const kernel::Shape& shape,
                                                                        const ElementMap& map)
{
    std::map<Hash256, std::vector<std::vector<double>>> out;
    for (const kernel::SubId sub : allElements(shape)) {
        const auto names = map.namesOf(sub);
        REQUIRE(!names.empty());
        for (const Hash256& name : names)
            out[name].push_back(geometryOf(shape, sub));
    }
    for (auto& [name, geometries] : out)
        std::sort(geometries.begin(), geometries.end());
    return out;
}

} // namespace ray::naming::test
