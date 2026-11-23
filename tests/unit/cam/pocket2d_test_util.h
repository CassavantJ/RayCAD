// Helpers of the 2D pocket operation tests (task T-054): parts built through the kernel (blocks with closed pockets,
// islands, an open step), tools and a machine, the ResolvedGeometry of a setup on the part, a pocket2d operation, and
// running the type with the independent toolpath invariant checker (tests/unit/cam/invariants) on every toolpath.
#pragma once

#include "invariants/checker.h"
#include "toolpath_dump.h"

#include "cam/ops/pocket2d.h" // src/cam's private header of the type

#include <ray/cam.h>
#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ray::cam::test {

inline constexpr double kPi = 3.14159265358979323846;

// ---------- Parts ----------

inline kernel::Shape box(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    if (!s) {
        FAIL("makeBox failed: " << s.error().message);
    }
    return *s;
}

inline kernel::Shape cut(const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Cut, a, b, ctx);
    if (!r) {
        FAIL("cut failed: " << r.error().message);
    }
    return r->shape;
}

inline kernel::ProfileCurve lineC(Vec2 a, Vec2 b)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
}
inline kernel::ProfileCurve arcC(Vec2 a, Vec2 b, Vec2 c, double r)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, r, true, 0};
}

// A counter-clockwise rounded rectangle (r = 0: sharp corners).
inline kernel::ProfileLoop roundedRect(double x0, double y0, double x1, double y1, double r)
{
    kernel::ProfileLoop l;
    if (r <= 0) {
        l.curves = {lineC({x0, y0}, {x1, y0}), lineC({x1, y0}, {x1, y1}), lineC({x1, y1}, {x0, y1}),
                    lineC({x0, y1}, {x0, y0})};
        return l;
    }
    l.curves = {lineC({x0 + r, y0}, {x1 - r, y0}), arcC({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, r),
                lineC({x1, y0 + r}, {x1, y1 - r}), arcC({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, r),
                lineC({x1 - r, y1}, {x0 + r, y1}), arcC({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, r),
                lineC({x0, y1 - r}, {x0, y0 + r}), arcC({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, r)};
    return l;
}
inline kernel::ProfileLoop circleLoop(double cx, double cy, double r)
{
    kernel::ProfileLoop l;
    l.curves = {kernel::ProfileCurve{kernel::ProfileCurve::Kind::Circle, {{cx, cy}}, r, true, 0}};
    return l;
}

// The prism of `loops` (outer first, then holes) from z0 up to z1.
inline kernel::Shape prism(std::vector<kernel::ProfileLoop> loops, double z0, double z1)
{
    const kernel::Profile p{kernel::Plane{Vec3{0, 0, z0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, std::move(loops)};
    JobContext ctx;
    auto r = kernel::extrude(p, z1 - z0, 0, ctx);
    if (!r) {
        FAIL("extrude failed: " << r.error().message);
    }
    return r->op.shape;
}

// The golden block: 100 x 70 x 20 (Z 0..20) with
//   pocket A (closed): x 8..52, y 8..62, corner radius 5, floor Z 12, with a round island r 5 at (30, 22) standing to
//     the top and a square island x 22..38, y 40..50 (corner radius 1) standing to Z 16 only;
//   pocket B (open): a step x 62..95, y 0..25 down to Z 14, open to the front (y < 0).
inline kernel::Shape goldenBlock()
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{100, 70, 20});
    s = cut(s, prism({roundedRect(8, 8, 52, 62, 5), circleLoop(30, 22, 5), roundedRect(22, 40, 38, 50, 1)}, 12, 21));
    s = cut(s, box(Vec3{21.5, 39.5, 16}, Vec3{38.5, 50.5, 21}));
    s = cut(s, box(Vec3{62, -1, 14}, Vec3{95, 25, 21}));
    return s;
}
// A block 60 x 40 x 15 with one closed pocket x0..x1, y0..y1 (corner radius r) down to floorZ.
inline kernel::Shape pocketBlock(double x0, double y0, double x1, double y1, double r, double floorZ)
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{60, 40, 15});
    return cut(s, prism({roundedRect(x0, y0, x1, y1, r)}, floorZ, 16));
}

// The up-facing planar faces at height z whose box lies in [x0, x1] x [y0, y1].
inline std::vector<kernel::SubId> upFaces(const kernel::Shape& s, double z, double x0 = -1e9, double y0 = -1e9,
                                          double x1 = 1e9, double y1 = 1e9)
{
    std::vector<kernel::SubId> out;
    const std::uint32_t n = s.count(kernel::SubKind::Face);
    for (std::uint32_t i = 0; i < n; ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, f);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        const auto sample = kernel::faceSample(s, f);
        if (!sample || sample->normal.z < 0.999) {
            continue;
        }
        const Box3& b = info->bounds;
        if (std::abs(b.min.z - z) > 1e-6 || std::abs(b.max.z - z) > 1e-6 || b.min.x < x0 - 1e-6 ||
            b.max.x > x1 + 1e-6 || b.min.y < y0 - 1e-6 || b.max.y > y1 + 1e-6) {
            continue;
        }
        out.push_back(f);
    }
    return out;
}
inline kernel::SubId upFace(const kernel::Shape& s, double z, double x0 = -1e9, double y0 = -1e9, double x1 = 1e9,
                            double y1 = 1e9)
{
    const auto faces = upFaces(s, z, x0, y0, x1, y1);
    INFO("an up-facing face at Z " << z);
    REQUIRE(faces.size() == 1);
    return faces.front();
}

// ---------- Tools and machine ----------

inline lib::Tool endMill(double diameter, double fluteLength = 25, double cornerRadius = 0)
{
    lib::Tool t;
    t.type = cornerRadius > 0 ? lib::ToolType::BullEndMill : lib::ToolType::FlatEndMill;
    t.flutes = 3;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 40;
    t.cornerRadius = cornerRadius;
    return t;
}

inline lib::MachineDef machine()
{
    lib::MachineDef m;
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 406, 25400, 16500, 5000},
              lib::AxisDef{'Z', -508, 0, 25400, 16500, 5000}};
    m.maxRpm = 12000;
    return m;
}

// ---------- Operation and geometry ----------

inline Operation pocketOp(const std::string& params, double rpm = 8000, double feed = 900)
{
    Operation op;
    op.type = "pocket2d";
    op.name = "Pocket";
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = feed / 2;
    op.cutting.feedRamp = feed * 0.75;
    op.cutting.feedLeadIn = feed;
    op.cutting.feedLeadOut = feed;
    op.heights.clearance = 15;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 40;
    return op;
}

// A setup on one part in the identity frame, the stock its box (grown by `grow` at the sides and on top).
inline ResolvedGeometry geometryOn(const kernel::Shape& part, const std::vector<kernel::SubId>& faces, double grow = 0)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = kernel::bounds(part);
    g.stockBounds.min.x -= grow;
    g.stockBounds.min.y -= grow;
    g.stockBounds.max.x += grow;
    g.stockBounds.max.y += grow;
    g.stockBounds.max.z += grow;
    for (const kernel::SubId& f : faces) {
        g.selected.push_back(f);
        g.selectedShapeIndex.push_back(0);
    }
    return g;
}

// A polygon of a rounded rectangle (each corner in 16 chords), counter-clockwise, for checker regions.
inline std::vector<Vec2> rectPoly(double x0, double y0, double x1, double y1, double r = 0)
{
    if (r <= 0) {
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    std::vector<Vec2> p;
    const auto corner = [&](double cx, double cy, double a0) {
        for (int k = 0; k <= 16; ++k) {
            const double a = a0 + k * (kPi / 2) / 16;
            p.push_back(Vec2{cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    };
    corner(x1 - r, y0 + r, -kPi / 2);
    corner(x1 - r, y1 - r, 0);
    corner(x0 + r, y1 - r, kPi / 2);
    corner(x0 + r, y0 + r, kPi);
    return p;
}
inline std::vector<Vec2> circlePoly(double cx, double cy, double r)
{
    std::vector<Vec2> p;
    for (int k = 0; k < 64; ++k) {
        const double a = 2 * kPi * k / 64;
        p.push_back(Vec2{cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return p;
}

// One run of the pocket2d type, checked by the independent checker.
struct Case {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = test::machine();
    ResolvedGeometry geom;
    std::vector<invariants::ClearClaim> clears; // the floors the toolpath must clear (coverage)
    std::vector<invariants::WallClaim> walls;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return ops::makePocket2dOperationType()->validate(input()); }
    Result<std::vector<std::string>> check() const { return ops::makePocket2dOperationType()->check(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return ops::makePocket2dOperationType()->generate(input(), ctx);
    }
    Result<ops::Pocket2dPlan> plan() const
    {
        JobContext ctx;
        return ops::planPocket2d(input(), ctx);
    }
    // The claims the operation makes about this case (pocket2d.h), from its parameters.
    invariants::Claims claims(double stockWall = 0, double stockFloor = 0, bool conventional = false,
                              double rampAngle = 2) const
    {
        invariants::Claims k = invariants::claimsFor(op, geom);
        k.wcsInMachine = Vec3{100, 100, -400};
        k.stockToLeaveWall = stockWall;
        k.stockToLeaveFloor = stockFloor;
        k.direction = conventional ? invariants::Direction::Conventional : invariants::Direction::Climb;
        k.maxRampAngleDeg = rampAngle + 1e-6;
        k.helixMinDiameter = tool.diameter * ops::kHelixMinFraction - 1e-6;
        k.clears = clears;
        k.walls = walls;
        return k;
    }
    // generate(), which must succeed and pass the independent checker with the claims.
    Toolpath path(const invariants::Claims& k) const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        const std::vector<invariants::Finding> found =
            invariants::check(invariants::CheckInput{*p, geom, tool, nullptr, &machine, k});
        INFO(invariants::describe(found));
        CHECK(found.empty());
        return *p;
    }
    Toolpath path() const { return path(claims()); }
};

inline invariants::ClearClaim clearClaim(std::vector<std::vector<Vec2>> loops, double floorZ, double allowed = 0.01)
{
    invariants::ClearClaim c;
    c.region = invariants::Region::of(std::move(loops));
    c.floorZ = floorZ;
    c.allowedAbove = allowed;
    return c;
}

inline std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Moves of a kind and role.
inline std::size_t countMoves(const Toolpath& p, MoveRole role, MoveKind kind)
{
    return static_cast<std::size_t>(
        std::count_if(p.moves.begin(), p.moves.end(), [&](const Move& m) { return m.role == role && m.kind == kind; }));
}
inline std::size_t countRole(const Toolpath& p, MoveRole role)
{
    return static_cast<std::size_t>(
        std::count_if(p.moves.begin(), p.moves.end(), [&](const Move& m) { return m.role == role; }));
}

} // namespace ray::cam::test
