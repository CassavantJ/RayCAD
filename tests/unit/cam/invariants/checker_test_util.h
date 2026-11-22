// Helpers of the invariant checker's tests (task T-058): parts built with the kernel, tools, a geometry on a part, and
// running the checker.
#pragma once

#include "checker.h"
#include "reference_paths.h"

#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace ray::cam::invariants::test {

inline kernel::Shape box(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    REQUIRE(s);
    return *s;
}

inline kernel::Shape cut(const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Cut, a, b, ctx);
    REQUIRE(r);
    return r->shape;
}

inline kernel::Shape cylinder(double x, double y, double z0, double r, double h)
{
    auto c = kernel::makeCylinder(Vec3{x, y, z0}, Vec3{0, 0, 1}, r, h);
    REQUIRE(c);
    return *c;
}

// A geometry on one part in the identity frame; the stock is the part's box unless given.
inline ResolvedGeometry geometryOn(const kernel::Shape& part, std::optional<Box3> stock = std::nullopt)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = stock ? *stock : kernel::bounds(part);
    return g;
}

inline lib::Tool flatMill(double diameter, double fluteLength = 20)
{
    lib::Tool t;
    t.type = lib::ToolType::FlatEndMill;
    t.flutes = 3;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 40;
    return t;
}

inline lib::Tool ballMill(double diameter, double fluteLength = 20)
{
    lib::Tool t = flatMill(diameter, fluteLength);
    t.type = lib::ToolType::BallEndMill;
    return t;
}

inline lib::MachineDef machine()
{
    lib::MachineDef m;
    m.axes = {lib::AxisDef{'X', 0, 762, 0, 0, 0}, lib::AxisDef{'Y', 0, 406, 0, 0, 0},
              lib::AxisDef{'Z', -508, 0, 0, 0, 0}};
    m.maxRpm = 12000;
    return m;
}

// Claims for a cutting toolpath: spindle on and clockwise, the stock top as the rapid floor.
inline Claims claims(const ResolvedGeometry& g, double rpm = 6000)
{
    Claims c;
    c.spindleRpm = rpm;
    c.rapidFloorZ = g.stockBounds.max.z;
    return c;
}

// The checker with one shared mesh per geometry (kept by the caller in `part`).
inline std::vector<Finding> run(const Toolpath& path, const ResolvedGeometry& g, const lib::Tool& tool, Claims c,
                                const lib::Holder* holder = nullptr, const lib::MachineDef* m = nullptr,
                                const CheckOptions& o = {})
{
    return check(CheckInput{path, g, tool, holder, m, std::move(c)}, o);
}

inline bool has(const std::vector<Finding>& f, const std::string& id)
{
    return std::any_of(f.begin(), f.end(), [&](const Finding& x) { return x.id == id; });
}

inline const Finding* find(const std::vector<Finding>& f, const std::string& id)
{
    const auto it = std::find_if(f.begin(), f.end(), [&](const Finding& x) { return x.id == id; });
    return it == f.end() ? nullptr : &*it;
}

// Every finding is `id` (and there is one).
inline void onlyThis(const std::vector<Finding>& f, const std::string& id)
{
    INFO("findings:\n" << describe(f));
    REQUIRE(!f.empty());
    for (const Finding& x : f) {
        CHECK(x.id == id);
    }
}

// `id` is found, and every finding is one of `ids` (consequences of the same mistake).
inline void mainly(const std::vector<Finding>& f, const std::string& id, std::vector<std::string> ids)
{
    INFO("findings:\n" << describe(f));
    REQUIRE(has(f, id));
    ids.push_back(id);
    for (const Finding& x : f) {
        CHECK(std::find(ids.begin(), ids.end(), x.id) != ids.end());
    }
}

inline void clean(const std::vector<Finding>& f)
{
    INFO("findings:\n" << describe(f));
    CHECK(f.empty());
}

} // namespace ray::cam::invariants::test
