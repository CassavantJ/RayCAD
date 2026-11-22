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
    const auto r = expandSection(p, nullptr, caps, c);
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
    // Top 0, clearance 10 mm (where the tool is), G99 except before a new run.
    const auto at10 = [](cam::Move m, cam::CycleReturn ret = cam::CycleReturn::RPlane) {
        return levels(m, 0, mm(10), ret);
    };
    const auto g98 = cam::CycleReturn::Initial;
    const auto p =
        pathFrom(0, 0, mm(25),
                 {rapid(0, 0, mm(10)),                                                                     // 0
                  at10(cycle(cam::MoveKind::CycleDrill, mm(5), mm(5), -mm(3), mm(2), 200000)),             // 1 first
                  at10(cycle(cam::MoveKind::CycleDrill, mm(10), mm(5), -mm(3), mm(2), 200000)),            // 2
                  at10(cycle(cam::MoveKind::CycleDrill, mm(15), mm(5), -mm(4), mm(2), 200000), g98),       // 3 deeper
                  at10(cycle(cam::MoveKind::CyclePeck, mm(20), mm(5), -mm(9), mm(2), 150000, mm(2)), g98), // 4 kind
                  cycleCancel(),                                                                           // 5
                  at10(cycle(cam::MoveKind::CycleDrill, mm(5), mm(10), -mm(3), mm(2), 200000)),            // 6
                  rapid(0, 0, mm(25))});                                                                   // 7
    const auto ev = expand(p, caps);
    std::vector<std::pair<EventKind, std::uint32_t>> seq;
    for (const auto& e : ev) {
        seq.emplace_back(e.kind, e.move);
    }
    const std::vector<std::pair<EventKind, std::uint32_t>> want = {
        {EventKind::Rapid, 0},    {EventKind::Cycle, 1}, {EventKind::Cycle, 2},    {EventKind::Cycle, 3},
        {EventKind::CycleEnd, 3}, {EventKind::Cycle, 4}, {EventKind::CycleEnd, 5}, {EventKind::Cycle, 6},
        {EventKind::CycleEnd, 6}, {EventKind::Rapid, 7}};
    CHECK(seq == want);
    CHECK(ev[1].first);
    CHECK_FALSE(ev[2].first);
    CHECK(ev[3].first); // the bottom changed
    CHECK(ev[5].first);
    CHECK(ev[5].cycle == CycleKind::Peck);
    CHECK(ev[5].peck == mm(2));
    CHECK(ev[5].clearance == mm(10)); // the IR's initial level
    CHECK(ev[1].clearance == mm(10));
    CHECK(ev[1].retract == mm(2));
    CHECK(ev[1].top == 0);
    CHECK(ev[1].z == -mm(3));
    CHECK(ev[1].returnLevel == cam::CycleReturn::RPlane);
    CHECK(ev[3].returnLevel == cam::CycleReturn::Initial);
    CHECK(ev[4].x == mm(15)); // CycleEnd: the tool at the last hole's return level, the clearance (G98)
    CHECK(ev[4].z == mm(10));
    CHECK(ev[7].first);
}

TEST_CASE("declared cycles: return levels, the IR's values, runs and the approach to the clearance", "[post][motion]")
{
    MotionCaps caps;
    for (std::size_t k = 0; k < kCycleKindCount; ++k) {
        caps.cycles[k] = true;
    }
    using K = EventKind;
    SECTION("the return level is per hole and does not make a hole `first`")
    {
        const auto p = pathFrom(0, 0, mm(10),
                                {levels(cycle(cam::MoveKind::CycleDrill, mm(5), 0, -mm(3), mm(2), 1000), 0, mm(10)),
                                 levels(cycle(cam::MoveKind::CycleDrill, mm(9), 0, -mm(3), mm(2), 1000), 0, mm(10),
                                        cam::CycleReturn::Initial),
                                 dwell(10)});
        const auto ev = expand(p, caps);
        REQUIRE(ev.size() == 4);
        CHECK(ev[0].first);
        CHECK_FALSE(ev[1].first);
        CHECK(ev[1].returnLevel == cam::CycleReturn::Initial);
        CHECK(ev[2].kind == K::CycleEnd);
        CHECK(ev[3].kind == K::Dwell); // the dwell happens where the last hole left the tool: (9, 0, clearance)
        CHECK((ev[3].x == mm(9) && ev[3].y == 0 && ev[3].z == mm(10)));
    }
    SECTION("above the clearance, rapids over the hole and down to it come first")
    {
        cam::Move tap = levels(cycle(cam::MoveKind::CycleTap, mm(5), mm(6), -mm(8), mm(3), 625000, 0, 0, 1250000), 0,
                               mm(5), cam::CycleReturn::Initial);
        tap.cycleLeftHand = 1;
        tap.cyclePeckClearance = um(200);
        const auto ev = expand(pathFrom(0, 0, mm(30), {tap}), caps);
        REQUIRE(ev.size() == 4);
        CHECK((ev[0].kind == K::Rapid && ev[0].x == mm(5) && ev[0].y == mm(6) && ev[0].z == mm(30)));
        CHECK((ev[1].kind == K::Rapid && ev[1].x == mm(5) && ev[1].y == mm(6) && ev[1].z == mm(5)));
        CHECK(ev[0].move == 0);
        REQUIRE(ev[2].kind == K::Cycle);
        CHECK(ev[2].leftHand);
        CHECK(ev[2].pitchNm == 1250000);
        CHECK(ev[2].peckClearance == um(200)); // as in the IR; 0 would leave it to the control
        CHECK(ev[2].clearance == mm(5));
        CHECK(ev[3].kind == K::CycleEnd);
        CHECK(ev[3].z == mm(5)); // G98
    }
    SECTION("a new left-hand flag or clearance starts a new run")
    {
        cam::Move right = levels(cycle(cam::MoveKind::CycleTap, mm(5), 0, -mm(8), mm(3), 625000, 0, 0, 1250000), 0,
                                 mm(5), cam::CycleReturn::Initial);
        cam::Move left = right;
        left.x = mm(15);
        left.cycleLeftHand = 1;
        cam::Move lower = left;
        lower.x = mm(25);
        lower.cycleClearance = mm(4);
        const auto ev = expand(pathFrom(mm(5), 0, mm(5), {right, left, lower}), caps);
        std::vector<K> kinds;
        for (const auto& e : ev) {
            kinds.push_back(e.kind);
        }
        // The third run starts above its clearance (at 5, the second's G98 level): a rapid over it and one down.
        const std::vector<K> want = {K::Cycle, K::CycleEnd, K::Cycle, K::CycleEnd,
                                     K::Rapid, K::Rapid,    K::Cycle, K::CycleEnd};
        CHECK(kinds == want);
        CHECK(ev[5].z == mm(4));
    }
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
    // cam.h: G99 here (the tool ends each hole at the R plane); the clearance is the R plane unless set.
    SECTION("drill with dwell")
    {
        const auto ev = expand(
            pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleDrill, mm(5), 0, -mm(3), mm(2), 100000, 0, 500)}), caps);
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
        const Nm re = cam::kDefaultPeckClearanceNm; // neither the move nor a machine says
        const std::vector<std::pair<K, Nm>> want = {{K::Rapid, mm(1)},       {K::Linear, -mm(1)}, {K::Rapid, mm(1)},
                                                    {K::Rapid, -mm(1) + re}, {K::Linear, -mm(3)}, {K::Rapid, mm(1)},
                                                    {K::Rapid, -mm(3) + re}, {K::Linear, -mm(5)}, {K::Rapid, mm(1)}};
        CHECK(kinds(ev) == want);
    }
    SECTION("chip break: back off between pecks")
    {
        const auto ev = expand(
            pathFrom(mm(5), 0, mm(10), {cycle(cam::MoveKind::CycleChipBreak, mm(5), 0, -mm(3), mm(1), 100000, mm(2))}),
            caps);
        const Nm back = cam::kDefaultPeckClearanceNm;
        const std::vector<std::pair<K, Nm>> want = {
            {K::Rapid, mm(1)}, {K::Linear, -mm(1)}, {K::Rapid, -mm(1) + back}, {K::Linear, -mm(3)}, {K::Rapid, mm(1)}};
        CHECK(kinds(ev) == want);
    }
    SECTION("the peck clearance: the move's, else the machine's, else 0.5 mm")
    {
        lib::MachineDef machine = makeMachine();
        machine.peckClearance = 0.25;
        cam::Move m = cycle(cam::MoveKind::CycleChipBreak, mm(5), 0, -mm(3), mm(1), 100000, mm(2));
        const auto path = pathFrom(mm(5), 0, mm(10), {m});
        const auto backOff = [&](const cam::Toolpath& p, const lib::MachineDef* def) {
            Collect c;
            REQUIRE(expandSection(p, def, caps, c).value_or(false));
            REQUIRE(c.events.size() == 5);
            return c.events[2].z - c.events[1].z;
        };
        CHECK(backOff(path, nullptr) == cam::kDefaultPeckClearanceNm);
        CHECK(backOff(path, &machine) == um(250));
        m.cyclePeckClearance = um(100);
        CHECK(backOff(pathFrom(mm(5), 0, mm(10), {m}), &machine) == um(100));
    }
    SECTION("G98: out to the clearance; a run that starts above it comes down to it first")
    {
        const Nm c = mm(6);
        const auto g98 = cam::CycleReturn::Initial;
        const auto drill = expand(
            pathFrom(0, 0, mm(20),
                     {levels(cycle(cam::MoveKind::CycleDrill, mm(5), 0, -mm(3), mm(1), 100000), -mm(1), c, g98)}),
            caps);
        const std::vector<std::pair<K, Nm>> want = {
            {K::Rapid, mm(20)}, {K::Rapid, c}, {K::Rapid, mm(1)}, {K::Linear, -mm(3)}, {K::Rapid, c}};
        CHECK(kinds(drill) == want);
        // Bore: feed out to R, then rapid to the clearance.
        const auto bore =
            expand(pathFrom(mm(5), 0, c,
                            {levels(cycle(cam::MoveKind::CycleBore, mm(5), 0, -mm(3), mm(1), 100000), -mm(1), c, g98)}),
                   caps);
        const std::vector<std::pair<K, Nm>> wantBore = {
            {K::Rapid, mm(1)}, {K::Linear, -mm(3)}, {K::Linear, mm(1)}, {K::Rapid, c}};
        CHECK(kinds(bore) == wantBore);
    }
    SECTION("bore and ream feed out")
    {
        for (const auto kind : {cam::MoveKind::CycleBore, cam::MoveKind::CycleReam}) {
            const auto ev = expand(pathFrom(mm(5), 0, mm(10), {cycle(kind, mm(5), 0, -mm(3), mm(1), 100000)}), caps);
            const std::vector<std::pair<K, Nm>> want = {{K::Rapid, mm(1)}, {K::Linear, -mm(3)}, {K::Linear, mm(1)}};
            CHECK(kinds(ev) == want);
            CHECK(ev[2].role == cam::MoveRole::Retract);
        }
    }
    SECTION("peck 0 drills in one feed")
    {
        const auto ev = expand(
            pathFrom(mm(5), 0, mm(10), {cycle(cam::MoveKind::CyclePeck, mm(5), 0, -mm(3), mm(1), 100000)}), caps);
        CHECK(ev.size() == 3);
    }
}

TEST_CASE("toolpaths the post cannot take are refused", "[post][motion]")
{
    MotionCaps caps;
    Collect c;
    auto tap =
        pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleTap, 0, 0, -mm(10), mm(2), 1250 * 1000, 0, 0, 1250000)});
    auto r = expandSection(tap, nullptr, caps, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::PostFailed);
    CHECK(r.error().message.find("tapping") != std::string::npos);

    auto comp = pathFrom(0, 0, 0, {linear(mm(1), 0, 0, 1000, cam::MoveRole::LeadIn, 1)});
    r = expandSection(comp, nullptr, caps, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("cutter compensation") != std::string::npos);
    caps.cutterComp = true;
    CHECK(expandSection(comp, nullptr, caps, c).has_value());

    auto noFeed = pathFrom(0, 0, 0, {linear(mm(1), 0, 0, 0)});
    CHECK_FALSE(expandSection(noFeed, nullptr, caps, c).has_value());

    auto upside = pathFrom(0, 0, mm(10), {cycle(cam::MoveKind::CycleDrill, 0, 0, mm(3), mm(2), 1000)});
    CHECK_FALSE(expandSection(upside, nullptr, caps, c).has_value());

    // cam.h's level order and run rule, declared or expanded.
    for (const bool declared : {false, true}) {
        INFO("declared " << declared);
        MotionCaps cycles;
        cycles.cycles[static_cast<std::size_t>(CycleKind::Drill)] = declared;
        const cam::Move ok = levels(cycle(cam::MoveKind::CycleDrill, 0, 0, -mm(3), mm(2), 1000), 0, mm(5));
        CHECK(expandSection(pathFrom(0, 0, mm(5), {ok}), nullptr, cycles, c).has_value());
        cam::Move topAboveR = ok;
        topAboveR.cycleTop = mm(3);
        auto bad = expandSection(pathFrom(0, 0, mm(5), {topAboveR}), nullptr, cycles, c);
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message.find("out of order") != std::string::npos);
        cam::Move rAboveClearance = ok;
        rAboveClearance.cycleRetract = mm(6);
        CHECK_FALSE(expandSection(pathFrom(0, 0, mm(9), {rAboveClearance}), nullptr, cycles, c).has_value());
        // The run starts with the tool below its clearance; and a second run after a G99 hole does too.
        bad = expandSection(pathFrom(0, 0, mm(4), {ok}), nullptr, cycles, c);
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message.find("move 0 starts a cycle run with the tool below") != std::string::npos);
        cam::Move other = ok;
        other.cycleClearance = mm(4);
        bad = expandSection(pathFrom(0, 0, mm(5), {ok, other}), nullptr, cycles, c);
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message.find("move 1 starts") != std::string::npos);
        CHECK(expandSection(pathFrom(0, 0, mm(5), {ok, ok}), nullptr, cycles, c).has_value()); // one run: fine
    }
}

TEST_CASE("a sink that stops ends the expansion", "[post][motion]")
{
    struct StopAfter final : MotionSink {
        int left = 2;
        bool onEvent(const MotionEvent&) override { return --left > 0; }
    } sink;
    auto p = pathFrom(0, 0, 0, {rapid(1, 0, 0), rapid(2, 0, 0), rapid(3, 0, 0)});
    const auto r = expandSection(p, nullptr, MotionCaps{}, sink);
    REQUIRE(r.has_value());
    CHECK_FALSE(*r);
    CHECK(sink.left == 0);
}

TEST_CASE("section stats", "[post][motion]")
{
    auto p = pathFrom(0, 0, mm(10),
                      {rapid(0, 0, mm(2)), linear(mm(10), 0, mm(2), mmPerMin(600)),
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
