// Helpers of the drilling operation tests (task T-035): parts built through the kernel, tools and a machine, the
// ResolvedGeometry a setup on the part gives, a drilling operation, and running the type's validate and generate.
#pragma once

#include "invariants/toolpath_invariants.h"
#include "toolpath_dump.h"

#include "cam/ops/drill.h" // src/cam's private header of the type

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
inline const double kTip118 = 1.0 / std::tan(59.0 * kPi / 180.0); // the point of a 118 degree drill per unit radius

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

// A hole tool: the closed half-section `rz` (radius, z) revolved a full turn about the vertical line through (x, y).
inline kernel::Shape revolved(double x, double y, const std::vector<Vec2>& rz)
{
    kernel::ProfileLoop loop;
    for (std::size_t i = 0; i < rz.size(); ++i) {
        loop.curves.push_back(
            kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {rz[i], rz[(i + 1) % rz.size()]}, 0, false, 0});
    }
    const kernel::Profile p{kernel::Plane{Vec3{x, y, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}}, {loop}};
    JobContext ctx;
    auto r = kernel::revolve(p, Vec3{x, y, 0}, Vec3{0, 0, 1}, 2 * kPi, ctx);
    if (!r) {
        FAIL("revolve failed: " << r.error().message);
    }
    return r->op.shape;
}

// The block of the golden toolpaths: 80 x 50 x 20 with a step down to Z 14 over x 60..80, y 25..50, and
//   H1 (15, 15) through, 6.6           H2 (35, 15) blind 6, 118 degree point, full diameter to Z 8
//   H3 (55, 15) blind 8, flat floor Z 10                 H4 (15, 35) counterbore 11 to Z 14, through 6.6
//   H5 (35, 35) countersink 10 x 90 degrees, through 5   H6 (70, 38) on the step: blind 5, 118 point, to Z 6
//   H7 (52, 38) blind 5, 118 point, to Z 6 (an M6 tap drill)                  H8 (70, 12) through 5
inline kernel::Shape goldenBlock()
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{80, 50, 20});
    s = cut(s, box(Vec3{60, 25, 14}, Vec3{81, 51, 21}));
    const std::vector<kernel::Shape> tools = {
        revolved(15, 15, {{0, 21}, {3.3, 21}, {3.3, -1}, {0, -1}}),
        revolved(35, 15, {{0, 21}, {3, 21}, {3, 8}, {0, 8 - 3 * kTip118}}),
        revolved(55, 15, {{0, 21}, {4, 21}, {4, 10}, {0, 10}}),
        revolved(15, 35, {{0, 21}, {5.5, 21}, {5.5, 14}, {3.3, 14}, {3.3, -1}, {0, -1}}),
        revolved(35, 35, {{0, 21}, {5, 21}, {5, 20}, {2.5, 17.5}, {2.5, -1}, {0, -1}}),
        revolved(70, 38, {{0, 15}, {2.5, 15}, {2.5, 6}, {0, 6 - 2.5 * kTip118}}),
        revolved(52, 38, {{0, 21}, {2.5, 21}, {2.5, 6}, {0, 6 - 2.5 * kTip118}}),
        revolved(70, 12, {{0, 21}, {2.5, 21}, {2.5, -1}, {0, -1}}),
    };
    for (const kernel::Shape& t : tools) {
        s = cut(s, t);
    }
    return s;
}

inline std::vector<RecognizedHole> holesOf(const kernel::Shape& shape, const HoleOptions& options = {})
{
    JobContext ctx;
    auto r = recognizeHoles(shape, Xform::identity(), ctx, options);
    if (!r) {
        FAIL("recognizeHoles failed: " << r.error().message);
    }
    return *r;
}

// The bore wall of the hole at (x, y): what a user picks.
inline kernel::SubId holeFace(const kernel::Shape& shape, double x, double y)
{
    const std::vector<RecognizedHole> holes = holesOf(shape);
    const auto it = std::find_if(holes.begin(), holes.end(), [&](const RecognizedHole& h) {
        return std::abs(toMm(h.x) - x) < 1e-6 && std::abs(toMm(h.y) - y) < 1e-6;
    });
    INFO("a hole at (" << x << ", " << y << ")");
    REQUIRE(it != holes.end());
    return it->axisSub;
}

// ---------- Tools and machine ----------

inline lib::Tool tool(lib::ToolType type, double diameter, double tipAngle, double fluteLength = 40)
{
    lib::Tool t;
    t.type = type;
    t.flutes = 2;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 40;
    t.tipAngleDeg = tipAngle;
    return t;
}
inline lib::Tool drill(double diameter, double tipAngle = 118, double fluteLength = 40)
{
    return tool(lib::ToolType::Drill, diameter, tipAngle, fluteLength);
}
inline lib::Tool spotDrill(double diameter, double tipAngle = 90, double tipDiameter = 0)
{
    lib::Tool t = tool(lib::ToolType::SpotDrill, diameter, tipAngle, 20);
    t.tipDiameter = tipDiameter;
    return t;
}
inline lib::Tool tap(double diameter, double pitch)
{
    lib::Tool t = tool(lib::ToolType::Tap, diameter, 0, 30);
    t.flutes = 3;
    t.threadPitch = pitch;
    return t;
}

inline lib::MachineDef machine()
{
    lib::MachineDef m;
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 406, 25400, 16500, 5000},
              lib::AxisDef{'Z', -508, 0, 25400, 16500, 5000}};
    m.maxRpm = 8100;
    m.rigidTapping = true;
    return m;
}

// ---------- Operation and geometry ----------

inline Operation drillOp(const std::string& params, double rpm = 1000, double feed = 150)
{
    Operation op;
    op.type = "drill";
    op.name = "Drill";
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = feed;
    op.heights.clearance = 15;
    op.heights.retract = 2;
    op.heights.feedPlane = 1;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 45;
    return op;
}

// A setup on one part in the identity frame, the stock its box.
inline ResolvedGeometry geometryOn(const kernel::Shape& part, const std::vector<kernel::SubId>& faces)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = kernel::bounds(part);
    for (const kernel::SubId& f : faces) {
        g.selected.push_back(f);
        g.selectedShapeIndex.push_back(0);
    }
    return g;
}

// One run of the drilling type.
struct Case {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = test::machine();
    ResolvedGeometry geom;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return ops::makeDrillOperationType()->validate(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return ops::makeDrillOperationType()->generate(input(), ctx);
    }
    Result<ops::DrillPlan> plan() const
    {
        JobContext ctx;
        return ops::planDrill(input(), ctx);
    }
    // generate(), which must succeed and keep every toolpath invariant.
    Toolpath path() const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        invariants::ToolpathRules rules;
        rules.rpm = op.cutting.spindleRpm;
        rules.stock = geom.stockBounds;
        rules.rapidFloorZ = toNm(geom.stockBounds.max.z);
        rules.floorZ = toNm(geom.stockBounds.min.z) - toNm(10); // nothing goes 10 mm below the stock
        rules.axes = machine.axes;
        rules.wcsInMachine = Vec3{100, 100, -400};
        const auto broken = invariants::checkToolpath(*p, rules);
        for (const auto& v : broken) {
            UNSCOPED_INFO("move " << v.move << ": " << v.rule);
        }
        CHECK(broken.empty());
        return *p;
    }
};

// The tip Z of the only (or first) cycle move.
inline Nm firstTip(const Toolpath& p)
{
    const auto it = std::find_if(p.moves.begin(), p.moves.end(), [](const Move& m) { return isCycleMove(m.kind); });
    REQUIRE(it != p.moves.end());
    return it->z;
}

inline std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace ray::cam::test
