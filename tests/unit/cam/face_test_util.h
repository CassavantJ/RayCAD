// Helpers of the facing operation tests (task T-052): a part built through the kernel, tools and a machine, the
// ResolvedGeometry of a setup with box stock around it, a facing operation, running the type's plan, validate and
// generate, and the toolpath invariant checker (tests/unit/cam/invariants) with the claims facing makes.
#pragma once

#include "checker.h" // the invariant checker (tests/unit/cam/invariants)
#include "toolpath_dump.h"

#include "cam/ops/face.h" // src/cam's private header of the type

#include <ray/cam.h>
#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ray::cam::test {

inline constexpr Nm kMm = 1'000'000;

inline kernel::Shape faceBox(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    if (!s) {
        FAIL("makeBox failed: " << s.error().message);
    }
    return *s;
}

// The block of the goldens: 80 x 50 x 20, its top at Z 20, with a 30 x 20 pocket 5 deep (x 10..40, y 15..35), so the
// part is not a plain box.
inline kernel::Shape faceBlock()
{
    const kernel::Shape block = faceBox(Vec3{0, 0, 0}, Vec3{80, 50, 20});
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Cut, block, faceBox(Vec3{10, 15, 15}, Vec3{40, 35, 21}), ctx);
    if (!r) {
        FAIL("cut failed: " << r.error().message);
    }
    return r->shape;
}

inline lib::Tool millTool(lib::ToolType type, double diameter, double corner = 0, double fluteLength = 20)
{
    lib::Tool t;
    t.type = type;
    t.flutes = 4;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = fluteLength;
    t.shoulderLength = fluteLength;
    t.overallLength = fluteLength + 40;
    t.cornerRadius = corner;
    return t;
}

inline lib::MachineDef faceMachine()
{
    lib::MachineDef m;
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 762, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 406, 25400, 16500, 5000},
              lib::AxisDef{'Z', -508, 0, 25400, 16500, 5000}};
    m.maxRpm = 12000;
    return m;
}

// A facing operation: heights from the stock top (clearance 15, retract 5, feed plane 2, top 0), bottom 0 (the part
// top), 1200 mm/min, plunge 400.
inline Operation faceOp(const std::string& params, double rpm = 6000, double feed = 1200)
{
    Operation op;
    op.type = "face";
    op.name = "Face";
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = 400;
    op.heights.clearance = 15;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 40;
    return op;
}

// A setup on `part` in the identity frame, the stock its box grown by `side` in X and Y and by `above` on top.
inline ResolvedGeometry stockAround(const kernel::Shape& part, double side, double above)
{
    ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    Box3 b = kernel::bounds(part);
    b.min.x -= side;
    b.min.y -= side;
    b.max.x += side;
    b.max.y += side;
    b.max.z += above;
    // Whole nanometres, as resolveSetup gives them.
    for (double* v : {&b.min.x, &b.min.y, &b.min.z, &b.max.x, &b.max.y, &b.max.z}) {
        *v = toMm(toNm(*v));
    }
    g.stockBounds = b;
    return g;
}

// The claims facing makes (face.h): coverage of the whole stock box down to the faced height, nothing below it.
inline invariants::Claims faceClaims(const Operation& op, const ResolvedGeometry& geom, double bottomZ)
{
    invariants::Claims k = invariants::claimsFor(op, geom);
    k.holderClearance = 0.5;
    k.wcsInMachine = Vec3{200, 100, -300};
    k.floorZ = bottomZ;
    k.clears = {invariants::ClearClaim{invariants::Region::box(), bottomZ, 0.002, "coverage"}};
    const double top = geom.stockBounds.max.z; // face.h LEVELS: heights from the stock top, the bottom from the part
    k.heights.clearance = top + op.heights.clearance;
    k.heights.retract = top + op.heights.retract;
    k.heights.feedPlane = top + op.heights.feedPlane;
    k.heights.top = top + op.heights.top;
    k.heights.bottom = bottomZ;
    return k;
}

// One run of the facing type.
struct FaceCase {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = faceMachine();
    ResolvedGeometry geom;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return ops::makeFaceOperationType()->validate(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return ops::makeFaceOperationType()->generate(input(), ctx);
    }
    Result<ops::FacePlan> plan() const
    {
        JobContext ctx;
        return ops::planFace(input(), ctx);
    }
    ops::FacePlan okPlan() const
    {
        auto p = plan();
        if (!p) {
            FAIL("planFace failed: " << p.error().message);
        }
        return *p;
    }
    // generate(), which must succeed and keep every invariant the checker knows.
    Toolpath path() const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        const ops::FacePlan plan = okPlan();
        const auto found = invariants::check(
            invariants::CheckInput{*p, geom, tool, nullptr, &machine, faceClaims(op, geom, toMm(plan.bottom))});
        UNSCOPED_INFO(invariants::describe(found));
        CHECK(found.empty());
        return *p;
    }
};

inline std::string faceReadText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace ray::cam::test
