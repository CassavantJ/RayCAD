// Host motion services (src/post/engine_motion.h): arc splitting and linearisation, cycle expansion, cycle runs.
#include "engine_motion.h"
#include "post_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;
using Catch::Matchers::WithinAbs;

namespace {

struct Collect final : MotionSink {
    std::vector<MotionEvent> events;
    bool onEvent(const MotionEvent& e) override
    {
        events.push_back(e);
        return true;
    }
};

cam::Toolpath pathFrom(Nm x, Nm y, Nm z, std::vector<cam::Move> moves)
{
    cam::Toolpath p{};
    p.startX = x;
    p.startY = y;
    p.startZ = z;
    p.moves = std::move(moves);
    return p;
}

std::vector<MotionEvent> expand(const cam::Toolpath& p, const MotionCaps& caps)
{
    Collect c;
    const auto r = expandSection(p, 0, caps, c);
    REQUIRE(r.has_value());
    REQUIRE(*r);
    return c.events;
}

double distance(Nm ax, Nm ay, Nm bx, Nm by)
{
    const double dx = static_cast<double>(bx - ax);
    const double dy = static_cast<double>(by - ay);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

TEST_CASE("a full circle is split to maxArcDegrees", "[post][motion]")
{
    // Full circle of radius 10 mm about (0, 0), from (10, 0) back to (10, 0), counter-clockwise.
    const auto p = pathFrom(mm(10), 0, 0, {arc(false, mm(10), 0, 0, 0, 0, 0, mmPerMin(500))});
    for (const int maxDeg : {360, 180, 120, 90, 45}) {
        MotionCaps caps;
        caps.maxArcDegrees = maxDeg;
        const auto ev = expand(p, caps);
        INFO("maxArcDegrees " << maxDeg);
        REQUIRE(ev.size() == static_cast<std::size_t>(360 / maxDeg));
        Nm px = mm(10), py = 0;
        for (const MotionEvent& e : ev) {
            CHECK(e.kind == EventKind::Arc);
            CHECK(e.move == 0);
            CHECK_FALSE(e.cw);
            CHECK_THAT(e.sweepDegrees, WithinAbs(maxDeg, 1e-9));
            CHECK(e.cx == 0);
            CHECK(e.cy == 0);
            CHECK(e.i == -px); // centre minus this piece's start
            CHECK(e.j == -py);
            CHECK_THAT(e.radiusNm, WithinAbs(10e6, 1e-6));
            CHECK_THAT(distance(0, 0, e.x, e.y), WithinAbs(10e6, 1.0)); // on the circle, to the rounding of 1 nm
            px = e.x;
            py = e.y;
        }
        CHECK(ev.back().x == mm(10)); // the last piece ends exactly at the IR end point
        CHECK(ev.back().y == 0);
    }
    // At 90 degrees the pieces end on the axes, exactly.
    MotionCaps caps;
    caps.maxArcDegrees = 90;
    const auto ev = expand(p, caps);
    CHECK((ev[0].x == 0 && ev[0].y == mm(10)));
    CHECK((ev[1].x == -mm(10) && ev[1].y == 0));
    CHECK((ev[2].x == 0 && ev[2].y == -mm(10)));
}

TEST_CASE("arcs in four quadrants, both directions, keep their sweep", "[post][motion]")
{
    // Quarter arcs about (0, 0) radius 5: from each axis point to the next, clockwise and counter-clockwise.
    const std::array<std::pair<Nm, Nm>, 4> axis = {{{mm(5), 0}, {0, mm(5)}, {-mm(5), 0}, {0, -mm(5)}}};
    for (std::size_t q = 0; q < 4; ++q) {
        const auto [sx, sy] = axis[q];
        const auto [ex, ey] = axis[(q + 1) % 4];
        MotionCaps caps;
        const auto ccw = expand(pathFrom(sx, sy, 0, {arc(false, ex, ey, 0, 0, 0, 0, 1000)}), caps);
        REQUIRE(ccw.size() == 1);
        CHECK_THAT(ccw[0].sweepDegrees, WithinAbs(90, 1e-9));
        const auto cw = expand(pathFrom(sx, sy, 0, {arc(true, ex, ey, 0, 0, 0, 0, 1000)}), caps);
        REQUIRE(cw.size() == 1);
        CHECK_THAT(cw[0].sweepDegrees, WithinAbs(270, 1e-9)); // the long way round
        caps.maxArcDegrees = 180;
        CHECK(expand(pathFrom(sx, sy, 0, {arc(true, ex, ey, 0, 0, 0, 0, 1000)}), caps).size() == 2);
    }
}

TEST_CASE("R arcs are split below 180 degrees", "[post][motion]")
{
    MotionCaps caps;
    caps.arcs = ArcOutput::R;
    caps.maxArcDegrees = 360;
    // A half circle: one piece of 180 is not allowed with R words.
    const auto half = expand(pathFrom(mm(5), 0, 0, {arc(false, -mm(5), 0, 0, 0, 0, 0, 1000)}), caps);
    REQUIRE(half.size() == 2);
    CHECK_THAT(half[0].sweepDegrees, WithinAbs(90, 1e-9));
    const auto full = expand(pathFrom(mm(5), 0, 0, {arc(true, mm(5), 0, 0, 0, 0, 0, 1000)}), caps);
    CHECK(full.size() == 3);
}

TEST_CASE("helical arcs interpolate the axis; planes XZ and YZ", "[post][motion]")
{
    MotionCaps caps;
    caps.maxArcDegrees = 90;
    // Helix: full turn about (0, 0) from (2, 0, 0) down to z = -1 mm.
    const auto ev = expand(pathFrom(mm(2), 0, 0, {arc(false, mm(2), 0, -mm(1), 0, 0, 0, 1000)}), caps);
    REQUIRE(ev.size() == 4);
    CHECK(ev[0].z == -um(250));
    CHECK(ev[1].z == -um(500));
    CHECK(ev[2].z == -um(750));
    CHECK(ev[3].z == -mm(1));
    CHECK(ev[1].cz == -um(250)); // the centre's Z is the piece's start Z
    CHECK(ev[1].k == 0);

    // XZ plane (G18): centre (x, z) = (0, 0); from (0, 0, 3) to (3, 0, 0). Seen from +Y (Z to the right, X up) that
    // is a quarter turn counter-clockwise (G3); clockwise it is the other three quarters.
    caps.maxArcDegrees = 360;
    const auto xz = expand(pathFrom(0, 0, mm(3), {arc(false, mm(3), 0, 0, 0, 0, 0, 1000, cam::ArcPlane::XZ)}), caps);
    REQUIRE(xz.size() == 1);
    CHECK_THAT(xz[0].sweepDegrees, WithinAbs(90, 1e-9));
    const auto xzCw = expand(pathFrom(0, 0, mm(3), {arc(true, mm(3), 0, 0, 0, 0, 0, 1000, cam::ArcPlane::XZ)}), caps);
    CHECK_THAT(xzCw[0].sweepDegrees, WithinAbs(270, 1e-9));
    CHECK(xz[0].i == 0);
    CHECK(xz[0].j == 0);
    CHECK(xz[0].k == -mm(3));
    // YZ plane (G19): centre (y, z) = (0, 0); from (0, 3, 0) to (0, 0, 3): counter-clockwise seen from +X.
    const auto yz = expand(pathFrom(0, mm(3), 0, {arc(false, 0, 0, mm(3), 0, 0, 0, 1000, cam::ArcPlane::YZ)}), caps);
    REQUIRE(yz.size() == 1);
    CHECK_THAT(yz[0].sweepDegrees, WithinAbs(90, 1e-9));
    CHECK(yz[0].j == -mm(3));
}

TEST_CASE("arcs are linearised within tolerance when the post has no arcs or no helix", "[post][motion]")
{
    const auto p = pathFrom(mm(10), 0, 0, {arc(false, -mm(10), 0, -mm(2), 0, 0, 0, mmPerMin(800))});
    for (const bool noArcs : {true, false}) {
        MotionCaps caps;
        if (noArcs) {
            caps.arcs = ArcOutput::None;
        } else {
            caps.helical = false;
        }
        const auto ev = expand(p, caps);
        REQUIRE(ev.size() > 10);
        Nm px = mm(10), py = 0;
        for (const MotionEvent& e : ev) {
            REQUIRE(e.kind == EventKind::Linear);
            CHECK(e.feed == mmPerMin(800));
            CHECK(e.move == 0);
            // The chord's midpoint is within the tolerance of the circle.
            const double mx = (static_cast<double>(px) + static_cast<double>(e.x)) / 2;
            const double my = (static_cast<double>(py) + static_cast<double>(e.y)) / 2;
            const double sagitta = 10e6 - std::sqrt(mx * mx + my * my);
            CHECK(sagitta <= static_cast<double>(kLinearizeToleranceNm) + 2.0);
            px = e.x;
            py = e.y;
        }
        CHECK(ev.back().x == -mm(10));
        CHECK(ev.back().z == -mm(2));
    }
    // A flat arc with helical off stays an arc.
    MotionCaps caps;
    caps.helical = false;
    CHECK(expand(pathFrom(mm(10), 0, 0, {arc(false, -mm(10), 0, 0, 0, 0, 0, 1000)}), caps).size() == 1);
}

TEST_CASE("declared cycles run modally and end with CycleEnd", "[post][motion]")
{
    MotionCaps caps;
    caps.cycles[static_cast<std::size_t>(CycleKind::Drill)] = true;
    caps.cycles[static_cast<std::size_t>(CycleKind::Peck)] = true;
    const auto p = pathFrom(0, 0, mm(25),
                            {rapid(0, 0, mm(10)),                                                     // 0
                             cycle(cam::MoveKind::CycleDrill, mm(5), mm(5), -mm(3), mm(2), 200000),   // 1 first
                             cycle(cam::MoveKind::CycleDrill, mm(10), mm(5), -mm(3), mm(2), 200000),  // 2
                             cycle(cam::MoveKind::CycleDrill, mm(15), mm(5), -mm(4), mm(2), 200000),  // 3 deeper
                             cycle(cam::MoveKind::CyclePeck, mm(20), mm(5), -mm(9), mm(2), 150000, mm(2)), // 4 kind
                             cycleCancel(),                                                           // 5
                             cycle(cam::MoveKind::CycleDrill, mm(5), mm(10), -mm(3), mm(2), 200000),  // 6
                             rapid(0, 0, mm(25))});                                                   // 7
    const auto ev = expand(p, caps);
    std::vector<std::pair<EventKind, std::uint32_t>> seq;
    for (const auto& e : ev) {
        seq.emplace_back(e.kind, e.move);
    }
    const std::vector<std::pair<EventKind, std::uint32_t>> want = {
        {EventKind::Rapid, 0}, {EventKind::Cycle, 1}, {EventKind::Cycle, 2},    {EventKind::Cycle, 3},
        {EventKind::CycleEnd, 3}, {EventKind::Cycle, 4}, {EventKind::CycleEnd, 5}, {EventKind::Cycle, 6},
        {EventKind::CycleEnd, 6}, {EventKind::Rapid, 7}};
    CHECK(seq == want);
    CHECK(ev[1].first);
    CHECK_FALSE(ev[2].first);
    CHECK(ev[3].first); // the bottom changed
    CHECK(ev[5].first);
    CHECK(ev[5].cycle == CycleKind::Peck);
    CHECK(ev[5].peck == mm(2));
    CHECK(ev[5].clearance == mm(2)); // G99: the drill run left the tool at its R plane
    CHECK(ev[1].clearance == mm(10)); // the Z before the run
    CHECK(ev[1].retract == mm(2));
    CHECK(ev[1].z == -mm(3));
    CHECK(ev[7].first);
}

TEST_CASE("undeclared cycles are expanded into plain moves", "[post][motion]")
{
    MotionCaps caps; // no cycles declared
    auto kinds = [](const std::vector<MotionEvent>& ev) {
        std::vector<std::pair<EventKind, Nm>> out;
        for (const auto& e : ev) {
            out.emplace_back(e.kind, e.z);
        }
        return out;
    };
    using K = EventKind;
    // cycle_conventions: G99, the tool ends each hole at the R plane.
    SECTION("drill with dwell")
    {
        const auto ev = expand(pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleDrill, mm(5), 0, -mm(3), mm(2),
                                                             100000, 0, 500)}),
                               caps);
        const std::vector<std::pair<K, Nm>> want = {
            {K::Rapid, mm(10)}, {K::Rapid, mm(2)}, {K::Linear, -mm(3)}, {K::Dwell, -mm(3)}, {K::Rapid, mm(2)}};
        CHECK(kinds(ev) == want);
        CHECK(ev[0].x == mm(5));
        CHECK(ev[2].feed == 100000);
        CHECK(ev[2].role == cam::MoveRole::Plunge);
        CHECK(ev[3].dwellMs == 500);
    }
    SECTION("peck: full retract, back down to the re-entry height")
    {
        const auto ev = expand(
            pathFrom(mm(5), 0, mm(10), {cycle(cam::MoveKind::CyclePeck, mm(5), 0, -mm(5), mm(1), 100000, mm(2))}),
            caps);
        const Nm re = kPeckReentryNm;
        const std::vector<std::pair<K, Nm>> want = {
            {K::Rapid, mm(1)},       {K::Linear, -mm(1)}, {K::Rapid, mm(1)},  {K::Rapid, -mm(1) + re},
            {K::Linear, -mm(3)},     {K::Rapid, mm(1)},   {K::Rapid, -mm(3) + re}, {K::Linear, -mm(5)},
            {K::Rapid, mm(1)}};
        CHECK(kinds(ev) == want);
    }
    SECTION("chip break: back off between pecks")
    {
        const auto ev = expand(pathFrom(mm(5), 0, mm(10), {cycle(cam::MoveKind::CycleChipBreak, mm(5), 0, -mm(3),
                                                                 mm(1), 100000, mm(2))}),
                               caps);
        const Nm back = kChipBreakBackoffNm;
        const std::vector<std::pair<K, Nm>> want = {{K::Rapid, mm(1)},         {K::Linear, -mm(1)},
                                                    {K::Rapid, -mm(1) + back}, {K::Linear, -mm(3)},
                                                    {K::Rapid, mm(1)}};
        CHECK(kinds(ev) == want);
    }
    SECTION("bore and ream feed out")
    {
        for (const auto kind : {cam::MoveKind::CycleBore, cam::MoveKind::CycleReam}) {
            const auto ev = expand(pathFrom(mm(5), 0, mm(10), {cycle(kind, mm(5), 0, -mm(3), mm(1), 100000)}), caps);
            const std::vector<std::pair<K, Nm>> want = {
                {K::Rapid, mm(1)}, {K::Linear, -mm(3)}, {K::Linear, mm(1)}};
            CHECK(kinds(ev) == want);
            CHECK(ev[2].role == cam::MoveRole::Retract);
        }
    }
    SECTION("peck 0 drills in one feed")
    {
        const auto ev =
            expand(pathFrom(mm(5), 0, mm(10), {cycle(cam::MoveKind::CyclePeck, mm(5), 0, -mm(3), mm(1), 100000)}), caps);
        CHECK(ev.size() == 3);
    }
}

TEST_CASE("toolpaths the post cannot take are refused", "[post][motion]")
{
    MotionCaps caps;
    Collect c;
    auto tap = pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleTap, 0, 0, -mm(10), mm(2), 1250 * 1000, 0, 0, 1250000)});
    auto r = expandSection(tap, 0, caps, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PostFailed);
    CHECK(r.error().message.find("tapping") != std::string::npos);

    auto comp = pathFrom(0, 0, 0, {linear(mm(1), 0, 0, 1000, cam::MoveRole::LeadIn, 1)});
    r = expandSection(comp, 0, caps, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("cutter compensation") != std::string::npos);
    caps.cutterComp = true;
    CHECK(expandSection(comp, 0, caps, c).has_value());

    auto noFeed = pathFrom(0, 0, 0, {linear(mm(1), 0, 0, 0)});
    CHECK_FALSE(expandSection(noFeed, 0, caps, c).has_value());

    auto upside = pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleDrill, 0, 0, mm(3), mm(2), 1000)});
    CHECK_FALSE(expandSection(upside, 0, caps, c).has_value());
}

TEST_CASE("a sink that stops ends the expansion", "[post][motion]")
{
    struct StopAfter final : MotionSink {
        int left = 2;
        bool onEvent(const MotionEvent&) override { return --left > 0; }
    } sink;
    auto p = pathFrom(0, 0, 0, {rapid(1, 0, 0), rapid(2, 0, 0), rapid(3, 0, 0)});
    const auto r = expandSection(p, 0, MotionCaps{}, sink);
    REQUIRE(r.has_value());
    CHECK_FALSE(*r);
    CHECK(sink.left == 0);
}

TEST_CASE("section stats", "[post][motion]")
{
    auto p = pathFrom(0, 0, mm(10), {rapid(0, 0, mm(2)), linear(mm(10), 0, mm(2), mmPerMin(600)),
                                     arc(false, -mm(10), 0, mm(2), 0, 0, 0, mmPerMin(600)), dwell(1500),
                                     linear(-mm(10), 0, -mm(4), mmPerMin(600))});
    const SectionStats s = sectionStats(p, 6000);
    CHECK(s.minZ == -mm(4));
    CHECK(s.maxZ == mm(10));
    const double cut = 10e6 + 10e6 * 3.14159265358979323846 + 6e6;
    CHECK_THAT(s.cutLengthNm, WithinAbs(cut, 1.0));
    // Feed moves at 600 mm/min, the rapid of 8 mm at 6000 mm/min, 1.5 s of dwell.
    CHECK_THAT(s.seconds, WithinAbs(cut / 1e6 / 600 * 60 + 8.0 / 6000 * 60 + 1.5, 1e-9));
}
