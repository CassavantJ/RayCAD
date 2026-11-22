// The canned-cycle rules of the toolpath invariant checker (cycle_invariants.h): a toolpath that keeps cam.h's rules
// passes, and each rule catches the toolpath built to break it.
#include "cycle_invariants.h"

#include <catch2/catch_test_macros.hpp>

#include <tuple>

using namespace ray;
using namespace ray::cam;
using ray::cam::invariants::checkCycles;
using ray::cam::invariants::Violation;

namespace {

constexpr Nm mm(std::int64_t v)
{
    return v * 1'000'000;
}

Move rapid(Nm x, Nm y, Nm z)
{
    Move m{};
    m.kind = MoveKind::Rapid;
    m.role = MoveRole::Link;
    m.x = x;
    m.y = y;
    m.z = z;
    return m;
}

// A hole at (x, 0): bottom -10, top 0, R 2, clearance 10.
Move hole(MoveKind kind, Nm x, CycleReturn ret = CycleReturn::RPlane)
{
    Move m{};
    m.kind = kind;
    m.role = MoveRole::Plunge;
    m.x = x;
    m.z = -mm(10);
    m.cycleTop = 0;
    m.cycleRetract = mm(2);
    m.cycleClearance = mm(10);
    m.cycleReturn = ret;
    m.feedMicronPerMin = 200'000;
    if (kind == MoveKind::CyclePeck || kind == MoveKind::CycleChipBreak) {
        m.cyclePeck = mm(3);
    }
    if (kind == MoveKind::CycleTap) {
        m.threadPitchNm = 1'250'000;  // M8x1.25
        m.feedMicronPerMin = 625'000; // at 500 rpm
    }
    return m;
}

Toolpath path(std::vector<Move> moves, Nm startZ = mm(10))
{
    Toolpath p{};
    p.startZ = startZ;
    p.moves = std::move(moves);
    return p;
}

std::vector<Violation> broken(Move m, std::optional<double> rpm = 500.0)
{
    return checkCycles(path({m}), rpm);
}

} // namespace

TEST_CASE("a toolpath that keeps the cycle rules passes", "[cam][invariants][cycle]")
{
    Move left = hole(MoveKind::CycleTap, mm(60));
    left.cycleLeftHand = 1;
    const Toolpath ok = path({
        rapid(0, 0, mm(30)),                                      // above the clearance: allowed
        hole(MoveKind::CycleDrill, mm(10)),                       // G99
        hole(MoveKind::CycleDrill, mm(20), CycleReturn::Initial), // the run's last hole: G98
        hole(MoveKind::CyclePeck, mm(30)),                        // a new run, at the clearance
        hole(MoveKind::CyclePeck, mm(40), CycleReturn::Initial),
        Move{},                   // (a Rapid to 0, 0, 0 ends the run ...)
        rapid(mm(50), 0, mm(12)), // ... and this goes back up
        hole(MoveKind::CycleTap, mm(50), CycleReturn::Initial),
        left, // a left-hand run, from the G98 level
    });
    CHECK(checkCycles(ok, 500.0).empty());
}

TEST_CASE("each cycle rule catches the move that breaks it", "[cam][invariants][cycle]")
{
    const auto only = [](std::vector<Violation> v, const char* rule) {
        INFO("rule " << rule);
        REQUIRE(v.size() == 1);
        CHECK(v[0].rule == rule);
    };
    Move m = hole(MoveKind::CycleDrill, 0);
    m.pad1 = 7;
    only(broken(m), "pad");
    m = hole(MoveKind::CycleDrill, 0);
    m.feedMicronPerMin = 0;
    only(broken(m), "feed");
    for (const auto& [top, r, clearance] :
         std::vector<std::tuple<Nm, Nm, Nm>>{{-mm(10), mm(2), mm(10)}, {mm(3), mm(2), mm(10)}, {0, mm(11), mm(10)}}) {
        m = hole(MoveKind::CycleDrill, 0);
        m.cycleTop = top;
        m.cycleRetract = r;
        m.cycleClearance = clearance;
        only(checkCycles(path({m}, mm(20))), "levels");
    }
    m = hole(MoveKind::CycleDrill, 0);
    m.cycleReturn = static_cast<CycleReturn>(2);
    only(broken(m), "return");
    m = hole(MoveKind::CyclePeck, 0);
    m.cyclePeck = -1;
    only(broken(m), "peck");
    m = hole(MoveKind::CycleDrill, 0);
    m.cyclePeck = mm(1); // a drill does not peck
    only(broken(m), "peck");
    m = hole(MoveKind::CycleChipBreak, 0);
    m.cyclePeckClearance = -1;
    only(broken(m), "peckClear");
    m = hole(MoveKind::CycleBore, 0);
    m.cycleDwellMs = -5;
    only(broken(m), "dwell");
    m = hole(MoveKind::CycleDrill, 0);
    m.cycleLeftHand = 1;
    only(broken(m), "leftHand");
    m = hole(MoveKind::CycleTap, 0);
    m.cycleLeftHand = 2;
    only(broken(m), "leftHand");
    m = hole(MoveKind::CycleTap, 0);
    m.threadPitchNm = 0;
    only(broken(m), "pitch");
    m = hole(MoveKind::CycleReam, 0);
    m.threadPitchNm = 1'000'000;
    only(broken(m), "pitch");
    // Tap feed = pitch times rpm, in IR units: 625 mm/min at 500 rpm for 1.25 mm, not at 501 or 500.5.
    m = hole(MoveKind::CycleTap, 0);
    CHECK(broken(m, 500.0).empty());
    CHECK(broken(m, std::nullopt).empty());
    only(broken(m, 501.0), "tapFeed");
    only(broken(m, 500.5), "tapFeed");
    // 14 TPI (1.8142857 mm, 1814286 nm) at 333 rpm is 604157.238 um/min: rounded half to even to the micrometre.
    m.threadPitchNm = 1'814'286;
    m.feedMicronPerMin = 604'157;
    CHECK(broken(m, 333.0).empty());
    m.feedMicronPerMin = 604'158;
    only(broken(m, 333.0), "tapFeed");
}

TEST_CASE("a cycle run must start at or above its clearance", "[cam][invariants][cycle]")
{
    // From below the clearance.
    std::vector<Violation> v = checkCycles(path({hole(MoveKind::CycleDrill, 0)}, mm(9)));
    REQUIRE(v.size() == 1);
    CHECK(v[0] == Violation{0, "runStart"});
    // A G99 hole, then a new run (another kind, a left-hand tap or another clearance) from the R plane.
    Move left = hole(MoveKind::CycleTap, mm(20));
    left.cycleLeftHand = 1;
    Move lower = hole(MoveKind::CycleDrill, mm(20));
    lower.cycleClearance = mm(9);
    for (const Move& next : {hole(MoveKind::CyclePeck, mm(20)), left, lower}) {
        const Move first =
            next.kind == MoveKind::CycleTap ? hole(MoveKind::CycleTap, 0) : hole(MoveKind::CycleDrill, 0);
        v = checkCycles(path({first, next}), 500.0);
        REQUIRE(v.size() == 1);
        CHECK(v[0] == Violation{1, "runStart"});
    }
    // The same after a CycleCancel or a Dwell: they end the run and do not move the tool.
    for (const MoveKind between : {MoveKind::CycleCancel, MoveKind::Dwell}) {
        Move stop{};
        stop.kind = between;
        stop.z = mm(50); // ignored: neither moves the tool
        v = checkCycles(path({hole(MoveKind::CycleDrill, 0), stop, hole(MoveKind::CycleDrill, mm(20))}));
        REQUIRE(v.size() == 1);
        CHECK(v[0] == Violation{2, "runStart"});
    }
    // Within a run, holes start at the previous return level: fine.
    CHECK(checkCycles(path({hole(MoveKind::CycleDrill, 0), hole(MoveKind::CycleDrill, mm(20))})).empty());
}
