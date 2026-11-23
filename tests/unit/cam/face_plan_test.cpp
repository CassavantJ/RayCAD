// The facing operation's rules (task T-052, face.h) against hand-computed values: the pass heights, the stroke
// positions and their coverage, the plan of the golden block, how linking joins the strokes (zigzag staying down beside
// the stock, one way retracting, every entry a feed descent in the air), the stroke direction for climb and
// conventional along X and along Y, and every refusal, each naming its cause, the same from validate and generate.
#include "face_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = faceBlock();
    return s;
}

// The golden block's setup: stock 2 mm wider on every side and 1.5 mm above the part (X -2..82, Y -2..52, top 21.5).
FaceCase onBlock(const std::string& params, lib::Tool t = millTool(lib::ToolType::FlatEndMill, 10))
{
    FaceCase c;
    c.op = faceOp(params);
    c.tool = std::move(t);
    c.geom = stockAround(block(), 2, 1.5);
    return c;
}

// The refusal of a case: validate and generate give the same error; returns its message.
std::string refusal(const FaceCase& c, ErrorCode code = ErrorCode::InvalidArgument)
{
    const Status v = c.validate();
    REQUIRE_FALSE(v);
    const auto g = c.generate();
    REQUIRE_FALSE(g);
    CHECK(v.error().code == code);
    CHECK(g.error().code == code);
    CHECK(v.error().message == g.error().message);
    return v.error().message;
}

// The index of every move of role Cut.
std::vector<std::size_t> cutMoves(const Toolpath& p)
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < p.moves.size(); ++i) {
        if (p.moves[i].role == MoveRole::Cut) {
            out.push_back(i);
        }
    }
    return out;
}

// The tool's position before move i.
std::array<Nm, 3> before(const Toolpath& p, std::size_t i)
{
    if (i == 0) {
        return {p.startX, p.startY, p.startZ};
    }
    return {p.moves[i - 1].x, p.moves[i - 1].y, p.moves[i - 1].z};
}

} // namespace

TEST_CASE("face: pass heights are equal steps of at most the stepdown, the last at the bottom", "[cam][face]")
{
    // 10 mm in steps of at most 3: ceil(10 / 3) = 4 passes of 2.5 mm.
    CHECK(ops::faceLevels(10 * kMm, 0, 3 * kMm) == std::vector<Nm>{7'500'000, 5'000'000, 2'500'000, 0});
    // Exactly one step: one pass at the bottom.
    CHECK(ops::faceLevels(21'500'000, 20'000'000, 1'500'000) == std::vector<Nm>{20'000'000});
    CHECK(ops::faceLevels(21'500'000, 20'000'000, 2'000'000) == std::vector<Nm>{20'000'000});
    // 1.5 mm in steps of 0.6: 3 passes of 0.5.
    CHECK(ops::faceLevels(21'500'000, 20'000'000, 600'000) == std::vector<Nm>{21'000'000, 20'500'000, 20'000'000});
    // Uneven nanometres: 1000003 nm in 2 passes, floor(1000003 / 2) = 500001 off the top first.
    CHECK(ops::faceLevels(1'000'003, 0, 1'000'000) == std::vector<Nm>{500'002, 0});
    // Nothing to face, or no step: none.
    CHECK(ops::faceLevels(0, 0, kMm).empty());
    CHECK(ops::faceLevels(kMm, 0, 0).empty());
}

TEST_CASE("face: stroke positions cover the range with flat bands at most the stepover apart", "[cam][face]")
{
    // The golden block across Y: [-2 - 2, 52 + 2] = [-4, 54], a 10 mm flat (F = 5), stepover 7.5:
    // first 1, last 49, L = 48, ceil(48 / 7.5) = 7 gaps, v_j = 1 + floor(j 48 / 7) mm.
    const std::vector<Nm> v = ops::strokePositions(-4 * kMm, 54 * kMm, 5 * kMm, 7'500'000);
    REQUIRE(v.size() == 8);
    CHECK(v[0] == kMm);
    CHECK(v[1] == kMm + 6'857'142); // 48e6 / 7 = 6857142.857..
    CHECK(v[2] == kMm + 13'714'285);
    CHECK(v[3] == kMm + 20'571'428);
    CHECK(v[7] == 49 * kMm);
    // Spacing exactly the flat width: the bands touch.
    CHECK(ops::strokePositions(0, 30 * kMm, 5 * kMm, 10 * kMm) == std::vector<Nm>{5 * kMm, 15 * kMm, 25 * kMm});
    // A range no wider than the flat: one stroke in its middle.
    CHECK(ops::strokePositions(0, 9 * kMm, 5 * kMm, 7 * kMm) == std::vector<Nm>{4'500'000});
    CHECK(ops::strokePositions(0, 10 * kMm, 5 * kMm, 7 * kMm) == std::vector<Nm>{5 * kMm});
    CHECK(ops::strokePositions(0, 1, 5 * kMm, 7 * kMm) == std::vector<Nm>{0});

    // Seeded cases: the first band reaches lo, the last hi, neighbours at most the spacing apart (so the bands, 2F
    // wide, overlap or touch), and no more strokes than needed.
    std::uint64_t s = 52;
    const auto next = [&s](std::int64_t lo, std::int64_t hi) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return lo + static_cast<std::int64_t>((s >> 33) % static_cast<std::uint64_t>(hi - lo + 1));
    };
    for (int i = 0; i < 10000; ++i) {
        const Nm lo = next(-500 * kMm, 500 * kMm);
        const Nm hi = lo + next(1, 200 * kMm);
        const Nm flat = next(kMm, 30 * kMm);
        const Nm spacing = next(flat / 4, 2 * flat);
        const auto p = ops::strokePositions(lo, hi, flat, spacing);
        INFO("lo " << lo << " hi " << hi << " F " << flat << " spacing " << spacing);
        REQUIRE_FALSE(p.empty());
        CHECK(p.front() - flat <= lo);
        CHECK(p.back() + flat >= hi);
        for (std::size_t j = 1; j < p.size(); ++j) {
            CHECK(p[j] > p[j - 1]);
            CHECK(p[j] - p[j - 1] <= spacing);
        }
        if (p.size() > 2) { // one fewer gap would be too wide
            const Nm length = (hi - flat) - (lo + flat);
            CHECK(length > spacing * static_cast<Nm>(p.size() - 2));
        }
    }
}

TEST_CASE("face: the plan of the golden block", "[cam][face]")
{
    const ops::FacePlan plan = onBlock(R"({"stepdown":0.6})").okPlan();
    // Stock top 21.5, part top 20 (the block's top), faced to 20 + 0 in three passes of 0.5.
    CHECK(plan.stockTop == 21'500'000);
    CHECK(plan.partTop == 20'000'000);
    CHECK(plan.top == 21'500'000);
    CHECK(plan.bottom == 20'000'000);
    CHECK(plan.levels == std::vector<Nm>{21'000'000, 20'500'000, 20'000'000});
    // The 10 mm flat end mill: R 5, F 5, the stepover by default 3/4 of the 10 mm flat, 7.5; overhang 2.
    CHECK(plan.toolRadius == 5 * kMm);
    CHECK(plan.flatHalf == 5 * kMm);
    CHECK(plan.stepover == 7'500'000);
    CHECK(plan.overhang == 2 * kMm);
    // Along X from -2 - 5 - 2 = -9 to 82 + 5 + 2 = 89; across at the eight positions of the stroke-position test.
    REQUIRE(plan.strokesPerLevel == 8);
    REQUIRE(plan.strokes.size() == 24);
    const std::vector<Nm> v = ops::strokePositions(-4 * kMm, 54 * kMm, 5 * kMm, 7'500'000);
    for (std::size_t j = 0; j < 8; ++j) {
        const ops::FaceStroke& s = plan.strokes[j];
        INFO("stroke " << j);
        CHECK(s.z == 21'000'000);
        CHECK(s.from.y == v[j]);
        CHECK(s.to.y == v[j]);
        // Zigzag: climb (+X, the stock on the left at +Y) first, then alternating.
        CHECK(s.from.x == (j % 2 == 0 ? -9 * kMm : 89 * kMm));
        CHECK(s.to.x == (j % 2 == 0 ? 89 * kMm : -9 * kMm));
    }
    // The second level runs the first backwards: it starts where the first ended.
    for (std::size_t j = 0; j < 8; ++j) {
        const ops::FaceStroke& a = plan.strokes[7 - j];
        const ops::FaceStroke& b = plan.strokes[8 + j];
        CHECK(b.from == a.to);
        CHECK(b.to == a.from);
        CHECK(b.z == 20'500'000);
        CHECK(plan.strokes[16 + j].from == plan.strokes[j].from); // the third as the first
    }
    // Linking: absolute heights from the stock top, in the given order, staying down for a stepover.
    CHECK(plan.link.clearance == 36'500'000);
    CHECK(plan.link.retract == 26'500'000);
    CHECK(plan.link.feedPlane == 23'500'000);
    CHECK(plan.link.keepOrder);
    CHECK(plan.link.maxStayDown == 7'500'000);
    CHECK(plan.link.toolRadius == 5 * kMm);
    CHECK(plan.link.feeds.plunge == 400'000);
    CHECK(plan.feed == 1'200'000);
    CHECK(plan.warnings.empty());
    JobContext ctx;
    auto uncovered = ops::faceUncovered(plan, ctx);
    REQUIRE(uncovered);
    CHECK(*uncovered == 0.0);
}

TEST_CASE("face: zigzag stays down beside the stock between strokes and levels", "[cam][face][link]")
{
    const FaceCase c = onBlock(R"({"stepdown":0.6})");
    const Toolpath p = c.path();
    const ops::FacePlan plan = c.okPlan();
    const std::vector<std::size_t> cuts = cutMoves(p);
    REQUIRE(cuts.size() == plan.strokes.size());
    // One approach: a rapid at the clearance above the first stroke's start, a rapid down to the feed plane, a feed
    // down in the air to the first level.
    CHECK(p.startX == -9 * kMm);
    CHECK(p.startY == kMm);
    CHECK(p.startZ == 36'500'000);
    REQUIRE(cuts.front() == 3);
    CHECK(p.moves[0].kind == MoveKind::Rapid);
    CHECK(p.moves[1].kind == MoveKind::Rapid);
    CHECK(p.moves[1].z == 23'500'000);
    CHECK(p.moves[2].kind == MoveKind::Linear);
    CHECK(p.moves[2].z == 21'000'000);
    CHECK(p.moves[2].feedMicronPerMin == 400'000);
    // From the first cut to the last: no rapid; every move between two strokes is a feed link outside the stock.
    for (std::size_t i = cuts.front(); i <= cuts.back(); ++i) {
        const Move& m = p.moves[i];
        INFO("move " << i);
        CHECK(m.kind == MoveKind::Linear);
        if (m.role != MoveRole::Cut) {
            CHECK(m.role == MoveRole::Link);
            CHECK(m.feedMicronPerMin == 1'200'000);
            CHECK((m.x <= -7 * kMm || m.x >= 87 * kMm)); // the tool (radius 5) at least 2 mm clear of the stock
        }
    }
    // Then out: feed up to the feed plane, rapid to the clearance.
    CHECK(p.moves.back().kind == MoveKind::Rapid);
    CHECK(p.moves.back().z == 36'500'000);
    CHECK(p.stats.minZ == 20.0);
    CHECK(p.stats.maxZ == 36.5);
}

TEST_CASE("face: one way retracts between strokes and enters each in the air", "[cam][face][link]")
{
    const FaceCase c = onBlock(R"({"pattern":"oneWay","stepdown":0.6})");
    const Toolpath p = c.path();
    const ops::FacePlan plan = c.okPlan();
    const std::vector<std::size_t> cuts = cutMoves(p);
    REQUIRE(cuts.size() == plan.strokes.size());
    for (std::size_t k = 0; k < cuts.size(); ++k) {
        const Move& m = p.moves[cuts[k]];
        const auto from = before(p, cuts[k]);
        INFO("stroke " << k);
        // Every stroke climb: +X.
        CHECK(from[0] == -9 * kMm);
        CHECK(m.x == 89 * kMm);
        // Entered by a feed descent at the plunge feed beside the stock, from the feed plane, after a rapid there.
        const Move& down = p.moves[cuts[k] - 1];
        CHECK(down.kind == MoveKind::Linear);
        CHECK(down.role == MoveRole::Approach);
        CHECK(down.feedMicronPerMin == 400'000);
        CHECK(before(p, cuts[k] - 1)[2] == 23'500'000);
        CHECK(p.moves[cuts[k] - 2].kind == MoveKind::Rapid);
        if (k > 0) {
            // The way back at the retract plane: feed up to the feed plane, rapid up, rapid across.
            bool across = false;
            for (std::size_t i = cuts[k - 1] + 1; i < cuts[k]; ++i) {
                across = across ||
                         (p.moves[i].kind == MoveKind::Rapid && p.moves[i].z == 26'500'000 && p.moves[i].x == -9 * kMm);
            }
            CHECK(across);
        }
    }
}

TEST_CASE("face: every stroke starts and ends with the tool clear of the stock", "[cam][face]")
{
    for (const char* params :
         {R"({"stepdown":0.6})", R"({"pattern":"oneWay","direction":"alongY"})", R"({"overhang":0,"stepover":10})"}) {
        INFO(params);
        const FaceCase c = onBlock(params);
        const ops::FacePlan plan = c.okPlan();
        const Box3& s = c.geom.stockBounds;
        const Nm clear = plan.toolRadius + plan.overhang;
        for (const ops::FaceStroke& k : plan.strokes) {
            for (const geom2d::Pt& q : {k.from, k.to}) {
                const bool outX = q.x <= toNm(s.min.x) - clear || q.x >= toNm(s.max.x) + clear;
                const bool outY = q.y <= toNm(s.min.y) - clear || q.y >= toNm(s.max.y) + clear;
                CHECK((outX || outY));
            }
        }
        c.path();
    }
}

TEST_CASE("face: climb and conventional along X and along Y", "[cam][face]")
{
    // With the spindle clockwise a stroke is climb with the uncut stock (higher v) on its left: +X along X, -Y along Y.
    const auto first = [](const std::string& params) {
        return onBlock(params).okPlan().strokes.front();
    };
    const ops::FaceStroke x = first(R"({"pattern":"oneWay"})");
    CHECK(x.to.x > x.from.x);
    const ops::FaceStroke xc = first(R"({"pattern":"oneWay","cutMode":"conventional"})");
    CHECK(xc.to.x < xc.from.x);
    const ops::FaceStroke y = first(R"({"pattern":"oneWay","direction":"alongY"})");
    CHECK(y.to.y < y.from.y);
    CHECK(y.from.x == 1 * kMm); // the first stroke at the low X side: the stock (X -2..82) grown by 2, plus F
    const ops::FaceStroke yc = first(R"({"pattern":"oneWay","direction":"alongY","cutMode":"conventional"})");
    CHECK(yc.to.y > yc.from.y);
    // Along Y: from 52 + 5 + 2 = 59 down to -9.
    CHECK(y.from.y == 59 * kMm);
    CHECK(y.to.y == -9 * kMm);
    // Zigzag starts climb whatever the cut mode says (it does not apply).
    const ops::FaceStroke z = first(R"({"direction":"alongY","cutMode":"conventional"})");
    CHECK(z.to.y < z.from.y);
}

TEST_CASE("face: a bull nose covers with its flat part; the stepover may equal it", "[cam][face]")
{
    // 12 mm bull nose, corner 1.5: flat 9 (F 4.5). Stepover 9 is allowed and still covers.
    const FaceCase c = onBlock(R"({"stepover":9})", millTool(lib::ToolType::BullEndMill, 12, 1.5));
    const ops::FacePlan plan = c.okPlan();
    CHECK(plan.flatHalf == 4'500'000);
    CHECK(plan.stepover == 9 * kMm);
    JobContext ctx;
    CHECK(*ops::faceUncovered(plan, ctx) == 0.0);
    c.path();
    // Default stepover: 3/4 of 9 = 6.75.
    CHECK(onBlock("{}", millTool(lib::ToolType::BullEndMill, 12, 1.5)).okPlan().stepover == 6'750'000);
}

TEST_CASE("face: the top height above the stock warns that passes cut air", "[cam][face]")
{
    FaceCase c = onBlock(R"({"stepdown":1})");
    c.op.heights.top = 1; // T = 22.5, B = 20: 3 passes, at 22.5 - 0.833333 (above the stock top 21.5), 20.833334, 20
    const ops::FacePlan plan = c.okPlan();
    REQUIRE(plan.levels.size() == 3);
    CHECK(plan.levels.front() == 22'500'000 - 833'333);
    auto warnings = ops::makeFaceOperationType()->check(c.input());
    REQUIRE(warnings);
    REQUIRE(warnings->size() == 1);
    CHECK_THAT(warnings->front(), ContainsSubstring("the first 1 pass cuts only air"));
    const Toolpath p = c.path();
    CHECK(p.warnings == *warnings);
}

TEST_CASE("face: refusals name their cause", "[cam][face][validate]")
{
    SECTION("a tool that is not a facing tool")
    {
        CHECK_THAT(refusal(onBlock("{}", millTool(lib::ToolType::Drill, 10))),
                   ContainsSubstring("Facing needs a face mill, a flat end mill or a bull nose end mill; the tool is a "
                                     "drill"));
        CHECK_THAT(refusal(onBlock("{}", millTool(lib::ToolType::BallEndMill, 10, 5))),
                   ContainsSubstring("the tool is a ball end mill"));
    }
    SECTION("a bull nose without a flat bottom")
    {
        CHECK_THAT(refusal(onBlock("{}", millTool(lib::ToolType::BullEndMill, 6, 3))),
                   ContainsSubstring("The 6 mm bull nose end mill has no flat bottom (its corner radius is 3 mm)"));
    }
    SECTION("a stepover larger than the tool")
    {
        CHECK_THAT(refusal(onBlock(R"({"stepover":10.5})")),
                   ContainsSubstring("The stepover (10.5 mm) is larger than the flat part of the 10 mm flat end mill, "
                                     "10 mm: the strokes would leave ridges between them"));
        CHECK_THAT(refusal(onBlock(R"({"stepover":10})", millTool(lib::ToolType::BullEndMill, 12, 1.5))),
                   ContainsSubstring("larger than the flat part of the 12 mm bull nose end mill, 9 mm (its diameter "
                                     "less two 1.5 mm corner radii)"));
    }
    SECTION("the spindle and the feed")
    {
        FaceCase c = onBlock("{}");
        c.op.cutting.clockwise = false;
        CHECK_THAT(refusal(c), ContainsSubstring("counter-clockwise (M4)"));
        c = onBlock("{}");
        c.op.cutting.spindleRpm = 0;
        CHECK_THAT(refusal(c), ContainsSubstring("Set a spindle speed"));
        c = onBlock("{}");
        c.op.cutting.spindleRpm = 15000;
        CHECK_THAT(refusal(c), ContainsSubstring("above the machine's maximum of 12000 rpm"));
        c = onBlock("{}");
        c.op.cutting.feedCut = 0;
        CHECK_THAT(refusal(c), ContainsSubstring("Set a cutting feed"));
    }
    SECTION("heights out of order")
    {
        FaceCase c = onBlock("{}");
        c.op.heights.clearance = 4;
        CHECK_THAT(refusal(c), ContainsSubstring("The clearance height (4) must not be below the retract height (5)"));
        c = onBlock("{}");
        c.op.heights.feedPlane = 6;
        CHECK_THAT(refusal(c), ContainsSubstring("The retract height (5) must not be below the feed plane (6)"));
        c = onBlock("{}");
        c.op.heights.top = 3;
        CHECK_THAT(refusal(c), ContainsSubstring("The feed plane (2) must not be below the top height (3)"));
        c = onBlock("{}");
        c.op.heights.top = -1;
        c.op.heights.feedPlane = -0.5;
        c.op.heights.retract = -0.2;
        CHECK_THAT(refusal(c), ContainsSubstring("is below the stock top: the tool would rapid through the stock"));
    }
    SECTION("a bottom below the part top")
    {
        FaceCase c = onBlock("{}");
        c.op.heights.bottom = -0.5;
        CHECK_THAT(refusal(c), ContainsSubstring("The bottom height (-0.5) is below the top of the part (Z20): facing "
                                                 "would cut into the part"));
    }
    SECTION("nothing to face")
    {
        FaceCase c = onBlock("{}");
        c.geom.stockBounds.max.z = 20; // the stock top at the part top
        CHECK_THAT(refusal(c),
                   ContainsSubstring("Nothing to face: the bottom height ends at Z20 (the part top Z20 + 0), "
                                     "not below the top height at Z20 (the stock top Z20 + 0)"));
        c = onBlock("{}");
        c.op.heights.bottom = 1.5;
        CHECK_THAT(refusal(c), ContainsSubstring("Nothing to face"));
    }
    SECTION("no stock")
    {
        FaceCase c = onBlock("{}");
        c.geom.stockBounds = Box3{};
        CHECK_THAT(refusal(c), ContainsSubstring("The setup has no stock to face"));
    }
    SECTION("a fixture in the swept area, and the clearance below a fixture")
    {
        FaceCase c = onBlock("{}");
        c.geom.fixtureShapes = {faceBox(Vec3{84, 10, 10}, Vec3{95, 40, 21})}; // a jaw beside the stock, up to Z 21
        CHECK_THAT(refusal(c, ErrorCode::ToolpathFailed),
                   ContainsSubstring("A fixture reaches Z21 inside the area the tool sweeps"));
        // Below the faced height it is fine.
        c.geom.fixtureShapes = {faceBox(Vec3{84, 10, 10}, Vec3{95, 40, 19.5})};
        c.path();
        // Outside the swept area (with overhang 0 the tool reaches X 82 + 5 + 5 = 92), but above the clearance.
        c.geom.fixtureShapes = {faceBox(Vec3{93, 10, 10}, Vec3{95, 40, 50})};
        c.op.paramsJson = R"({"overhang":0})";
        CHECK_THAT(refusal(c), ContainsSubstring("The clearance height ends at Z36.5, below the top of the stock, the "
                                                 "part or a fixture (Z50)"));
    }
    SECTION("passes deeper than the flutes or the stick-out")
    {
        CHECK_THAT(
            refusal(onBlock(R"({"stepdown":2})", millTool(lib::ToolType::FlatEndMill, 10, 0, 1))),
            ContainsSubstring("A pass cuts 1.5 mm deep, but the flutes of the 10 mm flat end mill are 1 mm long"));
        FaceCase c = onBlock("{}");
        c.op.tool.stickOut = 2;
        CHECK_THAT(refusal(c), ContainsSubstring("the tool sticks out only 2 mm: the holder needs 1 mm"));
        // The top below the stock top: the first pass still cuts from the stock top.
        c = onBlock(R"({"stepdown":0.5})", millTool(lib::ToolType::FlatEndMill, 10, 0, 0.8));
        c.op.heights.top = -0.5;
        c.op.heights.feedPlane = 0;
        CHECK_THAT(refusal(c), ContainsSubstring("A pass cuts 1 mm deep"));
    }
    SECTION("too many strokes")
    {
        CHECK_THAT(refusal(onBlock(R"({"stepover":0.001,"stepdown":0.001})")),
                   ContainsSubstring("The facing would take more than 100000 strokes"));
    }
    SECTION("beyond the machine's travel")
    {
        FaceCase c = onBlock("{}");
        c.machine.axes[1].maxTravel = 40; // the centre goes from Y 1 to Y 49
        CHECK_THAT(refusal(c, ErrorCode::ToolpathFailed),
                   ContainsSubstring("The tool's centre travels 48 mm in Y; the machine's Y travel is 40 mm"));
    }
    SECTION("parameters the schema does not allow")
    {
        CHECK_THAT(refusal(onBlock(R"({"stepdown":0})")), ContainsSubstring("Maximum stepdown"));
        CHECK_THAT(refusal(onBlock(R"({"pattern":"spiral"})")), ContainsSubstring("Pattern"));
        CHECK_THAT(refusal(onBlock(R"({"angle":45})")), ContainsSubstring("angle"));
    }
}

TEST_CASE("face: a setup without part bodies faces from the stock top", "[cam][face]")
{
    FaceCase c = onBlock("{}");
    c.geom.partShapes.clear();
    c.op.heights.bottom = -1;
    const ops::FacePlan plan = c.okPlan();
    CHECK(plan.partTop == plan.stockTop);
    CHECK(plan.bottom == 20'500'000);
    CHECK(plan.levels == std::vector<Nm>{20'500'000});
}

TEST_CASE("face: generation can be cancelled", "[cam][face]")
{
    const FaceCase c = onBlock(R"({"stepover":1,"stepdown":0.1})");
    CancelSource source;
    JobContext ctx;
    ctx.cancel = source.token();
    source.cancel();
    auto p = ops::makeFaceOperationType()->generate(c.input(), ctx);
    REQUIRE_FALSE(p);
    CHECK(p.error().code == ErrorCode::Cancelled);
}
