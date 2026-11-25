// The geometric invariants (task T-058): gouge, stock to leave, shank, holder, flute length, rapids through part and
// stock, plunges, ramps and helices, climb or conventional, coverage, walls left, engagement. For each, a toolpath that
// breaks it is caught and its correct twin passes; the part contacts are also tried at their stated error bound on
// both sides (checker.h: a pass means at most allowed + chord, a fail more than allowed - minCell sqrt(2) - chord).
#include "checker_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;
using namespace ray::cam::invariants::test;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A plate 60 x 40 x 20 (top Z 0) with a square-cornered pocket 40 x 20, floor Z -8, and a round pocket of radius 6
// at (30, 20) going on to Z -14 inside it.
kernel::Shape pocketPlate()
{
    kernel::Shape s = box(Vec3{0, 0, -20}, Vec3{60, 40, 0});
    s = cut(s, box(Vec3{10, 10, -8}, Vec3{50, 30, 1}));
    s = cut(s, cylinder(30, 20, -14, 6, 20));
    return s;
}

// Along the pocket's -Y wall at Z z, the tool's centre y from the wall; enters and leaves through the pocket's middle.
Toolpath wallPass(double y, double z)
{
    PathBuilder b(Vec3{40, 22, 10});
    b.rapid(Vec3{40, 22, 2}, MoveRole::Approach);
    b.feed(Vec3{40, 22, z}, 300, MoveRole::Plunge);
    b.feed(Vec3{40, y, z}, 500, MoveRole::Link);
    b.feed(Vec3{20, y, z}, 500, MoveRole::Cut);
    b.feed(Vec3{20, 22, z}, 500, MoveRole::Link);
    b.feed(Vec3{20, 22, z + 1}, 500, MoveRole::Retract); // off the floor at feed, then the rapid
    b.rapid(Vec3{20, 22, 10}, MoveRole::Retract);
    return b.take();
}

// A full circle round the round pocket's wall at radius r, at Z z.
Toolpath circlePass(double r, double z)
{
    PathBuilder b(Vec3{30, 20, 10});
    b.rapid(Vec3{30, 20, 2}, MoveRole::Approach);
    b.feed(Vec3{30, 20, z}, 300, MoveRole::Plunge);
    b.feed(Vec3{30 + r, 20, z}, 500, MoveRole::Link);
    b.arc(Vec3{30 + r, 20, z}, Vec2{30, 20}, true, 500);
    b.feed(Vec3{30, 20, z}, 500, MoveRole::Link);
    b.feed(Vec3{30, 20, z + 1}, 500, MoveRole::Retract);
    b.rapid(Vec3{30, 20, 10}, MoveRole::Retract);
    return b.take();
}

struct Plate {
    kernel::Shape part = pocketPlate();
    ResolvedGeometry g = geometryOn(part);
    lib::Tool tool = flatMill(6);
    Claims c = [this] {
        Claims k = claims(g);
        k.initialStock = InitialStock::Part; // no stock: the part contacts alone
        return k;
    }();
    std::vector<Finding> run(const Toolpath& p, const lib::Holder* h = nullptr) const
    {
        return test::run(p, g, tool, c, h);
    }
};

} // namespace

TEST_CASE("invariants: gouge of walls and floors, at the error bound on both sides", "[cam][invariants][geometry]")
{
    Plate k;
    const ErrorBounds e = errorBounds(CheckOptions{}, k.g.stockBounds);
    INFO("a contact passes up to tolerance - " << e.partBelow << " and fails from tolerance + " << e.partAbove);
    REQUIRE(e.partBelow < 0.003);
    REQUIRE(e.partAbove < 0.0012);
    // The -Y wall is Y 10; a 6 mm tool's centre at Y 13 touches it. The floor is Z -8.
    clean(k.run(wallPass(13, -8)));
    clean(k.run(wallPass(13 - 0.007, -8)));              // 7 um into the wall: under 10 um - partBelow, passes
    onlyThis(k.run(wallPass(13 - 0.0115, -8)), "gouge"); // 11.5 um: over 10 um + partAbove, fails
    clean(k.run(wallPass(13, -8 - 0.007)));              // the floor, the same
    onlyThis(k.run(wallPass(13, -8 - 0.0115)), "gouge");
    // A curved wall (meshed in chords of 1 um): the round pocket's wall at radius 6 from (30, 20).
    clean(k.run(circlePass(3, -14)));
    clean(k.run(circlePass(3.007, -14)));
    const auto f = k.run(circlePass(3.0115, -14));
    onlyThis(f, "gouge");
    CHECK(find(f, "gouge")->measured > 0.0099);
    CHECK(find(f, "gouge")->measured < 0.0115 + e.partAbove);
    // A gross gouge: measured close to the truth.
    const auto g = k.run(wallPass(12, -8));
    onlyThis(g, "gouge");
    CHECK(std::abs(find(g, "gouge")->measured - 1.0) < 0.3);
}

TEST_CASE("invariants: stock to leave is not cut into", "[cam][invariants][geometry]")
{
    Plate k;
    k.c.stockToLeaveWall = 0.5;
    k.c.stockToLeaveFloor = 0.2;
    clean(k.run(wallPass(13.5, -7.8)));
    onlyThis(k.run(wallPass(13.5 - 0.0115, -7.8)), "gouge");
    onlyThis(k.run(wallPass(13.5, -7.8 - 0.0115)), "gouge");
    clean(k.run(wallPass(13.5 - 0.007, -7.8 - 0.007)));
}

TEST_CASE("invariants: the tool above its flutes does not touch the part", "[cam][invariants][geometry]")
{
    Plate k;
    k.tool = flatMill(6, 5); // flutes 5 long: at Z -8 the flutes end at Z -3, the shank runs along the wall above
    clean(k.run(wallPass(13, -8)));
    k.tool.shankDiameter = 8; // a shank wider than the cut: it runs into the wall above Z -2
    onlyThis(k.run(wallPass(13, -8)), "shank");
}

TEST_CASE("invariants: the holder keeps its clearance to the part", "[cam][invariants][geometry]")
{
    Plate k;
    lib::Holder h;
    h.segments = {lib::HolderSegment{20, 44, 44}, lib::HolderSegment{40, 24, 32}};
    h.gaugeLength = 60;
    k.c.holderClearance = 0.5;
    // Cutting at Z -8, the holder's nose (radius 12) is stick-out - 8 above the part's top (Z 0).
    k.c.stickOut = 8.6; // 0.6 above: clear by more than 0.5
    clean(k.run(wallPass(13, -8), &h));
    k.c.stickOut = 8.4; // 0.4 above: 0.1 inside the clearance
    const auto f = k.run(wallPass(13, -8), &h);
    onlyThis(f, "holder");
    CHECK(std::abs(find(f, "holder")->measured - 0.1) < 0.01);
    k.c.stickOut = 7; // into the part (and so is the retract that starts there)
    const auto deep = k.run(wallPass(13, -8), &h);
    CHECK(has(deep, "holder"));
    CHECK(std::abs(find(deep, "holder")->measured - 1.5) < 0.15);
}

namespace {

// A block 60 x 40, top Z 0, under a stock box reaching Z 10: the stock above the part is what is cut.
struct Slab {
    kernel::Shape part = box(Vec3{0, 0, -10}, Vec3{60, 40, 0});
    ResolvedGeometry g = geometryOn(part, Box3{Vec3{0, 0, -10}, Vec3{60, 40, 10}});
    lib::Tool tool = flatMill(6);
    Claims c = claims(g);
    std::vector<Finding> run(const Toolpath& p) const { return test::run(p, g, tool, c); }
};

// A slot along X through the stock at Z z, entered from outside it.
Toolpath slot(double z, double y = 20)
{
    PathBuilder b(Vec3{-10, y, 20});
    b.rapid(Vec3{-10, y, z}, MoveRole::Approach);
    b.feed(Vec3{70, y, z}, 500);
    b.rapid(Vec3{70, y, 20}, MoveRole::Retract);
    return b.take();
}

} // namespace

TEST_CASE("invariants: stock never reaches above the flutes", "[cam][invariants][geometry]")
{
    Slab k;
    const ErrorBounds e = errorBounds(CheckOptions{}, k.g.stockBounds);
    INFO("a stock contact deeper than " << e.stockMiss << " is always found");
    k.tool = flatMill(6, 6);
    clean(k.run(slot(4))); // 6 deep with 6 mm of flutes
    k.tool = flatMill(6, 6 - e.stockMiss - 0.005);
    const auto f = k.run(slot(4));
    onlyThis(f, "fluteLength");
    k.tool = flatMill(6, 4);
    onlyThis(k.run(slot(4)), "fluteLength");
    // Two levels with 4 mm flutes: the second runs in the slot the first cut.
    PathBuilder b(Vec3{-10, 20, 20});
    b.rapid(Vec3{-10, 20, 7}, MoveRole::Approach);
    b.feed(Vec3{70, 20, 7}, 500);
    b.rapid(Vec3{70, 20, 20}, MoveRole::Retract);
    b.rapid(Vec3{-10, 20, 20}, MoveRole::Link);
    b.rapid(Vec3{-10, 20, 4}, MoveRole::Approach);
    b.feed(Vec3{70, 20, 4}, 500);
    b.rapid(Vec3{70, 20, 20}, MoveRole::Retract);
    clean(k.run(b.take()));
}

TEST_CASE("invariants: rapids do not run through part or stock", "[cam][invariants][geometry]")
{
    Slab k;
    k.c.rapidFloorZ = -20; // the M2 floor out of the way: only contacts
    const auto rapidAcross = [](double z) {
        PathBuilder b(Vec3{-10, 20, 20});
        b.rapid(Vec3{-10, 20, z});
        b.rapid(Vec3{70, 20, z});
        b.rapid(Vec3{70, 20, 20}, MoveRole::Retract);
        return b.take();
    };
    clean(k.run(rapidAcross(10.001)));
    onlyThis(k.run(rapidAcross(5)), "rapidStock");
    k.c.initialStock = InitialStock::Part; // no stock: the part alone
    clean(k.run(rapidAcross(0.001)));
    onlyThis(k.run(rapidAcross(-2)), "rapidPart");
    k.c.rapidClearance = 1; // rapids keep 1 mm away
    onlyThis(k.run(rapidAcross(0.5)), "rapidPart");
    clean(k.run(rapidAcross(1.001)));
}

TEST_CASE("invariants: a pointed tool's rapid just into uncut stock: the rapid floor catches it, samples may not",
          "[cam][invariants][geometry]")
{
    // Why the engraving plug-in claims no rapid floor below the stock top (src/cam/ops/engrave.h, LEVELS): over uncut
    // stock the rapid floor (rapidLow) is exact, while rapidStock sees stock only at its samples. A 30 degree engraver
    // rapids 0.05 mm into the stock: its point is 0.013 mm wide there. Down at four points 0.025 mm apart along the
    // grid's diagonal (so at least one lies 0.035 mm or more from every sample, whatever the grid's offset), the floor
    // at the stock top reports every one; a floor claimed 0.5 mm lower leaves them to the samples, which miss some.
    Slab k;
    k.tool = flatMill(3);
    k.tool.type = lib::ToolType::EngraveCutter;
    k.tool.tipAngleDeg = 30;
    k.tool.tipDiameter = 0;
    int missed = 0;
    for (int i = 0; i < 4; ++i) {
        const double d = 0.025 * i;
        PathBuilder b(Vec3{30 + d, 20 + d, 20});
        b.rapid(Vec3{30 + d, 20 + d, 9.95}, MoveRole::Approach);
        b.rapid(Vec3{30 + d, 20 + d, 20}, MoveRole::Retract);
        const Toolpath p = b.take();
        mainly(k.run(p), "rapidLow", {"rapidStock"});
        Claims lowered = k.c;
        lowered.rapidFloorZ = 9.5;
        const auto f = test::run(p, k.g, k.tool, lowered);
        for (const Finding& x : f) {
            CHECK(x.id == "rapidStock");
        }
        missed += f.empty() ? 1 : 0;
    }
    CHECK(missed > 0);
}

TEST_CASE("invariants: no straight plunge into stock with a tool that does not cut at its centre",
          "[cam][invariants][geometry]")
{
    Slab k;
    const auto plunge = [] {
        PathBuilder b(Vec3{30, 20, 20});
        b.rapid(Vec3{30, 20, 11}, MoveRole::Approach);
        b.feed(Vec3{30, 20, 6}, 200, MoveRole::Plunge);
        b.rapid(Vec3{30, 20, 20}, MoveRole::Retract);
        return b.take();
    };
    k.tool.centreCutting = false; // a flat end mill whose library entry says it does not cut at its centre
    onlyThis(k.run(plunge()), "plunge");
    k.c.centreCutting = true; // the same tool, the operation claiming it does
    clean(k.run(plunge()));
    k.c.centreCutting.reset();
    k.tool.centreCutting.reset(); // its entry says nothing: the type's default, which is centre cutting (after M3)
    clean(k.run(plunge()));
    k.tool.type = lib::ToolType::FaceMill; // not centre cutting by its type
    onlyThis(k.run(plunge()), "plunge");
    k.tool = ballMill(6); // centre cutting by its type
    clean(k.run(plunge()));
    k.tool.centreCutting = false; // unless its entry says otherwise
    onlyThis(k.run(plunge()), "plunge");
    // Into a pre-drilled hole (the stock has a 4 mm hole there): the dead centre meets no stock.
    k.tool = flatMill(6);
    k.tool.centreCutting = false;
    k.part = cut(k.part, cylinder(30, 20, -5, 2, 20));
    k.g = geometryOn(k.part, Box3{Vec3{0, 0, -10}, Vec3{60, 40, 0}});
    k.c = claims(k.g);
    k.c.initialStock = InitialStock::Part;
    k.c.allowedCuts = {AllowedCut{30, 20, 3, -5, 1}}; // the material round the hole is what the plunge cuts
    PathBuilder b(Vec3{30, 20, 20});
    b.rapid(Vec3{30, 20, 1}, MoveRole::Approach);
    b.feed(Vec3{30, 20, -4}, 200, MoveRole::Plunge);
    b.rapid(Vec3{30, 20, 20}, MoveRole::Retract);
    clean(k.run(b.take()));
}

TEST_CASE("invariants: ramp angle and helix diameter within their limits", "[cam][invariants][geometry]")
{
    Slab k;
    k.c.maxRampAngleDeg = 5;
    const auto ramp = [](double deg) {
        PathBuilder b(Vec3{10, 20, 20});
        b.rapid(Vec3{10, 20, 10.5}, MoveRole::Approach);
        b.feed(Vec3{10, 20, 10}, 200, MoveRole::Plunge); // to the stock top, in the air
        b.feed(Vec3{40, 20, 10 - 30 * std::tan(deg * kPi / 180)}, 300, MoveRole::Ramp);
        b.rapid(Vec3{b.position().x, 20, 20}, MoveRole::Retract);
        return b.take();
    };
    clean(k.run(ramp(4.9)));
    const auto f = k.run(ramp(5.2));
    onlyThis(f, "rampAngle");
    CHECK(std::abs(find(f, "rampAngle")->measured - 5.2) < 1e-6);
    // A helix: turns of 0.5 mm about (30, 20).
    k.c.maxRampAngleDeg.reset();
    k.c.helixMinDiameter = 3;
    const auto helix = [](double r) {
        PathBuilder b(Vec3{30 + r, 20, 20});
        b.rapid(Vec3{30 + r, 20, 10.5}, MoveRole::Approach);
        b.feed(Vec3{30 + r, 20, 10}, 200, MoveRole::Plunge);
        for (int t = 1; t <= 4; ++t) {
            b.arc(Vec3{30 + r, 20, 10 - 0.5 * t}, Vec2{30, 20}, true, 300, MoveRole::Ramp);
        }
        b.rapid(Vec3{30 + r, 20, 20}, MoveRole::Retract);
        return b.take();
    };
    clean(k.run(helix(1.5)));
    onlyThis(k.run(helix(1.4)), "helix");
    k.c.maxRampAngleDeg = 3; // 0.5 per turn of radius 1.5: 3.04 degrees
    onlyThis(k.run(helix(1.5)), "rampAngle");
}

TEST_CASE("invariants: climb or conventional along walls, as asked", "[cam][invariants][geometry]")
{
    // A block 40 x 30 under a stock box 2 mm wider at each side.
    const kernel::Shape part = box(Vec3{0, 0, -10}, Vec3{40, 30, 0});
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{-2, -2, -10}, Vec3{42, 32, 0}});
    const lib::Tool tool = flatMill(6);
    ContourRef r;
    r.depths = {-5};
    Claims c = claims(g);
    c.direction = Direction::Climb;
    r.climb = true;
    clean(run(contourRect(r), g, tool, c));
    r.climb = false;
    onlyThis(run(contourRect(r), g, tool, c), "direction");
    c.direction = Direction::Conventional;
    clean(run(contourRect(r), g, tool, c));
}

TEST_CASE("invariants: a region claimed cleared is cleared", "[cam][invariants][geometry]")
{
    // Facing 1 mm off the top of a plate with a 10 mm tool.
    const kernel::Shape part = box(Vec3{0, 0, -10}, Vec3{50, 30, 0});
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{0, 0, -10}, Vec3{50, 30, 1}});
    const lib::Tool tool = flatMill(10);
    Claims c = claims(g);
    c.clears = {ClearClaim{Region::box(), 0, 0.005, "coverage"}};
    FaceRef r;
    r.stock = g.stockBounds;
    r.toolRadius = 5;
    r.z = 0;
    r.stepover = 6;
    clean(run(zigzagFace(r), g, tool, c));
    r.stepover = 10.5; // wider than the tool: strips left
    onlyThis(run(zigzagFace(r), g, tool, c), "coverage");
    r.stepover = 6;
    r.z = 0.01; // 10 um of stock left over the face, 5 um allowed
    onlyThis(run(zigzagFace(r), g, tool, c), "coverage");
    r.z = 0.004;
    clean(run(zigzagFace(r), g, tool, c));
}

TEST_CASE("invariants: walls claimed finished keep no more than the stock to leave", "[cam][invariants][geometry]")
{
    const kernel::Shape part = box(Vec3{0, 0, -10}, Vec3{40, 30, 0});
    const ResolvedGeometry g = geometryOn(part, Box3{Vec3{-2, -2, -10}, Vec3{42, 32, 0}});
    const lib::Tool tool = flatMill(6);
    Claims c = claims(g);
    c.stockToLeaveWall = 0.5;
    c.walls = {WallClaim{Region::box(), -5, 0, 0.25}};
    ContourRef r;
    r.depths = {-5};
    r.stockToLeave = 0.5;
    clean(run(contourRect(r), g, tool, c));
    r.stockToLeave = 1.0; // a millimetre left where 0.5 was claimed
    const auto f = run(contourRect(r), g, tool, c);
    onlyThis(f, "stockLeft");
}

TEST_CASE("invariants: engagement within its limit", "[cam][invariants][geometry]")
{
    Slab k;
    k.c.maxEngagementDeg = 120;
    const auto f = k.run(slot(5)); // a full slot: 180 degrees
    onlyThis(f, "engagement");
    CHECK(std::abs(find(f, "engagement")->measured - 180) < 12);
    // 1 mm off the side of the stock: acos(1 - 1/3) = 48 degrees.
    const auto side = k.run(slot(5, -2));
    clean(side);
    k.c.maxEngagementDeg = 30;
    const auto tight = k.run(slot(5, -2));
    onlyThis(tight, "engagement");
    CHECK(std::abs(find(tight, "engagement")->measured - 48.2) < 10);
}

TEST_CASE("invariants: the tool as radius at height, by type", "[cam][invariants][geometry]")
{
    const auto tool = [](lib::ToolType type, double d) {
        lib::Tool t = flatMill(d, 12);
        t.type = type;
        return t;
    };
    const double eps = 1e-9;
    lib::Tool bull = tool(lib::ToolType::BullEndMill, 10);
    bull.cornerRadius = 2;
    const ToolProfile b = makeProfile(bull, nullptr, 0);
    CHECK(std::abs(b.radiusAt(0) - 3) < eps);
    CHECK(std::abs(b.radiusAt(1) - (3 + std::sqrt(3.0))) < eps);
    CHECK(std::abs(b.radiusAt(2) - 5) < eps);
    CHECK(std::abs(b.hlow(4.5, 0, 2) - (2 - std::sqrt(4 - 1.5 * 1.5))) < eps); // the corner's height at radius 4.5
    const ToolProfile ball = makeProfile(tool(lib::ToolType::BallEndMill, 6), nullptr, 0);
    CHECK(ball.radiusAt(0) == 0);
    CHECK(std::abs(ball.radiusAt(3) - 3) < eps);
    lib::Tool drill = tool(lib::ToolType::Drill, 6);
    drill.tipAngleDeg = 118;
    const ToolProfile dr = makeProfile(drill, nullptr, 0);
    const double point = 3 / std::tan(59 * kPi / 180);
    CHECK(std::abs(dr.radiusAt(point / 2) - 1.5) < 1e-9);
    CHECK(std::abs(dr.radiusAt(point) - 3) < 1e-9);
    lib::Tool chamfer = tool(lib::ToolType::ChamferMill, 10);
    chamfer.tipAngleDeg = 90;
    chamfer.tipDiameter = 1;
    const ToolProfile ch = makeProfile(chamfer, nullptr, 0);
    CHECK(std::abs(ch.radiusAt(0) - 0.5) < eps);
    CHECK(std::abs(ch.radiusAt(2) - 2.5) < eps);
    CHECK(std::abs(ch.radiusAt(4.5) - 5) < eps);
    lib::Tool thread = tool(lib::ToolType::ThreadMill, 10);
    thread.threadPitch = 1.5;
    thread.shoulderLength = 30;
    const ToolProfile th = makeProfile(thread, nullptr, 0);
    CHECK(std::abs(th.radiusAt(6) - 5) < eps);                   // the teeth
    CHECK(std::abs(th.radiusAt(20) - (5 - 0.6134 * 1.5)) < eps); // the neck clears the thread
    CHECK(std::abs(th.maxRadius(Zone::Cutting) - 5) < eps);
    // A stated neck (after M3): 6 mm single-profile, 1.5 mm of flutes, a 4.2 mm neck to 20 mm, a 6 mm shank from
    // there (a 45 degree cone from 2.1 to 3 mm radius, 0.9 mm high), and a neck up to the shoulder when its length is
    // 0.
    lib::Tool single = tool(lib::ToolType::ThreadMill, 6);
    single.fluteLength = 1.5;
    single.shankDiameter = 6;
    single.shoulderLength = 20;
    single.neckDiameter = 4.2;
    single.neckLength = 20;
    const ToolProfile sn = makeProfile(single, nullptr, 0);
    CHECK(std::abs(sn.radiusAt(1) - 3) < eps);
    CHECK(std::abs(sn.radiusAt(1.6) - 2.1) < eps);
    CHECK(std::abs(sn.radiusAt(19.9) - 2.1) < eps);
    CHECK(std::abs(sn.radiusAt(20.45) - 2.55) < eps);
    CHECK(std::abs(sn.radiusAt(25) - 3) < eps);
    single.neckLength = 0;
    single.shoulderLength = 15;
    const ToolProfile ns = makeProfile(single, nullptr, 0);
    CHECK(std::abs(ns.radiusAt(14.9) - 2.1) < eps);
    CHECK(std::abs(ns.radiusAt(16) - 3) < eps);
    // A neck below a longer shoulder: the body between them is min(cutting, shank) radius.
    single.neckLength = 10;
    single.shoulderLength = 20;
    single.shankDiameter = 8;
    const ToolProfile nb = makeProfile(single, nullptr, 0);
    CHECK(std::abs(nb.radiusAt(9.9) - 2.1) < eps);
    CHECK(std::abs(nb.radiusAt(15) - 3) < eps);
    CHECK(std::abs(nb.radiusAt(25) - 4) < eps);
    // A collet chuck 30 mm above the tip: nose 24 mm, taper to 32 at 70, flange 44 from 70 to the gauge line at 90.
    lib::Holder holder;
    holder.segments = {lib::HolderSegment{20, 44, 44}, lib::HolderSegment{40, 24, 32}};
    holder.gaugeLength = 60;
    const ToolProfile h = makeProfile(flatMill(10, 12), &holder, 30);
    CHECK(h.zoneBottom(Zone::Body) == 12);
    CHECK(h.zoneBottom(Zone::Holder) == 30);
    CHECK(std::abs(h.radiusAt(29) - 5) < eps);
    CHECK(std::abs(h.radiusAt(50) - 14) < eps);
    CHECK(std::abs(h.radiusAt(80) - 22) < eps);
    CHECK(std::abs(h.radiusAt(1000) - 22) < eps);
}
