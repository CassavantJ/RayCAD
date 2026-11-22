// Cycle-time estimate (task T-059, cam.h estimateCycleTime, src/cam/cycletime.cpp).
//
// The hand-calculated cases run on machine M below; each states its calculation, and the estimate must match it to
// 1 % (the model is exact for them, so they agree to rounding). The profile formulas (cycletime.cpp "Profile"):
//   from rest to rest over L at cruise v and acceleration A:
//     reaches cruise when L >= v^2 / A:   t = L / v + v / A
//     triangle otherwise:                 t = 2 sqrt(L / A)
//   between other speeds:  t = (v - vs)/A + (v - ve)/A + (L - (v^2 - vs^2)/2A - (v^2 - ve^2)/2A) / v
//
// Machine M (round numbers, not any real machine):
//   X, Y   rapid 12000 mm/min = 200 mm/s, max feed 6000 mm/min = 100 mm/s, acceleration 1000 mm/s^2
//   Z      rapid  6000 mm/min = 100 mm/s, max feed 3000 mm/min =  50 mm/s, acceleration  500 mm/s^2
//   tool change 3 s; peck clearance not set (the 0.5 mm fallback)
//
// Also: determinism (a golden of the result's bits, the same on every platform), the 1,000,000-move budget
// ([wallclock]: under 200 ms in an optimised build), and the comparison with times measured on real machines
// (tests/data/cycletime/measured.tsv; skipped while it has no records).
#include <ray/cam.h>
#include <ray/libraries.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam;

namespace {

Nm nm(double mm)
{
    return static_cast<Nm>(std::llround(mm * 1e6));
}

std::int32_t umMin(double mmMin)
{
    return static_cast<std::int32_t>(std::llround(mmMin * 1000.0));
}

lib::AxisDef axis(char letter, double rapid, double maxFeed, double accel)
{
    lib::AxisDef a;
    a.letter = letter;
    a.minTravel = -1000;
    a.maxTravel = 1000;
    a.rapidMmMin = rapid;
    a.maxFeedMmMin = maxFeed;
    a.accelMmS2 = accel;
    return a;
}

lib::MachineDef machineM()
{
    lib::MachineDef m;
    m.axes = {axis('X', 12000, 6000, 1000), axis('Y', 12000, 6000, 1000), axis('Z', 6000, 3000, 500)};
    m.toolChangeSec = 3;
    return m;
}

Move rapid(double x, double y, double z)
{
    Move m{};
    m.kind = MoveKind::Rapid;
    m.role = MoveRole::Link;
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    return m;
}

Move line(double x, double y, double z, double feedMmMin)
{
    Move m{};
    m.kind = MoveKind::Linear;
    m.role = MoveRole::Cut;
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    m.feedMicronPerMin = umMin(feedMmMin);
    return m;
}

Move arc(bool ccw, ArcPlane plane, double x, double y, double z, double i, double j, double k, double feedMmMin)
{
    Move m{};
    m.kind = ccw ? MoveKind::ArcCCW : MoveKind::ArcCW;
    m.role = MoveRole::Cut;
    m.plane = plane;
    m.x = nm(x), m.y = nm(y), m.z = nm(z);
    m.i = nm(i), m.j = nm(j), m.k = nm(k);
    m.feedMicronPerMin = umMin(feedMmMin);
    return m;
}

Move dwellMove(std::int32_t ms)
{
    Move m{};
    m.kind = MoveKind::Dwell;
    m.cycleDwellMs = ms;
    return m;
}

// A hole at (x, y): top 0, R 2, clearance 10, bottom `bottom`.
Move hole(MoveKind kind, double x, double y, double bottom, CycleReturn ret, double peck, double feedMmMin,
          std::int32_t dwellMs = 0)
{
    Move m{};
    m.kind = kind;
    m.role = MoveRole::Plunge;
    m.x = nm(x), m.y = nm(y), m.z = nm(bottom);
    m.cycleTop = 0;
    m.cycleRetract = nm(2);
    m.cycleClearance = nm(10);
    m.cycleReturn = ret;
    m.cyclePeck = nm(peck);
    m.cycleDwellMs = dwellMs;
    m.feedMicronPerMin = umMin(feedMmMin);
    return m;
}

Toolpath path(std::vector<Move> moves, double sx = 0, double sy = 0, double sz = 0)
{
    Toolpath tp{};
    tp.startX = nm(sx), tp.startY = nm(sy), tp.startZ = nm(sz);
    tp.moves = std::move(moves);
    return tp;
}

// One operation (tool 1) on M: its part without the first tool change.
CycleTime one(const Toolpath& tp, const lib::MachineDef& m = machineM(), const CycleTimeOptions& o = {})
{
    const CycleTimeOperation op{&tp, 1};
    return estimateCycleTime(std::span<const CycleTimeOperation>(&op, 1), m, o);
}

// Within 1 % of the hand calculation.
void near(double got, double hand)
{
    CHECK(got == Catch::Approx(hand).epsilon(0.01));
}

} // namespace

// ---------- rapids ----------

TEST_CASE("cycle time 1: a long rapid reaches its rapid rate", "[cycletime]")
{
    // X 0 -> 100. V = 200 mm/s, A = 1000 mm/s^2: V^2/A = 40 mm <= 100, so it cruises:
    //   t = 100/200 + 200/1000 = 0.5 + 0.2 = 0.7 s.
    const CycleTime t = one(path({rapid(100, 0, 0)}));
    near(t.rapidSec, 0.7);
    near(t.rapidMm, 100);
    CHECK(t.cutSec == 0);
    // The first operation loads its tool: 3 s.
    CHECK(t.toolChanges == 1);
    near(t.toolChangeSec, 3);
    near(t.totalSec, 3.7);
}

TEST_CASE("cycle time 2: a short rapid is a triangle", "[cycletime]")
{
    // X 0 -> 10: 10 < V^2/A = 40 mm, so it never reaches 200 mm/s: t = 2 sqrt(10/1000) = 2 x 0.1 = 0.2 s.
    near(one(path({rapid(10, 0, 0)})).rapidSec, 0.2);
}

TEST_CASE("cycle time 3: an axis-limited diagonal rapid, each axis on its own (the default)", "[cycletime]")
{
    // X 0 -> 100 and Z 0 -> -60 together, not interpolated: each axis from rest to rest, the slower one sets the time.
    //   X: 100 >= 40, t = 100/200 + 200/1000 = 0.7 s.
    //   Z: V = 100, A = 500, V^2/A = 20 <= 60, t = 60/100 + 100/500 = 0.6 + 0.2 = 0.8 s.
    //   t = max(0.7, 0.8) = 0.8 s; distance along the line sqrt(100^2 + 60^2) = 116.619 mm.
    const CycleTime t = one(path({rapid(100, 0, -60)}));
    near(t.rapidSec, 0.8);
    near(t.rapidMm, 116.619);
}

TEST_CASE("cycle time 4: the same diagonal rapid interpolated", "[cycletime]")
{
    // L = 116.619, u = (100, 0, -60)/L = (0.85749, 0, -0.51450).
    //   v = min(200/0.85749, 100/0.51450) = min(233.24, 194.36) = 194.36 mm/s     (Z limits the speed)
    //   A = min(1000/0.85749, 500/0.51450) = min(1166.2, 971.82) = 971.82 mm/s^2  (and the acceleration)
    //   v^2/A = 38.87 <= 116.619: t = 116.619/194.36 + 194.36/971.82 = 0.6 + 0.2 = 0.8 s, Z's own time.
    CycleTimeOptions o;
    o.rapidInterpolated = true;
    near(one(path({rapid(100, 0, -60)}), machineM(), o).rapidSec, 0.8);
}

TEST_CASE("cycle time 4b: where the two rapid modes differ", "[cycletime]")
{
    // A machine whose X accelerates slowly and whose Y is slow: X V = 200 mm/s, A = 200; Y V = 100 mm/s, A = 1000.
    // Rapid X 0 -> 100, Y 0 -> 100.
    //   Each axis on its own: X: V^2/A = 200 > 100, a triangle 2 sqrt(100/200) = 1.414214 s;
    //                         Y: V^2/A = 10 <= 100, 100/100 + 100/1000 = 1.1 s;  t = 1.414214 s.
    //   Interpolated: L = 141.421, u = (0.70711, 0.70711): v = min(282.84, 141.42) = 141.42 (Y),
    //     A = min(282.84, 1414.2) = 282.84 (X); v^2/A = 70.71 <= 141.42: t = 141.421/141.421 + 141.421/282.843
    //     = 1 + 0.5 = 1.5 s (the straight line must hold Y's speed and X's acceleration at once).
    lib::MachineDef m = machineM();
    m.axes[0] = axis('X', 12000, 6000, 200);
    m.axes[1] = axis('Y', 6000, 6000, 1000);
    near(one(path({rapid(100, 100, 0)}), m).rapidSec, 1.414214);
    CycleTimeOptions o;
    o.rapidInterpolated = true;
    near(one(path({rapid(100, 100, 0)}), m, o).rapidSec, 1.5);
}

TEST_CASE("cycle time 5: a machine that publishes nothing gets the documented defaults, and says so", "[cycletime]")
{
    // MachineDef{}: no axes, no tool change time. X 0 -> 100 at the defaults V = 10000/60 = 166.667 mm/s, A = 1000:
    //   V^2/A = 27.78 <= 100: t = 100/166.667 + 166.667/1000 = 0.6 + 0.16667 = 0.76667 s; tool change 10 s.
    const CycleTime t = one(path({rapid(100, 0, 0)}), lib::MachineDef{});
    near(t.rapidSec, 0.766667);
    near(t.toolChangeSec, 10);
    CHECK(t.assumptions.accelerationUnknown);
    CHECK(t.assumptions.rapidUnknown);
    CHECK(t.assumptions.maxFeedUnknown);
    CHECK(t.assumptions.toolChangeUnknown);
    CHECK(!t.assumptions.feedMissing);
    CHECK(t.assumptions.any());
    REQUIRE(t.notes.size() == 4);
    CHECK(t.notes[0].find("accelerations are not known") != std::string::npos);
    // M publishes everything: no assumption, no note.
    const CycleTime m = one(path({rapid(100, 0, 0)}));
    CHECK(!m.assumptions.any());
    CHECK(m.notes.empty());
    // A machine with rapids and tool change time but no accelerations (as every shipped definition): only that.
    lib::MachineDef partial = machineM();
    for (lib::AxisDef& a : partial.axes)
        a.accelMmS2 = 0;
    const CycleTime p = one(path({rapid(100, 0, 0)}), partial);
    CHECK(p.assumptions.accelerationUnknown);
    CHECK(!p.assumptions.rapidUnknown);
    CHECK(!p.assumptions.toolChangeUnknown);
    CHECK(p.notes.size() == 1);
}

// ---------- feeds ----------

TEST_CASE("cycle time 6: a long feed move reaches its feed", "[cycletime]")
{
    // X 0 -> 200 at F 3000 mm/min = 50 mm/s, A = 1000: v^2/A = 2.5 <= 200: t = 200/50 + 50/1000 = 4 + 0.05 = 4.05 s.
    const CycleTime t = one(path({line(200, 0, 0, 3000)}));
    near(t.cutSec, 4.05);
    near(t.cutMm, 200);
    CHECK(t.rapidSec == 0);
}

TEST_CASE("cycle time 7: a feed above the machine's maximum feed is held to it", "[cycletime]")
{
    // X 0 -> 200 at F 9000 mm/min = 150 mm/s; X's max feed is 100 mm/s: t = 200/100 + 100/1000 = 2 + 0.1 = 2.1 s.
    near(one(path({line(200, 0, 0, 9000)})).cutSec, 2.1);
}

TEST_CASE("cycle time 8: a short feed move is a triangle", "[cycletime]")
{
    // X 0 -> 1 at F 6000 = 100 mm/s: v^2/A = 10 > 1: t = 2 sqrt(1/1000) = 0.0632456 s.
    near(one(path({line(1, 0, 0, 6000)})).cutSec, 0.0632456);
}

TEST_CASE("cycle time 9: a diagonal feed held by the slower axis", "[cycletime]")
{
    // X 0 -> 30, Z 0 -> -40: L = 50, u = (0.6, 0, -0.8). F 6000 = 100 mm/s.
    //   v = min(100, 100/0.6, 50/0.8) = min(100, 166.7, 62.5) = 62.5 mm/s      (Z's max feed)
    //   A = min(1000/0.6, 500/0.8) = min(1666.7, 625) = 625 mm/s^2
    //   v^2/A = 6.25 <= 50: t = 50/62.5 + 62.5/625 = 0.8 + 0.1 = 0.9 s.
    near(one(path({line(30, 0, -40, 6000)})).cutSec, 0.9);
}

TEST_CASE("cycle time 10: a feed move without a feed takes the one before it", "[cycletime]")
{
    // X 0 -> 200 at F 3000 (4.05 s, case 6), then Y 0 -> 200 with F 0: modal F 3000. A 90 degree corner: both stop.
    //   t = 4.05 + 4.05 = 8.1 s, and the estimate says a feed was missing.
    Move second = line(200, 200, 0, 3000);
    second.feedMicronPerMin = 0;
    const CycleTime t = one(path({line(200, 0, 0, 3000), second}));
    near(t.cutSec, 8.1);
    CHECK(t.assumptions.feedMissing);
}

// ---------- arcs ----------

TEST_CASE("cycle time 11: a full circle held by its centripetal acceleration", "[cycletime]")
{
    // XY circle of radius 4 about the origin from (4, 0) back to (4, 0) (an end on the start ray: a full turn),
    // F 9000 = 150 mm/s. Plane A = min(1000, 1000) = 1000, plane max feed 100 mm/s.
    //   centripetal limit v = sqrt(A R) = sqrt(1000 x 4) = 63.2456 mm/s (below 150 and 100)
    //   L = 2 pi 4 = 25.1327 mm; v^2/A = 4 <= 25.13: t = 25.1327/63.2456 + 63.2456/1000 = 0.397384 + 0.063246
    //     = 0.460629 s.
    const CycleTime t = one(path({arc(true, ArcPlane::XY, 4, 0, 0, 0, 0, 0, 9000)}, 4, 0, 0));
    near(t.cutSec, 0.460629);
    near(t.cutMm, 25.1327);
}

TEST_CASE("cycle time 12: a quarter arc at its programmed feed", "[cycletime]")
{
    // CCW from (50, 0) to (0, 50) about the origin, F 3000 = 50 mm/s; sqrt(1000 x 50) = 223.6 does not limit.
    //   L = pi/2 x 50 = 78.5398; t = 78.5398/50 + 50/1000 = 1.570796 + 0.05 = 1.620796 s.
    near(one(path({arc(true, ArcPlane::XY, 0, 50, 0, 0, 0, 0, 3000)}, 50, 0, 0)).cutSec, 1.620796);
}

TEST_CASE("cycle time 13: a helix", "[cycletime]")
{
    // One CCW turn of radius 10 dropping 2 mm, F 1200 = 20 mm/s.
    //   in plane 2 pi 10 = 62.8319; L = sqrt(62.8319^2 + 2^2) = 62.8637; shares f = 0.999494, fw = 0.031815
    //   v = min(20, 100/f, sqrt(1000 x 10)/f = 100.05, 50/fw = 1571.6) = 20 mm/s
    //   A = min(1000/f, 500/fw) = min(1000.506, 15716) = 1000.506 mm/s^2
    //   t = 62.8637/20 + 20/1000.506 = 3.143184 + 0.019990 = 3.163174 s.
    near(one(path({arc(true, ArcPlane::XY, 10, 0, -2, 0, 0, 0, 1200)}, 10, 0, 0)).cutSec, 3.163174);
}

TEST_CASE("cycle time 14: an arc in the XZ plane is held to Z's acceleration and feed", "[cycletime]")
{
    // G18 half circle of radius 10 about the origin from (10, 0, 0) to (-10, 0, 0), F 3000 = 50 mm/s.
    //   plane axes Z and X: A = min(500, 1000) = 500, max feed min(50, 100) = 50; sqrt(500 x 10) = 70.7 does not limit.
    //   L = pi 10 = 31.4159; v^2/A = 5 <= 31.4: t = 31.4159/50 + 50/500 = 0.628319 + 0.1 = 0.728319 s.
    near(one(path({arc(true, ArcPlane::XZ, -10, 0, 0, 0, 0, 0, 3000)}, 10, 0, 0)).cutSec, 0.728319);
}

// ---------- junctions ----------

TEST_CASE("cycle time 15: collinear feed moves run through as one", "[cycletime]")
{
    // X 0 -> 100 -> 200 at F 3000: no turn, so the same as one 200 mm move (case 6): 4.05 s.
    near(one(path({line(100, 0, 0, 3000), line(200, 0, 0, 3000)})).cutSec, 4.05);
}

TEST_CASE("cycle time 16: a square corner stops", "[cycletime]")
{
    // X 0 -> 100, then Y 0 -> 100, F 3000: a 90 degree turn, a full stop. Each: 100/50 + 50/1000 = 2.05 s; 4.1 s.
    near(one(path({line(100, 0, 0, 3000), line(100, 100, 0, 3000)})).cutSec, 4.1);
}

TEST_CASE("cycle time 17: a small turn runs through, a larger one stops", "[cycletime]")
{
    // Two 100 mm moves at F 3000, the second turned by 5 degrees (<= 10): run through at 50 mm/s,
    //   t = 200/50 + 50/1000 = 4.05 s. Turned by 15 degrees (> 10): a stop, 2 x 2.05 = 4.1 s.
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const auto turned = [](double deg) {
        return path(
            {line(100, 0, 0, 3000), line(100 + 100 * std::cos(deg * kDeg), 100 * std::sin(deg * kDeg), 0, 3000)});
    };
    near(one(turned(5)).cutSec, 4.05);
    near(one(turned(15)).cutSec, 4.1);
    // With the threshold at 20 degrees, 15 runs through too.
    CycleTimeOptions o;
    o.cornerStopDeg = 20;
    near(one(turned(15), machineM(), o).cutSec, 4.05);
}

TEST_CASE("cycle time 18: a junction between two feeds runs at the lower one", "[cycletime]")
{
    // X 0 -> 100 at F 3000 (50 mm/s), then X 100 -> 200 at F 1200 (20 mm/s): through the junction at 20 mm/s.
    //   first, 0 -> 50 -> 20 mm/s: accelerating 1.25 mm, braking (50^2 - 20^2)/2000 = 1.05 mm,
    //     t1 = 50/1000 + 30/1000 + (100 - 2.3)/50 = 0.05 + 0.03 + 1.954 = 2.034 s
    //   second, 20 -> 20 -> 0 mm/s: braking 400/2000 = 0.2 mm, t2 = 20/1000 + (100 - 0.2)/20 = 0.02 + 4.99 = 5.01 s
    //   t = 7.044 s.
    near(one(path({line(100, 0, 0, 3000), line(200, 0, 0, 1200)})).cutSec, 7.044);
}

TEST_CASE("cycle time 19: short moves through a junction never reach their feed", "[cycletime]")
{
    // X 0 -> 1 -> 2 at F 6000 (100 mm/s): as one 2 mm move, v^2/A = 10 > 2: a triangle peaking at sqrt(1000 x 2)
    //   = 44.7 mm/s at the junction: t = 2 sqrt(2/1000) = 0.0894427 s.
    near(one(path({line(1, 0, 0, 6000), line(2, 0, 0, 6000)})).cutSec, 0.0894427);
}

TEST_CASE("cycle time 20: a line running tangent into an arc does not stop", "[cycletime]")
{
    // (0, 10) -> (10, 10) along +X, then CW about (10, 0) to (20, 0), starting along +X: tangent, run through.
    //   F 3000 = 50 mm/s; sqrt(1000 x 10) = 100 does not limit. L = 10 + pi/2 x 10 = 25.7080:
    //   t = 25.7080/50 + 50/1000 = 0.514159 + 0.05 = 0.564159 s.
    near(one(path({line(10, 10, 0, 3000), arc(false, ArcPlane::XY, 20, 0, 0, 10, 0, 0, 3000)}, 0, 10, 0)).cutSec,
         0.564159);
}

TEST_CASE("cycle time 21: a rapid between feeds stops the chain", "[cycletime]")
{
    // X 0 -> 100 at F 3000 (2.05 s), rapid X 100 -> 200 (0.7 s, case 1), X 200 -> 300 at F 3000 (2.05 s).
    const CycleTime t = one(path({line(100, 0, 0, 3000), rapid(200, 0, 0), line(300, 0, 0, 3000)}));
    near(t.cutSec, 4.1);
    near(t.rapidSec, 0.7);
    near(t.cutMm, 200);
    near(t.rapidMm, 100);
}

// ---------- canned cycles ----------

TEST_CASE("cycle time 22: a peck drilling cycle (G83), every peck and retract", "[cycletime]")
{
    // From (0, 0, 10) = the clearance: R 2, bottom -6, Q 3, c = 0.5 mm (fallback), G99, F 150 mm/min = 2.5 mm/s.
    // The motion (cam.h "Canned cycles"):
    //   rapid 10 -> 2 (8)
    //   feed 2 -> -1 (3); rapid -1 -> 2 (3)
    //   rapid 2 -> -0.5 (2.5); feed -0.5 -> -4 (3.5); rapid -4 -> 2 (6)
    //   rapid 2 -> -3.5 (5.5); feed -3.5 -> -6 (2.5, the last peck stops at the bottom)
    //   rapid -6 -> 2 (8, the return level R)
    // Feeds, Z A = 500: v^2/A = 0.0125 <= each, t = L/2.5 + 2.5/500: (3 + 3.5 + 2.5)/2.5 + 3 x 0.005 = 3.615 s.
    // Rapids, Z V = 100, A = 500: all under V^2/A = 20 mm, triangles 2 sqrt(d/500):
    //   8: 0.252982, 3: 0.154919, 2.5: 0.141421, 6: 0.219089, 5.5: 0.209762, 8: 0.252982; sum 1.231156 s, 33 mm.
    const CycleTime t = one(path({hole(MoveKind::CyclePeck, 0, 0, -6, CycleReturn::RPlane, 3, 150)}, 0, 0, 10));
    near(t.cutSec, 3.615);
    near(t.cutMm, 9);
    near(t.rapidSec, 1.231156);
    near(t.rapidMm, 33);
}

TEST_CASE("cycle time 23: a chip-breaking cycle (G73) returning to the initial level", "[cycletime]")
{
    // As case 22 but G73 and G98 (return to the clearance 10):
    //   rapid 10 -> 2 (8); feed 2 -> -1 (3); rapid up c: -1 -> -0.5 (0.5); feed -0.5 -> -4 (3.5);
    //   rapid -4 -> -3.5 (0.5); feed -3.5 -> -6 (2.5); rapid -6 -> 10 (16)
    // Feeds as case 22: 3.615 s. Rapids: 8: 0.252982, 0.5: 2 sqrt(0.001) = 0.063246 twice, 16: 2 sqrt(0.032) =
    // 0.357771;
    //   sum 0.737244 s, 25 mm.
    const CycleTime t = one(path({hole(MoveKind::CycleChipBreak, 0, 0, -6, CycleReturn::Initial, 3, 150)}, 0, 0, 10));
    near(t.cutSec, 3.615);
    near(t.rapidSec, 0.737244);
    near(t.rapidMm, 25);
}

TEST_CASE("cycle time 24: a tapping cycle feeds in and out", "[cycletime]")
{
    // M8x1.25 at 400 rpm: F = 500 mm/min = 8.3333 mm/s. From (0, 0, 10): rapid 10 -> 2 (0.252982 s); feed 2 -> -10
    //   (12 mm; v^2/A = 0.1389 <= 12: 12/8.3333 + 8.3333/500 = 1.44 + 0.016667 = 1.456667 s), the reversal (no time),
    //   feed -10 -> 2 (the same, 1.456667 s), G99: already at R. Feed 2.913333 s, rapid 0.252982 s.
    Move tap = hole(MoveKind::CycleTap, 0, 0, -10, CycleReturn::RPlane, 0, 500);
    tap.threadPitchNm = 1'250'000;
    const CycleTime t = one(path({tap}, 0, 0, 10));
    near(t.cutSec, 2.913333);
    near(t.rapidSec, 0.252982);
    near(t.cutMm, 24);
}

TEST_CASE("cycle time 25: dwells, in a cycle and on their own", "[cycletime]")
{
    // G82: a drilling cycle with 300 ms at the bottom, then a Dwell move of 1500 ms: 1.8 s of dwell.
    //   The drill from (0, 0, 2) = R: feed 2 -> -3 at F 150 (5 mm: 5/2.5 + 0.005 = 2.005 s), rapid -3 -> 2
    //   (5 mm: 2 sqrt(5/500) = 0.2 s).
    const CycleTime t =
        one(path({hole(MoveKind::CycleDrill, 0, 0, -3, CycleReturn::RPlane, 0, 150, 300), dwellMove(1500)}, 0, 0, 2));
    near(t.dwellSec, 1.8);
    near(t.cutSec, 2.005);
    near(t.rapidSec, 0.2);
    near(t.totalSec, 3 + 1.8 + 2.005 + 0.2);
}

// ---------- tool changes, operations and the setup ----------

TEST_CASE("cycle time 26: tool changes, per operation and for the setup", "[cycletime]")
{
    // Three operations: tool 1, tool 2, tool 2 again.
    //   op 0: tool change (3 s), X 0 -> 200 at F 3000 (4.05 s): 7.05 s
    //   op 1: tool change (3 s), rapid X 0 -> 100 (0.7 s): 3.7 s; it ends at (100, 0, 0)
    //   op 2: the same tool: a rapid from (100, 0, 0) to its start (200, 0, 0) (0.7 s), then X 200 -> 300 at F 3000
    //         (2.05 s): 2.75 s
    //   setup: 2 tool changes, 6 s; 13.5 s in all.
    const Toolpath a = path({line(200, 0, 0, 3000)});
    const Toolpath b = path({rapid(100, 0, 0)});
    const Toolpath c = path({line(300, 0, 0, 3000)}, 200, 0, 0);
    const std::vector<CycleTimeOperation> ops = {{&a, 1}, {&b, 2}, {&c, 2}};
    const CycleTime t = estimateCycleTime(ops, machineM());
    REQUIRE(t.operations.size() == 3);
    near(t.operations[0].totalSec, 7.05);
    near(t.operations[1].totalSec, 3.7);
    near(t.operations[2].totalSec, 2.75);
    CHECK(t.operations[2].toolChanges == 0);
    near(t.operations[2].rapidSec, 0.7);
    CHECK(t.toolChanges == 2);
    near(t.toolChangeSec, 6);
    near(t.totalSec, 13.5);
    // The setup is the sum of its operations, field by field.
    double total = 0, cut = 0;
    for (const CycleTimePart& p : t.operations)
        total += p.totalSec, cut += p.cutSec;
    CHECK(total == t.totalSec);
    CHECK(cut == t.cutSec);
    // An unknown tool number (0) is always a change; the plan draft's overload counts one per toolpath.
    const std::vector<CycleTimeOperation> unknown = {{&a, 0}, {&b, 0}, {&c, 0}};
    CHECK(estimateCycleTime(unknown, machineM()).toolChanges == 3);
    const Toolpath* const plain[] = {&a, &b, &c};
    CHECK(estimateCycleTime(std::span<const Toolpath* const>(plain), machineM()).toolChanges == 3);
    // An operation without a toolpath adds nothing, and the tool before it stays.
    const std::vector<CycleTimeOperation> gap = {{&a, 1}, {nullptr, 1}, {&b, 1}};
    const CycleTime g = estimateCycleTime(gap, machineM());
    CHECK(g.operations[1].totalSec == 0);
    CHECK(g.toolChanges == 1);
}

TEST_CASE("cycle time: nothing to time", "[cycletime]")
{
    CHECK(estimateCycleTime(std::span<const CycleTimeOperation>(), machineM()).totalSec == 0);
    // Zero-length motions, a cancel and a zero dwell take no time.
    Move cancel{};
    cancel.kind = MoveKind::CycleCancel;
    const CycleTime t = one(path({rapid(0, 0, 0), line(0, 0, 0, 1000), cancel, dwellMove(0)}));
    CHECK(t.totalSec == t.toolChangeSec);
    CHECK(t.cutMm == 0);
    CHECK(t.rapidMm == 0);
}

// ---------- determinism and speed ----------

namespace {

// A toolpath of `count` moves from integer arithmetic only (so its bits are the same everywhere): short cuts in any
// direction, rapids, quarter and half arcs, helix turns, XZ and YZ arcs, drilling and peck cycles, dwells.
Toolpath mixedToolpath(std::size_t count)
{
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    const auto next = [&s] {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return s;
    };
    Toolpath tp{};
    tp.startZ = nm(5);
    tp.moves.reserve(count);
    Nm x = 0, y = 0, z = nm(5);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t p = next() % 100;
        Move m{};
        m.role = MoveRole::Cut;
        m.feedMicronPerMin = static_cast<std::int32_t>(500'000 + next() % 3'000'000);
        if (x > nm(150) || x < -nm(150) || y > nm(150) || y < -nm(150)) {
            m.kind = MoveKind::Rapid;
            x = 0, y = 0, z = nm(5);
        } else if (p < 55) {
            m.kind = MoveKind::Linear;
            x += static_cast<Nm>(next() % 4'000'001) - 2'000'000;
            y += static_cast<Nm>(next() % 4'000'001) - 2'000'000;
        } else if (p < 65) {
            m.kind = MoveKind::Rapid;
            x = static_cast<Nm>(next() % 200'000'001) - 100'000'000;
            y = static_cast<Nm>(next() % 200'000'001) - 100'000'000;
        } else if (p < 90) { // a quarter or half turn about a centre on +X of the tool
            const Nm r = static_cast<Nm>(500'000 + next() % 19'500'000);
            m.kind = (next() & 1) != 0 ? MoveKind::ArcCCW : MoveKind::ArcCW;
            m.i = x + r, m.j = y, m.k = z;
            if ((next() & 1) != 0) {
                x += 2 * r;
            } else {
                x += r;
                y += m.kind == MoveKind::ArcCCW ? -r : r;
            }
        } else if (p < 93) { // a full helix turn
            const Nm r = static_cast<Nm>(2'000'000 + next() % 8'000'000);
            m.kind = MoveKind::ArcCCW;
            m.i = x + r, m.j = y, m.k = z;
            z -= 500'000;
        } else if (p < 95) { // a quarter in XZ or YZ
            const Nm r = static_cast<Nm>(1'000'000 + next() % 4'000'000);
            m.kind = MoveKind::ArcCW;
            if ((next() & 1) != 0) {
                m.plane = ArcPlane::XZ;
                m.i = x - r, m.j = y, m.k = z;
                x -= r, z += r;
            } else {
                m.plane = ArcPlane::YZ;
                m.i = x, m.j = y - r, m.k = z;
                y -= r, z += r;
            }
        } else if (p < 98) {
            m.kind = (next() & 1) != 0 ? MoveKind::CyclePeck : MoveKind::CycleDrill;
            m.role = MoveRole::Plunge;
            m.cycleTop = 0;
            m.cycleRetract = 2'000'000;
            m.cycleClearance = std::max<Nm>(z, 2'000'000);
            m.cyclePeck = 1'500'000;
            m.cycleReturn = (next() & 1) != 0 ? CycleReturn::Initial : CycleReturn::RPlane;
            m.feedMicronPerMin = 200'000;
            z = m.cycleReturn == CycleReturn::Initial ? m.cycleClearance : m.cycleRetract;
            m.x = x, m.y = y, m.z = -static_cast<Nm>(3'000'000 + next() % 9'000'000);
            tp.moves.push_back(m);
            continue;
        } else {
            m.kind = MoveKind::Dwell;
            m.cycleDwellMs = static_cast<std::int32_t>(next() % 500);
        }
        m.x = x, m.y = y, m.z = z;
        tp.moves.push_back(m);
    }
    return tp;
}

std::string bitsOf(const CycleTime& t)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "%016llx %016llx %016llx %016llx %016llx %016llx",
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.totalSec)),
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.cutSec)),
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.rapidSec)),
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.dwellSec)),
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.cutMm)),
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(t.rapidMm)));
    return buf;
}

bool optimisedBuild()
{
#if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return false;
#else
    return true;
#endif
#else
    return true;
#endif
}

} // namespace

TEST_CASE("cycle time: the same bits on every platform", "[cycletime]")
{
    // The golden is the bits of the totals of a 20,000-move toolpath built from integers, on M with a slower Y (so the
    // two rapid modes differ), both rapid modes. It was recorded on Windows (MSVC) and checked on Linux (GCC); a change
    // of the model changes it on purpose.
    const Toolpath tp = mixedToolpath(20'000);
    lib::MachineDef m = machineM();
    m.axes[1] = axis('Y', 9000, 5000, 700);
    const std::string dogleg = bitsOf(one(tp, m));
    CycleTimeOptions o;
    o.rapidInterpolated = true;
    const std::string straight = bitsOf(one(tp, m, o));
    CHECK(dogleg != straight);
    INFO("dogleg   " << dogleg);
    INFO("straight " << straight);
    CHECK(dogleg ==
          "40c67e52175176b6 40c145cf1e6fe05e 40a425dbc2c1bdbb 4056c604189374bd 41048084f0b48286 4110258579af2b97");
    CHECK(straight ==
          "40c67e61c457726f 40c145cf1e6fe05e 40a4261a76d9aca1 4056c604189374bd 41048084f0b48286 4110258579af2b97");
    // And the same twice in one process.
    CHECK(bitsOf(one(tp, m)) == dogleg);
}

TEST_CASE("cycle time: 1,000,000 moves in under 200 ms", "[cycletime][wallclock]")
{
    const Toolpath tp = mixedToolpath(1'000'000);
    const CycleTimeOperation op{&tp, 1};
    const lib::MachineDef m = machineM();
    double best = 1e9;
    CycleTime t;
    for (int run = 0; run < 5; ++run) {
        const auto t0 = std::chrono::steady_clock::now();
        t = estimateCycleTime(std::span<const CycleTimeOperation>(&op, 1), m);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::cout << "cycle time of 1,000,000 moves: best of 5 " << best << " ms (estimate " << t.totalSec << " s)\n";
    CHECK(t.totalSec > 0);
    if (optimisedBuild())
        CHECK(best < 200.0);
}

// ---------- real machines (M8) ----------

namespace {

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::byte> readBytes(const std::string& file)
{
    std::ifstream in(file, std::ios::binary);
    const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        out[i] = static_cast<std::byte>(s[i]);
    return out;
}

} // namespace

TEST_CASE("cycle time against times measured on real machines", "[cycletime]")
{
    // The format is described at the top of the file. Each record's estimate is compared with its measured time; the
    // error is printed, and a record fails when it is above the record's own allowed error.
    const std::string dir = RAY_CYCLETIME_DATA_DIR;
    std::ifstream in(dir + "/measured.tsv");
    REQUIRE(in.good());
    std::string text;
    int records = 0;
    while (std::getline(in, text)) {
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty() || text[0] == '#')
            continue;
        const std::vector<std::string> f = split(text, '\t');
        INFO("record: " << text);
        REQUIRE(f.size() == 6);
        const std::vector<std::string> files = split(f[0], ',');
        const std::vector<std::string> tools = split(f[2], ',');
        REQUIRE(files.size() == tools.size());
        std::ifstream machineIn(dir + "/" + f[1], std::ios::binary);
        const std::string machineJson((std::istreambuf_iterator<char>(machineIn)), std::istreambuf_iterator<char>());
        const auto machine = lib::machineFromJson(machineJson);
        REQUIRE(machine.has_value());
        std::vector<Toolpath> toolpaths;
        for (const std::string& file : files) {
            const std::vector<std::byte> bytes = readBytes(dir + "/" + file);
            auto tp = Toolpath::deserialize(bytes);
            REQUIRE(tp.has_value());
            toolpaths.push_back(std::move(*tp));
        }
        std::vector<CycleTimeOperation> ops;
        for (std::size_t i = 0; i < toolpaths.size(); ++i)
            ops.push_back({&toolpaths[i], std::stoi(tools[i])});
        const CycleTime t = estimateCycleTime(ops, *machine);
        const double measured = std::stod(f[3]);
        const double errorPct = (t.totalSec - measured) / measured * 100.0;
        std::cout << "measured " << measured << " s, estimated " << t.totalSec << " s, error " << errorPct << " % ("
                  << f[5] << ")\n";
        CHECK(std::fabs(errorPct) <= std::stod(f[4]));
        ++records;
    }
    if (records == 0)
        SKIP("no machine times recorded yet (tests/data/cycletime/measured.tsv; M8)");
}
