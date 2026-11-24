// The simulator's results (events, gouges, removed volumes, the stock and the carried solid) do not depend on the
// worker count, the batch boundaries, the keyframe spacing (thinning included), or whether a state was reached by a
// straight run or by seek; the events do not depend on the tier's cell (task T-072).
#include "simulator_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <random>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// A program with a little of everything: facing, a seeded floor gouge, arcs round a boss with a wall gouge, a rapid
// into the stock, peck and plain drilling, and ball-end moves over the part's curved boss.
SimFixture mixedFixture()
{
    SimFixture f;
    f.prog.stock = boxSoup(box(0, 0, 0, 40, 40, 16));
    f.prog.part = joined(boxSoup(box(0, 0, 0, 40, 40, 10)), tubeSoup(20, 20, 0, 5, 9, 14, sidesFor(5, 0.008)));
    Path face(-8, 2, 20);
    face.rapid(-8, 2, 12);
    bool fwd = true;
    for (double y = 2; y <= 38; y += 4) {
        face.feed(fwd ? -8 : 48, y, 12).feed(fwd ? 48 : -8, y, 12);
        fwd = !fwd;
    }
    face.rapid(-8, 38, 20).rapid(-8, 3, 20).rapid(-8, 3, 9.9).feed(48, 3, 9.9).rapid(48, 3, 20);
    f.prog.op(face.done(), endMill(6, 20, 60));
    f.prog.op(Path(28, -8, 20)
                  .rapid(28, -8, 11)
                  .feed(28, 20, 11)
                  .arc(false, 28, 20, 11, 20, 20)
                  .feed(27.95, 20, 11)
                  .arc(true, 12.05, 20, 11, 20, 20)
                  .rapid(12.05, 20, 30)
                  .rapid(30, 30, 30)
                  .rapid(30, 30, 11) // into the stock the facing left
                  .done(),
              endMill(6, 20, 60));
    f.prog.op(Path(34, 34, 20)
                  .cycle(cam::MoveKind::CyclePeck, 34, 34, 8, 16, 17, 20, 1.5)
                  .cycle(cam::MoveKind::CycleDrill, 6, 34, 11, 16, 17, 20)
                  .cancel()
                  .done(),
              endMill(4, 20, 60, lib::ToolType::Drill));
    Path ballPath(10, 10, 20);
    ballPath.rapid(10, 10, 15);
    for (int k = 0; k < 40; ++k) {
        const double a = 0.15 * k;
        ballPath.feed(20 + 8 * dmath::cos(a), 20 + 8 * dmath::sin(a), 12.5 + 0.05 * k);
    }
    f.prog.op(ballPath.done(), endMill(4, 15, 50, lib::ToolType::BallEndMill));
    return f;
}

struct Outcome {
    std::string events;
    std::vector<std::uint64_t> removedBits;
    std::uint64_t hash = 0;
    std::vector<double> solidPositions;
    std::vector<std::uint32_t> solidIndices;
    std::vector<std::uint64_t> seekHashes;
};

std::vector<TimelinePosition> samplePositions(const Program& p)
{
    std::vector<TimelinePosition> out;
    for (std::uint32_t op = 0; op < p.ops.size(); ++op) {
        const std::uint32_t moves = static_cast<std::uint32_t>(p.ops[op].path->moves.size());
        for (std::uint32_t m = 0; m < moves; m += 4) {
            out.push_back({op, m, 0.6});
        }
    }
    std::mt19937 rng(11u);
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

Outcome outcome(ISimulator& sim, const SimProgram& sp, const Program& p, double cell)
{
    SimOptions o{};
    o.resolution = StockResolution{cell};
    JobContext ctx;
    REQUIRE(sim.run(sp, o, ctx));
    Outcome r;
    r.events = eventsText(sim.events());
    const sim::detail::SimRunInfo info = sim::detail::simRunInfo(sim);
    for (double v : info.removedMm3) {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, 8);
        r.removedBits.push_back(u);
    }
    r.hash = sim::detail::triDexelHash(sim.stock());
    const kernel::TriSoup solid = sim.finalStock();
    r.solidPositions = solid.positions;
    r.solidIndices = solid.indices;
    for (const TimelinePosition& pos : samplePositions(p)) {
        REQUIRE(sim.seek(pos));
        r.seekHashes.push_back(sim::detail::triDexelHash(sim.stock()));
    }
    return r;
}

void checkSame(const Outcome& a, const Outcome& b)
{
    CHECK(a.events == b.events);
    CHECK(a.removedBits == b.removedBits);
    CHECK(a.hash == b.hash);
    CHECK(a.solidPositions == b.solidPositions);
    CHECK(a.solidIndices == b.solidIndices);
    CHECK(a.seekHashes == b.seekHashes);
}

} // namespace

TEST_CASE("Simulator results do not depend on workers, batches or keyframes", "[sim][simulator][determinism]")
{
    SimFixture f = mixedFixture();
    const SimProgram& sp = f.build();
    const double cell = 0.1;
    auto base = makeSimulator(jobs(), nullptr);
    const Outcome want = outcome(*base, sp, f.prog, cell);
    // The program does raise events (the gouges and the rapid), so their equality means something.
    CHECK(want.events.find("Gouge") != std::string::npos);
    CHECK(want.events.find("Rapid") != std::string::npos);
    CHECK(!want.seekHashes.empty());

    // Worker counts 1, 4 and 16.
    {
        for (const int w : {1, 4, 16}) {
            INFO("workers " << w);
            jobs().setWorkerCount(w);
            auto sim = makeSimulator(jobs(), nullptr);
            checkSame(outcome(*sim, sp, f.prog, cell), want);
        }
        jobs().setWorkerCount(8);
    }
    // Batches of one motion, of five, and of little work.
    {
        for (int k = 0; k < 3; ++k) {
            INFO("batching " << k);
            sim::detail::SimTuning t;
            if (k == 0) {
                t.maxBatchMotions = 1;
            } else if (k == 1) {
                t.maxBatchMotions = 5;
            } else {
                t.batchRays = 50000.0;
            }
            auto sim = makeSimulator(jobs(), nullptr);
            sim::detail::setSimTuning(*sim, t);
            checkSame(outcome(*sim, sp, f.prog, cell), want);
        }
    }
    // Keyframes every motion, every 13, and thinned to a one-byte budget.
    {
        for (int k = 0; k < 3; ++k) {
            INFO("keyframes " << k);
            sim::detail::SimTuning t;
            if (k == 0) {
                t.keyframeMotions = 1;
            } else if (k == 1) {
                t.keyframeMotions = 13;
            } else {
                t.keyframeRays = 1.0;
                t.keyframeBudgetBytes = 1;
            }
            auto sim = makeSimulator(jobs(), nullptr);
            sim::detail::setSimTuning(*sim, t);
            checkSame(outcome(*sim, sp, f.prog, cell), want);
            const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
            if (k == 0) {
                CHECK(info.keyframeMotions.size() == info.runMotions + 1);
            }
            if (k == 2) {
                CHECK(info.thinnings > 0);
                CHECK(info.keyframeMotions.size() <= 3);
            }
        }
    }
    // A state reached by seek is the state a straight run reaches.
    {
        // The first simulator, after its sampled seeks: from the start to the end, forward in small steps, then
        // straight to the end from the middle.
        REQUIRE(base->seek({0, 0, 0.0}));
        REQUIRE(base->seek(base->runEnd()));
        CHECK(sim::detail::triDexelHash(base->stock()) == want.hash);
        REQUIRE(base->seek({1, 3, 0.5}));
        for (std::uint32_t m = 3; m < f.prog.ops[1].path->moves.size(); ++m) {
            REQUIRE(base->seek({1, m, 0.2}));
        }
        REQUIRE(base->seek({99, 0, 0.0}));
        CHECK(sim::detail::triDexelHash(base->stock()) == want.hash);
        // And every sampled position against the stock cut without the simulator.
        const std::vector<TimelinePosition> positions = samplePositions(f.prog);
        for (std::size_t i = 0; i < positions.size(); ++i) {
            CHECK(want.seekHashes[i] == referenceHash(sp, cell, positions[i]));
        }
    }
}

TEST_CASE("Simulator events do not depend on the tier's cell", "[sim][simulator][determinism]")
{
    SimFixture f = mixedFixture();
    const SimProgram& sp = f.build();
    std::string first;
    for (const double cell : {0.2, 0.1, 0.05}) {
        INFO("cell " << cell);
        auto sim = makeSimulator(jobs(), nullptr);
        SimOptions o{};
        o.resolution = StockResolution{cell};
        JobContext ctx;
        REQUIRE(sim->run(sp, o, ctx));
        const std::string text = eventsText(sim->events());
        if (first.empty()) {
            first = text;
            CHECK(!first.empty());
        }
        CHECK(text == first);
    }
}
