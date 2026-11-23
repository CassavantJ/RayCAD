// The rules of the 2D contour operation (contour2d.h, task T-053) against hand-computed values: depths, offsets on
// lines and arcs to 1 um (the acceptance), sides and directions, open chains, leads, roughing, finish and spring
// passes, tabs of both shapes, and compensation in the computer, in the control and for wear. Every toolpath of the
// computer and wear also goes through the independent invariant checker (ContourCase::path).
#include "contour2d_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <set>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;
namespace c2 = ray::cam::ops::contour2d;
namespace g2 = ray::cam::geom2d;

namespace {

const kernel::Shape& part()
{
    static const kernel::Shape s = contourPart();
    return s;
}

// The signed distance (mm) from (x, y) to the boundary of the rectangle x0..x1 x y0..y1 with corners of radius r:
// positive outside, negative inside.
double roundedRectDistance(double x, double y, double x0, double y0, double x1, double y1, double r)
{
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const double qx = std::abs(x - cx) - ((x1 - x0) / 2 - r);
    const double qy = std::abs(y - cy) - ((y1 - y0) / 2 - r);
    const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
}

struct RectSpec {
    double x0, y0, x1, y1, r;
};
const RectSpec kBoss{20, 15, 80, 65, 8};
const RectSpec kPocket{35, 30, 65, 50, 5};
const RectSpec kPlate{0, 0, 100, 80, 10};

// Every span of `p` lies at signed distance d (mm) from the rounded rectangle: the line ends, the arc ends and
// middles, and each arc's centre is the corner's centre with radius r + d exactly (to 1 nm). Within 1 um elsewhere.
void onOffset(const g2::Path& p, const RectSpec& s, double d)
{
    REQUIRE_FALSE(p.spans.empty());
    g2::Pt cur = p.start;
    for (const g2::Span& span : p.spans) {
        const auto check = [&](g2::Pt q) {
            const double dd = roundedRectDistance(toMm(q.x), toMm(q.y), s.x0, s.y0, s.x1, s.y1, s.r);
            INFO("point " << toMm(q.x) << ", " << toMm(q.y));
            CHECK(std::abs(dd - d) <= 0.001);
        };
        check(cur);
        check(span.end);
        if (span.kind != g2::SpanKind::Line) {
            const double cx = toMm(span.centre.x), cy = toMm(span.centre.y);
            const bool cornerX = std::abs(cx - (s.x0 + s.r)) < 1e-9 || std::abs(cx - (s.x1 - s.r)) < 1e-9;
            const bool cornerY = std::abs(cy - (s.y0 + s.r)) < 1e-9 || std::abs(cy - (s.y1 - s.r)) < 1e-9;
            if (cornerX && cornerY) {
                CHECK(span.radius == toNm(s.r + d)); // exact: the corner's radius plus the offset
            } else {
                // A round join about a sharp corner of an earlier offset (not on these parts' smooth outlines).
                FAIL_CHECK("an arc not about a corner of the outline: " << cx << ", " << cy);
            }
            const double rr =
                dmath::hypot(static_cast<double>(cur.x - span.centre.x), static_cast<double>(cur.y - span.centre.y));
            CHECK(std::abs(rr - static_cast<double>(span.radius)) <= 1.0);
        }
        cur = span.end;
    }
}

// The cut moves of a toolpath at height z, projected (lines and arcs, from the move before each).
std::vector<std::pair<g2::Pt, Move>> cutsAt(const Toolpath& t, Nm z)
{
    std::vector<std::pair<g2::Pt, Move>> out;
    g2::Pt prev{t.startX, t.startY};
    for (const Move& m : t.moves) {
        if (m.role == MoveRole::Cut && m.z == z) {
            out.emplace_back(prev, m);
        }
        prev = g2::Pt{m.x, m.y};
    }
    return out;
}

double signedArea(const g2::Path& p)
{
    return g2::area(p);
}

ContourCase bossOutside(const std::string& params, double bottom = -15)
{
    ContourCase c;
    c.op = contourOp(params, bottom);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 15, 25})}); // one top edge: the rest by tangency
    return c;
}
ContourCase pocketWall(const std::string& params, double toolDiameter = 6)
{
    ContourCase c;
    c.op = contourOp(params, 0);
    c.tool = endMill(toolDiameter);
    c.geom = geometryOn(part(), {flatFaceAt(part(), 18, 50, 40)}); // the pocket floor: its outer loop
    return c;
}

} // namespace

TEST_CASE("contour2d: depths are equal steps of at most the stepdown, exact integers", "[cam][contour2d]")
{
    CHECK(c2::depthLevels(0, -15'000'000, 5'000'000) == std::vector<Nm>{-5'000'000, -10'000'000, -15'000'000});
    CHECK(c2::depthLevels(0, -10'000'000, 3'000'000) ==
          std::vector<Nm>{-2'500'000, -5'000'000, -7'500'000, -10'000'000});
    // 10 nm in 4 steps: 2.5 -> 2 (half to even), 5, 7.5 -> 8, 10.
    CHECK(c2::depthLevels(0, -10, 3) == std::vector<Nm>{-2, -5, -8, -10});
    CHECK(c2::depthLevels(5, -5, 0) == std::vector<Nm>{-5});   // no stepdown: one depth at the bottom
    CHECK(c2::depthLevels(0, -4, 100) == std::vector<Nm>{-4}); // a stepdown deeper than the cut
    CHECK(c2::depthLevels(0, 0, 1).empty());                   // nothing to cut
    CHECK(c2::depthLevels(0, -1001, 1).empty());               // more than kMaxDepths
    CHECK(c2::depthLevels(0, -1000, 1).size() == 1000);
}

TEST_CASE("contour2d: the outside of a boss, offsets exact on lines and arcs", "[cam][contour2d]")
{
    // 10 mm tool, 0.5 mm stock: the tool centre 5.5 mm off the outline: lines at X 14.5 and 85.5, Y 9.5 and 70.5,
    // corner arcs about (28, 23), (72, 23), (72, 57), (28, 57) of radius 8 + 5.5 = 13.5.
    const ContourCase c = bossOutside(R"({"stockToLeave":0.5,"maxStepdown":5})");
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 3); // Z 20, 15, 10
    CHECK(plan->passes[0].pass.start.z == 20'000'000);
    CHECK(plan->passes[1].pass.start.z == 15'000'000);
    CHECK(plan->passes[2].pass.start.z == 10'000'000);
    for (const c2::PlannedPass& pp : plan->passes) {
        onOffset(pp.centre, kBoss, 5.5);
        CHECK(pp.centre.closed);
        // Climb with the spindle clockwise outside: the material on the right, so clockwise round the boss.
        CHECK(signedArea(pp.centre) < 0);
        CHECK(pp.centre.spans.size() == 9); // 4 lines, 4 arcs, the longest side split where the pass starts
    }
    const Toolpath t = c.path();
    // The toolpath's cut moves at the bottom are those of the pass: on the offset, compensation off.
    const auto cuts = cutsAt(t, 10'000'000);
    REQUIRE(cuts.size() >= 8);
    for (const auto& [from, m] : cuts) {
        CHECK(m.compensation == 0);
        CHECK(std::abs(roundedRectDistance(toMm(m.x), toMm(m.y), 20, 15, 80, 65, 8) - 5.5) <= 0.001);
        CHECK(m.feedMicronPerMin == 800'000);
    }
    CHECK(t.stats.minZ == 10.0);
    CHECK(t.stats.maxZ == 35.0); // the clearance: 10 above the stock top
}

TEST_CASE("contour2d: the inside of a pocket from its floor face, conventional", "[cam][contour2d]")
{
    // 6 mm tool: the tool centre 3 mm inside: lines at X 38 and 62, Y 33 and 47; corner arcs of radius 5 - 3 = 2.
    const ContourCase c = pocketWall(R"({"direction":"conventional"})");
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    CHECK(plan->passes[0].pass.start.z == 18'000'000); // the floor (chain height + bottom 0)
    onOffset(plan->passes[0].centre, kPocket, -3);
    // Conventional (the tool on the right of its travel, the wall on its left) inside a pocket: clockwise.
    CHECK(signedArea(plan->passes[0].centre) < 0);
    (void)c.path();
    // Climb goes the other way round: counter-clockwise inside a pocket with the spindle clockwise.
    const auto climb = pocketWall("{}").plan();
    REQUIRE(climb);
    CHECK(signedArea(climb->passes[0].centre) > 0);
}

TEST_CASE("contour2d: an inside corner smaller than the tool leaves material, with a warning", "[cam][contour2d]")
{
    // A 12 mm tool in the R5 corners: the corner arcs have no offset (5 - 6 < 0): sharp corners, material left there.
    const ContourCase c = pocketWall("{}", 12);
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    for (const g2::Span& s : plan->passes[0].centre.spans) {
        CHECK(s.kind == g2::SpanKind::Line); // the four sides, trimmed where they meet
    }
    REQUIRE_FALSE(plan->warnings.empty());
    CHECK_THAT(plan->warnings.front(), ContainsSubstring("cannot reach the contour"));
    CHECK_THAT(plan->warnings.front(), ContainsSubstring("material is left there"));
    const Toolpath t = c.path();
    CHECK(t.warnings == plan->warnings);
}

TEST_CASE("contour2d: the tool side from the parameter", "[cam][contour2d]")
{
    // The pocket's top edges (Z 25), down to its floor (Z 18), 6 mm tool: inside, 3 mm in.
    const auto pocketTop = [](const std::string& params) {
        ContourCase c;
        c.op = contourOp(params, -7);
        c.tool = endMill(6);
        c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 30, 25})});
        return c;
    };
    const auto in = pocketTop(R"({"side":"inside"})").plan();
    REQUIRE(in);
    onOffset(in->passes[0].centre, kPocket, -3);
    // Left of the counter-clockwise chain is its inside too; automatic finds the material outside the pocket.
    const auto left = pocketTop(R"({"side":"left"})").plan();
    REQUIRE(left);
    CHECK(left->passes[0].centre == in->passes[0].centre);
    const auto automatic = pocketTop("{}").plan();
    REQUIRE(automatic);
    CHECK(automatic->passes[0].centre == in->passes[0].centre);
    // Outside (or right) the tool would run over the boss's top 3 mm off the pocket and cut into it: refused.
    for (const char* side : {R"({"side":"outside"})", R"({"side":"right"})"}) {
        const auto out = pocketTop(side).plan();
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ToolpathFailed);
        CHECK_THAT(out.error().message, ContainsSubstring("would cut into the part"));
    }
}

TEST_CASE("contour2d: an open chain along one edge, tool away from the material", "[cam][contour2d]")
{
    // The plate's front top edge alone (not followed round): (10, 0) to (90, 0) at Z 10, cut down to the plate's
    // bottom. The material is at +Y, so the tool runs along Y -5; climb keeps it on the right: from X 90 to X 10.
    ContourCase c;
    c.op = contourOp(R"({"followTangent":false,"leadType":"line"})", -10);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    const g2::Path& p = plan->passes[0].centre;
    CHECK_FALSE(p.closed);
    CHECK(p.start == g2::Pt{90'000'000, -5'000'000});
    REQUIRE(p.spans.size() == 1);
    CHECK(p.spans[0].end == g2::Pt{10'000'000, -5'000'000});
    const Toolpath t = c.path();
    // Line leads: 10 mm (the tool diameter) along the tangent, before and after.
    const auto in = movesOf(t, MoveRole::LeadIn);
    REQUIRE(in.size() == 1);
    CHECK(in[0].kind == MoveKind::Linear);
    CHECK(in[0].x == 90'000'000);
    const auto out = movesOf(t, MoveRole::LeadOut);
    REQUIRE(out.size() == 1);
    CHECK(out[0].x == 0);
    CHECK(out[0].y == -5'000'000);
    CHECK(t.startX == 100'000'000); // the lead-in's start
    CHECK(t.startY == -5'000'000);
}

TEST_CASE("contour2d: arc leads are tangent arcs of the tool's diameter on the tool's side", "[cam][contour2d]")
{
    const ContourCase c = bossOutside(R"({"leadSweep":90,"leadLength":3})", -2);
    const Toolpath t = c.path(); // the checker holds the lead joints tangent to 0.1 degree
    const auto in = movesOf(t, MoveRole::LeadIn);
    REQUIRE(in.size() == 2); // the straight part, then the arc
    CHECK(in[0].kind == MoveKind::Linear);
    REQUIRE((in[1].kind == MoveKind::ArcCW || in[1].kind == MoveKind::ArcCCW));
    // The arc ends at the pass start, its centre 10 mm (2 R) from there on the tool's side (outside the boss).
    const double r = dmath::hypot(static_cast<double>(in[1].x - in[1].i), static_cast<double>(in[1].y - in[1].j));
    CHECK(std::abs(r - 10'000'000.0) <= 2.0);
    CHECK(roundedRectDistance(toMm(in[1].i), toMm(in[1].j), 20, 15, 80, 65, 8) > 5.5 + 9.0);
    // The straight part is 3 mm long.
    const auto out = movesOf(t, MoveRole::LeadOut);
    REQUIRE(out.size() == 2);
    CHECK(out[1].kind == MoveKind::Linear);
}

TEST_CASE("contour2d: roughing passes, a finish pass and a spring pass", "[cam][contour2d]")
{
    // Two roughing passes 2 mm apart at each of three depths, leaving 0.5 stock + 0.3 finish allowance; the finish and
    // the spring pass at the bottom, leaving the 0.5 stock.
    const ContourCase c = bossOutside(
        R"({"stockToLeave":0.5,"maxStepdown":5,"roughPasses":2,"roughStepover":2,"finishPass":true,"finishAllowance":0.3,"springPass":true})");
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 3 * 2 + 2);
    const std::vector<std::pair<Nm, double>> expected = {{20'000'000, 5 + 0.5 + 0.3 + 2}, {20'000'000, 5 + 0.5 + 0.3},
                                                         {15'000'000, 5 + 0.5 + 0.3 + 2}, {15'000'000, 5 + 0.5 + 0.3},
                                                         {10'000'000, 5 + 0.5 + 0.3 + 2}, {10'000'000, 5 + 0.5 + 0.3},
                                                         {10'000'000, 5 + 0.5},           {10'000'000, 5 + 0.5}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("pass " << i);
        CHECK(plan->passes[i].pass.start.z == expected[i].first);
        onOffset(plan->passes[i].centre, kBoss, expected[i].second);
    }
    CHECK(plan->passes[6].radial == -1); // finish
    CHECK(plan->passes[7].radial == -2); // spring
    CHECK(plan->passes[7].centre == plan->passes[6].centre);
    (void)c.path();
}

namespace {

// The XY arc length along `p` of each of its vertical moves (rising or falling straight up and down).
struct Vertical {
    g2::Pt at;
    Nm from, to;
};
std::vector<Vertical> verticals(const Toolpath& t, Nm atZ, Nm tabZ)
{
    std::vector<Vertical> out;
    Move prev{};
    prev.x = t.startX;
    prev.y = t.startY;
    prev.z = t.startZ;
    for (const Move& m : t.moves) {
        if (m.role == MoveRole::Cut && m.kind == MoveKind::Linear && m.x == prev.x && m.y == prev.y &&
            ((prev.z == atZ && m.z == tabZ) || (prev.z == tabZ && m.z == atZ))) {
            out.push_back(Vertical{g2::Pt{m.x, m.y}, prev.z, m.z});
        }
        prev = m;
    }
    return out;
}

} // namespace

TEST_CASE("contour2d: rectangular tabs with a tool that cuts at its centre go straight up and down", "[cam][contour2d]")
{
    // The plate's outline (one edge picked, the rest by tangency) cut through with a 6 mm ball end mill, 4 tabs 5 wide
    // and 2 high: at Z 0 the tool rises to Z 2 over each tab, 5 + 6 mm apart along the tool-centre path.
    ContourCase c;
    c.op = contourOp(R"({"tabs":true,"tabCount":4,"tabWidth":5,"tabHeight":2})", -10);
    c.tool = endMill(6, 30, lib::ToolType::BallEndMill);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    const Toolpath t = c.path();
    const auto v = verticals(t, 0, 2'000'000);
    REQUIRE(v.size() == 8);
    const g2::Path& centre = plan->passes[0].centre;
    // The outline itself (clockwise like the pass), to see where the tabs sit on the part.
    g2::Path outline;
    {
        const auto P = [](double x, double y) {
            return g2::Pt{toNm(x), toNm(y)};
        };
        outline.start = P(0, 10);
        outline.spans = {g2::Span{g2::SpanKind::Line, P(0, 70), {}, 0},
                         g2::Span{g2::SpanKind::ArcCW, P(10, 80), P(10, 70), toNm(10)},
                         g2::Span{g2::SpanKind::Line, P(90, 80), {}, 0},
                         g2::Span{g2::SpanKind::ArcCW, P(100, 70), P(90, 70), toNm(10)},
                         g2::Span{g2::SpanKind::Line, P(100, 10), {}, 0},
                         g2::Span{g2::SpanKind::ArcCW, P(90, 0), P(90, 10), toNm(10)},
                         g2::Span{g2::SpanKind::Line, P(10, 0), {}, 0},
                         g2::Span{g2::SpanKind::ArcCW, P(0, 10), P(10, 10), toNm(10)}};
        outline.closed = true;
    }
    const double around = g2::length(outline);
    std::vector<double> onOutline;
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(v[2 * i].from == 0);   // up
        CHECK(v[2 * i + 1].to == 0); // and down
        const double up = c2::nearestS(centre, v[2 * i].at);
        const double down = c2::nearestS(centre, v[2 * i + 1].at);
        CHECK(std::abs(down - up - 11'000'000.0) <= 2.0); // the tab's 5 mm plus the tool's 6 mm, on the centre path
        onOutline.push_back(c2::nearestS(outline, g2::pointAt(centre, (up + down) / 2)));
    }
    // Spread evenly along the outline: a quarter of it apart.
    for (std::size_t i = 0; i < 4; ++i) {
        double gap = onOutline[(i + 1) % 4] - onOutline[i];
        if (gap < 0) {
            gap += around;
        }
        CHECK(std::abs(gap - around / 4) <= 1000.0);
    }
}

TEST_CASE("contour2d: rectangular tabs with a flat end mill ramp back down behind the tab", "[cam][contour2d]")
{
    // A 10 mm flat end mill does not cut at its centre: after each tab it goes on at the tab height for
    // Lr = 2 / tan 10 degrees, ramps back down to just behind the tab, and cuts forward again.
    ContourCase c;
    c.op = contourOp(R"({"tabs":true,"tabCount":2,"tabWidth":4,"tabHeight":2,"rampAngle":10})", -10);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    const Toolpath t = c.path(); // no plunge into stock: the checker's plunge rule
    const auto v = verticals(t, 0, 2'000'000);
    REQUIRE(v.size() == 2); // straight up only
    std::vector<Move> ramps;
    for (const Move& m : t.moves) {
        if (m.role == MoveRole::Ramp && m.z < 2'000'000) {
            ramps.push_back(m);
        }
    }
    REQUIRE_FALSE(ramps.empty());
    // The ramp's slope is 10 degrees (it ends at Z 0).
    Move prev{};
    double run = 0, drop = 0;
    for (std::size_t k = 0; k < t.moves.size(); ++k) {
        const Move& m = t.moves[k];
        if (m.role == MoveRole::Ramp && k > 0 && t.moves[k - 1].z > m.z && m.z >= 0 && t.moves[k - 1].z <= 2'000'000) {
            prev = t.moves[k - 1];
            run += dmath::hypot(static_cast<double>(m.x - prev.x), static_cast<double>(m.y - prev.y));
            drop += static_cast<double>(prev.z - m.z);
        }
    }
    REQUIRE(run > 0);
    CHECK(std::abs(std::atan2(drop, run) * 180 / kPiC - 10.0) < 0.01);
}

TEST_CASE("contour2d: a tab's ramp back down is never steeper than the ramp angle, piece by piece", "[cam][contour2d]")
{
    // A circle of radius 2 mm as 251 short lines (about 50 um each), a tab 1 mm wide and 0.1 mm high, a 3 mm tool that
    // does not cut at its centre: the ramp back down runs over several of the lines (38 at 3 degrees), each end's Z
    // rounded to a nanometre. The plain appendSloped can round a piece up to half a nanometre steeper than the angle;
    // the moves of tabbedMoves (the slope-capped appendSloped) never are.
    g2::Path circle;
    const int n = 251;
    const double r = 2'000'000;
    circle.start = g2::Pt{static_cast<Nm>(r), 0};
    for (int i = 1; i <= n; ++i) {
        const double a = 2 * kPiC * i / n;
        circle.spans.push_back(
            g2::Span{g2::SpanKind::Line,
                     i == n ? circle.start : g2::Pt{g2::roundNm(r * dmath::cos(a)), g2::roundNm(r * dmath::sin(a))},
                     {},
                     0});
    }
    circle.closed = true;
    const Nm z = -1'000'000;
    bool steeper = false;
    for (const double angle : {3.0, 7.0, 10.0, 25.0}) {
        INFO("ramp angle " << angle);
        const double tanA = dmath::tan(angle * kPiC / 180.0);
        c2::TabMoves tm;
        tm.shape = c2::TabShape::Rectangular;
        tm.zt = z + 100'000;
        tm.width = 1'000'000;
        tm.radius = 1'500'000;
        tm.centreCutting = false;
        tm.rampAngleDeg = angle;
        auto moves = c2::tabbedMoves(circle, z, {c2::Tab{5'000'000}}, tm, 600'000, 300'000, 450'000, 0);
        REQUIRE(moves);
        // Each ramp piece: its drop over its XY length at most tan(angle), exactly (integer ends, double compare).
        g2::Pt prev = circle.start;
        Nm prevZ = z;
        int pieces = 0;
        for (const Move& m : *moves) {
            if (m.role == MoveRole::Ramp) {
                ++pieces;
                const double run = dmath::hypot(static_cast<double>(m.x - prev.x), static_cast<double>(m.y - prev.y));
                INFO("ramp piece to " << m.x << ", " << m.y << ", " << m.z);
                CHECK(static_cast<double>(std::abs(prevZ - m.z)) <= run * tanA);
            }
            prev = g2::Pt{m.x, m.y};
            prevZ = m.z;
        }
        CHECK(pieces >= 3); // 0.1 / tan(angle) mm over lines about 50 um long
        // The same ramp by the plain appendSloped (proportional Z, rounded).
        const double lr = 100'000.0 / tanA * (1.0 + c2::kRampSlack);
        const double b = 5'000'000.0 + 500'000.0 + 1'500'000.0;
        g2::Path back = g2::reverse(g2::subPath(circle, b, b + lr));
        std::vector<Move> plain;
        link::appendSloped(back, tm.zt, z, MoveRole::Ramp, 450'000, plain);
        g2::Pt p = back.start;
        Nm pz = tm.zt;
        for (const Move& m : plain) {
            const double run = dmath::hypot(static_cast<double>(m.x - p.x), static_cast<double>(m.y - p.y));
            steeper = steeper || static_cast<double>(std::abs(pz - m.z)) > run * tanA;
            p = g2::Pt{m.x, m.y};
            pz = m.z;
        }
    }
    // Made that way, some piece of some of these ramps is steeper than its angle: the check above would catch it.
    CHECK(steeper);
}

TEST_CASE("contour2d: triangular tabs", "[cam][contour2d]")
{
    // Tabs 8 wide and 1 high with a 6 mm ball end mill: up from Z 0 over 4 mm, 6 mm flat at Z 1, down over 4 mm.
    ContourCase c;
    c.op = contourOp(R"({"tabs":true,"tabShape":"triangular","tabCount":3,"tabWidth":8,"tabHeight":1})", -10);
    c.tool = endMill(6, 30, lib::ToolType::BallEndMill);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    const Toolpath t = c.path();
    int rising = 0, falling = 0;
    Nm z = t.startZ;
    for (const Move& m : t.moves) {
        if (m.role == MoveRole::Cut && z >= 0 && z <= 1'000'000 && m.z >= 0 && m.z <= 1'000'000 && m.z != z) {
            (m.z > z ? rising : falling) += 1;
        }
        z = m.z;
    }
    CHECK(rising >= 3);
    CHECK(falling >= 3);
    // With a flat end mill the 14 degree flanks (atan(2 / 8)) are steeper than the default 3 degree ramp: refused.
    ContourCase flat = c;
    flat.tool = endMill(6);
    REQUIRE_FALSE(flat.validate());
    CHECK_THAT(flat.validate().error().message, ContainsSubstring("steeper than the ramp angle"));
    flat.op.paramsJson =
        R"({"tabs":true,"tabShape":"triangular","tabCount":3,"tabWidth":8,"tabHeight":1,"rampAngle":15})";
    (void)flat.path();
}

TEST_CASE("contour2d: compensation in the control programs the contour and switches G41 on a straight lead",
          "[cam][contour2d][compensation]")
{
    // The plate's outline, through, 10 mm tool, 0.2 stock: the programmed path is the outline offset by 0.2 only;
    // the control adds the 5 mm radius (what it cuts, C, is the outline offset by 5.2). Climb with M3: G41 (1).
    ContourCase c;
    c.op = contourOp(R"({"compensation":"control","leadLength":4,"stockToLeave":0.2})", -10);
    c.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    const auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->passes.size() == 1);
    const c2::PlannedPass& pp = plan->passes[0];
    onOffset(pp.programmed, kPlate, 0.2);
    onOffset(pp.centre, kPlate, 5.2);
    // What the control cuts is the programmed path offset by the radius: C again.
    JobContext ctx;
    g2::Region r;
    r.outer = g2::area(pp.programmed) > 0 ? pp.programmed : g2::reverse(pp.programmed);
    auto again = g2::offset(g2::Regions{g2::canonicalize(r)}, 5'000'000, ctx);
    REQUIRE(again);
    REQUIRE(again->size() == 1);
    onOffset(again->front().outer, kPlate, 5.2);

    auto t = c.generate();
    REQUIRE(t);
    // Compensation: off until the lead-in's straight move, on through the arc and the pass, off on the lead-out's
    // last (straight) move.
    std::uint8_t comp = 0;
    std::size_t switchedOn = 0, switchedOff = 0;
    for (const Move& m : t->moves) {
        if (m.compensation != comp) {
            CHECK(m.kind == MoveKind::Linear);
            (m.compensation != 0 ? switchedOn : switchedOff) += 1;
            if (m.compensation != 0) {
                CHECK(m.role == MoveRole::LeadIn);
                CHECK(m.compensation == 1);
            } else {
                CHECK(m.role == MoveRole::LeadOut);
            }
            comp = m.compensation;
        }
        if (m.role == MoveRole::Cut) {
            CHECK(m.compensation == 1);
            CHECK(std::abs(roundedRectDistance(toMm(m.x), toMm(m.y), 0, 0, 100, 80, 10) - 0.2) <= 0.001);
        }
    }
    CHECK(switchedOn == 1);
    CHECK(switchedOff == 1);
    CHECK(comp == 0);
    // The lead-in arc is the tool-centre arc grown by the radius: 10 + 5 mm about the same centre.
    const auto in = movesOf(*t, MoveRole::LeadIn);
    REQUIRE(in.size() == 2);
    const double r1 = dmath::hypot(static_cast<double>(in[1].x - in[1].i), static_cast<double>(in[1].y - in[1].j));
    CHECK(std::abs(r1 - 15'000'000.0) <= 2.0);
    // The invariant rules that do not read the tool's position (enum, arcs, compensation, continuity, heights).
    invariants::Claims k = c.claims();
    const auto f = invariants::check(invariants::CheckInput{*t, c.geom, c.tool, nullptr, &c.machine, k});
    for (const invariants::Finding& x : f) {
        INFO(invariants::describe(x));
        // Read as the tool centre, the programmed contour gouges the part (README "What the checker cannot see").
        CHECK((x.id == "gouge" || x.id == "direction" || x.id == "rapidPart" || x.id == "shank"));
    }
}

TEST_CASE("contour2d: compensation for wear: the tool-centre path with G41 on", "[cam][contour2d][compensation]")
{
    ContourCase computer;
    computer.op = contourOp(R"({"leadLength":4})", -10);
    computer.geom = geometryOn(part(), {lineEdgeAt(part(), Vec3{50, 0, 10})});
    ContourCase wear = computer;
    wear.op.paramsJson = R"({"compensation":"wear","leadLength":4})";
    const Toolpath a = computer.path();
    const Toolpath b = wear.path(); // the checker's compensation rule: on and off on lines, on the climb side
    REQUIRE(a.moves.size() == b.moves.size());
    for (std::size_t i = 0; i < a.moves.size(); ++i) {
        CHECK(a.moves[i].x == b.moves[i].x);
        CHECK(a.moves[i].y == b.moves[i].y);
        CHECK(a.moves[i].z == b.moves[i].z);
        CHECK(a.moves[i].compensation == 0);
        if (b.moves[i].role == MoveRole::Cut) {
            CHECK(b.moves[i].compensation == 1);
        }
    }
    // Conventional: the tool on the right, G42 (2).
    wear.op.paramsJson = R"({"compensation":"wear","leadLength":4,"direction":"conventional"})";
    const Toolpath conv = wear.path();
    for (const Move& m : conv.moves) {
        if (m.role == MoveRole::Cut) {
            CHECK(m.compensation == 2);
        }
    }
    // Inside the pocket, in the stock, a flat end mill must ramp in, which compensation does not allow: linking
    // refuses, naming the mode the operation asked for (wear), not compensation in the control.
    ContourCase inside = pocketWall(R"({"compensation":"wear","leadLength":2})");
    const Status refusal = inside.validate();
    REQUIRE_FALSE(refusal);
    CHECK_THAT(refusal.error().message, ContainsSubstring("cutter compensation for wear is on"));
    CHECK_THAT(refusal.error().message, !ContainsSubstring("in the control"));
}

TEST_CASE("contour2d: the nearest point along a path", "[cam][contour2d]")
{
    g2::Path p;
    p.start = g2::Pt{0, 0};
    p.spans = {g2::Span{g2::SpanKind::Line, g2::Pt{10'000'000, 0}, {}, 0},
               g2::Span{g2::SpanKind::ArcCCW, g2::Pt{20'000'000, 0}, g2::Pt{15'000'000, 0}, 5'000'000}};
    CHECK(c2::nearestS(p, g2::Pt{4'000'000, 3'000'000}) == 4'000'000.0);
    // On the half circle above (15, 0)... the arc runs counter-clockwise from (10, 0) through (15, -5) to (20, 0).
    const double s = c2::nearestS(p, g2::Pt{15'000'000, -9'000'000});
    CHECK(std::abs(s - (10'000'000.0 + 5'000'000.0 * kPiC / 2)) <= 1.0);
    CHECK(c2::nearestS(p, g2::Pt{-5'000'000, 0}) == 0.0);
}

TEST_CASE("contour2d: the same operation gives the same bytes every time", "[cam][contour2d]")
{
    const ContourCase c = bossOutside(R"({"maxStepdown":5,"roughPasses":2,"finishPass":true})");
    const auto first = take(c.generate()).serialize();
    for (int i = 0; i < 2; ++i) {
        CHECK(take(c.generate()).serialize() == first);
    }
}
