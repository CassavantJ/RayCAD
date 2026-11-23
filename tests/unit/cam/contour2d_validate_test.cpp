// Each refusal of the 2D contour operation on its own (contour2d.h CHECKS, TOOL TOO LARGE, LEADS, TABS; task T-053):
// the code and the words, and that generate refuses with the same message as validate.
#include "contour2d_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& part()
{
    static const kernel::Shape s = contourPart();
    return s;
}

ContourCase bossOutside(const std::string& params, double bottom = -15)
{
    ContourCase c;
    c.op = contourOp(params, bottom);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 15, 25})});
    return c;
}
ContourCase plateOutline(const std::string& params)
{
    ContourCase c;
    c.op = contourOp(params, -10);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    return c;
}
ContourCase pocketFloor(const std::string& params, double toolDiameter)
{
    ContourCase c;
    c.op = contourOp(params, 0);
    c.tool = endMill(toolDiameter);
    c.geom = geometryOn(part(), {flatFaceAt(part(), 18, 50, 40)});
    return c;
}

// validate refuses with `code` and words containing `words`, and generate the same.
void refused(const ContourCase& c, ErrorCode code, const std::string& words)
{
    const Status v = c.validate();
    REQUIRE_FALSE(v);
    INFO(v.error().message);
    CHECK(v.error().code == code);
    CHECK_THAT(v.error().message, ContainsSubstring(words));
    const auto g = c.generate();
    REQUIRE_FALSE(g);
    CHECK(g.error().message == v.error().message);
}

} // namespace

TEST_CASE("contour2d refuses: nothing picked, or a vertex", "[cam][contour2d][validate]")
{
    ContourCase c = bossOutside("{}");
    c.geom.selected.clear();
    c.geom.selectedShapeIndex.clear();
    refused(c, ErrorCode::InvalidArgument, "Pick the edges of the contour");
    c.geom.selected = {kernel::SubId{kernel::SubKind::Vertex, 0}};
    c.geom.selectedShapeIndex = {0};
    refused(c, ErrorCode::InvalidArgument, "is not an edge or a face");
}

TEST_CASE("contour2d refuses: a tool that is not an end mill", "[cam][contour2d][validate]")
{
    ContourCase c = bossOutside("{}");
    c.tool = endMill(6, 30, lib::ToolType::Drill);
    refused(c, ErrorCode::InvalidArgument, "the tool is a drill");
    c.tool = endMill(0);
    refused(c, ErrorCode::InvalidArgument, "The tool has no diameter");
}

TEST_CASE("contour2d refuses: spindle and feed", "[cam][contour2d][validate]")
{
    ContourCase c = bossOutside("{}");
    c.op.cutting.spindleRpm = 0;
    refused(c, ErrorCode::InvalidArgument, "Set a spindle speed");
    c.op.cutting.spindleRpm = 20000;
    refused(c, ErrorCode::InvalidArgument, "above the machine's maximum");
    c.op.cutting.spindleRpm = 6000;
    c.op.cutting.clockwise = false;
    refused(c, ErrorCode::InvalidArgument, "counter-clockwise (M4)");
    c.op.cutting.clockwise = true;
    c.op.cutting.feedCut = 0;
    refused(c, ErrorCode::InvalidArgument, "Set a cutting feed");
}

TEST_CASE("contour2d refuses: heights out of order", "[cam][contour2d][validate]")
{
    ContourCase c = bossOutside("{}");
    c.op.heights.clearance = 4;
    refused(c, ErrorCode::InvalidArgument, "clearance height (4) must not be below the retract height (5)");
    c.op.heights.clearance = 10;
    c.op.heights.feedPlane = 6;
    refused(c, ErrorCode::InvalidArgument, "retract height (5) must not be below the feed plane (6)");
    c.op.heights.feedPlane = 2;
    c.op.heights.top = 3;
    refused(c, ErrorCode::InvalidArgument, "feed plane (2) must not be below the top height (3)");
    c.op.heights.top = 0;
    c.op.heights.clearance = c.op.heights.retract = -1;
    c.op.heights.feedPlane = -1;
    c.op.heights.top = -2;
    refused(c, ErrorCode::InvalidArgument, "must not be below the top of the stock");
    // The bottom above the top: the chain is at Z 25, the top the stock's (Z 25), bottom +0 is not below it.
    ContourCase flat = bossOutside("{}", 0);
    refused(flat, ErrorCode::InvalidArgument, "must be above the bottom of the closed chain");
    ContourCase many = bossOutside(R"({"maxStepdown":0.001})");
    refused(many, ErrorCode::InvalidArgument, "raise the maximum stepdown");
}

TEST_CASE("contour2d refuses: flutes and stick-out too short", "[cam][contour2d][validate]")
{
    ContourCase c = bossOutside("{}");
    c.tool = endMill(10, 12);
    refused(c, ErrorCode::InvalidArgument, "a pass cuts 15 mm deep, but the tool's flutes are 12 mm long");
    c.tool = endMill(10, 12);
    c.op.paramsJson = R"({"maxStepdown":5})";
    REQUIRE(c.validate()); // 5 mm a depth fits
    c.op.paramsJson = R"({"maxStepdown":5,"finishPass":true})";
    refused(c, ErrorCode::InvalidArgument, "finish and spring passes cut the whole depth");
    c = bossOutside("{}");
    c.op.tool.stickOut = 14;
    refused(c, ErrorCode::InvalidArgument, "it sticks out only 14 mm");
}

TEST_CASE("contour2d refuses: a tool too large for the contour", "[cam][contour2d][validate]")
{
    // The pocket is 30 x 20: a 22 mm tool does not fit inside it.
    refused(pocketFloor("{}", 22), ErrorCode::ToolpathFailed, "The 22 mm tool does not fit inside the outer boundary");
    // A 12 mm tool fits but not into the R5 corners: in the computer that leaves material (a warning, rules test); in
    // the control the control would gouge or alarm there.
    refused(pocketFloor(R"({"compensation":"control","leadType":"line"})", 12), ErrorCode::ToolpathFailed,
            "cannot follow the contour (an inside corner or arc smaller than the tool");
}

TEST_CASE("contour2d refuses: a pass that would cut into the part elsewhere", "[cam][contour2d][validate]")
{
    // One wall of the pocket picked alone, a 12 mm tool beside it: at the wall's ends the tool runs into the R5
    // corners.
    ContourCase c;
    c.op = contourOp(R"({"followTangent":false})", 0);
    c.tool = endMill(12);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{35, 40, 18})});
    refused(c, ErrorCode::ToolpathFailed, "would cut into the part near");
}

TEST_CASE("contour2d refuses: picked edges that branch", "[cam][contour2d][validate]")
{
    // An L-shaped step: 0..40 x 0..40 x 0..10 with 0..20 x 0..40 x 10..20 on it. At (20, 0) seen from above three of
    // the picked edges meet: the lower block's front top edge, the upper block's front top edge and the step's inner
    // edge along Y.
    const kernel::Shape lower = prism({roundedRect(0, 0, 40, 40, 0)}, 0, 10);
    const kernel::Shape upper = prism({roundedRect(0, 0, 20, 40, 0)}, 10, 10);
    const kernel::Shape step = booleanOf(kernel::BoolOp::Union, lower, upper);
    ContourCase c;
    c.op = contourOp(R"({"followTangent":false})", -10);
    c.geom = geometryOn(step, {lineEdgeAt(step, Vec3{30, 0, 10}), lineEdgeAt(step, Vec3{10, 0, 20}),
                               lineEdgeAt(step, Vec3{20, 20, 10})});
    refused(c, ErrorCode::ToolpathFailed, "The picked edges branch at X20 Y0");
}

TEST_CASE("contour2d refuses: a face that is not flat and level, a side the chain cannot have",
          "[cam][contour2d][validate]")
{
    // A corner arc of the boss: a cylinder.
    ContourCase c = bossOutside("{}");
    for (std::uint32_t i = 0; i < part().count(kernel::SubKind::Face); ++i) {
        auto s = kernel::surfaceInfo(part(), kernel::SubId{kernel::SubKind::Face, i});
        if (s && s->type == kernel::SurfaceType::Cylinder) {
            c.geom.selected = {kernel::SubId{kernel::SubKind::Face, i}};
            break;
        }
    }
    refused(c, ErrorCode::ToolpathFailed, "is not a planar face perpendicular to the setup's Z axis");
    ContourCase open;
    open.op = contourOp(R"({"followTangent":false,"side":"outside"})", -10);
    open.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    refused(open, ErrorCode::InvalidArgument, "'Outside' needs a closed chain");
}

TEST_CASE("contour2d refuses: a side it cannot tell", "[cam][contour2d][validate]")
{
    // Two part bodies side by side: the edge where their top faces meet has material on both sides below it and none
    // above it.
    const kernel::Shape a = prism({roundedRect(0, 0, 40, 20, 0)}, 0, 10);
    const kernel::Shape b = prism({roundedRect(0, 20, 40, 40, 0)}, 0, 10);
    ContourCase c;
    c.op = contourOp(R"({"followTangent":false})", -5);
    c.geom = geometryOn(a, {lineEdgeAt(a, Vec3{20, 20, 10})});
    c.geom.partShapes.push_back(b);
    c.geom.stockBounds.max.y = 40;
    refused(c, ErrorCode::ToolpathFailed, "Cannot tell which side of the open chain");
}

TEST_CASE("contour2d refuses: leads that do not fit, leads compensation cannot use", "[cam][contour2d][validate]")
{
    // A 30 mm straight part before the lead arc does not fit in the 30 x 20 pocket (linking shrinks only the arc).
    refused(pocketFloor(R"({"leadLength":30})", 6), ErrorCode::ToolpathFailed, "does not fit the room");
    refused(plateOutline(R"({"compensation":"control"})"), ErrorCode::InvalidArgument,
            "is switched on and off on a straight move");
    refused(plateOutline(R"({"compensation":"wear","leadType":"none"})"), ErrorCode::InvalidArgument,
            "is switched on and off on a straight move");
}

TEST_CASE("contour2d refuses: tabs that cannot be made", "[cam][contour2d][validate]")
{
    // The plate's outline is cut from the stock top (Z 25) to the plate's bottom (Z 0): tabs 30 high reach above it.
    refused(plateOutline(R"({"tabs":true,"tabHeight":30})"), ErrorCode::InvalidArgument, "are not below the top");
    refused(plateOutline(R"({"tabs":true,"tabCount":40,"tabWidth":10})"), ErrorCode::ToolpathFailed,
            "tabs do not fit along the");
    refused(plateOutline(R"({"tabs":true,"compensation":"wear","leadLength":3})"), ErrorCode::InvalidArgument,
            "cutter compensation (G41/G42) cannot follow");
    refused(plateOutline(R"({"tabs":true,"rampAngle":0})"), ErrorCode::InvalidArgument,
            "need a ramp angle to come back down");
    refused(plateOutline(R"({"tabs":true,"tabShape":"triangular"})"), ErrorCode::InvalidArgument,
            "steeper than the ramp angle");
}

TEST_CASE("contour2d refuses: an entry no allowed kind can make", "[cam][contour2d][validate]")
{
    // Inside the pocket with in-control compensation: no ramp (compensation), no plunge (a flat end mill): linking
    // says why for each.
    refused(pocketFloor(R"({"compensation":"control","leadType":"line"})", 6), ErrorCode::ToolpathFailed,
            "cannot be entered");
}
