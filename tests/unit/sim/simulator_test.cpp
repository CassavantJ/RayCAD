// The simulator (task T-072): a full run with progress and cancel through cutBatch, seek anywhere against a stock cut
// without the simulator, the tool's pose, gouges with depth and move index on seeded-gouge programs (and none on passes
// that follow curved design faces), stopOnError, and finalStock as a closed solid.
#include "simulator_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

SimOptions options(double cell, bool checks = true)
{
    SimOptions o{};
    o.resolution = StockResolution{cell};
    o.safetyChecks = checks;
    return o;
}

Status runSim(ISimulator& sim, const SimProgram& sp, const SimOptions& o)
{
    JobContext ctx;
    return sim.run(sp, o, ctx);
}

std::vector<SimEvent> gougesOf(std::span<const SimEvent> events)
{
    std::vector<SimEvent> out;
    for (const SimEvent& e : events) {
        if (e.kind == EventKind::Gouge) {
            out.push_back(e);
        }
    }
    return out;
}

std::vector<std::string> summaryOf(std::span<const SimEvent> events, const Program& p)
{
    return summary(std::vector<SimEvent>(events.begin(), events.end()), p);
}

// One slot 6 wide and 3 deep across a 40 x 40 x 20 block, with a flat 6 mm end mill.
SimFixture slotFixture()
{
    SimFixture f;
    f.prog.stock = boxSoup(box(0, 0, 0, 40, 40, 20));
    f.prog.op(Path(-5, 20, 25).rapid(-5, 20, 17).feed(45, 20, 17).rapid(45, 20, 25).done(), endMill(6, 20, 60));
    return f;
}

// A pocket of many short moves: zigzag rows over a 60 x 60 block at two levels.
SimFixture manyMovesFixture(double step)
{
    SimFixture f;
    f.prog.stock = boxSoup(box(0, 0, 0, 60, 60, 20));
    Path p(-5, 2, 25);
    p.rapid(-5, 2, 18);
    for (double z : {18.0, 16.0}) {
        bool fwd = true;
        for (double y = 2; y <= 58; y += 4) {
            p.feed(fwd ? -5 : 65, y, z);
            for (double x = fwd ? -5 + step : 65 - step; fwd ? x <= 65 : x >= -5; x += fwd ? step : -step) {
                p.feed(x, y, z);
            }
            fwd = !fwd;
        }
    }
    p.rapid(-5, 58, 25);
    f.prog.op(p.done(), endMill(6, 20, 60));
    return f;
}

// The seeded-gouge program: a 40 x 40 x 10 part with a 10 x 10 boss to z 14, stock to z 16, a flat 6 mm end mill.
// Operation 0 faces at z 10 (on the part: no gouge), then a row at z 9.8 (move 5: 0.2 mm into the floor).
// Operation 1 runs round the boss at 3 mm from its walls with arcs at the corners (on the part: no gouge), then steps
// 0.1 mm into the left wall (move 9), runs along it (10) and rapids up along it (11): one gouge of 0.1 mm.
SimFixture gougeFixture()
{
    SimFixture f;
    f.prog.stock = boxSoup(box(0, 0, 0, 40, 40, 16));
    f.prog.part = joined(boxSoup(box(0, 0, 0, 40, 40, 10)), boxSoup(box(15, 15, 9, 25, 25, 14)));
    f.prog.op(Path(-8, 5, 20)
                  .rapid(-8, 5, 10)   // 0
                  .feed(48, 5, 10)    // 1: on the floor
                  .rapid(48, 5, 20)   // 2
                  .rapid(-8, 12, 20)  // 3
                  .rapid(-8, 12, 9.8) // 4
                  .feed(48, 12, 9.8)  // 5: 0.2 mm into the floor
                  .rapid(48, 12, 20)  // 6
                  .done(),
              endMill(6, 20, 60));
    f.prog.op(Path(12, -8, 20)
                  .rapid(12, -8, 11)             // 0
                  .feed(12, 25, 11)              // 1: along the left wall at 3 mm
                  .arc(true, 15, 28, 11, 15, 25) // 2
                  .feed(25, 28, 11)              // 3
                  .arc(true, 28, 25, 11, 25, 25) // 4
                  .feed(28, 15, 11)              // 5
                  .arc(true, 25, 12, 11, 25, 15) // 6
                  .feed(15, 12, 11)              // 7
                  .arc(true, 12, 15, 11, 15, 15) // 8
                  .feed(12.1, 15, 11)            // 9: 0.1 mm into the corner
                  .feed(12.1, 25, 11)            // 10: along the wall, 0.1 mm in
                  .rapid(12.1, 25, 20)           // 11: up along it
                  .done(),
              endMill(6, 20, 60));
    return f;
}

} // namespace

TEST_CASE("A run cuts every move through cutBatch, reports progress and ends at the end", "[sim][simulator]")
{
    SimFixture f = slotFixture();
    // The model frame: turned 90 degrees about Z and moved, so the pose is checked through modelFromWcs.
    f.modelFromWcs = Xform{{0, -1, 0, 100, 1, 0, 0, 0, 0, 0, 1, 50}};
    const SimProgram& sp = f.build();
    auto sim = makeSimulator(jobs(), makeTriDexelCpu(jobs()));
    std::vector<double> seen;
    std::vector<std::string> stages;
    JobContext ctx;
    ctx.progress = Progress([&](double v, std::string_view stage) {
        seen.push_back(v);
        stages.emplace_back(stage);
    });
    REQUIRE(sim->run(sp, options(0.1), ctx));
    REQUIRE(!seen.empty());
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(seen.back() == 1.0);
    CHECK(sim->events().empty());
    CHECK(sim->totalMoves() == 3);
    const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
    CHECK(info.ready);
    CHECK(info.batches >= 1);
    CHECK(info.batches < info.runMotions);
    // The slot: 6 wide, 3 deep, 40 long (its round ends lie outside the block); within one cell layer of its faces.
    double removed = 0;
    for (double v : info.removedMm3) {
        removed += v;
    }
    CHECK(std::abs(removed - 720.0) < (6 * 40 + 2 * 3 * 40) * 0.1);
    // At the end: the last move's end, in the model frame.
    const TimelinePosition end = sim->runEnd();
    CHECK(end.opIndex == 0);
    CHECK(end.moveIndex == 2);
    CHECK(end.t01 == 1.0);
    CHECK(sim->position().moveIndex == 2);
    Xform pose = sim->toolPose();
    CHECK(pose.m[0] == 0.0);
    CHECK(pose.m[1] == -1.0);
    CHECK(pose.m[4] == 1.0);
    CHECK(std::abs(pose.m[3] - (100.0 - 20.0)) < 1e-9);
    CHECK(std::abs(pose.m[7] - 45.0) < 1e-9);
    CHECK(std::abs(pose.m[11] - (25.0 + 50.0)) < 1e-9);
    // Halfway along the slot.
    REQUIRE(sim->seek({0, 1, 0.5}));
    pose = sim->toolPose();
    CHECK(std::abs(pose.m[3] - (100.0 - 20.0)) < 1e-9);
    CHECK(std::abs(pose.m[7] - 20.0) < 1e-9);
    CHECK(std::abs(pose.m[11] - (17.0 + 50.0)) < 1e-9);
    CHECK(sim->position().moveIndex == 1);
    CHECK(sim->position().t01 == 0.5);
    // Before the first move: the start point.
    REQUIRE(sim->seek({0, 0, 0.0}));
    pose = sim->toolPose();
    CHECK(std::abs(pose.m[7] - (-5.0)) < 1e-9);
    CHECK(std::abs(pose.m[11] - (25.0 + 50.0)) < 1e-9);
}

TEST_CASE("A run without stock is refused and a seek before a run is refused", "[sim][simulator]")
{
    SimFixture f = slotFixture();
    f.prog.stock.reset();
    const SimProgram& sp = f.build();
    auto sim = makeSimulator(jobs(), nullptr);
    CHECK(!sim->seek({0, 0, 0}));
    const Status s = runSim(*sim, sp, options(0.1));
    REQUIRE(!s);
    CHECK(s.error().code == ErrorCode::InvalidArgument);
    CHECK(sim->totalMoves() == 0);
    CHECK(sim->finalStock().indices.empty());
}

TEST_CASE("A cancelled run leaves the simulator empty and usable", "[sim][simulator]")
{
    SimFixture f = manyMovesFixture(0.25);
    const SimProgram& sp = f.build();
    const double cell = 0.05;
    auto fresh = makeSimulator(jobs(), nullptr);
    REQUIRE(runSim(*fresh, sp, options(cell)));
    const std::uint64_t want = sim::detail::triDexelHash(fresh->stock());
    const std::size_t wantEvents = fresh->events().size();

    for (const std::string stage : {"Checking moves", "Simulating material removal"}) {
        INFO(stage);
        auto sim = makeSimulator(jobs(), nullptr);
        CancelSource source;
        bool cancelledThere = false;
        JobContext ctx;
        ctx.cancel = source.token();
        ctx.progress = Progress([&](double v, std::string_view st) {
            if (!cancelledThere && st == stage && v < 1.0) {
                cancelledThere = source.cancel();
            }
        });
        const Status s = sim->run(sp, options(cell), ctx);
        REQUIRE(cancelledThere);
        REQUIRE(!s);
        CHECK(s.error().code == ErrorCode::Cancelled);
        CHECK(sim->totalMoves() == 0);
        CHECK(sim->events().empty());
        CHECK(!sim->seek({0, 0, 0}));
        CHECK(sim->finalStock().indices.empty());
        // Usable: the next run is the same as a fresh simulator's.
        REQUIRE(runSim(*sim, sp, options(cell)));
        CHECK(sim::detail::triDexelHash(sim->stock()) == want);
        CHECK(sim->events().size() == wantEvents);
    }
    // A token cancelled before the run starts.
    auto sim = makeSimulator(jobs(), nullptr);
    CancelSource source;
    source.cancel();
    JobContext ctx;
    ctx.cancel = source.token();
    const Status s = sim->run(sp, options(cell), ctx);
    REQUIRE(!s);
    CHECK(s.error().code == ErrorCode::Cancelled);
    CHECK(sim->totalMoves() == 0);
}

TEST_CASE("Seek anywhere gives the stock a direct cut gives, in any order", "[sim][simulator]")
{
    SimFixture f = gougeFixture();
    // A drilling operation too, so canned cycles are on the timeline.
    f.prog.op(Path(30, 5, 20)
                  .cycle(cam::MoveKind::CyclePeck, 30, 5, 11, 16, 17, 20, 1.5)
                  .cycle(cam::MoveKind::CycleDrill, 34, 5, 12, 16, 17, 20)
                  .cancel()
                  .done(),
              endMill(4, 20, 60, lib::ToolType::Drill));
    const SimProgram& sp = f.build();
    const double cell = 0.2;
    for (const std::uint32_t every : {0u, 1u, 3u}) {
        INFO("keyframe every " << every << " motions (0: by work)");
        auto sim = makeSimulator(jobs(), nullptr);
        sim::detail::SimTuning tuning;
        tuning.keyframeMotions = every;
        sim::detail::setSimTuning(*sim, tuning);
        REQUIRE(runSim(*sim, sp, options(cell, false)));
        CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, sim->runEnd()));
        std::vector<TimelinePosition> positions;
        for (std::uint32_t op = 0; op < f.prog.ops.size(); ++op) {
            const std::uint32_t moves = static_cast<std::uint32_t>(f.prog.ops[op].path->moves.size());
            for (std::uint32_t m = 0; m < moves; ++m) {
                for (double t : {0.0, 0.37, 1.0}) {
                    positions.push_back({op, m, t});
                }
            }
        }
        std::mt19937 rng(7u + every);
        std::shuffle(positions.begin(), positions.end(), rng);
        for (const TimelinePosition& p : positions) {
            INFO("op " << p.opIndex << " move " << p.moveIndex << " t " << p.t01);
            REQUIRE(sim->seek(p));
            CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, p));
            CHECK(sim->position().opIndex == p.opIndex);
            CHECK(sim->position().moveIndex == p.moveIndex);
        }
        // Forward playback, as a timeline plays: small steps.
        for (std::uint32_t m = 0; m < f.prog.ops[1].path->moves.size(); ++m) {
            for (double t = 0.0; t < 1.0; t += 0.25) {
                REQUIRE(sim->seek({1, m, t}));
                CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {1, m, t}));
            }
        }
        // Past the end, a move past an operation's end, t01 out of range.
        REQUIRE(sim->seek({99, 0, 0}));
        CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, sim->runEnd()));
        CHECK(sim->position().opIndex == sim->runEnd().opIndex);
        REQUIRE(sim->seek({0, 999, 0}));
        CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {1, 0, 0.0}));
        REQUIRE(sim->seek({1, 2, 7.0}));
        CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {1, 2, 1.0}));
        REQUIRE(sim->seek({1, 2, std::nan("")}));
        CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {1, 2, 0.0}));
    }
}

TEST_CASE("Seeded gouges are reported with their depth and move index", "[sim][simulator][gouge]")
{
    SimFixture f = gougeFixture();
    const SimProgram& sp = f.build();
    for (const double cell : {0.2, 0.1, 0.05}) {
        INFO("cell " << cell);
        auto sim = makeSimulator(jobs(), nullptr);
        REQUIRE(runSim(*sim, sp, options(cell)));
        INFO(describe(std::vector<SimEvent>(sim->events().begin(), sim->events().end())));
        // Exactly the two gouges, and nothing else (the program is otherwise safe).
        CHECK(summaryOf(sim->events(), f.prog) == std::vector<std::string>{"Gouge@0.5", "Gouge@1.9"});
        const std::vector<SimEvent> g = gougesOf(sim->events());
        REQUIRE(g.size() == 2);
        // Depths: the true one, plus at most the margins the report adds (an upper bound).
        CHECK(g[0].amountMm >= 0.2);
        CHECK(g[0].amountMm <= 0.2 + sim::detail::kGougePartChordMm + sim::detail::kArcChordTolMm + 2e-4);
        CHECK(g[1].amountMm >= 0.1);
        CHECK(g[1].amountMm <= 0.1 + sim::detail::kGougePartChordMm + sim::detail::kArcChordTolMm + 2e-4);
        CHECK(g[0].severity == Severity::Error);
        // Where the floor gouge starts: the tool, shrunk by the threshold, reaches the part at x 0.
        CHECK(std::abs(g[0].position.x - (-(3.0 - 0.04))) < 1e-3);
        CHECK(g[0].position.y == 12.0);
        CHECK(g[0].position.z == 9.8);
        // The wall gouge spans moves 9 to 11; the message names it and the tolerance.
        CHECK(g[1].message.find("this move and the 2 after it") != std::string::npos);
        CHECK(g[1].message.find("0.05 mm") != std::string::npos);
        CHECK(g[1].message.find("part's triangles") != std::string::npos);
    }
}

TEST_CASE("Passes on curved design faces are no gouges; a little further they are", "[sim][simulator][gouge]")
{
    // A boss (convex: the soup's chords lie inside the face) and a bore (concave: the soup's chords stick out into the
    // hole by up to 0.008 mm), each triangulated within 0.008 mm, cut round by full circles of a flat 6 mm end mill on
    // the face, then 0.1 mm into it.
    SimFixture f;
    f.prog.stock = boxSoup(box(0, 0, 0, 80, 40, 12));
    f.prog.part =
        joined(tubeSoup(20, 20, 0, 5, 0, 10, sidesFor(5, 0.008)), tubeSoup(60, 20, 10, 14, 0, 10, sidesFor(10, 0.008)));
    f.prog.op(Path(28, 20, 2)
                  .arc(true, 28, 20, 2, 20, 20)    // 0: round the boss on its face
                  .feed(27.9, 20, 2)               // 1: 0.1 mm in
                  .arc(true, 27.9, 20, 2, 20, 20)  // 2
                  .rapid(27.9, 20, 20)             // 3: straight up along it
                  .rapid(67, 20, 20)               // 4
                  .rapid(67, 20, 2)                // 5: into the bore
                  .arc(false, 67, 20, 2, 60, 20)   // 6: round the bore on its face
                  .feed(67.1, 20, 2)               // 7: 0.1 mm in
                  .arc(false, 67.1, 20, 2, 60, 20) // 8
                  .done(),
              endMill(6, 20, 60));
    const SimProgram& sp = f.build();
    auto sim = makeSimulator(jobs(), nullptr);
    SimOptions o = options(0.2, false);
    REQUIRE(runSim(*sim, sp, o));
    INFO(describe(std::vector<SimEvent>(sim->events().begin(), sim->events().end())));
    CHECK(summaryOf(sim->events(), f.prog) == std::vector<std::string>{"Gouge@0.1", "Gouge@0.7"});
    for (const SimEvent& e : sim->events()) {
        CHECK(e.amountMm >= 0.1);
        CHECK(e.amountMm <= 0.1 + 0.025);
    }
    // A tolerance below the minimum is raised, and the message says so.
    o.gougeTolMm = 0.01;
    REQUIRE(runSim(*sim, sp, o));
    CHECK(summaryOf(sim->events(), f.prog) == std::vector<std::string>{"Gouge@0.1", "Gouge@0.7"});
    REQUIRE(!sim->events().empty());
    CHECK(sim->events()[0].message.find("raised from 0.01 mm") != std::string::npos);
    CHECK(sim::detail::simRunInfo(*sim).gougeTolMm == sim::detail::kGougeMinTolMm);
    // A tolerance above the gouge: none.
    o.gougeTolMm = 0.2;
    REQUIRE(runSim(*sim, sp, o));
    CHECK(sim->events().empty());
    // Gouges off: none.
    o.gougeTolMm = 0.05;
    o.gouges = false;
    REQUIRE(runSim(*sim, sp, o));
    CHECK(sim->events().empty());
}

TEST_CASE("A tap into a hole modelled at its tap-drill size is no gouge; an end mill of its size is",
          "[sim][simulator][gouge]")
{
    // M8 x 1.25: tap drill 6.8 mm. The part is a block with that hole (a tube of inner radius 3.4).
    const auto run = [](const lib::Tool& tool, cam::MoveKind cycle) {
        SimFixture f;
        f.prog.stock = boxSoup(box(-10, -10, 0, 10, 10, 20));
        f.prog.part = tubeSoup(0, 0, 3.4, 10, 0, 20, sidesFor(3.4, 0.008));
        f.prog.op(Path(0, 0, 30).cycle(cycle, 0, 0, 5, 20, 22, 30).cancel().done(), tool);
        const SimProgram& sp = f.build();
        auto sim = makeSimulator(jobs(), nullptr);
        REQUIRE(runSim(*sim, sp, options(0.2, false)));
        return gougesOf(sim->events());
    };
    lib::Tool tap = endMill(8, 25, 70, lib::ToolType::Tap);
    tap.threadPitch = 1.25;
    CHECK(run(tap, cam::MoveKind::CycleTap).empty());
    // Without a pitch the tap is swept whole: reported.
    tap.threadPitch = 0;
    const std::vector<SimEvent> unknown = run(tap, cam::MoveKind::CycleTap);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0].amountMm >= 0.6);
    // A flat 8 mm end mill: 0.6 mm into the wall all round.
    const std::vector<SimEvent> g = run(endMill(8, 25, 70), cam::MoveKind::CycleDrill);
    REQUIRE(g.size() == 1);
    CHECK(g[0].amountMm >= 0.6);
    CHECK(g[0].amountMm <= 0.6 + 0.025);
}

TEST_CASE("A tool going wholly into the part reports its depth along the axis", "[sim][simulator][gouge]")
{
    // A 4 mm drill plunged 6 mm into a solid block (flutes 20 mm): the eroded tool's axis is in the part for 6 mm.
    SimFixture f;
    f.prog.stock = boxSoup(box(-10, -10, 0, 10, 10, 20));
    f.prog.part = boxSoup(box(-10, -10, 0, 10, 10, 15));
    f.prog.op(Path(0, 0, 25).rapid(0, 0, 16).feed(0, 0, 9).rapid(0, 0, 25).done(),
              endMill(4, 20, 60, lib::ToolType::Drill));
    const SimProgram& sp = f.build();
    auto sim = makeSimulator(jobs(), nullptr);
    REQUIRE(runSim(*sim, sp, options(0.2, false)));
    const std::vector<SimEvent> g = gougesOf(sim->events());
    REQUIRE(g.size() == 1);
    CHECK(g[0].moveIndex == 1);
    CHECK(g[0].amountMm >= 6.0);
    CHECK(g[0].amountMm <= 6.0 + 0.025);
    CHECK(g[0].message.find("wholly into the part") != std::string::npos);
    CHECK(g[0].message.find("along its axis") != std::string::npos);
}

TEST_CASE("stopOnError stops the removal at the end of the first error's move", "[sim][simulator]")
{
    SimFixture f = gougeFixture();
    const SimProgram& sp = f.build();
    const double cell = 0.2;
    auto sim = makeSimulator(jobs(), nullptr);
    SimOptions o = options(cell);
    o.stopOnError = true;
    REQUIRE(runSim(*sim, sp, o));
    CHECK(summaryOf(sim->events(), f.prog) == std::vector<std::string>{"Gouge@0.5"});
    const TimelinePosition end = sim->runEnd();
    CHECK(end.opIndex == 0);
    CHECK(end.moveIndex == 5);
    CHECK(end.t01 == 1.0);
    CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {0, 5, 1.0}));
    // Positions past the stop are the stop.
    REQUIRE(sim->seek({0, 0, 0.5}));
    REQUIRE(sim->seek({1, 4, 0.5}));
    CHECK(sim::detail::triDexelHash(sim->stock()) == referenceHash(sp, cell, {0, 5, 1.0}));
    CHECK(sim->position().opIndex == 0);
    CHECK(sim->position().moveIndex == 5);
    // The carried stock is the stock at the stop.
    const double v = closedSolidVolume(sim->finalStock());
    CHECK(v > 0);
}

TEST_CASE("finalStock is a closed solid: the stock at the run's end wherever the timeline is", "[sim][simulator]")
{
    SimFixture f = gougeFixture();
    const SimProgram& sp = f.build();
    auto sim = makeSimulator(jobs(), nullptr);
    REQUIRE(runSim(*sim, sp, options(0.2)));
    const kernel::TriSoup atEnd = sim->finalStock();
    const double volume = closedSolidVolume(atEnd);
    const sim::detail::TriDexelInfo info = sim::detail::triDexelInfo(sim->stock());
    // The solid's volume is the model's (Z rays), to rounding.
    CHECK(std::abs(volume - info.volume[0]) < 1e-3 * info.volume[0]);
    // The block less what the program cut.
    CHECK(volume < 40.0 * 40.0 * 16.0);
    CHECK(volume > 40.0 * 40.0 * 10.0);
    // Somewhere else on the timeline, the carried stock is the same bytes.
    REQUIRE(sim->seek({0, 3, 0.25}));
    const kernel::TriSoup elsewhere = sim->finalStock();
    CHECK(elsewhere.positions == atEnd.positions);
    CHECK(elsewhere.indices == atEnd.indices);
    // Material-only run (as for carried stock): no events, the same solid.
    auto carry = makeSimulator(jobs(), nullptr);
    SimOptions o = options(0.2);
    o.safetyChecks = false;
    o.gouges = false;
    REQUIRE(runSim(*carry, sp, o));
    CHECK(carry->events().empty());
    const kernel::TriSoup carried = carry->finalStock();
    CHECK(carried.positions == atEnd.positions);
    CHECK(carried.indices == atEnd.indices);
}
