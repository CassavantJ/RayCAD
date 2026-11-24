// The 2D adaptive roughing operation (task T-061): its rules (op.h) on kernel-built parts and spike S5's pockets, each
// refusal, the entries and their fall-back, depths, stock to leave, stay-down and retract links, the reduced feed, arc
// fitting, stock slices, the statistics, the warnings, cancel, and run time ([wallclock], split by tests/tiers.cmake).
// Every toolpath that is generated goes through the independent invariant checker (invariants/), unless the case says
// why not.
#include "adaptive2d_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

namespace ad = ray::cam::adaptive2d;

AdaptiveCase withParams(AdaptiveCase c, const std::string& params)
{
    c.op.paramsJson = params;
    return c;
}

// The error of generate, which must fail; validate (when `early`) fails with the same message.
Error refusal(const AdaptiveCase& c, bool early)
{
    auto p = c.generate();
    REQUIRE_FALSE(p);
    if (early) {
        auto v = c.validate();
        REQUIRE_FALSE(v);
        CHECK(v.error().message == p.error().message);
        CHECK(v.error().code == p.error().code);
    }
    return p.error();
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A block 30 x 30 x 10 with a round pocket of diameter d at (15, 15) down to Z 7.
AdaptiveCase roundPocketCase(double d, const std::string& params)
{
    const kernel::Shape part = cut(box(Vec3{0, 0, 0}, Vec3{30, 30, 10}), prism({circleLoop(15, 15, d / 2)}, 7, 11));
    AdaptiveCase c;
    c.op = adaptiveOp(params);
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 7)});
    c.clears = {clearClaim({circlePoly(15, 15, d / 2)}, 7)};
    return c;
}

// Arc length of an XY move from (x, y), mm.
double moveLength(const Move& m, Nm x, Nm y)
{
    if (m.kind == MoveKind::Linear) {
        return std::hypot(static_cast<double>(m.x - x), static_cast<double>(m.y - y)) / kNmPerMm;
    }
    const double r = std::hypot(static_cast<double>(x - m.i), static_cast<double>(y - m.j));
    const double a0 = std::atan2(static_cast<double>(y - m.j), static_cast<double>(x - m.i));
    const double a1 = std::atan2(static_cast<double>(m.y - m.j), static_cast<double>(m.x - m.i));
    double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
    while (sweep <= 0) {
        sweep += 2 * kPi;
    }
    return r * sweep / kNmPerMm;
}

double cutLengthMm(const Toolpath& p)
{
    double s = 0;
    Nm x = p.startX, y = p.startY;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Cut && m.kind != MoveKind::Rapid) {
            s += moveLength(m, x, y);
        }
        x = m.x;
        y = m.y;
    }
    return s;
}

} // namespace

// ---------- Refusals ----------

TEST_CASE("adaptive2d refuses what it cannot rough, naming the cause", "[cam][adaptive2d]")
{
    const AdaptiveCase base = islandsGoldenCase();
    SECTION("a tool that is not an end mill")
    {
        AdaptiveCase c = base;
        c.tool.type = lib::ToolType::BallEndMill;
        const Error e = refusal(c, true);
        CHECK(e.code == ErrorCode::InvalidArgument);
        CHECK(contains(e.message, "needs a flat or bull nose end mill"));
    }
    SECTION("a stepdown longer than the flutes")
    {
        AdaptiveCase c = withParams(base, R"({"stepdown":30})");
        const Error e = refusal(c, true);
        CHECK(contains(e.message, "is longer than the tool's flutes"));
    }
    SECTION("the smallest helix larger than the helix")
    {
        AdaptiveCase c = withParams(base, R"({"helixDiameter":50,"helixMinDiameter":60})");
        const Error e = refusal(c, true);
        CHECK(e.code == ErrorCode::InvalidArgument);
        CHECK(contains(e.message, "The smallest helix (60 %) is larger than the helix (50 %)"));
    }
    SECTION("a spindle turning counter-clockwise")
    {
        AdaptiveCase c = base;
        c.op.cutting.clockwise = false;
        CHECK(contains(refusal(c, true).message, "counter-clockwise"));
    }
    SECTION("heights out of order")
    {
        AdaptiveCase c = base;
        c.op.heights.feedPlane = -1;
        CHECK(contains(refusal(c, true).message, "must not be below the top height"));
    }
    SECTION("pre-drilled entry without holes, and holes narrower than the tool")
    {
        CHECK(contains(refusal(withParams(base, R"({"entry":"preDrill"})"), true).message, "none are given"));
        CHECK(contains(refusal(withParams(base, R"({"preDrillPoints":"20 15","preDrillDiameter":4})"), true).message,
                       "narrower than the 6 mm flat end mill"));
    }
    SECTION("a tool larger than the area")
    {
        AdaptiveCase c = base;
        c.tool = endMill(30, 30);
        const Error e = refusal(c, true);
        CHECK(e.code == ErrorCode::ToolpathFailed);
        CHECK(contains(e.message, "does not fit the area with its floor at Z15"));
    }
    SECTION("a slot too narrow for any entry names the width it needs")
    {
        // 6.1 mm with a 6 mm end mill: the tool centre has room, but no entry and no pass (the engine).
        const kernel::Shape part =
            cut(box(Vec3{0, 0, 0}, Vec3{50, 20, 10}), prism({obround(10, 6.95, 40, 13.05)}, 7, 11));
        AdaptiveCase c;
        c.op = adaptiveOp(R"({"stepdown":4,"stepover":20})");
        c.tool = endMill(6);
        c.geom = geometryOn(part, {upFace(part, 7)});
        CHECK(c.validate()); // the engine is not run by validate (op.h VALIDATE)
        const Error e = refusal(c, false);
        CHECK(e.code == ErrorCode::ToolpathFailed);
        INFO(e.message);
        CHECK(e.message.rfind("At Z7 in the area with its floor at Z7", 0) == 0);
        CHECK_FALSE(contains(e.message, "adaptive:"));
        CHECK((contains(e.message, "must be wider than") || contains(e.message, "needs the area to be at least")));
    }
    SECTION("a pre-drilled hole too small to start the passes in")
    {
        // 7 mm: at least the tool, but under the tool and the smallest helix (7.8 mm), which the passes need to start.
        AdaptiveCase c = withParams(
            preDrillGoldenCase(), R"({"stepdown":4,"entry":"preDrill","preDrillPoints":"20 15","preDrillDiameter":7})");
        const Error e = refusal(c, false);
        CHECK(e.code == ErrorCode::ToolpathFailed);
        INFO(e.message);
        CHECK(contains(e.message, "a pre-drilled hole needs to be at least 7.8"));
    }
}

TEST_CASE("adaptive2d's check runs the checks and gives the early warnings without the engine", "[cam][adaptive2d]")
{
    const AdaptiveCase c = withParams(islandsGoldenCase(), R"({"preDrillPoints":"20 15; 55 5","preDrillDiameter":9})");
    auto w = c.check();
    REQUIRE(w);
    REQUIRE(w->size() == 1);
    CHECK(w->front() == "The pre-drilled hole at X55 Y5 lies in none of the areas and is not used");
    // The same warning first in the toolpath's.
    auto p = c.generate();
    REQUIRE(p);
    REQUIRE_FALSE(p->warnings.empty());
    CHECK(p->warnings.front() == w->front());
}

// ---------- Depths, entries, stock to leave ----------

TEST_CASE("adaptive2d: equal steps no deeper than the stepdown, one engine run for straight walls", "[cam][adaptive2d]")
{
    const AdaptiveCase c = withParams(islandsGoldenCase(), R"({"stepdown":2,"stepover":30})");
    auto plan = c.plan();
    REQUIRE(plan);
    // Top 20, floor 15: ceil(5 / 2) = 3 steps of 5/3 mm, rounded to the nanometre half away from zero.
    REQUIRE(plan->depths.size() == 3);
    CHECK(plan->depths[0].z == 18'333'333);
    CHECK(plan->depths[1].z == 16'666'667);
    CHECK(plan->depths[2].z == 15'000'000);
    CHECK(plan->engineCalls == 1);
    CHECK(plan->depths[1].reused);
    CHECK(plan->depths[2].reused);
    const Toolpath p = c.path();
    // Every depth is cut, the next one entered by a helix from the one above (helix arcs at three levels).
    std::set<Nm> helixEnds;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Ramp && (m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW)) {
            helixEnds.insert(m.z);
        }
    }
    CHECK(helixEnds.count(18'333'333) == 1);
    CHECK(helixEnds.count(16'666'667) == 1);
    CHECK(helixEnds.count(15'000'000) == 1);
}

TEST_CASE("adaptive2d: a flat face adds a depth only where the area reaches it at that depth", "[cam][adaptive2d]")
{
    // pocket2d's rule (area_depths.h), shared: the same cases as pocket2d_test.cpp's, with adaptive2d's own room.
    const auto depthsOf = [](const kernel::Shape& part, double floorZ, double stepdown) {
        AdaptiveCase c;
        c.op = adaptiveOp(R"({"stepdown":)" + std::to_string(stepdown) + R"(,"stepover":20})");
        c.tool = endMill(6);
        c.geom = geometryOn(part, {upFace(part, floorZ)});
        JobContext ctx;
        auto plan = ops::planAdaptive2d(c.input(), ctx, ops::Adaptive2dStage::Regions);
        INFO((plan ? "" : plan.error().message));
        REQUIRE(plan);
        REQUIRE(plan->areas.size() == 1);
        return plan->areas[0].depths;
    };
    const kernel::Shape golden = goldenBlock();
    const kernel::Shape inner = outsidePlate(true, false);
    const kernel::Shape step = outsidePlate(true, true);
    const kernel::Shape ledge = outsidePlate(false, false, true);
    const kernel::Shape pad = padStepPart();
    // a low island inside the pocket: its top (Z 16) is a depth
    CHECK(depthsOf(golden, 12, 10) == std::vector<Nm>{16'000'000, 12'000'000});
    // a closed pocket inside the part, roughing its outside: no depth at its floor (Z 10)
    CHECK(depthsOf(inner, 5, 10) == std::vector<Nm>{5'000'000});
    // its floor at a depth the stepdown needs anyway: that depth
    CHECK(depthsOf(inner, 5, 5) == std::vector<Nm>{10'000'000, 5'000'000});
    // a step of the part open to the area (Z 11): a depth
    CHECK(depthsOf(step, 5, 10) == std::vector<Nm>{11'000'000, 5'000'000});
    // a step on a closed pocket's wall (Z 10), outside its footprint: no depth
    CHECK(depthsOf(ledge, 7, 10) == std::vector<Nm>{7'000'000});
    // a face only partly inside the area (the pad's top, Z 10): a depth
    CHECK(depthsOf(pad, 8, 5) == std::vector<Nm>{10'000'000, 8'000'000});

    // The outside of the plate with the inner pocket is roughed exactly as the plate without it.
    const kernel::Shape plain = outsidePlate(false, false);
    AdaptiveCase a, b;
    a.op = b.op = adaptiveOp(R"({"stepdown":10,"stepover":20})");
    a.tool = b.tool = endMill(6);
    a.geom = geometryOn(inner, {upFace(inner, 5)});
    b.geom = geometryOn(plain, {upFace(plain, 5)});
    auto pa = a.generate();
    auto pb = b.generate();
    REQUIRE(pa);
    REQUIRE(pb);
    CHECK(pa->hash() == pb->hash());
}

TEST_CASE("adaptive2d: the entry order of the setting, and the fall-back to a smaller helix", "[cam][adaptive2d]")
{
    SECTION("ramp first in a pocket where a helix would fit: a ramp, and no fall-back warning")
    {
        const AdaptiveCase c = withParams(islandsGoldenCase(), R"({"stepdown":5,"stepover":30,"entry":"ramp"})");
        auto plan = c.plan();
        REQUIRE(plan);
        for (const ops::Adaptive2dDepth& d : plan->depths) {
            CHECK(d.entries == std::vector<ad::EntryMode>{ad::EntryMode::Ramp});
        }
        const Toolpath p = c.path();
        CHECK(p.warnings.empty());
    }
    SECTION("a round pocket too small for the full helix and for a ramp: the smallest helix and up")
    {
        // 9 mm across with a 6 mm end mill: the tool centre has a circle of about 2.85 mm; the 4.5 mm helix does not
        // fit, nor a 6 mm ramp; a helix between the smallest (1.8 mm) and that does.
        const AdaptiveCase c = roundPocketCase(9, R"({"stepdown":4,"stepover":20})");
        auto plan = c.plan();
        REQUIRE(plan);
        REQUIRE(plan->depths.size() == 1);
        CHECK(plan->depths[0].entries == std::vector<ad::EntryMode>{ad::EntryMode::Helix});
        const Toolpath p = c.path();
        double radius = 0;
        for (const Move& m : p.moves) {
            if (m.role == MoveRole::Ramp && m.kind == MoveKind::ArcCCW) {
                radius = std::hypot(static_cast<double>(m.x - m.i), static_cast<double>(m.y - m.j)) / kNmPerMm;
            }
        }
        CHECK(radius >= 0.9 - 1e-6);
        CHECK(radius < 2.25);
        REQUIRE(p.warnings.size() == 1);
        CHECK(contains(p.warnings[0], "is entered by a helix"));
        CHECK(contains(p.warnings[0], "a helix of 4.5 mm"));
    }
    SECTION("the smallest helix made larger than the room: refused, naming what each entry needs")
    {
        const AdaptiveCase c = roundPocketCase(9, R"({"stepdown":4,"stepover":20,"helixMinDiameter":60})");
        const Error e = refusal(c, false);
        INFO(e.message);
        CHECK(contains(e.message, "a helix needs the area to be at least"));
        CHECK(contains(e.message, "a ramp needs a straight run of at least 6 mm"));
    }
}

TEST_CASE("adaptive2d leaves the stock asked for on the walls and the floor", "[cam][adaptive2d]")
{
    const AdaptiveCase c = withParams(islandsGoldenCase(),
                                      R"({"stepdown":5,"stepover":30,"stockToLeaveWall":0.3,"stockToLeaveFloor":0.2})");
    const Toolpath p = c.path(); // the checker: no gouge into the part grown by 0.3 on walls and 0.2 on the floor
    CHECK(p.stats.minZ == 15.2);
    // And the walls are left that thick at most: the claims with no stock to leave find no gouge either, and the
    // tool centre comes within 0.3 + its radius + 0.02 of the outer wall.
    double nearest = std::numeric_limits<double>::infinity();
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Cut && m.z == 15'200'000) {
            nearest = std::min(nearest, toMm(m.x) - 3.0);
        }
    }
    CHECK(nearest >= 3.3 - 0.01);
    CHECK(nearest <= 3.3 + 0.02);
}

// ---------- Links and feeds ----------

TEST_CASE("adaptive2d links: lifted tool-down links, retracts through linking, no rapid low over the stock",
          "[cam][adaptive2d]")
{
    const AdaptiveCase c = islandsGoldenCase();
    const Toolpath p = c.path(); // the checker: no rapid below the stock top over the stock (rapidLow, rapidStock)
    auto plan = c.plan();
    REQUIRE(plan);
    // Tool-down links: Link moves at the depth + 0.25 mm.
    std::size_t lifted = 0, low = 0;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Link && m.kind != MoveKind::Rapid) {
            lifted += m.z == 15'250'000 ? 1 : 0;
            low += m.z == 15'000'000 ? 1 : 0;
        }
    }
    CHECK(lifted > 0);
    CHECK(low > 0);
    // The engine's links that must leave the material cut the region into several passes; linking joined them.
    CHECK(plan->passes.size() > 1);
    for (const Move& m : p.moves) {
        if (m.kind == MoveKind::Rapid) {
            CHECK((m.z >= 22'000'000 || m.role == MoveRole::Retract || m.role == MoveRole::Approach));
        }
    }
}

TEST_CASE("adaptive2d cuts the flagged stretches at the reduced feed", "[cam][adaptive2d]")
{
    for (const auto& [percent, feed] : {std::pair{50, 600'000}, std::pair{70, 840'000}}) {
        const AdaptiveCase c = withParams(slotGoldenCase(), R"({"stepdown":4,"stepover":20,"reducedFeed":)" +
                                                                std::to_string(percent) + "}");
        const Toolpath p = c.path();
        std::set<std::int32_t> feeds;
        for (const Move& m : p.moves) {
            if (m.role == MoveRole::Cut) {
                feeds.insert(m.feedMicronPerMin);
            }
        }
        CHECK(feeds == std::set<std::int32_t>{feed, 1'200'000});
        // The ramp's line at depth, after the zigzag down, is a full-width cut: at the reduced feed.
        for (std::size_t i = 1; i < p.moves.size(); ++i) {
            if (p.moves[i - 1].role == MoveRole::Ramp && p.moves[i].role == MoveRole::Cut) {
                CHECK(p.moves[i].feedMicronPerMin == feed);
            }
        }
        // Ramps and helices at the ramp feed (0.75 x 1200 mm/min).
        for (const Move& m : p.moves) {
            if (m.role == MoveRole::Ramp) {
                CHECK(m.feedMicronPerMin == 900'000);
            }
        }
    }
}

// ---------- Arc fitting ----------

TEST_CASE("adaptive2d's fitted arcs stay within the fit tolerance of the engine's points and inside the area",
          "[cam][adaptive2d]")
{
    AdaptiveCase c = islandsGoldenCase();
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->outputs.size() == 1);
    // The engine's segments at the depth (cuts and tool-down links), nm.
    struct Seg {
        double ax, ay, bx, by;
    };
    std::vector<Seg> segs;
    for (const ad::Region& r : plan->outputs[0].regions) {
        for (const ad::Path& path : r.paths) {
            if (path.motion == ad::Motion::LinkRetract) {
                continue;
            }
            for (std::size_t i = 1; i < path.points.size(); ++i) {
                segs.push_back(Seg{static_cast<double>(path.points[i - 1].x), static_cast<double>(path.points[i - 1].y),
                                   static_cast<double>(path.points[i].x), static_cast<double>(path.points[i].y)});
            }
        }
    }
    const auto distance = [&](double x, double y) {
        double best = std::numeric_limits<double>::infinity();
        for (const Seg& s : segs) {
            const double dx = s.bx - s.ax, dy = s.by - s.ay;
            const double l2 = dx * dx + dy * dy;
            double t = l2 > 0 ? ((x - s.ax) * dx + (y - s.ay) * dy) / l2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            best = std::min(best, std::hypot(s.ax + t * dx - x, s.ay + t * dy - y));
        }
        return best;
    };
    JobContext ctx;
    auto tp = ops::adaptive2dToolpath(*plan, ctx);
    REQUIRE(tp);
    double worst = 0;
    std::size_t arcs = 0;
    Nm x = tp->startX, y = tp->startY;
    for (const Move& m : tp->moves) {
        const bool atDepth = (m.role == MoveRole::Cut || m.role == MoveRole::Link) && m.z == 15'000'000;
        if (atDepth && m.kind != MoveKind::Rapid) {
            // Samples every 0.1 mm along the move.
            const double len = moveLength(m, x, y);
            const int n = std::max(1, static_cast<int>(std::ceil(len / 0.1)));
            const bool arc = m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW;
            arcs += arc ? 1 : 0;
            const double r = std::hypot(static_cast<double>(x - m.i), static_cast<double>(y - m.j));
            const double a0 = std::atan2(static_cast<double>(y - m.j), static_cast<double>(x - m.i));
            const double a1 = std::atan2(static_cast<double>(m.y - m.j), static_cast<double>(m.x - m.i));
            double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
            while (sweep <= 0) {
                sweep += 2 * kPi;
            }
            for (int k = 0; k <= n; ++k) {
                const double t = static_cast<double>(k) / n;
                double px = 0, py = 0;
                if (!arc) {
                    px = static_cast<double>(x) + t * static_cast<double>(m.x - x);
                    py = static_cast<double>(y) + t * static_cast<double>(m.y - y);
                } else {
                    const double a = a0 + (m.kind == MoveKind::ArcCCW ? 1 : -1) * t * sweep;
                    px = static_cast<double>(m.i) + r * std::cos(a);
                    py = static_cast<double>(m.j) + r * std::sin(a);
                }
                worst = std::max(worst, distance(px, py));
            }
        }
        x = m.x;
        y = m.y;
    }
    INFO("largest distance from the engine's polylines: " << worst << " nm");
    CHECK(arcs > 100);
    CHECK(worst <= static_cast<double>(ops::kAdaptiveFitToleranceNm) + 2.0);
    // Inside the area: the tool centre within the walls less the tool radius (and the islands grown by it), to the
    // checker's 0.01 mm.
    invariants::Claims k = c.claims();
    k.containment = invariants::ContainClaim{
        invariants::Region::of({rectPoly(6, 6, 34, 24), circlePoly(13, 15, 6), rectPoly(20, 8, 32, 22, 4)}), 0.01};
    c.path(k);
}

TEST_CASE("adaptive2d on S5's pockets: at most one move per millimetre, p99 engagement and air cutting",
          "[cam][adaptive2d]")
{
    for (const char* id : {"p2", "p3"}) {
        for (const double load : {10.0, 20.0}) {
            AdaptiveCase c;
            c.op = adaptiveOp("{\"stepover\":" + std::to_string(load) + ",\"stepdown\":6}");
            c.tool = endMill(12.7, 30);
            const kernel::Shape part = s5Part(id, 5);
            c.geom = geometryOn(part, {upFace(part, 15)});
            c.op.tool.stickOut = 40;
            auto p = c.generate();
            REQUIRE(p);
            const MoveDensity d = cutDensity(*p);
            const double target = ad::angleForStepover(load / 100);
            WARN(id << " at " << load << " %: " << d.moves << " moves over " << d.lengthMm
                    << " mm at depth = " << d.perMm() << " per mm; p99 " << p->stats.p99EngagementDeg << " (target "
                    << target << "), max " << p->stats.maxEngagementDeg << ", air " << p->stats.airCutLengthMm
                    << " mm of " << p->stats.cutLengthMm << " mm fed");
            CHECK(d.perMm() <= 1.0);
            CHECK(p->stats.p99EngagementDeg >= target - 2);
            CHECK(p->stats.p99EngagementDeg <= target + 6);
            CHECK(p->stats.maxEngagementDeg >= p->stats.p99EngagementDeg);
            CHECK(p->stats.airCutLengthMm > 0);
            CHECK(p->stats.airCutLengthMm < 0.05 * p->stats.cutLengthMm);
            CHECK(p->warnings.empty());
        }
    }
}

// ---------- Stock slices ----------

TEST_CASE("adaptive2d with stock slices clears only what is left", "[cam][adaptive2d]")
{
    const AdaptiveCase base = islandsGoldenCase();
    const auto boxLoop = [](double x0, double y0, double x1, double y1) {
        return roundedRect(x0, y0, x1, y1, 0);
    };
    auto plain = base.generate();
    REQUIRE(plain);
    SECTION("slices that hold the whole stock box: the same bytes as without")
    {
        AdaptiveCase c = base;
        c.geom.stockSlices = std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>{
            {20.0, {boxLoop(0, 0, 40, 30)}}, {10.0, {boxLoop(0, 0, 40, 30)}}};
        auto p = c.generate();
        REQUIRE(p);
        CHECK(p->serialize() == plain->serialize());
    }
    SECTION("slices with the left of the pocket already gone: less to cut, entered from what is gone")
    {
        // Material left: the stock box less x 2..18 (across the whole pocket's height) from Z 10 up, the round island
        // (x 10..16) still standing in it.
        AdaptiveCase c = base;
        const std::vector<kernel::ProfileLoop> left = {boxLoop(0, 0, 2, 30), boxLoop(18, 0, 40, 30),
                                                       circleLoop(13, 15, 3)};
        c.geom.stockSlices =
            std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>{{20.0, left}, {10.0, left}};
        auto plan = c.plan();
        REQUIRE(plan);
        REQUIRE(plan->depths.size() == 1);
        CHECK(plan->depths[0].entries == std::vector<ad::EntryMode>{ad::EntryMode::FromCleared});
        REQUIRE_FALSE(plan->stock.cleared.empty());
        JobContext ctx;
        auto p = ops::adaptive2dToolpath(*plan, ctx);
        REQUIRE(p);
        CHECK(cutLengthMm(*p) < 0.75 * cutLengthMm(*plain));
        CHECK(countRole(*p, MoveRole::Ramp) == 0); // no helix: a descent where the material is gone
        // The checker reads the stock box, not the slices (README "What the checker cannot see"), so only the rules
        // that do not depend on the stock: no gouge, no rapid below the stock top over the stock. The descent where the
        // material is gone reads to it as a plunge into the stock box, as a pre-drilled hole does.
        invariants::Claims k = c.claims();
        k.clears.clear();
        k.centreCutting = true;
        c.path(k);
    }
    SECTION("slices that leave out the part: the part is still material")
    {
        // A slice that has only a strip at the left: everything else, the islands included, would be empty.
        AdaptiveCase c = base;
        c.geom.stockSlices =
            std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>{{10.0, {boxLoop(0, 0, 2, 30)}}};
        invariants::Claims k = c.claims();
        k.clears.clear();
        c.path(k); // no gouge into the islands or the walls
    }
    SECTION("a slice listed only above the depth tells the engine nothing there")
    {
        // Linking may use it (the material above Z 19 is gone: entries start lower), the engine does not.
        AdaptiveCase c = base;
        c.geom.stockSlices =
            std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>{{19.0, {boxLoop(0, 0, 2, 30)}}};
        auto plan = c.plan();
        auto before = base.plan();
        REQUIRE(plan);
        REQUIRE(before);
        REQUIRE(plan->outputs.size() == 1);
        CHECK(ad::toText(plan->outputs[0]) == ad::toText(before->outputs[0]));
        CHECK(plan->stock.cleared.size() == 1);
        CHECK(plan->stock.cleared[0].z == 19'000'000);
    }
}

// ---------- Statistics and warnings ----------

TEST_CASE("adaptive2d's statistics: the 99th percentile by length", "[cam][adaptive2d]")
{
    CHECK(ops::percentileByLength({}, 0.99) == 0);
    // 98 mm at 40 degrees, 1 mm at 50, 1 mm at 90: 99 % of the length is at or below 50.
    CHECK(ops::percentileByLength({{90, 1}, {40, 98}, {50, 1}}, 0.99) == 50);
    CHECK(ops::percentileByLength({{90, 2}, {40, 98}}, 0.99) == 90);
    const Toolpath p = slotGoldenCase().path();
    CHECK(p.stats.maxEngagementDeg == 180); // the ramp's line at depth, estimated as a full-width cut
    CHECK(p.stats.p99EngagementDeg > 0);
}

TEST_CASE("adaptive2d's warnings are in words for the machinist, with where and at which depths", "[cam][adaptive2d]")
{
    const Toolpath p = slotGoldenCase().path();
    REQUIRE(p.warnings.size() == 1);
    CHECK(p.warnings[0] == "At Z7: the area around X25 Y10 is entered by a ramp: a helix of 4.5 mm does not fit");
    // At two depths of the same slot: one line naming both.
    AdaptiveCase two = withParams(slotGoldenCase(), R"({"stepdown":1.5,"stepover":20})");
    auto q = two.generate();
    REQUIRE(q);
    REQUIRE(q->warnings.size() == 1);
    CHECK(q->warnings[0] ==
          "At Z8.5 and Z7: the area around X25 Y10 is entered by a ramp: a helix of 4.5 mm does not fit");
}

TEST_CASE("adaptive2d stops when cancelled", "[cam][adaptive2d]")
{
    const AdaptiveCase c = islandsGoldenCase();
    CancelSource source;
    source.cancel();
    JobContext ctx{source.token(), Progress{}};
    auto p = ops::makeAdaptive2dOperationType()->generate(c.input(), ctx);
    REQUIRE_FALSE(p);
    CHECK(p.error().code == ErrorCode::Cancelled);
}

// ---------- Run time ----------

TEST_CASE("adaptive2d's run time on S5's circle and islands pockets, and what the operation adds to the engine",
          "[cam][adaptive2d][wallclock]")
{
    using Clock = std::chrono::steady_clock;
    for (const auto& [id, limit] : {std::pair{"p2", 0.5}, std::pair{"p3", 3.0}}) {
        AdaptiveCase c;
        c.op = adaptiveOp(R"({"stepover":20,"stepdown":6})");
        c.tool = endMill(12.7, 30);
        c.op.tool.stickOut = 40;
        const kernel::Shape part = s5Part(id, 5);
        c.geom = geometryOn(part, {upFace(part, 15)});
        auto plan = c.plan();
        REQUIRE(plan);
        // The engine alone, on the input the operation gave it (op.h AT EACH DEPTH).
        ad::Input in;
        JobContext shrinkCtx;
        auto shrunk = geom2d::offset(plan->areas[0].region[0], -ops::kAdaptiveFitToleranceNm, shrinkCtx);
        REQUIRE(shrunk);
        in.regions = *shrunk;
        in.stock = {geom2d::canonicalize(
            geom2d::Region{geom2d::canonicalize(geom2d::Path{
                               geom2d::toPt(Vec2{c.geom.stockBounds.min.x, c.geom.stockBounds.min.y}),
                               {geom2d::Span{geom2d::SpanKind::Line,
                                             geom2d::toPt(Vec2{c.geom.stockBounds.max.x, c.geom.stockBounds.min.y})},
                                geom2d::Span{geom2d::SpanKind::Line,
                                             geom2d::toPt(Vec2{c.geom.stockBounds.max.x, c.geom.stockBounds.max.y})},
                                geom2d::Span{geom2d::SpanKind::Line,
                                             geom2d::toPt(Vec2{c.geom.stockBounds.min.x, c.geom.stockBounds.max.y})},
                                geom2d::Span{geom2d::SpanKind::Line,
                                             geom2d::toPt(Vec2{c.geom.stockBounds.min.x, c.geom.stockBounds.min.y})}},
                               true}),
                           {}})};
        in.toolDiameter = toNm(12.7);
        in.stepover = 0.2;
        in.stockToLeave = 0;
        in.helixDiameter = geom2d::roundNm(12.7e6 * 0.75);
        in.helixMinDiameter = geom2d::roundNm(12.7e6 * 0.3);
        double engine = 1e300, op = 1e300;
        for (int i = 0; i < 3; ++i) {
            JobContext ctx;
            const auto t0 = Clock::now();
            auto e = ad::planAdaptive(in, {}, ctx);
            const auto t1 = Clock::now();
            REQUIRE(e);
            auto p = c.generate();
            const auto t2 = Clock::now();
            REQUIRE(p);
            engine = std::min(engine, std::chrono::duration<double>(t1 - t0).count());
            op = std::min(op, std::chrono::duration<double>(t2 - t1).count());
        }
        WARN(id << " at 20 % load, one depth: the operation " << op << " s, the engine alone " << engine
                << " s, the operation adds " << op - engine << " s (PLAN.md section 9 High tier: " << limit << " s)");
        // What the operation adds (the part's slices, the areas, arc fitting, linking): small beside the engine.
        CHECK(op - engine <= 0.25);
        // The row itself: the engine's own test holds it (adaptive_acceptance_test.cpp); the operation within it plus
        // what it adds.
        CHECK(op <= std::max(limit, engine) + 0.25);
    }
}
