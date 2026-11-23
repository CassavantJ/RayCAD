// Helpers of the slot and chamfer2d operation tests (task T-055): parts built through the kernel, tools and a machine,
// the ResolvedGeometry of a setup on a part, the operations, running the types, finding faces and edges by where they
// are, and the golden-file check (tests/golden/toolpaths, as drill_golden_test.cpp does it).
#pragma once

#include "invariants/checker.h"
#include "invariants/property.h"
#include "invariants/toolpath_invariants.h"
#include "toolpath_dump.h"

#include "cam/ops/chamfer2d.h" // src/cam's private headers of the types
#include "cam/ops/slot.h"

#include <ray/cam.h>
#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ray::cam::test {

inline constexpr double kPi = 3.14159265358979323846;

// ---------- Parts ----------

inline kernel::ProfileCurve lineC(Vec2 a, Vec2 b)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
}
inline kernel::ProfileCurve arcC(Vec2 a, Vec2 b, Vec2 c, bool ccw)
{
    return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, 0, ccw, 0};
}

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

// A straight slot outline (a stadium): centre line a -> b, width w.
inline kernel::ProfileLoop stadium(Vec2 a, Vec2 b, double w)
{
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    const double ux = (b.x - a.x) / len, uy = (b.y - a.y) / len;
    const double nx = -uy * w / 2, ny = ux * w / 2; // left of a -> b
    const Vec2 a1{a.x - nx, a.y - ny}, b1{b.x - nx, b.y - ny}, b2{b.x + nx, b.y + ny}, a2{a.x + nx, a.y + ny};
    kernel::ProfileLoop l;
    l.curves = {lineC(a1, b1), arcC(b1, b2, b, true), lineC(b2, a2), arcC(a2, a1, a, true)};
    return l;
}

// An arc slot: centre line on the circle (c, R) from angle a0 to a1 (degrees, counter-clockwise), width w.
inline kernel::ProfileLoop arcSlot(Vec2 c, double R, double a0, double a1, double w)
{
    const auto at = [&](double r, double deg) {
        return Vec2{c.x + r * std::cos(deg * kPi / 180), c.y + r * std::sin(deg * kPi / 180)};
    };
    const double ro = R + w / 2, ri = R - w / 2;
    kernel::ProfileLoop l;
    l.curves = {arcC(at(ro, a0), at(ro, a1), c, true), arcC(at(ro, a1), at(ri, a1), at(R, a1), true),
                arcC(at(ri, a1), at(ri, a0), c, false), arcC(at(ri, a0), at(ro, a0), at(R, a0), true)};
    return l;
}

inline kernel::ProfileLoop rectLoop(double x0, double y0, double x1, double y1)
{
    kernel::ProfileLoop l;
    l.curves = {lineC({x0, y0}, {x1, y0}), lineC({x1, y0}, {x1, y1}), lineC({x1, y1}, {x0, y1}),
                lineC({x0, y1}, {x0, y0})};
    return l;
}

// The slot block: 100 x 60 x 20 (Z 0 to 20) with
//   S1 a straight slot, centre line (15, 12) -> (55, 12), 8 wide, floor Z 14
//   S2 an arc slot, centre line on the circle about (70, 25) of radius 15 from 110 to 180 degrees, 6 wide, floor Z 16
//   S3 a slot open at X 100: centre line (72, 50) -> beyond the block, 10 wide, floor Z 15
//   P  a rectangular pocket (10, 30) .. (30, 50), floor Z 15 (not a slot)
inline kernel::Shape slotBlock()
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{100, 60, 20});
    s = cut(s, prism({stadium({15, 12}, {55, 12}, 8)}, 14, 21));
    s = cut(s, prism({arcSlot({70, 25}, 15, 110, 180, 6)}, 16, 21));
    s = cut(s, prism({stadium({72, 50}, {110, 50}, 10)}, 15, 21));
    s = cut(s, prism({rectLoop(10, 30, 30, 50)}, 15, 21));
    return s;
}

// The chamfer block: 80 x 50 x 20 (Z 0 to 20) with
//   a pocket (10, 10) .. (40, 40) with 4 mm corner radii, floor Z 12
//   a 10 mm through hole at (60, 15)
//   a shallow pocket (50, 30) .. (70, 42), floor Z 19.7 (0.3 deep)
inline kernel::ProfileLoop roundedRect(double x0, double y0, double x1, double y1, double r)
{
    kernel::ProfileLoop l;
    l.curves = {lineC({x0 + r, y0}, {x1 - r, y0}), arcC({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, true),
                lineC({x1, y0 + r}, {x1, y1 - r}), arcC({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, true),
                lineC({x1 - r, y1}, {x0 + r, y1}), arcC({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, true),
                lineC({x0, y1 - r}, {x0, y0 + r}), arcC({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, true)};
    return l;
}
inline kernel::Shape chamferBlock()
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{80, 50, 20});
    s = cut(s, prism({roundedRect(10, 10, 40, 40, 4)}, 12, 21));
    auto hole = kernel::makeCylinder(Vec3{60, 15, -1}, Vec3{0, 0, 1}, 5, 22);
    REQUIRE(hole);
    s = cut(s, *hole);
    s = cut(s, prism({rectLoop(50, 30, 70, 42)}, 19.7, 21));
    return s;
}

// ---------- Finding faces and edges ----------

// The up-facing planar face at height z whose box contains (x, y).
inline kernel::SubId floorAt(const kernel::Shape& s, double x, double y, double z)
{
    std::optional<kernel::SubId> found;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face) && !found; ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, f);
        const auto sample = kernel::faceSample(s, f);
        if (!info || !sample || info->type != kernel::SurfaceType::Plane || sample->normal.z < 0.999) {
            continue;
        }
        const Box3& b = info->bounds;
        if (std::abs(b.min.z - z) < 1e-6 && x >= b.min.x - 1e-6 && x <= b.max.x + 1e-6 && y >= b.min.y - 1e-6 &&
            y <= b.max.y + 1e-6) {
            found = f;
        }
    }
    INFO("a floor at (" << x << ", " << y << ", " << z << ")");
    REQUIRE(found.has_value());
    return *found;
}

// The edges lying at height z (both ends, and a circle's centre) inside the box [x0, x1] x [y0, y1].
inline std::vector<kernel::SubId> edgesAt(const kernel::Shape& s, double z, double x0, double y0, double x1, double y1)
{
    std::vector<kernel::SubId> out;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId e{kernel::SubKind::Edge, i};
        const auto c = kernel::curveInfo(s, e);
        if (!c || std::abs(c->start.z - z) > 1e-6 || std::abs(c->end.z - z) > 1e-6) {
            continue;
        }
        const auto inBox = [&](Vec3 p) {
            return p.x >= x0 - 1e-6 && p.x <= x1 + 1e-6 && p.y >= y0 - 1e-6 && p.y <= y1 + 1e-6;
        };
        if (!inBox(c->start) || !inBox(c->end)) {
            continue;
        }
        if (c->type == kernel::CurveType::Circle && (std::abs(c->center.z - z) > 1e-6 || !inBox(c->center))) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

// ---------- Tools and machine ----------

inline lib::Tool endMill(double diameter, double fluteLength = 25)
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

inline lib::Tool chamferMill(double diameter, double tipAngle = 90, double tipDiameter = 0, double fluteLength = 10)
{
    lib::Tool t;
    t.type = lib::ToolType::ChamferMill;
    t.flutes = 4;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 50;
    t.tipAngleDeg = tipAngle;
    t.tipDiameter = tipDiameter;
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

// ---------- Operations and geometry ----------

inline Operation millOp(const std::string& type, const std::string& params, double rpm = 6000, double feed = 600)
{
    Operation op;
    op.type = type;
    op.name = type;
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = feed / 2;
    op.cutting.feedRamp = feed * 3 / 4;
    op.heights.clearance = 15;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 50;
    return op;
}

// A setup on one part in the identity frame, the stock its box.
inline ResolvedGeometry geometryOn(const kernel::Shape& part, const std::vector<kernel::SubId>& picked)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = kernel::bounds(part);
    for (const kernel::SubId& f : picked) {
        g.selected.push_back(f);
        g.selectedShapeIndex.push_back(0);
    }
    return g;
}

// One run of a type.
struct Case {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = test::machine();
    ResolvedGeometry geom;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    std::unique_ptr<IOperationType> type() const
    {
        return op.type == "slot" ? ops::makeSlotOperationType() : ops::makeChamfer2dOperationType();
    }
    Status validate() const { return type()->validate(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return type()->generate(input(), ctx);
    }
    // generate(), which must succeed and keep the M2 toolpath rules and every invariant of the checker.
    Toolpath path(const std::function<void(invariants::Claims&)>& claims = {}) const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        invariants::ToolpathRules rules;
        rules.rpm = op.cutting.spindleRpm;
        rules.stock = geom.stockBounds;
        rules.rapidFloorZ = toNm(geom.stockBounds.max.z);
        rules.floorZ = toNm(geom.stockBounds.min.z);
        rules.axes = machine.axes;
        rules.wcsInMachine = Vec3{100, 100, -400};
        const auto broken = invariants::checkToolpath(*p, rules);
        for (const auto& v : broken) {
            UNSCOPED_INFO("move " << v.move << ": " << v.rule);
        }
        CHECK(broken.empty());
        invariants::Claims k = invariants::claimsFor(op, geom);
        k.wcsInMachine = Vec3{100, 100, -400};
        if (claims) {
            claims(k);
        }
        const auto found = invariants::check(invariants::CheckInput{*p, geom, tool, nullptr, &machine, k});
        INFO(invariants::describe(found));
        CHECK(found.empty());
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

// ---------- Goldens (drill_golden_test.cpp's convention) ----------

inline bool updatingGoldens()
{
    const char* v = std::getenv("RAY_UPDATE_TOOLPATH_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

inline void writeFile(const std::string& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

// The toolpath is the golden <name>: both files match it and each other.
inline void matchesGolden(const std::string& name, const Toolpath& p)
{
    const auto raw = p.serialize();
    const std::string bytes(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::string text = dumpToolpath(p);
    const std::string dir = RAY_TOOLPATH_GOLDEN_DIR;
    if (updatingGoldens()) {
        writeFile(dir + "/" + name + ".rctp", bytes);
        writeFile(dir + "/" + name + ".txt", text);
    }
    const std::string storedBytes = readText(dir + "/" + name + ".rctp");
    const std::string storedText = readText(dir + "/" + name + ".txt");
    REQUIRE_FALSE(storedBytes.empty());
    INFO(name << ":\n" << text);
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText);
}

// The checker's claim that chamfering the edges is intended: the wedge each chamfer removes (invariants::chamferCut),
// `depth` mm deep at the tool's flank angle (tan of half the tip angle), grown by `grow` mm.
inline void chamferClaims(invariants::Claims& k, const kernel::Shape& s, const std::vector<kernel::SubId>& edges,
                          double depth, double tanHalf, double grow = 0.02)
{
    for (const kernel::SubId e : edges) {
        auto cut = invariants::chamferCut(s, e, depth, tanHalf, grow);
        REQUIRE(cut);
        k.allowedCuts.push_back(std::move(*cut));
    }
}

// The moves of a toolpath with role `role`.
inline std::vector<Move> movesOf(const Toolpath& p, MoveRole role)
{
    std::vector<Move> out;
    std::copy_if(p.moves.begin(), p.moves.end(), std::back_inserter(out),
                 [&](const Move& m) { return m.role == role; });
    return out;
}

} // namespace ray::cam::test
