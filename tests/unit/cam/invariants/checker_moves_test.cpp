// The invariants on the moves themselves (task T-058): for each, a toolpath built to break it is caught, and its
// correct twin passes; near misses at the stated limits on both sides. Every path here runs in the air above a plain
// block, so only the rule under test can fire.
#include "checker_test_util.h"

#include <catch2/catch_test_macros.hpp>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;
using namespace ray::cam::invariants::test;

namespace {

struct Block {
    kernel::Shape part = box(Vec3{0, 0, -10}, Vec3{50, 30, 0});
    ResolvedGeometry g = geometryOn(part);
    lib::Tool tool = flatMill(6);
    lib::MachineDef m = machine();
    Claims c = claims(g);

    std::vector<Finding> run(const Toolpath& p) const { return test::run(p, g, tool, c, nullptr, &m); }
};

// A rapid down, a feed across in the air at Z 10, a rapid up.
PathBuilder air()
{
    PathBuilder b(Vec3{-10, -10, 20});
    b.rapid(Vec3{-10, -10, 10}, MoveRole::Approach);
    b.feed(Vec3{60, -10, 10}, 500);
    return b;
}

Toolpath finish(PathBuilder& b)
{
    b.rapid(Vec3{b.position().x, b.position().y, 20}, MoveRole::Retract);
    return b.take();
}

} // namespace

TEST_CASE("invariants: a toolpath in the air keeps every rule", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    PathBuilder b = air();
    clean(k.run(finish(b)));
}

TEST_CASE("invariants: enum, a field outside its enumeration", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    PathBuilder b = air();
    b.last().role = static_cast<MoveRole>(9);
    onlyThis(k.run(finish(b)), "enum");
}

TEST_CASE("invariants: spindle on, in the machine's range, turning the right way", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    PathBuilder b = air();
    const Toolpath p = finish(b);
    k.c.spindleRpm = 0;
    onlyThis(k.run(p), "spindle");
    k.c.spindleRpm = 12001; // the machine's maximum is 12000
    onlyThis(k.run(p), "spindle");
    k.c.spindleRpm = 12000;
    clean(k.run(p));
    k.c.clockwise = false;
    onlyThis(k.run(p), "spindle");
}

TEST_CASE("invariants: arcs well formed", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    // A half circle in the air, then its end radius off by 2 delta nm (start radius 5 mm + delta, end 5 mm - delta).
    const auto half = [&](Nm delta) {
        PathBuilder b = air();
        b.arc(Vec3{70, -10, 10}, Vec2{65, -10}, true, 500);
        b.last().i += delta;
        return finish(b);
    };
    clean(k.run(half(0)));
    clean(k.run(half(1)));         // 2 nm: the limit, kept
    const auto f = k.run(half(2)); // 4 nm
    onlyThis(f, "arc");
    CHECK(find(f, "arc")->measured == 4);
    // A radius under 1 um, and an arc whose ends are 0.5 um apart (a full turn or nothing).
    PathBuilder tiny = air();
    tiny.arc(Vec3{60.0008, -10, 10}, Vec2{60.0004, -10}, true, 500);
    onlyThis(k.run(finish(tiny)), "arc");
    PathBuilder close = air();
    close.arc(Vec3{60.0005, -10, 10}, Vec2{65, -10}, true, 500);
    onlyThis(k.run(finish(close)), "arc");
    PathBuilder full = air(); // start == end: a full turn, well formed
    full.arc(Vec3{60, -10, 10}, Vec2{65, -10}, true, 500);
    clean(k.run(finish(full)));
}

TEST_CASE("invariants: cutter compensation on and off on linear moves, balanced, on the asked side",
          "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    const auto path = [](std::uint8_t onArc, std::uint8_t onRapid, bool offAtEnd, std::uint8_t side) {
        PathBuilder b = air();
        b.feed(Vec3{60, 0, 10}, 500, MoveRole::LeadIn);
        b.last().compensation = side; // on, on a linear move
        b.feed(Vec3{60, 20, 10}, 500);
        b.last().compensation = side;
        b.arc(Vec3{60, 30, 10}, Vec2{60, 25}, false, 500); // clockwise: ends heading +X, into the lead-out
        b.last().compensation = onArc;
        b.feed(Vec3{70, 30, 10}, 500, MoveRole::LeadOut);
        b.last().compensation = offAtEnd ? 0 : onArc;
        b.rapid(Vec3{70, 30, 20}, MoveRole::Retract);
        b.last().compensation = onRapid;
        return b.take();
    };
    clean(k.run(path(1, 0, true, 1)));
    onlyThis(k.run(path(0, 0, true, 1)), "compensation"); // turned off on an arc
    onlyThis(k.run(path(1, 1, false, 1)),
             "compensation"); // still on at the end (and on the last rapid it is unchanged)
    // The side: climb with a clockwise spindle keeps the material on the right, so the tool is left of it (G41, 1).
    k.c.direction = Direction::Climb;
    clean(k.run(path(1, 0, true, 1)));
    onlyThis(k.run(path(2, 0, true, 2)), "compensation");
    k.c.direction = Direction::Conventional;
    clean(k.run(path(2, 0, true, 2)));
    // A cycle with compensation on.
    PathBuilder b = air();
    b.rapid(Vec3{60, -10, 20});
    Move hole{};
    hole.kind = MoveKind::CycleDrill;
    hole.role = MoveRole::Plunge;
    hole.x = toNm(60);
    hole.y = toNm(-10);
    hole.z = toNm(5);
    hole.cycleTop = toNm(8);
    hole.cycleRetract = toNm(9);
    hole.cycleClearance = toNm(20);
    hole.cycleReturn = CycleReturn::Initial;
    hole.feedMicronPerMin = 100000;
    hole.compensation = 1;
    Toolpath p = b.take();
    p.moves.push_back(hole);
    k.c.direction = Direction::Any;
    const auto f = k.run(p);
    REQUIRE(has(f, "compensation"));
    hole.compensation = 0;
    p.moves.back() = hole;
    clean(k.run(p));
}

TEST_CASE("invariants: continuity, a dwell or cycle cancel carries the tool's position", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    for (const Nm dz : {Nm{0}, toNm(1)}) {
        PathBuilder b = air();
        Toolpath p = b.take();
        Move dwell = p.moves.back();
        dwell.kind = MoveKind::Dwell;
        dwell.feedMicronPerMin = 0;
        dwell.cycleDwellMs = 500;
        dwell.z += dz;
        p.moves.push_back(dwell);
        if (dz == 0) {
            clean(k.run(p));
        } else {
            onlyThis(k.run(p), "continuity");
        }
    }
}

TEST_CASE("invariants: lead-ins and lead-outs meet the cut tangentially", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    constexpr double kPi = 3.14159265358979323846;
    const auto path = [&](double inDeg, double outDeg) {
        PathBuilder b(Vec3{-10, -10, 20});
        b.rapid(Vec3{-10, -10, 10}, MoveRole::Approach);
        b.feed(Vec3{0, -10, 10}, 500, MoveRole::LeadIn);
        b.feed(Vec3{20, -10 + 20 * std::tan(inDeg * kPi / 180), 10}, 500, MoveRole::Cut);
        const Vec3 at = b.position();
        b.feed(Vec3{at.x + 10, at.y + 10 * std::tan(outDeg * kPi / 180 + inDeg * kPi / 180), 10}, 500,
               MoveRole::LeadOut);
        return finish(b);
    };
    clean(k.run(path(0, 0)));
    clean(k.run(path(0.09, 0))); // under the 0.1 degree limit
    onlyThis(k.run(path(0.2, 0)), "leadTangent");
    onlyThis(k.run(path(0, -0.2)), "leadTangent");
    // A tangent arc lead-in, as the contour reference makes, passes.
    PathBuilder arc(Vec3{-10, -14, 20});
    arc.rapid(Vec3{-10, -14, 10}, MoveRole::Approach);
    arc.arc(Vec3{-6, -10, 10}, Vec2{-6, -14}, true, 500, MoveRole::LeadIn); // ends heading -X
    arc.feed(Vec3{-20, -10, 10}, 500, MoveRole::Cut);
    clean(k.run(finish(arc)));
}

TEST_CASE("invariants: heights in order, the toolpath starts and ends at the clearance", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    PathBuilder b = air();
    const Toolpath p = finish(b);
    k.c.heights.clearance = 20;
    k.c.heights.retract = 5;
    k.c.heights.feedPlane = 1;
    k.c.heights.top = 0;
    k.c.heights.bottom = -5;
    clean(k.run(p));
    k.c.heights.retract = 21;
    onlyThis(k.run(p), "heights");
    k.c.heights.retract = 5;
    k.c.heights.clearance = 20.5; // the path starts at 20
    onlyThis(k.run(p), "heights");
}

TEST_CASE("invariants: no move outside the containment region plus tolerance", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    PathBuilder b = air();                           // feeds from (-10, -10) to (60, -10)
    b.arc(Vec3{60, 0, 10}, Vec2{60, -5}, true, 500); // bulges to X 65
    const Toolpath p = finish(b);
    const auto region = [](double x1) {
        return Region::of({{{-20, -20}, {x1, -20}, {x1, 40}, {-20, 40}}});
    };
    k.c.containment = ContainClaim{region(65.005), 0.01};
    clean(k.run(p)); // 5 um outside at the arc's bulge, 10 um allowed
    k.c.containment = ContainClaim{region(64.98), 0.01};
    const auto f = k.run(p);
    onlyThis(f, "containment");
    CHECK(std::abs(find(f, "containment")->measured - 0.02) < 1e-3);
}

TEST_CASE("invariants: machine travel, end points and the bulge of arcs", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400}; // machine X 0..762: WCS X -100..662
    const auto path = [](double cx) {
        PathBuilder b(Vec3{cx, 5, 20});
        b.rapid(Vec3{cx, 5, 10});
        b.arc(Vec3{cx, -5, 10}, Vec2{cx, 0}, true, 500); // through (cx - 5, 0)
        return finish(b);
    };
    clean(k.run(path(-95)));                  // touches WCS X -100: machine X 0
    onlyThis(k.run(path(-95.002)), "travel"); // 2 um beyond, only at the arc's bulge
    PathBuilder far = air();
    far.feed(Vec3{663, -10, 10}, 500);
    onlyThis(k.run(finish(far)), "travel");
}

TEST_CASE("invariants: the M2 rules run inside the one entry point", "[cam][invariants]")
{
    Block k;
    k.c.wcsInMachine = Vec3{100, 100, -400};
    {
        PathBuilder b = air();
        b.rapid(Vec3{60, 0, 10});
        b.last().feedMicronPerMin = 5;
        onlyThis(k.run(finish(b)), "rapidFeed");
    }
    {
        PathBuilder b = air();
        b.feed(Vec3{60, 0, 10}, 0);
        onlyThis(k.run(finish(b)), "feedMove");
    }
    {
        // Rapid floor 5 over the stock: a rapid at Z 3 across it touches nothing but breaks the floor.
        Block r;
        r.c.wcsInMachine = Vec3{100, 100, -400};
        r.c.rapidFloorZ = 5;
        PathBuilder b(Vec3{-10, 15, 20});
        b.rapid(Vec3{-10, 15, 3});
        b.rapid(Vec3{60, 15, 3});
        onlyThis(r.run(finish(b)), "rapidLow");
    }
    {
        k.c.floorZ = 9.5;
        PathBuilder b = air();
        b.feed(Vec3{60, 0, 9}, 500);
        onlyThis(k.run(finish(b)), "belowFloor");
        k.c.floorZ.reset();
    }
    {
        // A drill cycle whose R plane is above its clearance.
        PathBuilder b(Vec3{60, -10, 20});
        Toolpath p = b.take();
        Move hole{};
        hole.kind = MoveKind::CycleDrill;
        hole.role = MoveRole::Plunge;
        hole.x = toNm(60);
        hole.y = toNm(-10);
        hole.z = toNm(5);
        hole.cycleTop = toNm(8);
        hole.cycleRetract = toNm(21);
        hole.cycleClearance = toNm(20);
        hole.cycleReturn = CycleReturn::Initial;
        hole.feedMicronPerMin = 100000;
        p.moves.push_back(hole);
        const auto f = k.run(p);
        REQUIRE(has(f, "levels"));
        hole.cycleRetract = toNm(9);
        p.moves.back() = hole;
        clean(k.run(p));
    }
}
