// The general rules of the toolpath invariant checker (toolpath_invariants.h, task T-035): each rule finds the move
// that breaks it and passes a toolpath that keeps it.
#include "toolpath_invariants.h"

#include <catch2/catch_test_macros.hpp>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

constexpr Nm mm(std::int64_t v)
{
    return v * 1'000'000;
}

Move rapid(Nm x, Nm y, Nm z)
{
    Move m{};
    m.kind = MoveKind::Rapid;
    m.role = MoveRole::Link;
    m.x = x;
    m.y = y;
    m.z = z;
    return m;
}
Move linear(Nm x, Nm y, Nm z, std::int32_t feed)
{
    Move m = rapid(x, y, z);
    m.kind = MoveKind::Linear;
    m.role = MoveRole::Cut;
    m.feedMicronPerMin = feed;
    return m;
}
Move drillAt(Nm x, Nm y, Nm bottom, Nm r, Nm clearance, CycleReturn ret)
{
    Move m = rapid(x, y, bottom);
    m.kind = MoveKind::CycleDrill;
    m.role = MoveRole::Plunge;
    m.feedMicronPerMin = 100'000;
    m.cycleRetract = r;
    m.cycleTop = r - mm(2);
    m.cycleClearance = clearance;
    m.cycleReturn = ret;
    return m;
}

Toolpath pathOf(std::vector<Move> moves, Nm startZ = mm(50), Nm startX = 0, Nm startY = 0)
{
    Toolpath p;
    p.startX = startX;
    p.startY = startY;
    p.startZ = startZ;
    p.moves = std::move(moves);
    return p;
}

// Stock 0..100 x 0..50 x 0..20.
ToolpathRules stockRules()
{
    ToolpathRules r;
    r.stock = Box3{Vec3{0, 0, 0}, Vec3{100, 50, 20}};
    r.rapidFloorZ = mm(20);
    return r;
}

} // namespace

TEST_CASE("invariants: feeds on rapids and feed moves", "[cam][invariants]")
{
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(10)), linear(mm(5), 0, mm(10), 1000)})).empty());
    Move fast = rapid(0, 0, mm(10));
    fast.feedMicronPerMin = 5;
    CHECK(checkToolpath(pathOf({fast})) == std::vector<Violation>{{0, "rapidFeed"}});
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(10)), linear(mm(5), 0, mm(10), 0)})) ==
          std::vector<Violation>{{1, "feedMove"}});
}

TEST_CASE("invariants: no rapid below the floor over the stock", "[cam][invariants]")
{
    const ToolpathRules r = stockRules();
    // Above the stock top, or low but beside the stock: fine.
    CHECK(checkToolpath(pathOf({rapid(mm(10), mm(10), mm(25)), rapid(mm(90), mm(40), mm(20))}), r).empty());
    CHECK(checkToolpath(
              pathOf({rapid(mm(-10), mm(-10), mm(5)), rapid(mm(-10), mm(60), mm(5))}, mm(5), -mm(10), -mm(10)), r)
              .empty());
    // Down into the stock, and across it low.
    CHECK(checkToolpath(pathOf({rapid(mm(10), mm(10), mm(25)), rapid(mm(10), mm(10), mm(15))}), r) ==
          std::vector<Violation>{{1, "rapidLow"}});
    CHECK(checkToolpath(pathOf({rapid(mm(-10), mm(25), mm(5)), rapid(mm(110), mm(25), mm(5))}, mm(5)), r) ==
          std::vector<Violation>{{0, "rapidLow"}, {1, "rapidLow"}});
    // A cycle run returning to the R plane (Z 12, below the stock top) travels low to the next hole: G99 where G98
    // was needed. Returning to the initial level it is fine.
    const auto twoHoles = [](CycleReturn first) {
        return pathOf({rapid(mm(10), mm(10), mm(40)), drillAt(mm(10), mm(10), -mm(5), mm(12), mm(40), first),
                       drillAt(mm(30), mm(10), -mm(5), mm(12), mm(40), CycleReturn::Initial)});
    };
    CHECK(checkToolpath(twoHoles(CycleReturn::Initial), r).empty());
    CHECK(checkToolpath(twoHoles(CycleReturn::RPlane), r) == std::vector<Violation>{{2, "rapidLow"}});
}

TEST_CASE("invariants: nothing below the floor", "[cam][invariants]")
{
    ToolpathRules r;
    r.floorZ = -mm(3);
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(10)), drillAt(0, 0, -mm(3), mm(2), mm(10), CycleReturn::Initial)}), r)
              .empty());
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(10)), drillAt(0, 0, -mm(4), mm(2), mm(10), CycleReturn::Initial)}), r) ==
          std::vector<Violation>{{1, "belowFloor"}});
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(10)), linear(0, 0, -mm(5), 100)}), r) ==
          std::vector<Violation>{{1, "belowFloor"}});
}

TEST_CASE("invariants: inside the machine's travel", "[cam][invariants]")
{
    ToolpathRules r;
    r.axes = {lib::AxisDef{'X', 0, 500, 0, 0, 0}, lib::AxisDef{'Y', 0, 0, 0, 0, 0}, // Y not known
              lib::AxisDef{'Z', -400, 0, 0, 0, 0}};
    r.wcsInMachine = Vec3{100, 100, -300};
    CHECK(checkToolpath(pathOf({rapid(mm(350), mm(9000), mm(50)), linear(mm(-100), 0, -mm(99), 100)}), r).empty());
    CHECK(checkToolpath(pathOf({rapid(mm(401), 0, mm(50))}), r) == std::vector<Violation>{{0, "travel"}});
    CHECK(checkToolpath(pathOf({rapid(0, 0, mm(50)), linear(0, 0, -mm(101), 100)}), r) ==
          std::vector<Violation>{{1, "travel"}});
    // A cycle's clearance counts too: Z 310 is above the top of Z travel.
    CHECK(checkToolpath(
              pathOf({rapid(0, 0, mm(310)), drillAt(0, 0, -mm(5), mm(2), mm(310), CycleReturn::Initial)}, mm(310)),
              r) == std::vector<Violation>{{0, "travel"}, {1, "travel"}});
}
