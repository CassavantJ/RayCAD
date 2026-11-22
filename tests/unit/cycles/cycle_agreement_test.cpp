// The backplot (src/sim) and the post host (src/post) read canned cycles the same way (cam.h "Canned cycles").
// src/sim and src/post may not depend on each other (PLAN.md 1.1; only api links both), so this test, which links the
// two, has a folder of its own. The same cycle moves go to buildBackplot and to the post host's expandSection:
//  - with no cycle declared, every motion the host expands (rapid or feed, end point, feed and role of each feed) is
//    exactly the motion the backplot draws, move by move;
//  - with every cycle declared (tapping too, which the host never expands), the rapids the host emits before a run's
//    first hole are the backplot's first motions of that move, and the end of each hole as the Cycle event states it
//    (x, y and the return level) is the backplot's last vertex of that move.
// Every kind, G98 and G99, a run that starts above its clearance, peck clearances from the move, the machine and the
// 0.5 mm fallback, dwells and a left-hand tap. T-059 adds the third expansion, the cycle-time estimate's (src/cam
// cycletime.h), which must move as the backplot draws. Coordinates are compared as the backplot stores them (float mm,
// identity frame): the host's integer nanometres converted by the same expression, so "equal" means equal.
#include "cycletime.h"
#include "engine_motion.h"

#include <ray/cam.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <ostream>
#include <string>
#include <vector>

using namespace ray;

namespace {

constexpr Nm mm(double v)
{
    return static_cast<Nm>(v * 1'000'000.0 + (v < 0 ? -0.5 : 0.5));
}

float toFloat(Nm v)
{
    return static_cast<float>(static_cast<double>(v) / kNmPerMm); // as the backplot converts
}
float feedFloat(std::int32_t umPerMin)
{
    return static_cast<float>(static_cast<double>(umPerMin) / 1000.0);
}

struct Motion {
    bool rapid;
    float x, y, z;
    float feed;        // 0 for rapids
    std::uint8_t role; // feeds only (the host's rapid events carry no role)
    bool operator==(const Motion&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Motion& m)
{
    return os << (m.rapid ? "rapid" : "feed") << " to (" << m.x << ", " << m.y << ", " << m.z << ") F" << m.feed
              << " role " << int(m.role);
}

cam::Move rapid(double x, double y, double z)
{
    cam::Move m{};
    m.kind = cam::MoveKind::Rapid;
    m.role = cam::MoveRole::Link;
    m.x = mm(x);
    m.y = mm(y);
    m.z = mm(z);
    return m;
}

// A move without motion (CycleCancel, Dwell).
cam::Move noMotion(cam::MoveKind kind)
{
    cam::Move m{};
    m.kind = kind;
    return m;
}

// A hole: top 0, R 2, clearance 10, bottom `bottom`, 150 mm/min.
cam::Move hole(cam::MoveKind kind, double x, double y, double bottom, cam::CycleReturn ret, double peck = 0,
               std::int32_t dwellMs = 0)
{
    cam::Move m{};
    m.kind = kind;
    m.role = cam::MoveRole::Plunge;
    m.x = mm(x);
    m.y = mm(y);
    m.z = mm(bottom);
    m.cycleTop = 0;
    m.cycleRetract = mm(2);
    m.cycleClearance = mm(10);
    m.cycleReturn = ret;
    m.cyclePeck = mm(peck);
    m.cycleDwellMs = dwellMs;
    m.feedMicronPerMin = 150'000;
    if (kind == cam::MoveKind::CycleTap) {
        m.threadPitchNm = 1'250'000;
        m.feedMicronPerMin = 625'000; // 500 rpm
    }
    return m;
}

constexpr auto g98 = cam::CycleReturn::Initial;
constexpr auto g99 = cam::CycleReturn::RPlane;

cam::Toolpath path(std::vector<cam::Move> moves, double startZ = 30)
{
    cam::Toolpath tp{};
    tp.startZ = mm(startZ);
    tp.moves = std::move(moves);
    return tp;
}

struct Scenario {
    std::string name;
    cam::Toolpath tp;
};

// Every kind but tapping (which the host never expands): runs from above and at the clearance, G98 and G99, pecks
// that divide the depth and that do not, a peck deeper than the hole, dwells, cancels and plain moves between.
std::vector<Scenario> expandable()
{
    using K = cam::MoveKind;
    std::vector<Scenario> s;
    s.push_back({"drill: G99, G99 with dwell, G98",
                 path({hole(K::CycleDrill, 10, 0, -6, g99), hole(K::CycleDrill, 20, 0, -6.5, g99, 0, 300),
                       hole(K::CycleDrill, 30, 5, -6, g98), rapid(30, 5, 30)})});
    cam::Move ownClearance = hole(K::CyclePeck, 20, 0, -11, g99, 4);
    ownClearance.cyclePeckClearance = mm(0.3);
    s.push_back({"peck (G83): from above the clearance, the move's own clearance, a peck deeper than the hole",
                 path({hole(K::CyclePeck, 10, 0, -12, g98, 3, 200), ownClearance, hole(K::CyclePeck, 30, 0, -4, g98, 9),
                       hole(K::CyclePeck, 40, 0, -5.25, g98, 0)})});
    cam::Move ownBackOff = hole(K::CycleChipBreak, 20, 10, -9, g98, 2.5);
    ownBackOff.cyclePeckClearance = mm(1.1);
    s.push_back({"chip break (G73)", path({hole(K::CycleChipBreak, 10, 10, -9, g99, 2.5), ownBackOff,
                                           hole(K::CycleChipBreak, 30, 10, -3, g99, 1, 100)},
                                          10)});
    cam::Move clearanceIsR = hole(K::CycleDrill, 70, 0, -3, g98);
    clearanceIsR.cycleClearance = clearanceIsR.cycleRetract; // a new run whose clearance is its R plane
    s.push_back({"bore and ream (G85, G89), cancel, dwell and plain moves between runs",
                 path({hole(K::CycleBore, 10, 0, -8, g99, 0, 400), hole(K::CycleBore, 20, 0, -8, g98),
                       noMotion(cam::MoveKind::CycleCancel), hole(K::CycleReam, 30, 0, -10, g99),
                       hole(K::CycleReam, 40, 0, -10, g98, 0, 150), noMotion(cam::MoveKind::Dwell), rapid(50, 0, 25),
                       hole(K::CycleDrill, 60, 0, -3, g99), clearanceIsR})});
    return s;
}

std::vector<lib::MachineDef> machines()
{
    lib::MachineDef unknown{};
    lib::MachineDef known{};
    known.peckClearance = 0.25;
    return {unknown, known};
}

struct Collect final : post::MotionSink {
    std::vector<post::MotionEvent> events;
    bool onEvent(const post::MotionEvent& e) override
    {
        events.push_back(e);
        return true;
    }
};

// The motions the backplot draws for move m (vertices of kind Rapid or Linear; a move without motion has none).
std::vector<Motion> drawn(const sim::Backplot& bp, std::size_t m)
{
    std::vector<Motion> out;
    for (std::uint32_t v = bp.moveFirstVertex[m]; v < bp.moveFirstVertex[m + 1]; ++v) {
        const sim::BackplotVertex& b = bp.lineVertices[v];
        if (b.kind == static_cast<std::uint8_t>(cam::MoveKind::Rapid)) {
            out.push_back({true, b.x, b.y, b.z, 0.0f, 0});
        } else if (b.kind == static_cast<std::uint8_t>(cam::MoveKind::Linear)) {
            out.push_back({false, b.x, b.y, b.z, b.feed, b.role});
        }
    }
    return out;
}

// The host's motion events, by IR move.
std::vector<std::vector<Motion>> posted(const std::vector<post::MotionEvent>& events, std::size_t moves)
{
    std::vector<std::vector<Motion>> out(moves);
    for (const post::MotionEvent& e : events) {
        if (e.kind == post::EventKind::Rapid) {
            out[e.move].push_back({true, toFloat(e.x), toFloat(e.y), toFloat(e.z), 0.0f, 0});
        } else if (e.kind == post::EventKind::Linear) {
            out[e.move].push_back({false, toFloat(e.x), toFloat(e.y), toFloat(e.z), feedFloat(e.feed),
                                   static_cast<std::uint8_t>(e.role)});
        }
    }
    return out;
}

} // namespace

TEST_CASE("expanded cycles: the post host moves exactly as the backplot draws", "[cycles][sim][post]")
{
    for (const lib::MachineDef& machine : machines()) {
        for (const Scenario& s : expandable()) {
            INFO(s.name << ", machine peck clearance " << machine.peckClearance);
            const sim::Backplot bp = sim::buildBackplot(s.tp, Xform::identity(), 0.01, &machine);
            Collect c;
            const auto r = post::expandSection(s.tp, &machine, post::MotionCaps{}, c);
            REQUIRE(r.has_value());
            REQUIRE(*r);
            const auto byMove = posted(c.events, s.tp.moves.size());
            std::size_t motions = 0;
            for (std::size_t m = 0; m < s.tp.moves.size(); ++m) {
                INFO("move " << m);
                CHECK(byMove[m] == drawn(bp, m));
                motions += byMove[m].size();
                // The host dwells at the bottom where the move has a dwell (the backplot draws none).
                const cam::Move& mv = s.tp.moves[m];
                if (post::cycleKindOfMove(mv.kind) && mv.cycleDwellMs > 0) {
                    bool dwelt = false;
                    for (const post::MotionEvent& e : c.events) {
                        dwelt |= e.move == m && e.kind == post::EventKind::Dwell && e.dwellMs == mv.cycleDwellMs &&
                                 e.z == mv.z;
                    }
                    CHECK(dwelt);
                }
            }
            CHECK(motions > s.tp.moves.size()); // the cycles were expanded
        }
    }
}

// T-059: the cycle-time estimate (src/cam, cycletime.h expandCycle) times cycles from an expansion of its own (cam may
// depend on neither sim nor post). It must move exactly as the backplot draws, every kind (tapping too), and dwell at
// the bottom where the move has a dwell.
TEST_CASE("cycle time: the estimate's expansion moves exactly as the backplot draws", "[cycles][sim][cam]")
{
    using K = cam::MoveKind;
    std::vector<Scenario> scenarios = expandable();
    cam::Move left = hole(K::CycleTap, 20, 0, -12, g99);
    left.cycleLeftHand = 1;
    scenarios.push_back({"tap (G84) from above the clearance, then a left-hand tap (G74)",
                         path({hole(K::CycleTap, 10, 0, -12, g98, 0, 100), left, rapid(30, 0, 40)})});
    for (const lib::MachineDef& machine : machines()) {
        for (const Scenario& s : scenarios) {
            INFO(s.name << ", machine peck clearance " << machine.peckClearance);
            const sim::Backplot bp = sim::buildBackplot(s.tp, Xform::identity(), 0.01, &machine);
            cam::cycletime::Point pos{s.tp.startX, s.tp.startY, s.tp.startZ};
            std::size_t cycles = 0;
            for (std::size_t m = 0; m < s.tp.moves.size(); ++m) {
                INFO("move " << m);
                const cam::Move& mv = s.tp.moves[m];
                if (!cam::cycletime::isCycleMotion(mv.kind)) {
                    if (mv.kind != K::Dwell && mv.kind != K::CycleCancel)
                        pos = {mv.x, mv.y, mv.z};
                    continue;
                }
                ++cycles;
                std::vector<Motion> timed;
                bool dwelt = false;
                pos = cam::cycletime::expandCycle(mv, pos, &machine, [&](const cam::cycletime::CycleStep& st) {
                    if (st.kind == cam::cycletime::StepKind::Dwell) {
                        dwelt |= st.dwellMs == mv.cycleDwellMs && st.to.z == mv.z;
                        return;
                    }
                    const bool isRapid = st.kind == cam::cycletime::StepKind::Rapid;
                    timed.push_back({isRapid, toFloat(st.to.x), toFloat(st.to.y), toFloat(st.to.z),
                                     isRapid ? 0.0f : feedFloat(st.feed),
                                     isRapid ? std::uint8_t{0} : static_cast<std::uint8_t>(st.role)});
                });
                CHECK(timed == drawn(bp, m));
                CHECK(dwelt == (mv.cycleDwellMs > 0));
            }
            CHECK(cycles > 0);
        }
    }
}

TEST_CASE("declared cycles: the post host and the backplot agree where each hole starts and ends",
          "[cycles][sim][post]")
{
    using K = cam::MoveKind;
    std::vector<Scenario> scenarios = expandable();
    cam::Move left = hole(K::CycleTap, 20, 0, -12, g99);
    left.cycleLeftHand = 1;
    cam::Move leftAgain = left;
    leftAgain.x = mm(30);
    leftAgain.cycleReturn = g98;
    scenarios.push_back({"tap (G84) from above the clearance, then a left-hand run (G74)",
                         path({hole(K::CycleTap, 10, 0, -12, g98, 0, 100), left, leftAgain, rapid(30, 0, 40)})});
    post::MotionCaps caps;
    for (std::size_t k = 0; k < post::kCycleKindCount; ++k) {
        caps.cycles[k] = true;
    }
    for (const lib::MachineDef& machine : machines()) {
        for (const Scenario& s : scenarios) {
            INFO(s.name << ", machine peck clearance " << machine.peckClearance);
            const sim::Backplot bp = sim::buildBackplot(s.tp, Xform::identity(), 0.01, &machine);
            Collect c;
            const auto r = post::expandSection(s.tp, &machine, caps, c);
            REQUIRE(r.has_value());
            REQUIRE(*r);
            const auto byMove = posted(c.events, s.tp.moves.size());
            std::size_t holes = 0;
            for (const post::MotionEvent& e : c.events) {
                if (e.kind != post::EventKind::Cycle) {
                    continue;
                }
                ++holes;
                INFO("move " << e.move);
                const cam::Move& mv = s.tp.moves[e.move];
                const std::vector<Motion> d = drawn(bp, e.move);
                REQUIRE(!d.empty());
                // The host's rapids before the hole are the backplot's first motions (steps 1 and 2 of cam.h).
                const std::vector<Motion>& before = byMove[e.move];
                REQUIRE(before.size() < d.size());
                CHECK(before == std::vector<Motion>(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(before.size())));
                // Where the event says the tool ends: over the hole at its return level.
                const Nm endZ = e.returnLevel == cam::CycleReturn::Initial ? e.clearance : e.retract;
                CHECK(Motion{d.back().rapid, toFloat(e.x), toFloat(e.y), toFloat(endZ), d.back().feed, d.back().role} ==
                      d.back());
                // And the event carries the IR's values unchanged.
                CHECK((e.z == mv.z && e.top == mv.cycleTop && e.retract == mv.cycleRetract &&
                       e.clearance == mv.cycleClearance && e.peck == mv.cyclePeck &&
                       e.peckClearance == mv.cyclePeckClearance && e.dwellMs == mv.cycleDwellMs &&
                       e.pitchNm == mv.threadPitchNm && e.leftHand == (mv.cycleLeftHand != 0) &&
                       e.returnLevel == mv.cycleReturn));
            }
            CHECK(holes > 0);
            // A CycleEnd says where the run left the tool: the backplot's last vertex of the run's last hole.
            for (const post::MotionEvent& e : c.events) {
                if (e.kind == post::EventKind::CycleEnd && post::cycleKindOfMove(s.tp.moves[e.move].kind)) {
                    const std::vector<Motion> d = drawn(bp, e.move);
                    REQUIRE(!d.empty());
                    CHECK((toFloat(e.x) == d.back().x && toFloat(e.y) == d.back().y && toFloat(e.z) == d.back().z));
                }
            }
        }
    }
}
