// The setup checks (added after T-074; src/sim/verify.h, SETUP): fixtures against the part and the initial stock.
// Touching is no overlap, nor is 0.01 mm (rounding); a clear overlap with the stock only is a Warning, with the part an
// Error; a stock carried from an earlier setup (its cut walls up to half a cell proud) is allowed for; a fixture clear
// of everything, one solid wholly inside another, faces at an angle and a curved part; the same events for any worker
// count; the simulator lists them first, before the moves, and stopOnError passes over them.
#include "simulator_test_util.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// The stock and the part of most cases: a 100 x 60 x 20 block with the part 2 mm inside it on four sides and the
// top (its bottom on the stock's).
const Box3 kStock = box(0, 0, -20, 100, 60, 0);
const Box3 kPart = box(2, 2, -20, 98, 58, -2);

// A vise jaw along the stock's front (-Y) face, its clamping face at y = `face`, from z = -15 up above the stock.
kernel::TriSoup frontJaw(double face)
{
    return boxSoup(box(-10, face - 20, -15, 110, face, 5));
}

std::vector<SimEvent> setupOf(const Program& p, sim::detail::OverlapStats* stats = nullptr)
{
    Program copy = p;
    const sim::detail::VerifyInput in = copy.input();
    JobContext ctx;
    Result<std::vector<SimEvent>> r = sim::detail::setupEvents(in, jobs(), ctx, stats);
    REQUIRE(r.has_value());
    return *r;
}

Program blockJob()
{
    Program p;
    p.stock = boxSoup(kStock);
    p.part = boxSoup(kPart);
    return p;
}

// A soup turned by `deg` about the Z axis through (cx, cy).
kernel::TriSoup turned(kernel::TriSoup s, double deg, double cx, double cy)
{
    const double a = deg * kPi / 180.0, c = dmath::cos(a), sn = dmath::sin(a);
    for (std::size_t i = 0; i + 2 < s.positions.size(); i += 3) {
        const double x = s.positions[i] - cx, y = s.positions[i + 1] - cy;
        s.positions[i] = cx + c * x - sn * y;
        s.positions[i + 1] = cy + sn * x + c * y;
    }
    return s;
}

bool inside(const Box3& b, const Vec3& p, double slack = 1e-9)
{
    return p.x >= b.min.x - slack && p.x <= b.max.x + slack && p.y >= b.min.y - slack && p.y <= b.max.y + slack &&
           p.z >= b.min.z - slack && p.z <= b.max.z + slack;
}

} // namespace

TEST_CASE("fixtures against the stock and the part: touching, rounding, the stock only, the part",
          "[sim][verify][setup]")
{
    SECTION("touching: the jaw's face on the stock's front face, the stock on a floor plate: nothing")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(0));
        p.fixtures.push_back(boxSoup(box(-20, -20, -40, 120, 80, -20))); // the floor under the stock and the part
        sim::detail::OverlapStats stats;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<SimEvent> ev = setupOf(p, &stats);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        INFO(describe(ev));
        CHECK(ev.empty());
        CHECK(stats.capped == 0);
        WARN("touching faces: " << stats.triangles << " triangles, " << stats.patches << " patches, " << ms << " ms on "
                                << jobs().workerCount() << " workers");
    }
    SECTION("rounding: 0.01 mm into the stock, and the floor 0.01 mm into the part: nothing")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(0.01));
        p.fixtures.push_back(boxSoup(box(-20, -20, -40, 120, 80, -19.99)));
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(describe(ev));
        CHECK(ev.empty());
    }
    SECTION("0.5 mm into the stock, not the part (2 mm in): a Warning")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(0.5));
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(describe(ev));
        REQUIRE(ev.size() == 1);
        const SimEvent& e = ev[0];
        CHECK(e.kind == EventKind::FixtureHitsStock);
        CHECK(e.severity == Severity::Warning);
        CHECK(e.moveIndex == kSetupEvent);
        CHECK(e.operation == OperationId{});
        CHECK_THAT(e.message, ContainsSubstring("Fixture 1 overlaps the stock"));
        CHECK_THAT(e.message, ContainsSubstring("not the part"));
        // A point of the overlap: in the jaw and in the stock, as deep as found (at most the overlap's 0.5).
        CHECK(inside(box(-10, -19.5, -15, 110, 0.5, 5), e.position));
        CHECK(inside(kStock, e.position));
        CHECK(e.amountMm > sim::detail::kFixtureOverlapMm);
        CHECK(e.amountMm <= 0.5 + 1e-9);
    }
    SECTION("3 mm in: into the part too: one Error, no Warning")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(3));
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(describe(ev));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == EventKind::FixtureHitsPart);
        CHECK(ev[0].severity == Severity::Error);
        CHECK(ev[0].moveIndex == kSetupEvent);
        CHECK_THAT(ev[0].message, ContainsSubstring("Fixture 1 cuts into the part"));
        CHECK(inside(kPart, ev[0].position));
        CHECK(ev[0].amountMm > sim::detail::kFixtureOverlapMm);
        CHECK(ev[0].amountMm <= 1.0 + 1e-9);
    }
    SECTION("just past the tolerance: 0.07 mm into the stock is found (patches settle within a quarter of it)")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(0.07));
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(describe(ev));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == EventKind::FixtureHitsStock);
    }
    SECTION("a fixture clear of everything, and none at all: nothing")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(-5));
        p.fixtures.push_back(boxSoup(box(200, 200, 0, 220, 220, 10)));
        CHECK(setupOf(p).empty());
        Program none = blockJob();
        CHECK(setupOf(none).empty());
    }
    SECTION("several fixtures: one event each, in fixture order, the clear one without")
    {
        Program p = blockJob();
        p.fixtures.push_back(frontJaw(3));                            // into the part
        p.fixtures.push_back(frontJaw(-5));                           // clear
        p.fixtures.push_back(boxSoup(box(99, -10, -15, 120, 70, 5))); // 1 mm into the stock's +X side only
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(describe(ev));
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].kind == EventKind::FixtureHitsPart);
        CHECK_THAT(ev[0].message, ContainsSubstring("Fixture 1 "));
        CHECK(ev[1].kind == EventKind::FixtureHitsStock);
        CHECK_THAT(ev[1].message, ContainsSubstring("Fixture 3 "));
    }
}

TEST_CASE("one solid wholly inside another, faces at an angle, a curved part", "[sim][verify][setup]")
{
    SECTION("a block wholly inside the stock (no surface of the stock in it), and the stock wholly inside a block")
    {
        Program p;
        p.stock = boxSoup(kStock);
        p.fixtures.push_back(boxSoup(box(40, 20, -15, 60, 40, -5)));
        std::vector<SimEvent> ev = setupOf(p);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == EventKind::FixtureHitsStock);
        Program q;
        q.stock = boxSoup(kStock);
        q.fixtures.push_back(boxSoup(box(-10, -10, -30, 110, 70, 10)));
        ev = setupOf(q);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].kind == EventKind::FixtureHitsStock);
    }
    SECTION("turned 30 degrees about Z: touching, 0.01 mm in, 0.5 mm in")
    {
        for (const double in : {0.0, 0.01, 0.5}) {
            Program p;
            p.stock = turned(boxSoup(kStock), 30, 50, 30);
            p.fixtures.push_back(turned(frontJaw(in), 30, 50, 30));
            const std::vector<SimEvent> ev = setupOf(p);
            INFO(in << " mm in\n" << describe(ev));
            if (in < sim::detail::kFixtureOverlapMm) {
                CHECK(ev.empty());
            } else {
                REQUIRE(ev.size() == 1);
                CHECK(ev[0].kind == EventKind::FixtureHitsStock);
            }
        }
    }
    SECTION("a round part (chords within 0.009 mm) between two jaws: touching it, then 1 mm into it")
    {
        for (const double in : {0.0, 1.0}) {
            Program p;
            p.stock = boxSoup(box(-25, -25, 0, 25, 25, 30));
            p.part = tubeSoup(0, 0, 0, 20, 0, 30, sidesFor(20, 0.009));
            p.fixtures.push_back(boxSoup(box(20 - in, -30, 5, 40, 30, 25)));
            p.fixtures.push_back(boxSoup(box(-40, -30, 5, -20 + in, 30, 25)));
            sim::detail::OverlapStats stats;
            const std::vector<SimEvent> ev = setupOf(p, &stats);
            INFO(in << " mm in\n" << describe(ev));
            if (in == 0.0) {
                // The jaws stand 5 mm into the square stock beside the round part: the stock only.
                REQUIRE(ev.size() == 2);
                CHECK(ev[0].kind == EventKind::FixtureHitsStock);
                CHECK(ev[1].kind == EventKind::FixtureHitsStock);
            } else {
                REQUIRE(ev.size() == 2);
                CHECK(ev[0].kind == EventKind::FixtureHitsPart);
                CHECK(ev[1].kind == EventKind::FixtureHitsPart);
            }
            CHECK(stats.capped == 0);
        }
    }
}

TEST_CASE("a stock carried from an earlier setup: its cut walls stand up to half a cell proud, which is allowed for",
          "[sim][verify][setup][carried]")
{
    // Setup 1: a 40 x 40 x 20 block, its +X side cut back to x = 38.9 above z = 5, simulated at the carried stock's
    // cell (cam::kCarriedStockCellMm, 0.25 mm). The carried soup's wall stands where its 0.25 mm columns put it (at
    // x = 39.0: the column whose ray at x = 38.875 holds material), 0.1 mm proud of the true wall.
    const double cell = 0.25;
    SimFixture one;
    one.prog.stock = boxSoup(box(0, 0, 0, 40, 40, 20));
    one.prog.op(Path(41.9, -10, 25).rapid(41.9, -10, 5).feed(41.9, 50, 5).rapid(41.9, 50, 25).done(),
                endMill(6, 25, 60));
    const SimProgram& sp = one.build();
    auto sim = makeSimulator(jobs(), nullptr);
    SimOptions o{};
    o.resolution = StockResolution{cell};
    o.safetyChecks = false;
    o.gouges = false;
    JobContext ctx;
    REQUIRE(sim->run(sp, o, ctx));
    const kernel::TriSoup carried = sim->finalStock();
    REQUIRE_FALSE(carried.indices.empty());

    // Setup 2: a jaw against the true cut wall (x = 38.9, above the cut's floor), then 1 mm into it.
    const auto check = [&](double face, double stockCell) {
        Program p;
        p.stock = carried;
        p.fixtures.push_back(boxSoup(box(face, -10, 6, 60, 50, 25)));
        const sim::detail::VerifyInput in = [&] {
            sim::detail::VerifyInput v = p.input();
            v.stockCellMm = stockCell;
            return v;
        }();
        JobContext c;
        Result<std::vector<SimEvent>> r = sim::detail::setupEvents(in, jobs(), c);
        REQUIRE(r.has_value());
        return *r;
    };
    std::vector<SimEvent> ev = check(38.9, cell);
    INFO(describe(ev));
    CHECK(ev.empty());
    ev = check(37.9, cell);
    INFO(describe(ev));
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == EventKind::FixtureHitsStock);
    // Without the allowance the 0.1 mm the columns stand proud would read as an overlap.
    ev = check(38.9, 0);
    INFO(describe(ev));
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].amountMm > sim::detail::kFixtureOverlapMm);
    CHECK(ev[0].amountMm <= 0.1 + 1e-3);
    // SimProgram::stockCellMm reaches the check.
    SimFixture two;
    two.prog.stock = carried;
    two.prog.fixtures.push_back(boxSoup(box(38.9, -10, 6, 60, 50, 25)));
    SimProgram& sp2 = two.build();
    sp2.stockCellMm = cell;
    CHECK(sim::detail::verifyInputFrom(sp2).stockCellMm == cell);
}

TEST_CASE("the setup's events are the same for any worker count", "[sim][verify][setup]")
{
    Program p = blockJob();
    p.fixtures.push_back(turned(frontJaw(0.4), 3, 50, 0));
    p.fixtures.push_back(boxSoup(box(97, -10, -15, 120, 70, 5)));
    p.fixtures.push_back(tubeSoup(50, 30, 0, 5, -25, -12, 64));
    std::string first;
    for (const int workers : {1, 3, 16, 1}) {
        jobs().setWorkerCount(workers);
        const std::vector<SimEvent> ev = setupOf(p);
        INFO(workers << " workers\n" << describe(ev));
        CHECK(ev.size() == 3);
        if (first.empty()) {
            first = eventsText(ev);
        } else {
            CHECK(eventsText(ev) == first);
        }
    }
    jobs().setWorkerCount(8);
}

TEST_CASE("the simulator lists the setup's events first; stopOnError passes over them", "[sim][verify][setup]")
{
    // A rapid into the stock at move 1 (an Error of a move), and a jaw 0.5 mm into the stock (a Warning of the setup)
    // and a floor 1 mm into the part (an Error of the setup).
    SimFixture f;
    f.prog.stock = boxSoup(kStock);
    f.prog.part = boxSoup(kPart);
    f.prog.fixtures.push_back(frontJaw(0.5));
    f.prog.fixtures.push_back(boxSoup(box(-20, -20, -40, 120, 80, -19)));
    f.prog.op(Path(50, 30, 10).rapid(60, 30, 10).rapid(60, 30, -1).rapid(60, 30, 10).done(), endMill(6, 20, 60));
    const SimProgram& sp = f.build();
    for (const bool stop : {false, true}) {
        auto sim = makeSimulator(jobs(), nullptr);
        SimOptions o{};
        o.resolution = StockResolution{0.2};
        o.stopOnError = stop;
        JobContext ctx;
        REQUIRE(sim->run(sp, o, ctx));
        const std::vector<SimEvent> ev(sim->events().begin(), sim->events().end());
        INFO((stop ? "stopOnError\n" : "\n") << describe(ev));
        REQUIRE(ev.size() >= 3);
        CHECK(ev[0].kind == EventKind::FixtureHitsStock);
        CHECK(ev[1].kind == EventKind::FixtureHitsPart);
        CHECK(ev[0].moveIndex == kSetupEvent);
        CHECK(ev[1].moveIndex == kSetupEvent);
        CHECK(ev[2].kind == EventKind::RapidIntoStock);
        CHECK(ev[2].moveIndex == 1);
        // Stopped at the move's Error, not at the setup's.
        CHECK(sim->runEnd().moveIndex == (stop ? 1u : 2u));
    }
    // verifyFast gives them too, first.
    auto sim = makeSimulator(jobs(), nullptr);
    JobContext ctx;
    const Result<std::vector<SimEvent>> fast = sim->verifyFast(sp, ctx);
    REQUIRE(fast.has_value());
    REQUIRE(fast->size() >= 3);
    CHECK((*fast)[0].kind == EventKind::FixtureHitsStock);
    CHECK((*fast)[1].kind == EventKind::FixtureHitsPart);
}
