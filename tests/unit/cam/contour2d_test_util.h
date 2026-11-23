// Helpers of the 2D contour operation tests (task T-053): a part built through the kernel, edge and face pickers,
// tools, a machine, the ResolvedGeometry of a setup on the part, a contour operation, and running the type's plan,
// generate and the toolpath invariant checker (tests/unit/cam/invariants, which shares no code with the generator).
#pragma once

#include "checker.h"
#include "toolpath_dump.h"

#include "cam/ops/contour2d.h" // src/cam's private header of the type

#include <ray/cam.h>
#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ray::cam::test {

inline constexpr double kPiC = 3.14159265358979323846;

template <class T>
T take(Result<T> r)
{
    if (!r) {
        FAIL(r.error().message);
    }
    return std::move(*r);
}

// ---------- Parts ----------

inline kernel::ProfileCurve lineC(Vec2 a, Vec2 b)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
}
inline kernel::ProfileCurve arcC(Vec2 a, Vec2 b, Vec2 c)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, 0, true, 0};
}
// A rectangle x0..x1 x y0..y1 with corners of radius r (r 0: sharp), counter-clockwise.
inline kernel::ProfileLoop roundedRect(double x0, double y0, double x1, double y1, double r)
{
    kernel::ProfileLoop l;
    if (r <= 0) {
        l.curves = {lineC({x0, y0}, {x1, y0}), lineC({x1, y0}, {x1, y1}), lineC({x1, y1}, {x0, y1}),
                    lineC({x0, y1}, {x0, y0})};
        return l;
    }
    l.curves = {lineC({x0 + r, y0}, {x1 - r, y0}), arcC({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}),
                lineC({x1, y0 + r}, {x1, y1 - r}), arcC({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}),
                lineC({x1 - r, y1}, {x0 + r, y1}), arcC({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}),
                lineC({x0, y1 - r}, {x0, y0 + r}), arcC({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r})};
    return l;
}
inline kernel::Shape prism(const std::vector<kernel::ProfileLoop>& loops, double z0, double height)
{
    JobContext ctx;
    const kernel::Profile p{kernel::Plane{Vec3{0, 0, z0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, loops};
    return take(kernel::extrude(p, height, 0, ctx)).op.shape;
}
inline kernel::Shape booleanOf(kernel::BoolOp op, const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    return take(kernel::boolean(op, a, b, ctx)).shape;
}

// The contour test part: a plate 0..100 x 0..80, Z 0..10, corners R10; a boss 20..80 x 15..65, Z 10..25, corners R8;
// a pocket 35..65 x 30..50 in the boss, corners R5, its floor at Z 18. The stock is the part's box (top Z 25).
inline kernel::Shape contourPart()
{
    const kernel::Shape plate = prism({roundedRect(0, 0, 100, 80, 10)}, 0, 10);
    const kernel::Shape boss = prism({roundedRect(20, 15, 80, 65, 8)}, 10, 15);
    const kernel::Shape pocket = prism({roundedRect(35, 30, 65, 50, 5)}, 18, 8);
    return booleanOf(kernel::BoolOp::Cut, booleanOf(kernel::BoolOp::Union, plate, boss), pocket);
}

// A straight edge whose middle is at `mid` (model frame), as a user picks it.
inline kernel::SubId lineEdgeAt(const kernel::Shape& shape, Vec3 mid)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId e{kernel::SubKind::Edge, i};
        auto c = kernel::curveInfo(shape, e);
        if (!c || c->type != kernel::CurveType::Line) {
            continue;
        }
        const Vec3 m{(c->start.x + c->end.x) / 2, (c->start.y + c->end.y) / 2, (c->start.z + c->end.z) / 2};
        if (std::abs(m.x - mid.x) < 1e-6 && std::abs(m.y - mid.y) < 1e-6 && std::abs(m.z - mid.z) < 1e-6) {
            return e;
        }
    }
    FAIL("no straight edge with its middle at " << mid.x << ", " << mid.y << ", " << mid.z);
    return {};
}
// A planar face perpendicular to Z at height z whose box contains (x, y).
inline kernel::SubId flatFaceAt(const kernel::Shape& shape, double z, double x, double y)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto s = kernel::surfaceInfo(shape, f);
        if (!s || s->type != kernel::SurfaceType::Plane || std::abs(std::abs(s->axis.z) - 1) > 1e-9) {
            continue;
        }
        if (std::abs(s->bounds.max.z - z) < 1e-6 && std::abs(s->bounds.min.z - z) < 1e-6 && s->bounds.min.x <= x &&
            x <= s->bounds.max.x && s->bounds.min.y <= y && y <= s->bounds.max.y) {
            return f;
        }
    }
    FAIL("no flat face at Z " << z << " over " << x << ", " << y);
    return {};
}

// ---------- Tools and machine ----------

inline lib::Tool endMill(double diameter, double fluteLength = 30, lib::ToolType type = lib::ToolType::FlatEndMill)
{
    lib::Tool t;
    t.type = type;
    t.flutes = 3;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 40;
    if (type == lib::ToolType::BallEndMill) {
        t.cornerRadius = diameter / 2;
    }
    return t;
}

inline lib::MachineDef contourMachine()
{
    lib::MachineDef m;
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', -100, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', -100, 406, 25400, 16500, 5000},
              lib::AxisDef{'Z', -508, 100, 25400, 16500, 5000}};
    m.maxRpm = 12000;
    return m;
}

// ---------- Operation and geometry ----------

// Heights: clearance 10, retract 5, feed plane 2 above the stock top, top 0 (the stock top), bottom `bottom` from the
// chain's height.
inline Operation contourOp(const std::string& params, double bottom = 0, double rpm = 6000, double feed = 800)
{
    Operation op;
    op.type = "contour2d";
    op.name = "Contour";
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = feed / 4;
    op.cutting.feedRamp = feed / 2;
    op.cutting.feedLeadIn = feed;
    op.cutting.feedLeadOut = feed;
    op.heights.clearance = 10;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = bottom;
    op.tool.stickOut = 45;
    return op;
}

// A setup on one part in the identity frame, the stock its box grown by `grow` in X and Y.
inline ResolvedGeometry geometryOn(const kernel::Shape& part, const std::vector<kernel::SubId>& picks, double grow = 0)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = kernel::bounds(part);
    g.stockBounds.min.x -= grow;
    g.stockBounds.min.y -= grow;
    g.stockBounds.max.x += grow;
    g.stockBounds.max.y += grow;
    for (const kernel::SubId& s : picks) {
        g.selected.push_back(s);
        g.selectedShapeIndex.push_back(0);
    }
    return g;
}

// One run of the contour type.
struct ContourCase {
    Operation op;
    lib::Tool tool = endMill(10);
    lib::MachineDef machine = contourMachine();
    ResolvedGeometry geom;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return ops::makeContour2dOperationType()->validate(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return ops::makeContour2dOperationType()->generate(input(), ctx);
    }
    Result<ops::contour2d::Contour2dPlan> plan() const
    {
        JobContext ctx;
        return ops::contour2d::planContour2d(input(), ctx);
    }
    // What the operation claims, for the checker: its spindle and heights, the direction, the stock to leave.
    invariants::Claims claims() const
    {
        invariants::Claims k = invariants::claimsFor(op, geom);
        const bool conventional = op.paramsJson.find("\"conventional\"") != std::string::npos;
        k.direction = conventional ? invariants::Direction::Conventional : invariants::Direction::Climb;
        const auto number = [&](const std::string& key) {
            const std::string pat = "\"" + key + "\":";
            const auto at = op.paramsJson.find(pat);
            return at == std::string::npos ? 0.0 : std::atof(op.paramsJson.c_str() + at + pat.size());
        };
        k.stockToLeaveWall = number("stockToLeave");
        k.stockToLeaveFloor = number("stockToLeaveAxial");
        const double top = geom.stockBounds.max.z;
        k.heights.clearance = top + op.heights.clearance;
        k.heights.retract = top + op.heights.retract;
        k.heights.feedPlane = top + op.heights.feedPlane;
        return k;
    }
    // generate(), which must succeed and keep every invariant the checker knows.
    Toolpath path() const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        const std::vector<invariants::Finding> f =
            invariants::check(invariants::CheckInput{*p, geom, tool, nullptr, &machine, claims()});
        INFO("invariant checker:\n" << invariants::describe(f));
        CHECK(f.empty());
        return *p;
    }
};

inline std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// The moves of a toolpath of one role.
inline std::vector<Move> movesOf(const Toolpath& p, MoveRole role)
{
    std::vector<Move> out;
    for (const Move& m : p.moves) {
        if (m.role == role) {
            out.push_back(m);
        }
    }
    return out;
}

} // namespace ray::cam::test
