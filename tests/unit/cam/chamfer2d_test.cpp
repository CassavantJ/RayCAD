// The chamfer operation (task T-055): the chamfer's depth, the tool's axis offset and tip depth against hand-computed
// values, the centre paths beside an outside edge, a pocket edge, a hole edge and an open edge, the cut direction, the
// collision proof, each refusal on its own (validate and generate refusing with the same words), and the invariant
// checker on every toolpath, with the claim that cutting the chamfer's wedge is intended (and without it: a gouge).
#include "slot_chamfer_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = chamferBlock();
    return s;
}

// The four top edges of the block's outside.
std::vector<kernel::SubId> outside()
{
    std::vector<kernel::SubId> out;
    for (const auto& side : {std::array<double, 4>{0, 0, 80, 0}, std::array<double, 4>{80, 0, 80, 50},
                             std::array<double, 4>{0, 50, 80, 50}, std::array<double, 4>{0, 0, 0, 50}}) {
        const auto e = edgesAt(block(), 20, side[0], side[1], side[2], side[3]);
        out.insert(out.end(), e.begin(), e.end());
    }
    REQUIRE(out.size() == 4);
    return out;
}
std::vector<kernel::SubId> pocketEdges()
{
    return edgesAt(block(), 20, 10, 10, 40, 40);
}
std::vector<kernel::SubId> holeEdges()
{
    return edgesAt(block(), 20, 54.9, 9.9, 65.1, 20.1);
}
std::vector<kernel::SubId> shallowEdges()
{
    return edgesAt(block(), 20, 50, 30, 70, 42);
}

Case chamferCase(const std::vector<kernel::SubId>& edges, lib::Tool t, const std::string& params = "{}",
                 const kernel::Shape& part = block())
{
    Case c;
    c.op = millOp("chamfer2d", params, 8000, 800);
    c.tool = std::move(t);
    c.geom = geometryOn(part, edges);
    return c;
}

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

// Signed area (mm^2, counter-clockwise positive) of the polygon through the end points of the cut moves.
double cutArea(const Toolpath& p)
{
    const std::vector<Move> cuts = movesOf(p, MoveRole::Cut);
    double a = 0;
    for (std::size_t k = 0; k < cuts.size(); ++k) {
        const Move& m = cuts[k];
        const Move& n = cuts[(k + 1) % cuts.size()];
        a += toMm(m.x) * toMm(n.y) - toMm(n.x) * toMm(m.y);
    }
    return a / 2;
}

} // namespace

TEST_CASE("chamfer geometry: depth from the width and the flank angle, the axis offset, the tip depth",
          "[cam][chamfer]")
{
    // 90 degree chamfer mill, sharp: c = w / tan 45 = w; X = 0 + o tan 45 = o; the tip c + o below the edge.
    auto g90 = ops::chamferGeometry(chamferMill(6, 90), 0.5, 0.3);
    REQUIRE(g90);
    CHECK(g90->depth == 500'000);
    CHECK(g90->axis == 300'000);
    CHECK(g90->tipBelow == 800'000);
    // 60 degree chamfer mill with a 0.2 mm tip: c = 0.5 / tan 30 = 0.866025 mm; X = 0.1 + 0.3 tan 30 = 0.273205 mm.
    auto g60 = ops::chamferGeometry(chamferMill(6, 60, 0.2), 0.5, 0.3);
    REQUIRE(g60);
    CHECK(g60->depth == 866'025);
    CHECK(g60->axis == 273'205);
    CHECK(g60->tipBelow == 1'166'025);
    // 120 degree spot drill: c = 0.5 / tan 60 = 0.288675 mm; X = 0.3 tan 60 = 0.519615 mm.
    lib::Tool spot = chamferMill(6, 120);
    spot.type = lib::ToolType::SpotDrill;
    auto g120 = ops::chamferGeometry(spot, 0.5, 0.3);
    REQUIRE(g120);
    CHECK(g120->depth == 288'675);
    CHECK(g120->axis == 519'615);
    CHECK(g120->tipBelow == 588'675);
    // No tip offset, a sharp tool: the axis runs on the edge.
    auto g0 = ops::chamferGeometry(chamferMill(6, 90), 1, 0);
    REQUIRE(g0);
    CHECK(g0->axis == 0);
    CHECK(g0->tipBelow == 1'000'000);
    // A tool without a point angle.
    CHECK_FALSE(ops::chamferGeometry(chamferMill(6, 0), 0.5, 0.3));
}

TEST_CASE("chamfer: the outside of the block, climb and conventional", "[cam][chamfer]")
{
    const std::vector<kernel::SubId> edges = outside();
    const auto claim = [&](invariants::Claims& k) {
        chamferClaims(k, block(), edges, 0.5, 1.0);
        k.initialStock = invariants::InitialStock::Part;
        k.direction = invariants::Direction::Climb;
    };
    const Toolpath climb = chamferCase(edges, chamferMill(6)).path(claim);
    // One closed pass round the block 0.3 mm outside it, at Z 20 - 0.5 - 0.3, the corners rounded (radius 0.3).
    const std::vector<Move> cuts = movesOf(climb, MoveRole::Cut);
    REQUIRE_FALSE(cuts.empty());
    int arcs = 0;
    for (const Move& m : cuts) {
        CHECK(m.z == 19'200'000);
        if (m.kind == MoveKind::Linear) {
            const bool onSide = m.x == -300'000 || m.x == 80'300'000 || m.y == -300'000 || m.y == 50'300'000;
            CHECK(onSide);
        } else {
            ++arcs;
            CHECK(std::abs(std::hypot(static_cast<double>(m.x - m.i), static_cast<double>(m.y - m.j)) - 300'000) <= 1);
        }
    }
    CHECK(arcs == 4);
    // Climb with the spindle clockwise keeps the wall on the tool's right: clockwise round an outside.
    CHECK(cutArea(climb) < 0);
    const Toolpath conventional =
        chamferCase(edges, chamferMill(6), R"({"direction":"conventional"})").path([&](invariants::Claims& k) {
            claim(k);
            k.direction = invariants::Direction::Conventional;
        });
    CHECK(cutArea(conventional) > 0);
    // The plunge beside the edge, then the cut.
    const std::vector<Move> plunges = movesOf(climb, MoveRole::Plunge);
    REQUIRE(plunges.size() == 1);
    CHECK(plunges[0].z == 19'200'000);
    CHECK(plunges[0].feedMicronPerMin == 400'000);
}

TEST_CASE("chamfer: a pocket's edge and a hole's edge, the tool inside them", "[cam][chamfer]")
{
    const auto claimFor = [](const std::vector<kernel::SubId>& edges) {
        return [edges](invariants::Claims& k) {
            chamferClaims(k, block(), edges, 0.5, 1.0);
            k.initialStock = invariants::InitialStock::Part;
            k.direction = invariants::Direction::Climb;
        };
    };
    const Toolpath pocket = chamferCase(pocketEdges(), chamferMill(6)).path(claimFor(pocketEdges()));
    for (const Move& m : movesOf(pocket, MoveRole::Cut)) {
        CHECK(m.z == 19'200'000);
        if (m.kind == MoveKind::Linear) {
            const bool onSide = m.x == 10'300'000 || m.x == 39'700'000 || m.y == 10'300'000 || m.y == 39'700'000;
            CHECK(onSide);
        } else {
            // The 4 mm corners less the 0.3 mm axis offset.
            CHECK(std::abs(std::hypot(static_cast<double>(m.x - m.i), static_cast<double>(m.y - m.j)) - 3'700'000) <=
                  1);
        }
    }
    CHECK(cutArea(pocket) > 0); // climb inside a pocket: counter-clockwise
    const Toolpath hole = chamferCase(holeEdges(), chamferMill(6)).path(claimFor(holeEdges()));
    const std::vector<Move> cuts = movesOf(hole, MoveRole::Cut);
    REQUIRE_FALSE(cuts.empty());
    for (const Move& m : cuts) {
        CHECK(m.kind == MoveKind::ArcCCW);
        CHECK(m.i == 60'000'000);
        CHECK(m.j == 15'000'000);
        CHECK(std::abs(std::hypot(static_cast<double>(m.x - m.i), static_cast<double>(m.y - m.j)) - 4'700'000) <= 1);
        CHECK(m.z == 19'200'000);
    }
}

TEST_CASE("chamfer: an open edge, extended past its ends", "[cam][chamfer]")
{
    const std::vector<kernel::SubId> front = edgesAt(block(), 20, 0, 0, 80, 0);
    REQUIRE(front.size() == 1);
    const Toolpath p =
        chamferCase(front, chamferMill(6), R"({"tangentEdges":false,"extension":2})").path([&](invariants::Claims& k) {
            chamferClaims(k, block(), front, 0.5, 1.0);
            k.initialStock = invariants::InitialStock::Part;
        });
    const std::vector<Move> cuts = movesOf(p, MoveRole::Cut);
    REQUIRE(cuts.size() == 1);
    // Climb along the front: the wall on the tool's right, so from X 82 to X -2, 0.3 mm in front of the edge.
    CHECK(cuts[0].x == -2'000'000);
    CHECK(cuts[0].y == -300'000);
    const std::vector<Move> plunges = movesOf(p, MoveRole::Plunge);
    REQUIRE(plunges.size() == 1);
    CHECK(plunges[0].x == 82'000'000);
    CHECK(plunges[0].y == -300'000);
}

TEST_CASE("chamfer: the checker sees what a chamfer cuts", "[cam][chamfer]")
{
    // Without the claim that the chamfer's wedge is meant to be cut, the toolpath gouges the part; with a wedge
    // shallower than the chamfer (0.3 mm instead of 0.5), still; with the right wedge it is clean (the cases above).
    const std::vector<kernel::SubId> edges = outside();
    auto p = chamferCase(edges, chamferMill(6)).generate();
    REQUIRE(p);
    const ResolvedGeometry geom = geometryOn(block(), edges);
    const lib::Tool t = chamferMill(6);
    const auto run = [&](double depth) {
        invariants::Claims k = invariants::claimsFor(millOp("chamfer2d", "{}", 8000, 800), geom);
        k.initialStock = invariants::InitialStock::Part;
        if (depth > 0) {
            chamferClaims(k, block(), edges, depth, 1.0);
        }
        return invariants::check(invariants::CheckInput{*p, geom, t, nullptr, nullptr, k});
    };
    for (const double depth : {0.0, 0.3}) {
        const auto found = run(depth);
        INFO("wedge " << depth << " deep:\n" << invariants::describe(found));
        REQUIRE_FALSE(found.empty());
        CHECK(std::all_of(found.begin(), found.end(), [](const invariants::Finding& f) { return f.id == "gouge"; }));
    }
    CHECK(run(0.5).empty());
}

TEST_CASE("chamfer refusals, each naming its cause", "[cam][chamfer]")
{
    SECTION("the tool tip would hit a floor below the chamfer")
    {
        const Error e =
            refusedBoth(chamferCase(shallowEdges(), chamferMill(6)),
                        "Chamfering the edge at X50 Y30 Z20, the 6 mm chamfer mill's tip would cut the part "
                        "or a fixture at");
        CHECK(e.code == ErrorCode::ToolpathFailed);
        CHECK_THAT(e.message, ContainsSubstring("below the chamfer (a wall or floor too close): reduce the tip offset "
                                                "or the chamfer width"));
        // A chamfer that ends above the floor (0.1 wide, no tip offset: the tip at Z 19.9) is fine.
        CHECK(chamferCase(shallowEdges(), chamferMill(6), R"({"width":0.1,"tipOffset":0})").validate());
    }
    SECTION("the tool does not fit inside a hole with its axis that far from the edge")
    {
        refusedBoth(chamferCase(holeEdges(), chamferMill(20), R"({"tipOffset":5.2})"),
                    "The 20 mm chamfer mill does not fit inside the edge loop at");
    }
    SECTION("the cone does not reach the top of the chamfer, the cutting part too short")
    {
        refusedBoth(chamferCase(outside(), chamferMill(1)),
                    "A 0.5 mm chamfer with a 0.3 mm tip offset needs the cone to reach 0.8 mm from the tool's axis; "
                    "the 1 mm chamfer mill reaches 0.5 mm: use a larger tool, a smaller tip offset or a narrower "
                    "chamfer");
        refusedBoth(chamferCase(outside(), chamferMill(6, 90, 0, 0.5)),
                    "The chamfer and its tip offset go 0.8 mm below the edge, but the tool's cutting part is 0.5 mm "
                    "long");
    }
    SECTION("tools that are not pointed or not made for it")
    {
        refusedBoth(chamferCase(outside(), chamferMill(6, 0)), "The chamfer mill has no point angle");
        refusedBoth(chamferCase(outside(), endMill(6)),
                    "The tool is a flat end mill; chamfering needs a chamfer mill or a spot drill");
    }
    SECTION("an edge that is not the top of a wall, or not level, or along Z")
    {
        refusedBoth(chamferCase(edgesAt(block(), 12, 10, 10, 40, 40), chamferMill(6)),
                    "has material on both sides just below it: it is not the top edge of a wall");
        // A block with a sloping top: its edges in the sloping face's ends are not level.
        kernel::ProfileLoop side;
        side.curves = {lineC({0, 0}, {40, 0}), lineC({40, 0}, {40, 10}), lineC({40, 10}, {0, 20}),
                       lineC({0, 20}, {0, 0})};
        const kernel::Profile prof{kernel::Plane{Vec3{0, 0, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}}, {side}};
        JobContext ctx;
        auto wedge = kernel::extrude(prof, 30, 0, ctx);
        REQUIRE(wedge);
        const kernel::Shape s = wedge->op.shape;
        std::vector<kernel::SubId> sloped, vertical;
        for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge); ++i) {
            const kernel::SubId e{kernel::SubKind::Edge, i};
            const auto c = kernel::curveInfo(s, e);
            if (!c) {
                continue;
            }
            if (sloped.empty() && std::abs(c->start.z - c->end.z) > 5 && std::abs(c->start.x - c->end.x) > 5) {
                sloped.push_back(e);
            }
            if (vertical.empty() && std::abs(c->start.x - c->end.x) < 1e-9 && std::abs(c->start.y - c->end.y) < 1e-9) {
                vertical.push_back(e);
            }
        }
        REQUIRE(sloped.size() == 1);
        refusedBoth(chamferCase(sloped, chamferMill(6), "{}", s), "is not level (it runs from Z10 to Z20)");
        REQUIRE(vertical.size() == 1);
        refusedBoth(chamferCase(vertical, chamferMill(6), "{}", s),
                    "The picked edges run along the setup's Z axis: pick the top edges of the walls");
    }
    SECTION("a face, or nothing, selected")
    {
        refusedBoth(chamferCase({kernel::SubId{kernel::SubKind::Face, 0}}, chamferMill(6)),
                    "Selection 1 is not an edge: pick the edges to chamfer");
        refusedBoth(chamferCase({}, chamferMill(6)), "Pick the edges to chamfer: the top edge of each wall");
    }
    SECTION("heights out of order, the spindle, the stick-out")
    {
        Case c = chamferCase(outside(), chamferMill(6));
        c.op.heights.clearance = 4;
        refusedBoth(c, "The clearance height (4 above the stock top) must not be below the retract height (5)");
        c = chamferCase(outside(), chamferMill(6));
        c.op.heights.feedPlane = 6;
        refusedBoth(c, "The retract height (5 above the stock top) must not be below the feed plane (6)");
        c = chamferCase(outside(), chamferMill(6));
        c.op.cutting.clockwise = false;
        refusedBoth(c, "The spindle is set to turn counter-clockwise (M4)");
        c = chamferCase(outside(), chamferMill(6));
        c.op.tool.stickOut = 1.5;
        refusedBoth(c, "The tool sticks out 1.5 mm, but the chamfer at X0 Y0 needs 1.8 mm for the holder to stay 1 mm "
                       "above the stock and the part");
    }
}

TEST_CASE("chamfer: the same operation gives the same bytes every time", "[cam][chamfer]")
{
    std::vector<kernel::SubId> edges = outside();
    const auto more = holeEdges();
    edges.insert(edges.end(), more.begin(), more.end());
    const Case c = chamferCase(edges, chamferMill(6));
    auto first = c.generate();
    REQUIRE(first);
    for (int i = 0; i < 3; ++i) {
        auto again = c.generate();
        REQUIRE(again);
        CHECK(again->serialize() == first->serialize());
    }
}
