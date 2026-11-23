// The slot operation (task T-055): slot recognition on kernel-built slots (straight, arc, open-ended), the depths
// against hand-computed values, the passes and the ramp entry the toolpath has, and each refusal on its own (asserting
// the code and the words, and that generate refuses with the same message as validate).
#include "slot_chamfer_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;
namespace g = ray::cam::geom2d;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = slotBlock();
    return s;
}

Case slotCase(const std::vector<kernel::SubId>& floors, lib::Tool t, const std::string& params = "{}")
{
    Case c;
    c.op = millOp("slot", params);
    c.tool = std::move(t);
    c.geom = geometryOn(block(), floors);
    return c;
}

kernel::SubId s1()
{
    return floorAt(block(), 35, 12, 14);
}
kernel::SubId s2()
{
    return floorAt(block(), 70 + 15 * std::cos(145 * kPi / 180), 25 + 15 * std::sin(145 * kPi / 180), 16);
}
kernel::SubId s3()
{
    return floorAt(block(), 90, 50, 15);
}

g::Pt endOf(const g::Path& p)
{
    return p.spans.empty() ? p.start : p.spans.back().end;
}

// validate and generate refuse with the same message, which contains `words`.
Error refusedBoth(const Case& c, const std::string& words)
{
    const Status v = c.validate();
    REQUIRE_FALSE(v);
    INFO(v.error().message);
    CHECK_THAT(v.error().message, ContainsSubstring(words));
    const auto gen = c.generate();
    REQUIRE_FALSE(gen);
    CHECK(gen.error().message == v.error().message);
    CHECK(gen.error().code == v.error().code);
    return v.error();
}

} // namespace

TEST_CASE("slot depths: evenly spread, at most the step down apart, the last at the floor", "[cam][slot]")
{
    // 6 mm from Z 20 to Z 14: step 1 -> 6 depths 19 .. 14; step 2.5 -> ceil(6 / 2.5) = 3 depths of 2 mm; step 4 -> 2
    // depths of 3 mm; step 6 and more -> one depth.
    CHECK(ops::slotDepths(20'000'000, 14'000'000, 1'000'000) ==
          std::vector<Nm>{19'000'000, 18'000'000, 17'000'000, 16'000'000, 15'000'000, 14'000'000});
    CHECK(ops::slotDepths(20'000'000, 14'000'000, 2'500'000) == std::vector<Nm>{18'000'000, 16'000'000, 14'000'000});
    CHECK(ops::slotDepths(20'000'000, 14'000'000, 4'000'000) == std::vector<Nm>{17'000'000, 14'000'000});
    CHECK(ops::slotDepths(20'000'000, 14'000'000, 6'000'000) == std::vector<Nm>{14'000'000});
    CHECK(ops::slotDepths(20'000'000, 14'000'000, 9'000'000) == std::vector<Nm>{14'000'000});
    // 1 mm in steps of 0.3: ceil(1 / 0.3) = 4 depths of 0.25 mm.
    CHECK(ops::slotDepths(0, -1'000'000, 300'000) == std::vector<Nm>{-250'000, -500'000, -750'000, -1'000'000});
    // 1 mm in 3 steps of 1/3 mm: rounded half to even at each depth, the last exact.
    CHECK(ops::slotDepths(0, -1'000'000, 340'000) == std::vector<Nm>{-333'333, -666'667, -1'000'000});
    CHECK(ops::slotDepths(0, 0, 1'000'000).empty());
}

TEST_CASE("slot recognition: a straight slot, an arc slot and a slot open at one end", "[cam][slot]")
{
    JobContext ctx;
    SECTION("straight: the centre line between the half circles' centres, width 8")
    {
        auto s = ops::recognizeSlot(block(), s1(), Xform::identity(), ctx);
        REQUIRE(s);
        CHECK(s->width == 8'000'000);
        CHECK(s->floorZ == 14'000'000);
        CHECK_FALSE(s->open[0]);
        CHECK_FALSE(s->open[1]);
        REQUIRE(s->centre.spans.size() == 1);
        CHECK(s->centre.spans[0].kind == g::SpanKind::Line);
        const std::pair<g::Pt, g::Pt> ends{s->centre.start, endOf(s->centre)};
        const g::Pt a{15'000'000, 12'000'000}, b{55'000'000, 12'000'000};
        CHECK(((ends.first == a && ends.second == b) || (ends.first == b && ends.second == a)));
        CHECK(ops::extendOpenEnds(*s, 5'000'000) == s->centre); // closed at both ends: nothing to extend
    }
    SECTION("arc: the centre line is the arc of radius 15 about (70, 25), width 6")
    {
        auto s = ops::recognizeSlot(block(), s2(), Xform::identity(), ctx);
        REQUIRE(s);
        CHECK(s->width == 6'000'000);
        REQUIRE(s->centre.spans.size() == 1);
        const g::Span& arc = s->centre.spans[0];
        CHECK(arc.kind != g::SpanKind::Line);
        CHECK(std::abs(arc.centre.x - 70'000'000) <= 2);
        CHECK(std::abs(arc.centre.y - 25'000'000) <= 2);
        CHECK(std::abs(arc.radius - 15'000'000) <= 2);
        // Its length: 15 mm x 70 degrees.
        CHECK(std::abs(g::length(s->centre) - 15e6 * 70 * kPi / 180) < 10);
    }
    SECTION("open at X 100: the centre line ends at the open side's middle and is extended past it")
    {
        auto s = ops::recognizeSlot(block(), s3(), Xform::identity(), ctx);
        REQUIRE(s);
        CHECK(s->width == 10'000'000);
        CHECK(s->open[0] != s->open[1]);
        const g::Pt closedEnd{72'000'000, 50'000'000}, openEnd{100'000'000, 50'000'000};
        const g::Pt from = s->open[0] ? endOf(s->centre) : s->centre.start;
        const g::Pt to = s->open[0] ? s->centre.start : endOf(s->centre);
        CHECK(from == closedEnd);
        CHECK(to == openEnd);
        // Extended by 5 mm (radius) + 1 mm past the open end, straight on.
        const g::Path e = ops::extendOpenEnds(*s, 6'000'000);
        const g::Pt beyond = s->open[0] ? e.start : endOf(e);
        CHECK(beyond == g::Pt{106'000'000, 50'000'000});
    }
}

TEST_CASE("slot toolpath: one pass per depth along the centre line, each entered by a zigzag ramp", "[cam][slot]")
{
    Case c = slotCase({s1()}, endMill(8), R"({"stepDown":2.5,"rampAngle":4})");
    const Toolpath p = c.path([](invariants::Claims& k) { k.maxRampAngleDeg = 4; });
    // Depths 18, 16, 14 (the stock top is the block's top, Z 20).
    std::vector<Nm> cutZ;
    for (const Move& m : movesOf(p, MoveRole::Cut)) {
        if (cutZ.empty() || cutZ.back() != m.z) {
            cutZ.push_back(m.z);
        }
        CHECK(m.y == 12'000'000);
        CHECK((m.x == 15'000'000 || m.x == 55'000'000));
        CHECK(m.feedMicronPerMin == 600'000);
    }
    CHECK(cutZ == std::vector<Nm>{18'000'000, 16'000'000, 14'000'000});
    // Every ramp move stays on the centre line and descends at most 4 degrees.
    const std::vector<Move> ramps = movesOf(p, MoveRole::Ramp);
    REQUIRE_FALSE(ramps.empty());
    for (const Move& m : ramps) {
        CHECK(m.y == 12'000'000);
        CHECK(m.x >= 15'000'000);
        CHECK(m.x <= 55'000'000);
        CHECK(m.feedMicronPerMin == 450'000);
    }
    // Heights: clearance Z 35, the retract plane Z 25 (between slots), ramps start 0.5 mm above the material.
    CHECK(p.startZ == 35'000'000);
    CHECK(p.moves.back().z == 35'000'000);
}

TEST_CASE("slot toolpath: an arc slot and an open slot, together, with the checker", "[cam][slot]")
{
    Case c = slotCase({s2()}, endMill(6), R"({"stepDown":2})");
    const Toolpath arc = c.path([](invariants::Claims& k) { k.maxRampAngleDeg = 3; });
    CHECK_FALSE(movesOf(arc, MoveRole::Cut).empty());
    Case open = slotCase({s3()}, endMill(10), R"({"stepDown":2.5})");
    const Toolpath o = open.path([](invariants::Claims& k) { k.maxRampAngleDeg = 3; });
    // The passes run 6 mm past the open end, to X 106.
    Nm xMax = 0;
    for (const Move& m : movesOf(o, MoveRole::Cut)) {
        xMax = std::max(xMax, m.x);
    }
    CHECK(xMax == 106'000'000);
}

TEST_CASE("slot: a tool a little narrower than the slot is accepted with a warning", "[cam][slot]")
{
    Case c = slotCase({s1()}, endMill(7.97));
    REQUIRE(c.validate());
    auto warnings = c.type()->check(c.input());
    REQUIRE(warnings);
    REQUIRE(warnings->size() == 1);
    CHECK_THAT(warnings->front(), ContainsSubstring("cuts the 8 mm slot at X15 Y12 0.03 mm narrower than the model"));
    const Toolpath p = c.path();
    CHECK(p.warnings == *warnings);
}

TEST_CASE("slot refusals, each naming its cause", "[cam][slot]")
{
    SECTION("a tool wider than the slot")
    {
        const Error e = refusedBoth(slotCase({s1()}, endMill(8.01)), "The 8.01 mm flat end mill is wider than the 8 mm "
                                                                     "slot at X15 Y12: it would cut into both walls");
        CHECK(e.code == ErrorCode::InvalidArgument);
    }
    SECTION("a tool narrower than the slot beyond the width tolerance points at the pocket")
    {
        refusedBoth(slotCase({s1()}, endMill(6)), "The 6 mm flat end mill is narrower than the 8 mm slot at X15 Y12: a "
                                                  "centre-line slot is as wide as its tool; use a tool of the slot's "
                                                  "width, or machine the slot as a 2D pocket (pocket2d)");
        // A wider tolerance lets it through.
        CHECK_FALSE(slotCase({s1()}, endMill(7.9)).validate());
        CHECK(slotCase({s1()}, endMill(7.9), R"({"widthTolerance":0.2})").validate());
    }
    SECTION("a ball or bull nose end mill, or a drill")
    {
        lib::Tool ball = endMill(8);
        ball.type = lib::ToolType::BallEndMill;
        refusedBoth(slotCase({s1()}, ball),
                    "The tool is a ball end mill; a centre-line slot is cut with a flat end mill");
        lib::Tool drill = endMill(8);
        drill.type = lib::ToolType::Drill;
        drill.tipAngleDeg = 118;
        refusedBoth(slotCase({s1()}, drill), "The tool is a drill; a centre-line slot is cut with a flat end mill");
    }
    SECTION("a rectangular pocket is not a slot")
    {
        const Error e = refusedBoth(slotCase({floorAt(block(), 20, 40, 15)}, endMill(8)),
                                    "is not a slot: a slot's ends are half circles of its width (what a centre-line "
                                    "cut leaves) or open sides; machine this face as a 2D pocket (pocket2d)");
        CHECK(e.code == ErrorCode::ToolpathFailed);
        CHECK_THAT(e.message, ContainsSubstring("Selection 1 (the face centred at X20 Y40 Z15)"));
    }
    SECTION("a face that is not a floor: the block's side, the block's top with its openings")
    {
        kernel::SubId side{};
        for (std::uint32_t i = 0; i < block().count(kernel::SubKind::Face); ++i) {
            const kernel::SubId f{kernel::SubKind::Face, i};
            const auto info = kernel::surfaceInfo(block(), f);
            if (info && info->type == kernel::SurfaceType::Plane && std::abs(info->bounds.min.x) < 1e-9 &&
                std::abs(info->bounds.max.x) < 1e-9) {
                side = f;
            }
        }
        refusedBoth(slotCase({side}, endMill(8)), "is not a flat face square to the setup's Z axis");
        refusedBoth(slotCase({floorAt(block(), 1, 1, 20)}, endMill(8)), "has an island in it, so it is not a slot");
    }
    SECTION("an edge, or nothing, selected")
    {
        refusedBoth(slotCase({kernel::SubId{kernel::SubKind::Edge, 0}}, endMill(8)),
                    "Selection 1 is not a face: pick the floor face of each slot");
        refusedBoth(slotCase({}, endMill(8)), "Pick the slots to cut: select the floor face of each slot");
    }
    SECTION("heights out of order")
    {
        Case c = slotCase({s1()}, endMill(8));
        c.op.heights.retract = 1;
        refusedBoth(c, "The retract height (1 above the stock top) must not be below the feed plane (2)");
        c = slotCase({s1()}, endMill(8));
        c.op.heights.clearance = 3;
        refusedBoth(c, "The clearance height (3 above the stock top) must not be below the retract height (5)");
        c = slotCase({s1()}, endMill(8));
        c.op.heights.top = 3;
        refusedBoth(c, "The feed plane (2 above the stock top) must not be below the top height (3)");
        c = slotCase({s1()}, endMill(8));
        c.op.heights = Heights{15, -1, -2, -3, 0};
        refusedBoth(c, "must not be below the stock top: the tool travels across at it");
    }
    SECTION("the floor at or above the top height")
    {
        Case c = slotCase({s1()}, endMill(8), R"({"floorStock":0.5})");
        c.op.heights.top = -6.5;
        refusedBoth(c, "The floor of the 8 mm slot at X15 Y12 (Z14.5 with the floor stock) is not below the top height "
                       "(Z13.5)");
    }
    SECTION("flutes or stick-out too short")
    {
        refusedBoth(slotCase({s1()}, endMill(8, 5)),
                    "The slot at X15 Y12 is 6 mm deep from the top height, but the tool's flutes are 5 mm long");
        Case c = slotCase({s1()}, endMill(8));
        c.op.tool.stickOut = 6.5;
        refusedBoth(c, "The tool sticks out 6.5 mm, but the slot at X15 Y12 needs 7 mm for the holder to stay 1 mm "
                       "above the stock and the part");
    }
    SECTION("the spindle")
    {
        Case c = slotCase({s1()}, endMill(8));
        c.op.cutting.clockwise = false;
        refusedBoth(c, "The spindle is set to turn counter-clockwise (M4)");
        c = slotCase({s1()}, endMill(8));
        c.op.cutting.spindleRpm = 20000;
        refusedBoth(c, "The spindle speed 20000 rpm is above the machine's maximum of 12000 rpm");
        c = slotCase({s1()}, endMill(8));
        c.op.cutting.feedCut = 0;
        refusedBoth(c, "Set a cutting feed");
    }
    SECTION("a slot too short for the ramp")
    {
        // The straight slot's centre line is 40 mm long; a shortest run of 50 mm cannot fit.
        const Case c = slotCase({s1()}, endMill(8), R"({"minRampRun":50})");
        REQUIRE(c.validate()); // linking decides the entry: generate refuses
        const auto gen = c.generate();
        REQUIRE_FALSE(gen);
        CHECK(gen.error().code == ErrorCode::ToolpathFailed);
        CHECK_THAT(gen.error().message,
                   ContainsSubstring("ramp: the cut offers 40 mm for the ramp, which needs a run of at least 50 mm"));
    }
}

TEST_CASE("slot: an overhang above the floor is refused (a T-slot's foot)", "[cam][slot]")
{
    // A T-slot running along X: an 8 mm throat from the top (Z 20) down to Z 12, a 16 mm foot from Z 12 down to Z 6,
    // its floor at Z 6 under the throat's shoulders. The foot's floor, as a slot 16 wide, has material over it.
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{60, 40, 20});
    s = cut(s, box(Vec3{-1, 16, 12}, Vec3{61, 24, 21}));
    s = cut(s, box(Vec3{-1, 12, 6}, Vec3{61, 28, 12}));
    Case c;
    c.op = millOp("slot", "{}");
    c.tool = endMill(16);
    c.geom = geometryOn(s, {floorAt(s, 30, 20, 6)});
    const Error e = refusedBoth(c, "the tool would cut into the part or a fixture at");
    CHECK(e.code == ErrorCode::ToolpathFailed);
    CHECK_THAT(e.message,
               ContainsSubstring("the slot is narrower there than at its floor, or something stands over it"));
}

TEST_CASE("slot: the same operation gives the same bytes every time", "[cam][slot]")
{
    const Case c = slotCase({s1(), s2(), s3()}, endMill(8), R"({"widthTolerance":1})");
    // Three slots of widths 8, 6 and 10: the 8 mm tool is wider than the 6 mm one.
    CHECK_FALSE(c.validate());
    const Case one = slotCase({s1()}, endMill(8));
    const auto first = one.path().serialize();
    for (int i = 0; i < 3; ++i) {
        auto again = one.generate();
        REQUIRE(again);
        CHECK(again->serialize() == first);
    }
}
