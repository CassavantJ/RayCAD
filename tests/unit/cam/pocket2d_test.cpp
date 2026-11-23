// The 2D pocket operation (task T-054): its rules (pocket2d.h) against hand-computed values on kernel-built parts, the
// coverage check, finishing, entries, open edges, islands, and each refusal with validate and generate agreeing.
// Every toolpath goes through the independent invariant checker (Case::path) with coverage claims on its floors.
#include "pocket2d_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <limits>
#include <map>
#include <set>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;
namespace g2 = ray::cam::geom2d;

namespace {

constexpr Nm kMm = 1'000'000;

const kernel::Shape& block()
{
    static const kernel::Shape s = goldenBlock();
    return s;
}

// The planar path of each pass at depth z, with its bounds.
std::vector<g2::Path> pathsAt(const ops::Pocket2dPlan& plan, Nm z)
{
    std::vector<g2::Path> out;
    for (const link::Pass& p : plan.passes) {
        if (p.start.z == z) {
            auto path = link::planarPath(p);
            REQUIRE(path);
            out.push_back(*path);
        }
    }
    return out;
}

struct Box2 {
    Nm x0, y0, x1, y1;
    auto operator<=>(const Box2&) const = default;
};
Box2 boxOf(const g2::Path& p)
{
    const g2::Bounds b = g2::bounds(p);
    return Box2{b.min.x, b.min.y, b.max.x, b.max.y};
}
std::multiset<Box2> boxesAt(const ops::Pocket2dPlan& plan, Nm z)
{
    std::multiset<Box2> out;
    for (const g2::Path& p : pathsAt(plan, z)) {
        out.insert(boxOf(p));
    }
    return out;
}
Box2 mmBox(double x0, double y0, double x1, double y1)
{
    return Box2{toNm(x0), toNm(y0), toNm(x1), toNm(y1)};
}

// A plate with a rectangular pocket (sharp corners) from (x0, y0) to (x1, y1) down to Z floorZ, its floor face, and
// the coverage claim on that floor.
Case rectCase(double x0, double y0, double x1, double y1, double floorZ, const std::string& params, double tool = 6)
{
    Case c;
    const kernel::Shape part = pocketBlock(x0, y0, x1, y1, 0, floorZ);
    c.op = pocketOp(params);
    c.tool = endMill(tool);
    c.geom = geometryOn(part, {upFace(part, floorZ)});
    c.clears = {clearClaim({rectPoly(x0, y0, x1, y1)}, floorZ)};
    return c;
}

// Both validate (when `fromValidate`) and generate refuse with `code` and a message containing `words`.
void refused(const Case& c, ErrorCode code, const std::string& words, bool fromValidate = true)
{
    auto g = c.generate();
    REQUIRE_FALSE(g);
    INFO(g.error().message);
    CHECK(g.error().code == code);
    CHECK_THAT(g.error().message, ContainsSubstring(words));
    auto k = c.check();
    REQUIRE_FALSE(k);
    CHECK(k.error().message == g.error().message);
    const Status v = c.validate();
    if (fromValidate) {
        REQUIRE_FALSE(v);
        CHECK(v.error().message == g.error().message);
    } else {
        CHECK(v);
    }
}

bool helical(const Move& m)
{
    return (m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW) && m.role == MoveRole::Ramp;
}

} // namespace

TEST_CASE("pocket2d depths: equal steps no deeper than the stepdown, from the top height to the floor",
          "[cam][pocket2d]")
{
    SECTION("roughing only")
    {
        // Stock top Z 15, floor Z 7: 8 mm in ceil(8 / 3) = 3 steps of 8 / 3 = 2.666667 mm (rounded to the nm).
        const Case c = rectCase(10, 10, 50, 30, 7, R"({"stepdown":3})");
        auto plan = c.plan();
        REQUIRE(plan);
        REQUIRE(plan->pockets.size() == 1);
        CHECK(plan->pockets[0].depths == std::vector<Nm>{12'333'333, 9'666'667, 7'000'000});
        CHECK(plan->pockets[0].top == 15 * kMm);
        CHECK(plan->pockets[0].bottom == 7 * kMm);
        CHECK_FALSE(plan->pockets[0].open);
        c.path();
    }
    SECTION("stock to leave on the floor, a floor finishing depth, a lower top height")
    {
        // top 15 - 1 = 14; bottom 7 + 0.1 = 7.1; roughing to 7.1 + 0.2 = 7.3: 6.7 mm in ceil(6.7 / 2.5) = 3 steps.
        Case c = rectCase(10, 10, 50, 30, 7, R"({"stepdown":2.5,"stockToLeaveFloor":0.1,"finishFloor":true})");
        c.op.heights.top = -1;
        auto plan = c.plan();
        REQUIRE(plan);
        CHECK(plan->pockets[0].depths == std::vector<Nm>{11'766'667, 9'533'333, 7'300'000, 7'100'000});
        const Toolpath p = c.path(c.claims(0, 0.1));
        CHECK(p.stats.minZ == 7.1);
    }
    SECTION("a flat top inside the pocket is a depth of its own")
    {
        // The golden block's pocket A: steps of 8 / 3 from Z 20 to its floor at Z 12, and the square island's top at Z
        // 16, which no step reaches.
        Case c;
        c.op = pocketOp(R"({"stepdown":3})");
        c.tool = endMill(6);
        c.geom = geometryOn(block(), {upFace(block(), 12)});
        auto plan = c.plan();
        REQUIRE(plan);
        CHECK(plan->pockets[0].depths == std::vector<Nm>{17'333'333, 16'000'000, 14'666'667, 12'000'000});
    }
}

TEST_CASE("pocket2d rings: the room is the pocket shrunk by the tool radius and the stock to leave, then a stepover "
          "at a time, cut climb from the inside out",
          "[cam][pocket2d]")
{
    // Pocket x 10..50, y 10..30; 6 mm tool, 0.5 mm on the walls: the room is x 13.5..46.5, y 13.5..26.5 (13 mm wide),
    // rings every 3 mm (50 %): 13, 7 and 1 mm wide.
    Case c = rectCase(10, 10, 50, 30, 10, R"({"stepdown":5,"stepover":50,"stockToLeaveWall":0.5})");
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->pockets[0].depths == std::vector<Nm>{10 * kMm});
    CHECK(boxesAt(*plan, 10 * kMm) == std::multiset<Box2>{mmBox(13.5, 13.5, 46.5, 26.5), mmBox(16.5, 16.5, 43.5, 23.5),
                                                          mmBox(19.5, 19.5, 40.5, 20.5)});
    for (const g2::Path& p : pathsAt(*plan, 10 * kMm)) {
        CHECK(g2::area(p) > 0); // counter-clockwise: the region on the left, climb with M3
    }
    // Inside out: the innermost ring has the lowest level.
    std::map<Box2, int> level;
    for (const link::Pass& p : plan->passes) {
        level[boxOf(*link::planarPath(p))] = p.level;
        CHECK(p.rotatable);
        CHECK(p.cluster == 0);
    }
    CHECK(level[mmBox(19.5, 19.5, 40.5, 20.5)] < level[mmBox(16.5, 16.5, 43.5, 23.5)]);
    CHECK(level[mmBox(16.5, 16.5, 43.5, 23.5)] < level[mmBox(13.5, 13.5, 46.5, 26.5)]);
    REQUIRE(plan->pockets[0].coverage.size() == 1);
    CHECK(plan->pockets[0].coverage[0].rings == 3);
    CHECK(plan->pockets[0].coverage[0].halfRings == 0);
    const Toolpath p = c.path(c.claims(0.5));
    // The step from one ring to the next stays down: one entry only (a helix), no plunge.
    CHECK(countRole(p, MoveRole::Plunge) == 0);
    CHECK(std::count_if(p.moves.begin(), p.moves.end(), helical) > 0);
    CHECK(countMoves(p, MoveRole::Approach, MoveKind::Rapid) == 2); // to the start, down to the feed plane

    SECTION("conventional reverses every ring")
    {
        c.op.paramsJson = R"({"stepdown":5,"stepover":50,"stockToLeaveWall":0.5,"cutMode":"conventional"})";
        auto conventional = c.plan();
        REQUIRE(conventional);
        for (const g2::Path& q : pathsAt(*conventional, 10 * kMm)) {
            CHECK(g2::area(q) < 0);
        }
        c.path(c.claims(0.5, 0, true));
    }
}

TEST_CASE("pocket2d coverage: rings are added where a stepover above the tool radius would leave the middle uncut",
          "[cam][pocket2d]")
{
    // Pocket y 10..24 (14 wide), 6 mm tool at 90 % (5.4 mm): the room is 8 mm wide, so the only ring (its boundary)
    // cuts 3 mm in from each side and leaves a 2 mm band in the middle; a ring at 2.7 mm (s / 2) cuts it.
    Case c = rectCase(10, 10, 50, 24, 10, R"({"stepdown":5,"stepover":90})");
    auto plan = c.plan();
    REQUIRE(plan);
    const ops::PocketCoverage& cv = plan->pockets[0].coverage.at(0);
    CHECK(cv.rings == 1);
    CHECK(cv.halfRings == 1);
    CHECK(boxesAt(*plan, 10 * kMm) ==
          std::multiset<Box2>{mmBox(13, 13, 47, 21), mmBox(13 + 2.7, 13 + 2.7, 47 - 2.7, 21 - 2.7)});
    // What the flat leaves of the area it can reach is narrower than the scallop limit.
    JobContext ctx;
    auto wide = g2::offset(cv.left, -ops::kScallopLimitNm / 2, ctx);
    REQUIRE(wide);
    CHECK(wide->empty());
    // Without the added ring it would not be: the check is not vacuous.
    auto reach = g2::offset(cv.room, 3 * kMm, ctx);
    REQUIRE(reach);
    std::vector<g2::Path> outer;
    for (const g2::Region& r : cv.room) {
        outer.push_back(r.outer);
    }
    auto swept = g2::sweptArea(outer, 3 * kMm, ctx);
    REQUIRE(swept);
    auto left = g2::subtract(*reach, *swept, ctx);
    REQUIRE(left);
    auto leftWide = g2::offset(*left, -ops::kScallopLimitNm / 2, ctx);
    REQUIRE(leftWide);
    CHECK(g2::area(*leftWide) > 30.0 * 1e12); // about 2 mm x 28 mm
    // And the independent checker agrees that the floor is cleared.
    c.path();
}

TEST_CASE("pocket2d coverage on the golden block: only corner remnants the tool cannot reach stay uncut",
          "[cam][pocket2d]")
{
    for (const double stepover : {30.0, 45.0, 70.0, 95.0}) {
        INFO("stepover " << stepover << " %");
        Case c;
        c.op = pocketOp(R"({"stepdown":4,"stepover":)" + std::to_string(stepover) + "}");
        c.tool = endMill(6);
        c.geom = geometryOn(block(), {upFace(block(), 12), upFace(block(), 14)});
        c.clears = {clearClaim({rectPoly(8, 8, 52, 62, 5), circlePoly(30, 22, 5), rectPoly(22, 40, 38, 50, 1)}, 12),
                    clearClaim({rectPoly(22, 40, 38, 50, 1)}, 16), clearClaim({rectPoly(62, 0, 95, 25)}, 14)};
        auto plan = c.plan();
        REQUIRE(plan);
        JobContext ctx;
        for (const ops::PocketReport& pk : plan->pockets) {
            for (const ops::PocketCoverage& cv : pk.coverage) {
                auto wide = g2::offset(cv.left, -ops::kScallopLimitNm / 2, ctx);
                REQUIRE(wide);
                CHECK(wide->empty());
            }
        }
        c.path();
    }
}

TEST_CASE("pocket2d islands: the rings keep the tool radius from each island; a lower island is cut over",
          "[cam][pocket2d]")
{
    Case c;
    c.op = pocketOp(R"({"stepdown":3,"stepover":40})");
    c.tool = endMill(6);
    c.geom = geometryOn(block(), {upFace(block(), 12)});
    auto plan = c.plan();
    REQUIRE(plan);
    // Above the square island's top (Z 16) only the round island stands: the loops round it are its circle (r 5)
    // grown by the tool radius and more. Below, the square island (x 22..38, y 40..50) too, grown by 3 mm.
    const auto islandLoops = [&](Nm z) {
        std::multiset<Box2> out;
        for (const g2::Path& p : pathsAt(*plan, z)) {
            if (g2::area(p) < 0) {
                out.insert(boxOf(p));
            }
        }
        return out;
    };
    const std::multiset<Box2> above = islandLoops(17'333'333);
    const std::multiset<Box2> below = islandLoops(12 * kMm);
    CHECK(above.count(mmBox(22, 14, 38, 30)) == 1);
    CHECK(above.count(mmBox(19, 37, 41, 53)) == 0);
    CHECK(below.count(mmBox(22, 14, 38, 30)) == 1);
    CHECK(below.count(mmBox(19, 37, 41, 53)) == 1);
    // The square island's top is cleared down to Z 16 (a depth of its own), and the floor round both islands.
    c.clears = {clearClaim({rectPoly(8, 8, 52, 62, 5), circlePoly(30, 22, 5), rectPoly(22, 40, 38, 50, 1)}, 12),
                clearClaim({rectPoly(22, 40, 38, 50, 1)}, 16)};
    c.path();
}

TEST_CASE("pocket2d open edges: the footprint runs out past the open side, so the tool clears the edge",
          "[cam][pocket2d]")
{
    Case c;
    c.op = pocketOp(R"({"stepdown":3,"stockToLeaveWall":0.3})");
    c.tool = endMill(8);
    c.geom = geometryOn(block(), {upFace(block(), 14)});
    auto plan = c.plan();
    REQUIRE(plan);
    const ops::PocketReport& pk = plan->pockets.at(0);
    CHECK(pk.open);
    // The step's front edge (y 0, x 62..95) is open: the strip outside it is 2 r + the stock to leave = 8.3 mm wide;
    // its other three edges are walls.
    const g2::Bounds fb = g2::bounds(pk.footprint.at(0));
    CHECK(fb.min.y == -8'300'000);
    CHECK(fb.max.y == 25 * kMm);
    CHECK(fb.min.x == 62 * kMm); // square at the ends: it does not wrap round the faces beside the edge
    CHECK(fb.max.x == 95 * kMm);
    // The rings reach past the edge (the outermost one 8.3 - 4.3 = 4 mm out), and stay 4.3 mm from the walls.
    Nm minY = std::numeric_limits<Nm>::max(), minX = minY, maxX = std::numeric_limits<Nm>::min(), maxY = maxX;
    for (const g2::Path& p : pathsAt(*plan, 14 * kMm)) {
        const Box2 b = boxOf(p);
        minY = std::min(minY, b.y0);
        maxY = std::max(maxY, b.y1);
        minX = std::min(minX, b.x0);
        maxX = std::max(maxX, b.x1);
    }
    CHECK(minY == -4 * kMm);
    CHECK(maxY == 25 * kMm - 4'300'000);
    CHECK(minX == 62 * kMm + 4'300'000);
    CHECK(maxX == 95 * kMm - 4'300'000);
    // No warning about the strip, which is room to run out, not pocket.
    const Toolpath p = c.path(c.claims(0.3));
    CHECK(p.warnings.empty());
    c.clears = {clearClaim({rectPoly(62, 0, 95, 25)}, 14)};
    c.path(c.claims(0.3));
}

TEST_CASE("pocket2d finishing: roughing leaves the allowances, the floor is a depth of its own, the walls get one "
          "pass per depth with arc leads",
          "[cam][pocket2d]")
{
    // Pocket x 10..50, y 10..30 to Z 7 (8 deep), 6 mm tool, stepdown 4: roughing to 7.25 (floor allowance 0.25) in
    // two steps, then the floor at Z 7; the roughing room is 3 + 0.2 + 0.3 (stock, wall allowance) in from the walls,
    // the finishing passes 3 + 0.2.
    Case c = rectCase(10, 10, 50, 30, 7,
                      R"({"stepdown":4,"stockToLeaveWall":0.2,"finishWalls":true,"wallAllowance":0.3,)"
                      R"("finishFloor":true,"floorAllowance":0.25})");
    auto plan = c.plan();
    REQUIRE(plan);
    const std::vector<Nm> depths{11'125'000, 7'250'000, 7'000'000};
    CHECK(plan->pockets[0].depths == depths);
    std::size_t finishing = 0;
    for (const link::Pass& p : plan->passes) {
        const Box2 b = boxOf(*link::planarPath(p));
        if (p.leadIn.kind == link::LeadKind::Arc) {
            ++finishing;
            CHECK(b == mmBox(13.2, 13.2, 46.8, 26.8));
            CHECK(p.leadOut.kind == link::LeadKind::Arc);
            CHECK(p.leadIn.radius == 1'500'000); // r / 2
            CHECK(p.leadIn.side == g2::Side::Left);
        } else {
            CHECK(b.x0 >= toNm(13.5));
            CHECK(b.x1 <= toNm(46.5));
        }
    }
    CHECK(finishing == depths.size());
    // Finishing after all roughing, top down.
    int lastRough = -1, firstFinish = std::numeric_limits<int>::max();
    for (const link::Pass& p : plan->passes) {
        if (p.leadIn.kind == link::LeadKind::Arc) {
            firstFinish = std::min(firstFinish, p.level);
        } else {
            lastRough = std::max(lastRough, p.level);
        }
    }
    CHECK(lastRough < firstFinish);
    c.walls = {invariants::WallClaim{invariants::Region::of({rectPoly(10, 10, 50, 30)}), 7.0, 15.0, 0.05}};
    const Toolpath p = c.path(c.claims(0.2));
    CHECK(countRole(p, MoveRole::LeadIn) >= depths.size());
    CHECK(countRole(p, MoveRole::LeadOut) >= depths.size());
    CHECK(p.stats.minZ == 7.0);
}

TEST_CASE("pocket2d entries: helix, ramp, pre-drilled holes; never a plunge with an end mill", "[cam][pocket2d]")
{
    SECTION("helix")
    {
        const Case c = rectCase(10, 10, 50, 30, 9, R"({"stepdown":3})");
        const Toolpath p = c.path();
        CHECK(std::count_if(p.moves.begin(), p.moves.end(), helical) > 0);
        CHECK(countRole(p, MoveRole::Plunge) == 0);
    }
    SECTION("ramp")
    {
        const Case c = rectCase(10, 10, 50, 30, 9, R"({"stepdown":3,"entry":"ramp","rampAngle":3})");
        const Toolpath p = c.path(c.claims(0, 0, false, 3));
        CHECK(std::count_if(p.moves.begin(), p.moves.end(), helical) == 0);
        CHECK(countMoves(p, MoveRole::Ramp, MoveKind::Linear) > 0);
        CHECK(countRole(p, MoveRole::Plunge) == 0);
    }
    SECTION("a pocket too small for a helix is ramped into")
    {
        // 12 x 12 with a 6 mm tool: a 6 mm square room; the 5.4 mm helix and its smaller tries do not fit... the
        // helix shrinks to fit or the ramp takes over; either way no plunge.
        const Case c = rectCase(10, 10, 22, 22, 9, R"({"stepdown":3,"helixDiameter":200})");
        const Toolpath p = c.path();
        CHECK(countRole(p, MoveRole::Plunge) == 0);
    }
    SECTION("pre-drilled holes")
    {
        // Holes at (30, 20), the pocket's middle, 8 mm wide: each depth enters there by a feed straight down.
        const Case c = rectCase(10, 10, 50, 30, 9,
                                R"({"stepdown":3,"entry":"preDrill","preDrillPoints":"30 20","preDrillDiameter":8})");
        auto plan = c.plan();
        REQUIRE(plan);
        REQUIRE(plan->options.preDrills.size() == 1);
        CHECK(plan->options.preDrills[0].at == g2::Pt{30 * kMm, 20 * kMm});
        CHECK(plan->options.preDrills[0].bottom == 9 * kMm);
        CHECK(plan->options.entryOrder == std::vector<EntryKind>{EntryKind::PreDrill});
        invariants::Claims k = c.claims();
        // The checker knows nothing of earlier operations (README.md, "What the checker cannot see"), so it would see
        // the plunge into the drilled hole as one into stock; the test checks below that every plunge is at the hole.
        k.centreCutting = true;
        const Toolpath p = c.path(k);
        std::size_t plunges = 0;
        for (const Move& m : p.moves) {
            if (m.role == MoveRole::Plunge) {
                ++plunges;
                CHECK(m.x == 30 * kMm);
                CHECK(m.y == 20 * kMm);
            }
        }
        CHECK(plunges == plan->pockets[0].depths.size());
        CHECK(std::count_if(p.moves.begin(), p.moves.end(), helical) == 0);
    }
    SECTION("a pre-drilled hole in no pocket is reported and not used")
    {
        const Case c = rectCase(10, 10, 50, 30, 9,
                                R"({"stepdown":3,"preDrillPoints":"30 20; 55 5",)"
                                R"("preDrillDiameter":7})");
        auto warnings = c.check();
        REQUIRE(warnings);
        REQUIRE(warnings->size() == 1);
        CHECK_THAT(warnings->front(), ContainsSubstring("The pre-drilled hole at X55 Y5 lies in none of the pockets"));
    }
}

TEST_CASE("pocket2d narrow regions the tool cannot enter are reported, not skipped silently", "[cam][pocket2d]")
{
    // A pocket with a 4 mm wide arm (x 40..55, y 18..22) and a 6 mm tool.
    kernel::ProfileLoop loop;
    const std::vector<Vec2> pts{{10, 10}, {40, 10}, {40, 18}, {55, 18}, {55, 22}, {40, 22}, {40, 30}, {10, 30}};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        loop.curves.push_back(lineC(pts[i], pts[(i + 1) % pts.size()]));
    }
    const kernel::Shape part = cut(box(Vec3{0, 0, 0}, Vec3{60, 40, 15}), prism({loop}, 10, 16));
    Case c;
    c.op = pocketOp(R"({"stepdown":5})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 10)});
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->pockets[0].narrow.size() == 1);
    const g2::Bounds nb = g2::bounds(plan->pockets[0].narrow[0]);
    CHECK(nb.max.x == 55 * kMm);
    CHECK(nb.min.x > 40 * kMm);
    auto warnings = c.check();
    REQUIRE(warnings);
    REQUIRE(warnings->size() == 1);
    CHECK_THAT(warnings->front(), ContainsSubstring("is narrower than the 6 mm flat end mill can reach into"));
    const Toolpath p = c.path();
    CHECK(p.warnings == *warnings);
    // The rectangle itself is cleared.
    c.clears = {clearClaim({rectPoly(10, 10, 40, 30)}, 10)};
    c.path();
}

TEST_CASE("pocket2d bull nose: the flat clears the floor; a stepover wider than the flat is refused", "[cam][pocket2d]")
{
    Case c = rectCase(10, 10, 50, 30, 9, R"({"stepdown":3,"stepover":60})");
    c.tool = endMill(8, 25, 1); // flat 2 x (4 - 1) = 6 mm wide; 60 % is 4.8 mm
    // The checker sees the corner radius standing at the walls: the floor is clear to that height there.
    c.clears = {clearClaim({rectPoly(10, 10, 50, 30)}, 9, 1.01)};
    c.path();
    c.op.paramsJson = R"({"stepdown":3,"stepover":80})"; // 6.4 mm
    refused(c, ErrorCode::InvalidArgument, "is not less than the width of the bull nose's flat (6 mm)");
    c.tool = endMill(8, 25, 4);
    refused(c, ErrorCode::InvalidArgument, "is not smaller than its radius");
}

TEST_CASE("pocket2d refusals name the cause", "[cam][pocket2d]")
{
    const Case good = rectCase(10, 10, 50, 30, 9, R"({"stepdown":3})");
    REQUIRE(good.validate());
    SECTION("the tool")
    {
        Case c = good;
        c.tool.type = lib::ToolType::Drill;
        refused(c, ErrorCode::InvalidArgument, "A 2D pocket needs a flat or bull nose end mill; the tool is a drill");
        c = good;
        c.tool.diameter = 0;
        refused(c, ErrorCode::InvalidArgument, "The tool has no usable diameter");
        c = good;
        c.tool.fluteLength = 2;
        refused(c, ErrorCode::InvalidArgument, "The stepdown (3 mm) is longer than the tool's flutes (2 mm)");
        c = good;
        c.tool.shankDiameter = 8;
        c.tool.shoulderLength = 4;
        refused(c, ErrorCode::InvalidArgument, "shank is wider than its cut");
        c = good;
        c.op.tool.stickOut = 6;
        refused(c, ErrorCode::InvalidArgument, "it sticks out only 6 mm");
    }
    SECTION("the spindle and feeds")
    {
        Case c = good;
        c.op.cutting.clockwise = false;
        refused(c, ErrorCode::InvalidArgument, "counter-clockwise (M4)");
        c = good;
        c.op.cutting.spindleRpm = 20000;
        refused(c, ErrorCode::InvalidArgument, "above the machine's maximum of 12000 rpm");
        c = good;
        c.op.cutting.feedCut = 0;
        refused(c, ErrorCode::InvalidArgument, "Set a cutting feed");
    }
    SECTION("heights out of order")
    {
        Case c = good;
        c.op.heights.clearance = 3;
        refused(c, ErrorCode::InvalidArgument, "clearance height (3) must not be below the retract height (5)");
        c = good;
        c.op.heights.feedPlane = 6;
        refused(c, ErrorCode::InvalidArgument, "retract height (5) must not be below the feed plane (6)");
        c = good;
        c.op.heights.top = 3;
        refused(c, ErrorCode::InvalidArgument, "feed plane (2) must not be below the top height (3)");
        c = good;
        c.op.heights.retract = -1;
        c.op.heights.feedPlane = -2;
        c.op.heights.top = -3;
        refused(c, ErrorCode::InvalidArgument, "must not be below the top of the stock");
        c = good;
        c.op.heights.top = -7;
        refused(c, ErrorCode::InvalidArgument, "The top height (Z8) is not above the bottom of the pocket");
    }
    SECTION("the geometry")
    {
        Case c = good;
        c.geom.selected.clear();
        c.geom.selectedShapeIndex.clear();
        refused(c, ErrorCode::InvalidArgument, "Pick the floor of each pocket");
        c = good;
        c.geom.selected = {kernel::SubId{kernel::SubKind::Edge, 0}};
        refused(c, ErrorCode::InvalidArgument, "is not a face");
        // A wall (a face along Z) and the block's bottom face (looking down).
        const kernel::Shape& part = good.geom.partShapes[0];
        kernel::SubId wall{}, bottom{};
        for (std::uint32_t i = 0; i < part.count(kernel::SubKind::Face); ++i) {
            const kernel::SubId f{kernel::SubKind::Face, i};
            const auto s = kernel::faceSample(part, f);
            REQUIRE(s);
            if (std::abs(s->normal.z) < 1e-9) {
                wall = f;
            }
            if (s->normal.z < -0.999) {
                bottom = f;
            }
        }
        c = good;
        c.geom.selected = {wall};
        refused(c, ErrorCode::InvalidArgument, "is not a flat face square to the setup's Z axis");
        c = good;
        c.geom.selected = {bottom};
        refused(c, ErrorCode::InvalidArgument, "looks down the setup's Z axis");
    }
    SECTION("the tool does not fit the pocket")
    {
        const Case c = rectCase(10, 10, 15, 15, 9, R"({"stepdown":3})");
        refused(c, ErrorCode::ToolpathFailed, "The 6 mm flat end mill does not fit the pocket with its floor at Z9");
        // With the stock to leave and the allowance the room is gone too, and the message says so.
        const Case tight = rectCase(10, 10, 16.5, 16.5, 9,
                                    R"({"stepdown":3,"stockToLeaveWall":0.2,)"
                                    R"("finishWalls":true,"wallAllowance":0.2})");
        refused(tight, ErrorCode::ToolpathFailed, "the stock to leave and the finishing allowance");
    }
    SECTION("no entry fits: refused by generation (and by check), naming why each kind did not fit")
    {
        // 7 x 7 with a 6 mm tool: the room is a 1 mm square; the smallest helix (1.8 mm) does not fit and the ring
        // (4 mm round) is shorter than a ramp's run (the tool diameter).
        const Case c = rectCase(10, 10, 17, 17, 9, R"({"stepdown":3})");
        refused(c, ErrorCode::ToolpathFailed, "cannot be entered", false);
        auto g = c.generate();
        CHECK_THAT(g.error().message, ContainsSubstring("helix: a helix of 5.4 mm down to 1.8 mm does not fit"));
        CHECK_THAT(g.error().message, ContainsSubstring("ramp: the closed pass is 4 mm long"));
        // Pre-drilled holes only, none near enough: refused the same way.
        Case drilled = rectCase(10, 10, 50, 30, 9,
                                R"({"stepdown":3,"entry":"preDrill","preDrillPoints":"12 12",)"
                                R"("preDrillDiameter":8})");
        refused(drilled, ErrorCode::ToolpathFailed, "no pre-drilled point", false);
    }
    SECTION("parameters")
    {
        Case c = good;
        c.op.paramsJson = R"({"stepover":99})";
        refused(c, ErrorCode::InvalidArgument, "Stepover");
        c.op.paramsJson = R"({"entry":"preDrill"})";
        refused(c, ErrorCode::InvalidArgument, "none are given");
        c.op.paramsJson = R"({"preDrillPoints":"30 20"})";
        refused(c, ErrorCode::InvalidArgument, "narrower than the 6 mm flat end mill");
        c.op.paramsJson = R"({"preDrillPoints":"30 x","preDrillDiameter":8})";
        refused(c, ErrorCode::InvalidArgument, "point 1 has no second number");
        c.op.paramsJson = R"({"preDrillPoints":"30 20 40","preDrillDiameter":8})";
        refused(c, ErrorCode::InvalidArgument, "after point 1 comes something other than ';'");
    }
}

TEST_CASE("pocket2d point lists", "[cam][pocket2d]")
{
    auto a = ops::parsePointList(" 20 15; 60.5,-15 ;7 8");
    REQUIRE(a);
    REQUIRE(a->size() == 3);
    CHECK(((*a)[1].x == 60.5 && (*a)[1].y == -15));
    auto empty = ops::parsePointList("  ");
    REQUIRE(empty);
    CHECK(empty->empty());
    CHECK_FALSE(ops::parsePointList("1"));
    CHECK_FALSE(ops::parsePointList("a b"));
}

TEST_CASE("pocket2d generation is deterministic and can be cancelled", "[cam][pocket2d]")
{
    Case c;
    c.op = pocketOp(R"({"stepdown":4,"finishWalls":true})");
    c.tool = endMill(6);
    c.geom = geometryOn(block(), {upFace(block(), 12), upFace(block(), 14)});
    auto first = c.generate();
    REQUIRE(first);
    for (int i = 0; i < 2; ++i) {
        auto again = c.generate();
        REQUIRE(again);
        CHECK(again->serialize() == first->serialize());
    }
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), {}};
    auto cancelled = ops::makePocket2dOperationType()->generate(c.input(), ctx);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
}

TEST_CASE("pocket2d rings end where the room does: a small rounded room shrunk past its middle", "[cam][pocket2d]")
{
    // Seed 255839108 of the invariant property test: geom2d::offset returned a region (growing with every ring) for
    // this room shrunk past its inradius, so the rings never ended. pocket2d takes such a result as empty.
    kernel::Shape s = box(Vec3{0, 0, -14}, Vec3{88, 50, 0});
    s = cut(s, prism({roundedRect(37, 4.9, 51.4, 17.5, 2.8)}, -6.9, 1));
    s = cut(s, prism({roundedRect(66.3, 4.1, 82.3, 20.9, 3.4)}, -6.1, 1));
    Case c;
    c.op = pocketOp(R"({"finishFloor":true,"finishWalls":true,"floorAllowance":0.302,"helixDiameter":70.582,)"
                    R"("rampAngle":3.709,"stepdown":2.272,"stepover":80.499,"stockToLeaveFloor":0.041,)"
                    R"("stockToLeaveWall":0.23,"wallAllowance":0.224})",
                    6093, 1837);
    c.op.heights.clearance = 20;
    c.op.heights.feedPlane = 1.3;
    c.tool = endMill(4, 12.1);
    c.geom = geometryOn(s, {upFace(s, -6.9), upFace(s, -6.1)});
    c.geom.stockBounds.max.z = 0.6;
    auto plan = c.plan();
    REQUIRE(plan);
    for (const ops::PocketReport& pk : plan->pockets) {
        for (const ops::PocketCoverage& cv : pk.coverage) {
            CHECK(cv.rings <= 3);
        }
    }
    c.clears = {clearClaim({rectPoly(37, 4.9, 51.4, 17.5, 2.8)}, -6.9, 0.01),
                clearClaim({rectPoly(66.3, 4.1, 82.3, 20.9, 3.4)}, -6.1, 0.01)};
    c.path(c.claims(0.23, 0.041, false, 3.709));
}
