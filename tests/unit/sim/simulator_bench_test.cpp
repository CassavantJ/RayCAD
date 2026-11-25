// The simulator on the S6 benchmark (task T-072; PLAN.md section 9: "Material-removal sim, 100k moves at tier
// resolution" Low 0.2 mm 20 s, Medium 0.1 mm 15 s, High 0.05 mm 10 s on 8 or more threads; "Timeline seek" Low 250 ms,
// Medium 150 ms, High 100 ms). The workload of tridexel_test_util.h as two operations (its jumps as a retract, a rapid
// over and a rapid down), with a part under the deepest cut so every motion goes through the gouge test. Measures the
// run, the safety checks, keyframes (count, memory, thinning), seeks to random positions, worst-case seeks (just before
// a keyframe, from after it: a restore and a whole interval recut) and playback steps. Timed against the wall clock, so
// it runs alone, as its own test ray_sim_simulator_bench ("[simulator-bench]", hidden from the suite;
// tests/tiers.cmake), apart from the stock model's benchmark in ray_sim_tests_wallclock so that neither takes much over
// a minute. The limits are checked only in an optimised build without sanitizers, which also runs the full 112k-move
// workload (other builds a thinned one). Every tier's seek budget is checked, High's too since the stock model copies
// tiles rather than whole strips on write (tridexel.h, SNAPSHOTS): its keyframes all fit High's keyframe budget.
// The same benchmark also runs the facing-heavy plate of the stress programs (below) at every tier, seeking into every
// keyframe interval, and times the finishing program's run at High. The stress programs themselves (facing, finishing,
// a long program, a large stock) are hidden tests "[stress]", for the report of what the keyframe policy holds and
// where it cannot (simulator.h, KEYFRAMES).
#include "simulator_test_util.h"

#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#endif

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

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

double seconds(std::chrono::steady_clock::time_point a)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

double processPeakMb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

// The benchmark workload as a program.
SimFixture benchFixture(int lanes, double stepover)
{
    const Workload w = benchmarkWorkload(lanes, stepover);
    SimFixture f;
    f.prog.stock = boxSoup(benchmarkStock());
    f.prog.part = boxSoup(box(0, 0, 0, 152.4, 101.6, 4.0));
    for (const Workload::Pass& pass : w.passes) {
        const Vec3 first = pass.moves.front().from;
        Path p(first.x, first.y, 30.0);
        p.rapid(first.x, first.y, first.z + 1.0).feed(first.x, first.y, first.z);
        Vec3 cur = first;
        for (const IStockModel::CutMove& m : pass.moves) {
            if (m.from.x != cur.x || m.from.y != cur.y || m.from.z != cur.z) {
                p.rapid(cur.x, cur.y, 30.0)
                    .rapid(m.from.x, m.from.y, 30.0)
                    .rapid(m.from.x, m.from.y, m.from.z + 1.0)
                    .feed(m.from.x, m.from.y, m.from.z);
            }
            p.feed(m.to.x, m.to.y, m.to.z);
            cur = m.to;
        }
        p.rapid(cur.x, cur.y, 30.0);
        const lib::Tool t = pass.tool == 0 ? tool(lib::ToolType::FlatEndMill, 12.7, 30.0)
                                           : tool(lib::ToolType::BallEndMill, 6.35, 25.0);
        f.prog.op(p.done(), t);
    }
    return f;
}

struct Tier {
    const char* name;
    double cell;
    int workers;
    double runBudget100k; // s per 100k moves
    double seekBudgetMs;
};

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5));
    return v[i];
}

// Checks the run's time against its row (checkRun) and the seeks against theirs (checkSeek), in an optimised build.
void runTier(const Tier& t, const SimProgram& sp, std::uint64_t moves, std::uint64_t budget = 0, bool checkRun = true,
             bool checkSeek = true)
{
    jobs().setWorkerCount(t.workers);
    auto sim = makeSimulator(jobs(), makeTriDexelCpu(jobs()));
    sim::detail::SimTuning tuning;
    tuning.keyframeBudgetBytes = budget;
    sim::detail::setSimTuning(*sim, tuning);
    SimOptions o{};
    o.resolution = StockResolution{t.cell};
    o.safetyChecks = false; // timed on their own (tier-independent)
    double runS = 0;
    const Result<bool> done = jobs()
                                  .submit<bool>(JobInfo{"bench", JobPriority::Normal, false},
                                                [&](JobContext& ctx) -> Result<bool> {
                                                    const auto t0 = std::chrono::steady_clock::now();
                                                    const Status s = sim->run(sp, o, ctx);
                                                    runS = seconds(t0);
                                                    if (!s) {
                                                        return tl::unexpected(s.error());
                                                    }
                                                    return true;
                                                })
                                  .wait();
    REQUIRE(done);
    const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
    CHECK(sim->events().empty());
    const double per100k = runS * 100000.0 / static_cast<double>(info.motions);

    // Seeks, as the timeline asks them: from a worker (a job), one at a time.
    const auto timedSeek = [&](TimelinePosition p) {
        double ms = 0;
        const Result<bool> r = jobs()
                                   .submit<bool>(JobInfo{"seek", JobPriority::Interactive, false},
                                                 [&](JobContext&) -> Result<bool> {
                                                     const auto t0 = std::chrono::steady_clock::now();
                                                     const Status s = sim->seek(p);
                                                     ms = seconds(t0) * 1e3;
                                                     return s ? Result<bool>(true) : tl::unexpected(s.error());
                                                 })
                                   .wait();
        REQUIRE(r);
        return ms;
    };
    const auto at = [&](std::uint64_t motion, double t01) {
        motion = std::min<std::uint64_t>(motion, info.motions - 1);
        return TimelinePosition{info.motionOp[motion], info.motionMove[motion], t01};
    };
    std::mt19937 rng(5u);
    std::uniform_int_distribution<std::uint64_t> pick(0, info.motions - 1);
    // Fewer seeks where they are not checked (each can take seconds there).
    const int randomSeeks = checkSeek ? 40 : 12;
    const std::size_t worstSeeks = checkSeek ? 12 : 4;
    std::vector<double> random;
    for (int i = 0; i < randomSeeks; ++i) {
        random.push_back(timedSeek(at(pick(rng), 0.5)));
    }
    // Worst case: just before a keyframe, coming from after it.
    std::vector<double> worst;
    const std::vector<std::uint64_t>& keys = info.keyframeMotions;
    const std::size_t intervals = keys.size() > 1 ? keys.size() - 1 : 0;
    const std::size_t stride = std::max<std::size_t>(1, intervals / worstSeeks);
    for (std::size_t k = 0; k < intervals; k += stride) {
        timedSeek(sim->runEnd());
        worst.push_back(timedSeek(at(keys[k + 1] - 1, 0.9)));
    }
    // Playback: one move forward at a time.
    timedSeek(at(info.motions / 2, 0.0));
    const TimelinePosition from = sim->position();
    double playMs = 0;
    int steps = 0;
    for (std::uint32_t m = from.moveIndex; m < from.moveIndex + 200 && m < sp.toolpaths[from.opIndex]->moves.size();
         ++m) {
        playMs += timedSeek({from.opIndex, m, 0.5});
        ++steps;
    }
    // The carried solid at the coarse cells.
    double solidMs = 0;
    std::size_t solidTris = 0;
    if (t.cell >= 0.1) {
        const auto t0 = std::chrono::steady_clock::now();
        const kernel::TriSoup solid = sim->finalStock();
        solidMs = seconds(t0) * 1e3;
        solidTris = solid.indices.size() / 3;
    }
    const double randomMax = *std::max_element(random.begin(), random.end());
    const double worstMax = worst.empty() ? 0.0 : *std::max_element(worst.begin(), worst.end());
    char line[1024];
    std::snprintf(
        line, sizeof line,
        "T-072 bench %s: cell %.2f mm, %d workers, %llu moves, %llu motions: run (removal and gouges) %.2f s "
        "(%.2f s per 100k), %llu batches, gouge test on %llu motions; keyframes %zu (spacing %.0f M rays, %d "
        "thinnings) pinning %.0f MB of %.0f; seek random median %.1f ms p95 %.1f ms max %.1f ms; worst case (a whole "
        "interval) max %.1f ms over %zu; playback %.2f ms per move; finalStock %zu triangles in %.0f ms; "
        "process peak %.0f MB",
        t.name, t.cell, t.workers, static_cast<unsigned long long>(moves),
        static_cast<unsigned long long>(info.motions), runS, per100k, static_cast<unsigned long long>(info.batches),
        static_cast<unsigned long long>(info.gougeMotionsTested), keys.size(), info.keyframeRays / 1e6, info.thinnings,
        static_cast<double>(info.keyframeBytes) / (1024.0 * 1024.0),
        static_cast<double>(info.keyframeBudgetBytes) / (1024.0 * 1024.0), percentile(random, 0.5),
        percentile(random, 0.95), randomMax, worstMax, worst.size(), steps > 0 ? playMs / steps : 0.0, solidTris,
        solidMs, processPeakMb());
    std::cout << line << std::endl;
    CHECK(info.keyframeBytes <= info.keyframeBudgetBytes);
    if (optimisedBuild() && checkRun) {
        CHECK(per100k <= t.runBudget100k);
    }
    if (optimisedBuild() && checkSeek) {
        CHECK(randomMax <= t.seekBudgetMs);
        CHECK(worstMax <= t.seekBudgetMs);
    }
    jobs().setWorkerCount(8);
}

} // namespace

// Not run by default ([.]): how well the work measure (rayCost) predicts the stock model's time, per cell, per part of
// the workload (facing, pocket, scallops): nanoseconds per ray over chunks of 256 motions. What the keyframe and batch
// constants of simulator.h were set from.
TEST_CASE("Simulator work measure calibration", "[.][sim][simulator][calibrate]")
{
    SimFixture f = benchFixture(11, 0.25);
    const SimProgram& sp = f.build();
    const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
    const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
    for (const int workers : {4, 8}) {
        jobs().setWorkerCount(workers);
        for (const double cell : {0.2, 0.1, 0.05}) {
            std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
            m->reset({}, benchmarkStock(), StockResolution{cell});
            const sim::detail::TriDexelInfo info = sim::detail::triDexelInfo(*m);
            const Vec3 c{info.cx, info.cy, info.cz};
            std::vector<double> nsPerRay[3];
            double totalRays = 0, totalS = 0;
            std::size_t i = 0;
            // Facing: operation 0 at its first level (z 24.4 and above); pocket: the rest of it; scallops: operation 1.
            const auto partOf = [&](const sim::detail::Motion& mo) {
                return mo.op == 1 ? 2 : (mo.a.z >= 24.0 && mo.b.z >= 24.0 ? 0 : 1);
            };
            while (i < motions.size()) {
                const std::uint32_t op = motions[i].op;
                const int part = partOf(motions[i]);
                std::vector<IStockModel::CutMove> moves;
                double rays = 0;
                const ToolShape& shape = sp.tools[op];
                double radius = 0;
                for (const Vec2& v : shape.cuttingProfile) {
                    radius = std::max(radius, v.x);
                }
                const double height = shape.cuttingProfile.back().y;
                while (i < motions.size() && motions[i].op == op && partOf(motions[i]) == part && moves.size() < 256) {
                    moves.push_back({motions[i].a, motions[i].b, motions[i].rapid});
                    rays += sim::detail::rayCost(motions[i], radius, height, benchmarkStock(), c);
                    ++i;
                }
                const auto t0 = std::chrono::steady_clock::now();
                m->cutBatch(shape, moves, {});
                const double s = seconds(t0);
                totalRays += rays;
                totalS += s;
                nsPerRay[part].push_back(s * 1e9 / rays);
            }
            for (int part = 0; part < 3; ++part) {
                std::printf("calibrate: %d workers, cell %.2f, %s: ns per ray median %.2f p90 %.2f max %.2f over %zu "
                            "chunks\n",
                            workers, cell,
                            part == 0   ? "facing"
                            : part == 1 ? "pocket"
                                        : "scallops",
                            percentile(nsPerRay[part], 0.5), percentile(nsPerRay[part], 0.9),
                            percentile(nsPerRay[part], 1.0), nsPerRay[part].size());
            }
            std::printf("calibrate: %d workers, cell %.2f: %.0f M rays in %.2f s, %.2f ns per ray\n", workers, cell,
                        totalRays / 1e6, totalS, totalS * 1e9 / totalRays);
            std::fflush(stdout);
        }
    }
    jobs().setWorkerCount(8);
}

TEST_CASE("Simulator benchmark: run and seek at every tier's resolution", "[.][sim][simulator][bench][simulator-bench]")
{
    const bool full = optimisedBuild();
    SimFixture f = full ? benchFixture(11, 0.25) : benchFixture(2, 2.0);
    const SimProgram& sp = f.build();
    std::uint64_t moves = 0;
    for (const auto& tp : sp.toolpaths) {
        moves += tp->moves.size();
    }
    if (full) {
        REQUIRE(moves > 100000);
    }
    // The safety checks once: they do not depend on the tier.
    {
        auto sim = makeSimulator(jobs(), nullptr);
        JobContext ctx;
        const auto t0 = std::chrono::steady_clock::now();
        const Result<std::vector<SimEvent>> ev = sim->verifyFast(sp, ctx);
        const double s = seconds(t0);
        REQUIRE(ev);
        std::printf("T-072 bench: safety checks (verifyFast, any tier, 8 workers) on %llu moves: %.2f s, %zu events\n",
                    static_cast<unsigned long long>(moves), s, ev->size());
        // The gouge test alone (any tier).
        const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
        const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
        const auto t1 = std::chrono::steady_clock::now();
        const Result<std::vector<sim::detail::GougeEvent>> g = sim::detail::findGouges(in, motions, 0.05, jobs(), ctx);
        REQUIRE(g);
        CHECK(g->empty());
        std::printf("T-072 bench: gouge test (any tier, 8 workers) on %zu motions: %.3f s\n", motions.size(),
                    seconds(t1));
        // Against a part of many triangles that the ball's scallops run on: a block to z 5 whose top is a grid of
        // 0.25 mm squares (the lowest scallops touch it).
        if (full) {
            kernel::TriSoup part;
            const int nx = 610, ny = 406;
            const double dx = 152.4 / nx, dy = 101.6 / ny, top = 5.0;
            for (int j = 0; j <= ny; ++j) {
                for (int i = 0; i <= nx; ++i) {
                    part.positions.insert(part.positions.end(), {i * dx, j * dy, top});
                }
            }
            const auto at = [&](int i, int j) {
                return static_cast<std::uint32_t>(j * (nx + 1) + i);
            };
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    part.indices.insert(part.indices.end(), {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j),
                                                             at(i + 1, j + 1), at(i, j + 1)});
                }
            }
            const kernel::TriSoup base = boxSoup(box(0, 0, 0, 152.4, 101.6, top));
            // The block's bottom and sides (its top is the grid).
            const auto b0 = static_cast<std::uint32_t>(part.positions.size() / 3);
            part.positions.insert(part.positions.end(), base.positions.begin(), base.positions.end());
            for (std::size_t t = 0; t < base.indices.size(); t += 3) {
                const bool isTop = base.positions[3 * base.indices[t] + 2] == top &&
                                   base.positions[3 * base.indices[t + 1] + 2] == top &&
                                   base.positions[3 * base.indices[t + 2] + 2] == top;
                if (!isTop) {
                    part.indices.insert(part.indices.end(),
                                        {b0 + base.indices[t], b0 + base.indices[t + 1], b0 + base.indices[t + 2]});
                }
            }
            sim::detail::VerifyInput in2 = in;
            in2.part = &part;
            sim::detail::GougeStats gs;
            const auto t2 = std::chrono::steady_clock::now();
            const Result<std::vector<sim::detail::GougeEvent>> g2 =
                sim::detail::findGouges(in2, motions, 0.05, jobs(), ctx, &gs);
            REQUIRE(g2);
            CHECK(g2->empty());
            std::printf("T-072 bench: gouge test against a %zu-triangle part under the scallops: %.3f s\n",
                        part.indices.size() / 3, seconds(t2));
        }
        std::fflush(stdout);
    }
    runTier({"Low", 0.2, 4, 20.0, 250.0}, sp, moves);
    runTier({"Medium", 0.1, 4, 15.0, 150.0}, sp, moves);
    runTier({"High", 0.05, 8, 10.0, 100.0}, sp, moves);
}

// Not run by default ([.]): what each keyframe costs against what actually changed in the stock between it and the
// next, at the simulator's keyframe spacing (kKeyframeRays) and batches, keyframes never thinned; and the time of a
// worst-case seek into each interval (restore the keyframe, recut the interval). The copy-on-write unit's diagnosis.
TEST_CASE("Simulator keyframe cost diagnosis", "[.][sim][simulator][cowdiag]")
{
    SimFixture f = benchFixture(11, 0.25);
    const SimProgram& sp = f.build();
    const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
    const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
    struct Cfg {
        double cell;
        int workers;
    };
    for (const Cfg cfg : {Cfg{0.2, 4}, Cfg{0.1, 4}, Cfg{0.05, 8}}) {
        jobs().setWorkerCount(cfg.workers);
        const Result<bool> done =
            jobs()
                .submit<bool>(
                    JobInfo{"diag", JobPriority::Normal, false},
                    [&](JobContext&) -> Result<bool> {
                        std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
                        m->reset(sp.stockSolid, benchmarkStock(), StockResolution{cfg.cell});
                        const sim::detail::TriDexelInfo ti = sim::detail::triDexelInfo(*m);
                        const Vec3 c{ti.cx, ti.cy, ti.cz};
                        std::vector<double> radius(sp.tools.size()), height(sp.tools.size());
                        for (std::size_t k = 0; k < sp.tools.size(); ++k) {
                            for (const Vec2& v : sp.tools[k].cuttingProfile) {
                                radius[k] = std::max(radius[k], v.x);
                            }
                            height[k] = sp.tools[k].cuttingProfile.back().y;
                        }
                        std::vector<double> rays(motions.size());
                        for (std::size_t i = 0; i < motions.size(); ++i) {
                            rays[i] = sim::detail::rayCost(motions[i], radius[motions[i].op], height[motions[i].op],
                                                           benchmarkStock(), c);
                        }
                        // As the simulator's batches (simulator.cpp batchEnd).
                        const auto batchEnd = [&](std::size_t from, std::size_t limit) {
                            double r = 0;
                            std::size_t j = from;
                            while (j < limit && motions[j].op == motions[from].op &&
                                   j - from < sim::detail::kMaxBatchMotions) {
                                if (j > from && r + rays[j] > sim::detail::kBatchRays) {
                                    break;
                                }
                                r += rays[j];
                                ++j;
                            }
                            return j;
                        };
                        std::vector<IStockModel::CutMove> moves;
                        const auto cutRange = [&](std::size_t from, std::size_t to) {
                            while (from < to) {
                                const std::size_t j = batchEnd(from, to);
                                moves.clear();
                                for (std::size_t k = from; k < j; ++k) {
                                    moves.push_back({motions[k].a, motions[k].b, motions[k].rapid});
                                }
                                m->cutBatch(sp.tools[motions[from].op], moves, {});
                                from = j;
                            }
                        };
                        struct Interval {
                            std::size_t from, to;
                            double rays, runMs, seekMs, restoreMs;
                            sim::detail::SnapshotCost cost;
                        };
                        std::vector<Interval> ivs;
                        std::shared_ptr<const IStockModel::Snapshot> prev = m->snapshot();
                        std::size_t prevMotion = 0, i = 0;
                        double since = 0, runMs = 0;
                        while (i < motions.size()) {
                            const std::size_t j = batchEnd(i, motions.size());
                            for (std::size_t k = i; k < j; ++k) {
                                since += rays[k];
                            }
                            const auto t0 = std::chrono::steady_clock::now();
                            cutRange(i, j);
                            runMs += seconds(t0) * 1e3;
                            i = j;
                            if (since >= sim::detail::kKeyframeRays || i == motions.size()) {
                                Interval iv{prevMotion, i, since, runMs, 0, 0, sim::detail::snapshotCost(*m, *prev)};
                                std::shared_ptr<const IStockModel::Snapshot> cur = m->snapshot();
                                const auto t1 = std::chrono::steady_clock::now();
                                m->restore(*prev);
                                iv.restoreMs = seconds(t1) * 1e3;
                                cutRange(prevMotion, i);
                                iv.seekMs = seconds(t1) * 1e3;
                                m->restore(*cur);
                                ivs.push_back(iv);
                                prev = cur;
                                prevMotion = i;
                                since = 0;
                                runMs = 0;
                            }
                        }
                        double sumPriv = 0, sumChanged = 0, grid[3] = {0, 0, 0}, gridChanged[3] = {0, 0, 0};
                        std::vector<double> priv, seek;
                        for (const Interval& iv : ivs) {
                            double p = 0;
                            for (int g = 0; g < 3; ++g) {
                                p += static_cast<double>(iv.cost.privateBytes[g]);
                                grid[g] += static_cast<double>(iv.cost.privateBytes[g]);
                                gridChanged[g] += static_cast<double>(iv.cost.changedBytes[g]);
                                sumChanged += static_cast<double>(iv.cost.changedBytes[g]);
                            }
                            sumPriv += p;
                            priv.push_back(p / (1024.0 * 1024.0));
                            seek.push_back(iv.seekMs);
                        }
                        const double mb = 1024.0 * 1024.0;
                        std::printf(
                            "cowdiag cell %.2f, %d workers: %zu keyframes at %.0f M rays; pinned in all %.0f MB "
                            "(Z %.0f X %.0f Y %.0f), per keyframe median %.1f max %.1f MB; changed dexels "
                            "%.0f MB (Z %.0f X %.0f Y %.0f); seek (restore + recut an interval) median %.1f "
                            "p90 %.1f max %.1f ms\n",
                            cfg.cell, cfg.workers, ivs.size(), sim::detail::kKeyframeRays / 1e6, sumPriv / mb,
                            grid[0] / mb, grid[1] / mb, grid[2] / mb, percentile(priv, 0.5), percentile(priv, 1.0),
                            sumChanged / mb, gridChanged[0] / mb, gridChanged[1] / mb, gridChanged[2] / mb,
                            percentile(seek, 0.5), percentile(seek, 0.9), percentile(seek, 1.0));
                        // The slowest intervals: where they are and what their time is made of.
                        std::vector<std::size_t> order(ivs.size());
                        for (std::size_t k = 0; k < order.size(); ++k) {
                            order[k] = k;
                        }
                        std::sort(order.begin(), order.end(),
                                  [&](std::size_t a, std::size_t b) { return ivs[a].seekMs > ivs[b].seekMs; });
                        for (std::size_t k = 0; k < std::min<std::size_t>(6, order.size()); ++k) {
                            const Interval& iv = ivs[order[k]];
                            double p = 0;
                            for (int g = 0; g < 3; ++g) {
                                p += static_cast<double>(iv.cost.privateBytes[g]);
                            }
                            std::printf(
                                "cowdiag   slow interval motions %zu-%zu (op %u, z %.1f): %.0f M rays, run %.1f ms, "
                                "seek %.1f ms (restore %.2f), pinned %.1f MB\n",
                                iv.from, iv.to, motions[iv.from].op, motions[iv.from].a.z, iv.rays / 1e6, iv.runMs,
                                iv.seekMs, iv.restoreMs, p / mb);
                        }
                        std::fflush(stdout);
                        return true;
                    })
                .wait();
        REQUIRE(done);
    }
    jobs().setWorkerCount(8);
}

// Not run by default ([.]): the keyframe memory against the worst seek at the Medium and High cells, and a program a
// tenth the size, for the report.
TEST_CASE("Simulator keyframe budget sweep at the High cell", "[.][sim][simulator][sweep]")
{
    SimFixture f = benchFixture(11, 0.25);
    const SimProgram& sp = f.build();
    std::uint64_t moves = 0;
    for (const auto& tp : sp.toolpaths) {
        moves += tp->moves.size();
    }
    {
        SimFixture small = benchFixture(1, 2.0);
        const SimProgram& ssp = small.build();
        std::uint64_t smallMoves = 0;
        for (const auto& tp : ssp.toolpaths) {
            smallMoves += tp->moves.size();
        }
        runTier({"Low, 11k moves", 0.2, 4, 20.0, 250.0}, ssp, smallMoves, 0, false, false);
        runTier({"Medium, 11k moves", 0.1, 4, 15.0, 150.0}, ssp, smallMoves, 0, false, false);
        runTier({"High, 11k moves", 0.05, 8, 10.0, 100.0}, ssp, smallMoves, 0, false, false);
    }
    for (const std::uint64_t mb : {192u, 320u}) {
        runTier({"Medium sweep", 0.1, 4, 15.0, 150.0}, sp, moves, mb * sim::detail::kMiB, false, false);
    }
    for (const std::uint64_t mb : {512u, 1024u, 1536u}) {
        runTier({"High sweep", 0.05, 8, 10.0, 100.0}, sp, moves, mb * sim::detail::kMiB, false, false);
    }
}

// ---------- Stress programs: what the S6 workload does not cover ----------
// Programs made with the real operation types of src/cam where they exist (face, pocket2d, adaptive2d through
// cam::registerBuiltinOperationTypes, as the engine runs them), and ball-end raster finishing written here (there is no
// 3D finishing operation yet), on plates far larger than S6's stock:
//   facing   a 300 x 200 x 30 plate faced 6 mm down in 0.5 mm steps with a 50.8 mm face mill: twelve passes over the
//            whole plate, each rewriting every Z ray (at 0.05 mm one pass changes 24 M dexels, 460 MB);
//   finish   the same plate: a 260 x 160 pocket roughed 6 mm deep in 0.5 mm steps (pocket2d, 16 mm end mill), then a
//            wavy surface on its floor semi-finished and finished three times over (10 mm ball along X at +0.4 mm,
//            6 mm ball along Y at +0.15, along X at 0): many light passes over the same area;
//   long     a 300 x 200 x 40 block: faced, an adaptive2d pocket 6 mm deep, a pocket2d finish of it, and the finishing
//            rasters at finer steps: over 300k moves;
//   large    a 500 x 350 x 40 plate at High: faced twice, a 420 x 270 pocket in three steps and one surface raster
//            (a Z grid of 10000 x 7000 rays).
namespace {

const cam::OperationRegistry& opRegistry()
{
    static const cam::OperationRegistry reg = [] {
        cam::OperationRegistry r;
        cam::registerBuiltinOperationTypes(r);
        return r;
    }();
    return reg;
}

lib::MachineDef bigMachine()
{
    lib::MachineDef m;
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 1600, 25400, 16500, 5000}, lib::AxisDef{'Y', 0, 1000, 25400, 16500, 5000},
              lib::AxisDef{'Z', -600, 0, 25400, 16500, 5000}};
    m.maxRpm = 15000;
    return m;
}

lib::Tool cutter(lib::ToolType type, double diameter, double flute, double corner = 0)
{
    lib::Tool t;
    t.type = type;
    t.flutes = 4;
    t.diameter = diameter;
    t.shankDiameter = std::min(diameter, 25.0);
    t.fluteLength = flute;
    t.shoulderLength = flute;
    t.overallLength = flute + 50;
    t.cornerRadius = corner;
    return t;
}

cam::Operation camOp(const char* type, const std::string& params, double rpm, double feed)
{
    cam::Operation op;
    op.type = type;
    op.name = type;
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.cutting.feedPlunge = feed / 2;
    op.cutting.feedRamp = feed * 0.75;
    op.cutting.feedLeadIn = feed;
    op.cutting.feedLeadOut = feed;
    op.heights.clearance = 15;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 45;
    return op;
}

kernel::Shape kbox(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    REQUIRE(s);
    return *s;
}

// The plate x 0..sx, y 0..sy, z 0..sz with a pocket x0..x1, y0..y1 (corner radius r) down to floorZ.
kernel::Shape pocketPlate(double sx, double sy, double sz, double x0, double y0, double x1, double y1, double r,
                          double floorZ)
{
    const auto line = [](Vec2 a, Vec2 b) {
        return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Line, {a, b}, 0, false, 0};
    };
    const auto arc = [](Vec2 a, Vec2 b, Vec2 c, double rad) {
        return kernel::ProfileCurve{kernel::ProfileCurve::Kind::Arc, {a, b, c}, rad, true, 0};
    };
    kernel::ProfileLoop l;
    l.curves = {line({x0 + r, y0}, {x1 - r, y0}), arc({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, r),
                line({x1, y0 + r}, {x1, y1 - r}), arc({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, r),
                line({x1 - r, y1}, {x0 + r, y1}), arc({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, r),
                line({x0, y1 - r}, {x0, y0 + r}), arc({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, r)};
    const kernel::Profile p{kernel::Plane{Vec3{0, 0, floorZ}, Vec3{0, 0, 1}, Vec3{1, 0, 0}}, {l}};
    JobContext ctx;
    auto prism = kernel::extrude(p, sz + 1 - floorZ, 0, ctx);
    INFO((prism ? std::string() : prism.error().message));
    REQUIRE(prism);
    auto cutResult = kernel::boolean(kernel::BoolOp::Cut, kbox({0, 0, 0}, {sx, sy, sz}), prism->op.shape, ctx);
    REQUIRE(cutResult);
    return cutResult->shape;
}

// The up-facing planar face at height z (the pocket's floor).
kernel::SubId floorFace(const kernel::Shape& s, double z)
{
    std::vector<kernel::SubId> out;
    const std::uint32_t n = s.count(kernel::SubKind::Face);
    for (std::uint32_t i = 0; i < n; ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, f);
        const auto sample = kernel::faceSample(s, f);
        if (info && sample && info->type == kernel::SurfaceType::Plane && sample->normal.z > 0.999 &&
            std::abs(info->bounds.min.z - z) < 1e-6 && std::abs(info->bounds.max.z - z) < 1e-6) {
            out.push_back(f);
        }
    }
    REQUIRE(out.size() == 1);
    return out.front();
}

// One operation of a registered type, generated as the engine would.
std::shared_ptr<const cam::Toolpath> generated(const cam::Operation& op, const lib::Tool& tool,
                                               const kernel::Shape& part, const Box3& stock,
                                               std::vector<kernel::SubId> faces = {})
{
    cam::ResolvedGeometry g;
    g.wcsFromModel = Xform::identity();
    g.partShapes = {part};
    g.stockBounds = stock;
    for (const kernel::SubId& f : faces) {
        g.selected.push_back(f);
        g.selectedShapeIndex.push_back(0);
    }
    const lib::MachineDef machine = bigMachine();
    const cam::IOperationType* type = opRegistry().find(op.type);
    REQUIRE(type != nullptr);
    JobContext ctx;
    Result<cam::Toolpath> tp = type->generate(cam::GenerateInput{op, tool, nullptr, machine, g}, ctx);
    INFO(op.type << ": " << (tp ? std::string() : tp.error().message));
    REQUIRE(tp);
    return std::make_shared<const cam::Toolpath>(std::move(*tp));
}

// Ball-end raster finishing of z = zf(x, y) + offset over [x0, x1] x [y0, y1]: lines along X (or Y) `stepover` apart
// in moves of `seg` mm, zigzag, staying down between lines.
std::shared_ptr<const cam::Toolpath> raster(double x0, double y0, double x1, double y1, bool alongX, double stepover,
                                            double seg, double offset, double (*zf)(double, double))
{
    const double a0 = alongX ? x0 : y0, a1 = alongX ? x1 : y1; // along the lines
    const double c0 = alongX ? y0 : x0, c1 = alongX ? y1 : x1; // across them
    const auto pt = [&](double a, double c) {
        const double x = alongX ? a : c, y = alongX ? c : a;
        return Vec3{x, y, zf(x, y) + offset};
    };
    const Vec3 s = pt(a0, c0);
    Path p(s.x, s.y, 60);
    p.rapid(s.x, s.y, s.z + 2).feed(s.x, s.y, s.z);
    const int lines = static_cast<int>(std::floor((c1 - c0) / stepover)) + 1;
    const int segs = std::max(1, static_cast<int>(std::ceil((a1 - a0) / seg)));
    for (int l = 0; l < lines; ++l) {
        const double c = c0 + stepover * l;
        const bool fwd = l % 2 == 0;
        if (l > 0) {
            const Vec3 q = pt(fwd ? a0 : a1, c);
            p.feed(q.x, q.y, q.z);
        }
        for (int k = 1; k <= segs; ++k) {
            const double a = fwd ? a0 + (a1 - a0) * k / segs : a1 - (a1 - a0) * k / segs;
            const Vec3 q = pt(a, c);
            p.feed(q.x, q.y, q.z);
        }
    }
    const Vec3 e = pt(lines % 2 == 1 ? a1 : a0, c0 + stepover * (lines - 1));
    p.rapid(e.x, e.y, 60);
    return p.done();
}

double wavy(double x, double y)
{
    return 20.0 + 1.6 * dmath::sin(2 * kPi * x / 70.0) * dmath::cos(2 * kPi * y / 55.0) + 0.004 * (x - 150);
}

struct StressProgram {
    const char* name;
    SimFixture f;
    std::uint64_t moves = 0;
};

void finish(StressProgram& p, const Box3& stock)
{
    p.f.prog.stock = boxSoup(stock);
    p.f.prog.part = boxSoup(box(stock.min.x, stock.min.y, stock.min.z, stock.max.x, stock.max.y, 2.0));
    const SimProgram& sp = p.f.build();
    for (const auto& tp : sp.toolpaths) {
        p.moves += tp->moves.size();
    }
}

StressProgram facingProgram()
{
    StressProgram p{"facing", {}};
    const Box3 stock = box(0, 0, 0, 300, 200, 30);
    const lib::Tool mill = cutter(lib::ToolType::FaceMill, 50.8, 6);
    p.f.prog.op(
        generated(camOp("face", R"({"stepdown":0.5})", 1500, 900), mill, kbox({0, 0, 0}, {300, 200, 24}), stock), mill);
    finish(p, stock);
    return p;
}

StressProgram finishProgram(double scale = 1.0)
{
    StressProgram p{scale == 1.0 ? "finish" : "long", {}};
    const double top = scale == 1.0 ? 30.0 : 40.0;
    const Box3 stock = box(0, 0, 0, 300, 200, top);
    const lib::Tool rougher = cutter(lib::ToolType::FlatEndMill, 16, 30);
    const kernel::Shape part =
        pocketPlate(300, 200, top - (scale == 1.0 ? 0.0 : 1.0), 20, 20, 280, 180, 10, top - (scale == 1.0 ? 6.0 : 7.0));
    const double floorZ = top - (scale == 1.0 ? 6.0 : 7.0);
    if (scale != 1.0) {
        const lib::Tool mill = cutter(lib::ToolType::FaceMill, 50.8, 6);
        p.f.prog.op(generated(camOp("face", R"({"stepdown":0.5})", 1500, 900), mill, part, stock), mill);
        p.f.prog.op(generated(camOp("adaptive2d",
                                    R"({"stepdown":3,"stepover":12,"stockToLeaveFloor":0.3,)"
                                    R"("stockToLeaveWall":0.3})",
                                    6000, 1500),
                              rougher, part, stock, {floorFace(part, floorZ)}),
                    rougher);
        p.f.prog.op(generated(camOp("pocket2d", R"({"stepdown":3,"stepover":40,"finishWalls":true})", 6000, 900),
                              rougher, part, stock, {floorFace(part, floorZ)}),
                    rougher);
    } else {
        p.f.prog.op(generated(camOp("pocket2d", R"({"stepdown":0.5,"stepover":45})", 6000, 900), rougher, part, stock,
                              {floorFace(part, floorZ)}),
                    rougher);
    }
    const lib::Tool ball10 = cutter(lib::ToolType::BallEndMill, 10, 25);
    const lib::Tool ball6 = cutter(lib::ToolType::BallEndMill, 6, 20);
    // The surface lies 2 to 6 mm below the pocket's floor.
    const double s = scale == 1.0 ? 1.0 : 0.75, base = scale == 1.0 ? 0.0 : 9.0;
    p.f.prog.op(raster(40, 40, 260, 160, true, 1.0 * s, 1.0, base + 0.4, wavy), ball10);
    p.f.prog.op(raster(40, 40, 260, 160, false, 0.4 * s, 1.0, base + 0.15, wavy), ball6);
    p.f.prog.op(raster(40, 40, 260, 160, true, 0.3 * s, 1.0, base, wavy), ball6);
    finish(p, stock);
    return p;
}

StressProgram largeProgram()
{
    StressProgram p{"large", {}};
    const Box3 stock = box(0, 0, 0, 500, 350, 40);
    const kernel::Shape part = pocketPlate(500, 350, 39, 40, 40, 460, 310, 12, 33);
    const lib::Tool mill = cutter(lib::ToolType::FaceMill, 63, 6);
    p.f.prog.op(generated(camOp("face", R"({"stepdown":0.5})", 1200, 900), mill, part, stock), mill);
    const lib::Tool rougher = cutter(lib::ToolType::FlatEndMill, 20, 30);
    p.f.prog.op(generated(camOp("pocket2d", R"({"stepdown":2,"stepover":50})", 5000, 1200), rougher, part, stock,
                          {floorFace(part, 33)}),
                rougher);
    const lib::Tool ball6 = cutter(lib::ToolType::BallEndMill, 6, 20);
    p.f.prog.op(raster(150, 100, 350, 250, true, 0.5, 1.0, 12.0, wavy), ball6);
    finish(p, stock);
    return p;
}

struct StressResult {
    double runS = 0;
    std::size_t keyframes = 0;
    int thinnings = 0;
    double pinnedMb = 0, budgetMb = 0, spacingM = 0;
    double worstMs = 0, medianMs = 0, p90Ms = 0;
    std::uint64_t worstFrom = 0, worstTo = 0;
    std::uint64_t endHash = 0, midHash = 0;
};

// Runs `sp` at the tier's cell and seeks into EVERY keyframe interval at its worst (just before the next keyframe,
// coming from the end: a restore and the whole interval recut). Prints one line.
StressResult stressTier(const char* program, const Tier& t, const SimProgram& sp, std::uint64_t moves,
                        const sim::detail::SimTuning& tuning = {})
{
    jobs().setWorkerCount(t.workers);
    StressResult res;
    auto sim = makeSimulator(jobs(), makeTriDexelCpu(jobs()));
    sim::detail::setSimTuning(*sim, tuning);
    SimOptions o{};
    o.resolution = StockResolution{t.cell};
    o.safetyChecks = false;
    o.gouges = false;
    const Result<bool> done = jobs()
                                  .submit<bool>(JobInfo{"bench", JobPriority::Normal, false},
                                                [&](JobContext& ctx) -> Result<bool> {
                                                    const auto t0 = std::chrono::steady_clock::now();
                                                    const Status s = sim->run(sp, o, ctx);
                                                    res.runS = seconds(t0);
                                                    if (!s) {
                                                        return tl::unexpected(s.error());
                                                    }
                                                    return true;
                                                })
                                  .wait();
    REQUIRE(done);
    const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
    res.endHash = sim::detail::triDexelHash(sim->stock());
    const auto timedSeek = [&](TimelinePosition p) {
        double ms = 0;
        const Result<bool> r = jobs()
                                   .submit<bool>(JobInfo{"seek", JobPriority::Interactive, false},
                                                 [&](JobContext&) -> Result<bool> {
                                                     const auto t0 = std::chrono::steady_clock::now();
                                                     const Status s = sim->seek(p);
                                                     ms = seconds(t0) * 1e3;
                                                     return s ? Result<bool>(true) : tl::unexpected(s.error());
                                                 })
                                   .wait();
        REQUIRE(r);
        return ms;
    };
    const auto at = [&](std::uint64_t motion, double t01) {
        motion = std::min<std::uint64_t>(motion, info.motions - 1);
        return TimelinePosition{info.motionOp[motion], info.motionMove[motion], t01};
    };
    const std::vector<std::uint64_t>& keys = info.keyframeMotions;
    // The work measure of every motion (as simulator.cpp counts it), to see what the slowest intervals cost per ray.
    std::vector<double> rays;
    {
        const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
        const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
        const sim::detail::TriDexelInfo ti = sim::detail::triDexelInfo(sim->stock());
        Box3 b{ti.origin, {ti.origin.x + ti.cx * ti.nx, ti.origin.y + ti.cy * ti.ny, ti.origin.z + ti.cz * ti.nz}};
        for (const sim::detail::Motion& m : motions) {
            const ToolShape& shape = sp.tools[m.op];
            double radius = 0;
            for (const Vec2& v : shape.cuttingProfile) {
                radius = std::max(radius, v.x);
            }
            const double height = shape.cuttingProfile.empty() ? 0.0 : shape.cuttingProfile.back().y;
            rays.push_back(sim::detail::rayCost(m, radius, height, b, {ti.cx, ti.cy, ti.cz}));
        }
    }
    struct Interval {
        std::uint64_t from, to;
        double ms, rays, writes;
    };
    std::vector<Interval> ivs;
    std::vector<double> seeks;
    for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
        // Best of two: this machine's speed wanders under other load.
        timedSeek(sim->runEnd());
        const std::uint64_t w0 = sim::detail::triDexelWrites(sim->stock());
        double ms = timedSeek(at(keys[k + 1] - 1, 0.9));
        const double writes = static_cast<double>(sim::detail::triDexelWrites(sim->stock()) - w0);
        timedSeek(sim->runEnd());
        ms = std::min(ms, timedSeek(at(keys[k + 1] - 1, 0.9)));
        if (ms > res.worstMs) {
            res.worstMs = ms;
            res.worstFrom = keys[k];
            res.worstTo = keys[k + 1];
        }
        seeks.push_back(ms);
        double r = 0;
        for (std::uint64_t m = keys[k]; m < keys[k + 1] && m < rays.size(); ++m) {
            r += rays[m];
        }
        ivs.push_back({keys[k], keys[k + 1], ms, r, writes});
        std::printf("ivdata %s %s %u %.0f %.0f %.2f\n", program, t.name,
                    info.motionOp[std::min<std::uint64_t>(keys[k], info.motions - 1)], r, writes, ms);
    }
    std::sort(ivs.begin(), ivs.end(), [](const Interval& a, const Interval& b) { return a.ms > b.ms; });
    for (std::size_t k = 0; k < std::min<std::size_t>(4, ivs.size()); ++k) {
        const Interval& iv = ivs[k];
        std::printf(
            "stress %s %s:   slow interval motions %llu-%llu (op %u): %.1f M rays, %.1f M writes, seek %.1f ms, "
            "%.2f ns per ray\n",
            program, t.name, static_cast<unsigned long long>(iv.from), static_cast<unsigned long long>(iv.to),
            info.motionOp[std::min<std::uint64_t>(iv.from, info.motions - 1)], iv.rays / 1e6, iv.writes / 1e6, iv.ms,
            iv.ms * 1e6 / std::max(1.0, iv.rays));
    }
    std::fflush(stdout);
    timedSeek(at(info.motions / 2, 0.5));
    res.midHash = sim::detail::triDexelHash(sim->stock());
    res.keyframes = keys.size();
    res.thinnings = info.thinnings;
    res.pinnedMb = static_cast<double>(info.keyframeBytes) / (1024.0 * 1024.0);
    res.budgetMb = static_cast<double>(info.keyframeBudgetBytes) / (1024.0 * 1024.0);
    res.spacingM = info.keyframeRays / 1e6;
    res.medianMs = percentile(seeks, 0.5);
    res.p90Ms = percentile(seeks, 0.9);
    const sim::detail::TriDexelInfo ti = sim::detail::triDexelInfo(sim->stock());
    char line[1024];
    std::snprintf(line, sizeof line,
                  "stress %s %s: cell %.2f mm, %d workers, %llu moves, %llu motions, grid %d x %d x %d, model %.0f MB; "
                  "run %.2f s; keyframes %zu (spacing %.0f M rays, %d thinnings) pinning %.0f MB of %.0f; seek into "
                  "every interval: median %.1f p90 %.1f max %.1f ms (motions %llu-%llu, op %u); hash end %016llx "
                  "mid %016llx; process peak %.0f MB",
                  program, t.name, t.cell, t.workers, static_cast<unsigned long long>(moves),
                  static_cast<unsigned long long>(info.motions), ti.nx, ti.ny, ti.nz,
                  static_cast<double>(ti.bytes) / (1024.0 * 1024.0), res.runS, res.keyframes, res.spacingM,
                  res.thinnings, res.pinnedMb, res.budgetMb, res.medianMs, res.p90Ms, res.worstMs,
                  static_cast<unsigned long long>(res.worstFrom), static_cast<unsigned long long>(res.worstTo),
                  info.motionOp[std::min<std::uint64_t>(res.worstFrom, info.motions - 1)],
                  static_cast<unsigned long long>(res.endHash), static_cast<unsigned long long>(res.midHash),
                  processPeakMb());
    std::cout << line << std::endl;
    CHECK(info.keyframeBytes <= info.keyframeBudgetBytes);
    jobs().setWorkerCount(8);
    return res;
}

const Tier kTiers[] = {{"Low", 0.2, 4, 20.0, 250.0}, {"Medium", 0.1, 4, 15.0, 150.0}, {"High", 0.05, 8, 10.0, 100.0}};

} // namespace

// Part of the section 9 benchmark (ray_sim_simulator_bench): the facing-heavy plate, the stress program that cost the
// most keyframe memory per second of seek (every pass rewrites every Z ray), at every tier. Each tier must keep every
// keyframe (no thinning: the equal tiles a pass leaves are shared, tridexel.h KEYFRAMES) and seek into every interval
// within its budget. The seek times are the best of two tries per interval, so a moment of other load on this machine
// does not fail it; measured with it quiet they are half the budget or less (High 49 ms, simulator.h). In other builds
// only Low runs, unchecked.
TEST_CASE("Simulator benchmark: a facing-heavy plate at every tier", "[.][sim][simulator][bench][simulator-bench]")
{
    StressProgram p = facingProgram();
    for (const Tier& t : kTiers) {
        if (!optimisedBuild() && t.cell < 0.2) {
            continue;
        }
        const StressResult r = stressTier(p.name, t, p.f.sp, p.moves);
        CHECK(r.thinnings == 0);
        if (optimisedBuild()) {
            CHECK(r.worstMs <= t.seekBudgetMs);
        }
    }
}

// Part of the section 9 benchmark (ray_sim_simulator_bench): the finishing program (a pocket roughed in twelve levels,
// then three ball rasters over it: the work S6 does not have, a batch's moves within a few strips) at High on 8
// workers, its run within High's 10 s per 100k moves. Measured with the machine quiet (hidden test "[stress-finish]"):
// 5.3 to 5.4 s per 100k, 6.6 with other load starting (before strips were split into parts, the lateral and Z skips
// and the parallel look through the intern table: 11.7 to 12.1). Its seeks are not checked: it is thinned at High and
// its worst seek is over 100 ms (simulator.h, KEYFRAMES). Not run in other builds.
TEST_CASE("Simulator benchmark: the finishing program at High", "[.][sim][simulator][bench][simulator-bench]")
{
    if (!optimisedBuild()) {
        SKIP("timed only in an optimised build");
    }
    StressProgram p = finishProgram();
    runTier(kTiers[2], p.f.sp, p.moves, 0, true, false);
}

// Not run by default ([.]): each stress program at every tier (the large one at High only), measured over every
// keyframe interval. What the keyframe policy was checked against beyond S6.
TEST_CASE("Simulator stress: facing", "[.][sim][simulator][stress][stress-facing]")
{
    StressProgram p = facingProgram();
    for (const Tier& t : kTiers) {
        stressTier(p.name, t, p.f.sp, p.moves);
    }
}
// Interning tiles at keyframes (tridexel.h, KEYFRAMES) and reusing dropped tiles change no result: the same stock,
// removed volumes and seeks with it on and off, on one worker and on eight, with a keyframe every few motions (so
// thousands of tiles are interned, shared across strips and copied again by the next cuts). Not a benchmark: it
// lives here beside the stress programs it was written for, and runs in the suite.
TEST_CASE("Interning keyframe tiles changes no result", "[sim][simulator][determinism]")
{
    SimFixture f = benchFixture(1, 2.0);
    const SimProgram& sp = f.build();
    struct Outcome {
        std::uint64_t end = 0;
        std::vector<double> removed;
        std::vector<std::uint64_t> seeks;
        std::uint64_t pinned = 0;
    };
    const auto outcome = [&](bool intern, int workers) {
        jobs().setWorkerCount(workers);
        auto sim = makeSimulator(jobs(), makeTriDexelCpu(jobs()));
        sim::detail::SimTuning tu;
        tu.internTiles = intern;
        tu.keyframeMotions = 97;
        sim::detail::setSimTuning(*sim, tu);
        SimOptions o{};
        o.resolution = StockResolution{0.2};
        o.safetyChecks = false;
        o.gouges = false;
        JobContext ctx;
        REQUIRE(sim->run(sp, o, ctx));
        Outcome out;
        out.end = sim::detail::triDexelHash(sim->stock());
        const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
        out.removed = info.removedMm3;
        out.pinned = info.keyframeBytes;
        // Backward through the program, then forward, then back to the end.
        std::vector<TimelinePosition> at;
        for (std::uint64_t m = info.motions; m > 0; m -= std::min<std::uint64_t>(m, 733)) {
            at.push_back({info.motionOp[m - 1], info.motionMove[m - 1], 0.4});
        }
        for (std::uint64_t m = 0; m < info.motions; m += 1201) {
            at.push_back({info.motionOp[m], info.motionMove[m], 0.7});
        }
        at.push_back(sim->runEnd());
        for (const TimelinePosition& p : at) {
            REQUIRE(sim->seek(p));
            out.seeks.push_back(sim::detail::triDexelHash(sim->stock()));
        }
        jobs().setWorkerCount(8);
        return out;
    };
    const Outcome plain = outcome(false, 8);
    CHECK(plain.seeks.back() == plain.end);
    for (const int workers : {1, 8}) {
        INFO("workers " << workers);
        const Outcome o = outcome(true, workers);
        CHECK(o.end == plain.end);
        CHECK(o.removed == plain.removed);
        CHECK(o.seeks == plain.seeks);
        // The facing pass and the pocket's floors leave equal tiles: interned, the keyframes pin less.
        CHECK(o.pinned < plain.pinned);
    }
}

// Not run by default ([.]): what the work measure leaves out, on the stress programs: per chunk of up to 256 motions of
// one operation, cut on ONE worker and timed by the process's CPU time (less disturbed by other load than the wall
// clock), the rays (rayCost) and the writes (triDexelWrites) against the time. Lines "caldata cell op rays writes ns"
// for a fit.
TEST_CASE("Simulator work measure calibration on the stress programs", "[.][sim][simulator][calibrate-stress]")
{
    // The process's CPU cycles (all threads), not wall time: other load does not count.
    const auto cpuNs = [] {
#if defined(_WIN32)
        ULONG64 cycles = 0;
        QueryProcessCycleTime(GetCurrentProcess(), &cycles);
        return static_cast<double>(cycles);
#else
        return 0.0;
#endif
    };
    StressProgram programs[] = {facingProgram(), finishProgram()};
    jobs().setWorkerCount(1);
    for (StressProgram& p : programs) {
        const SimProgram& sp = p.f.sp;
        const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
        const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
        Box3 stock{{sp.stockSolid.positions[0], sp.stockSolid.positions[1], sp.stockSolid.positions[2]},
                   {sp.stockSolid.positions[0], sp.stockSolid.positions[1], sp.stockSolid.positions[2]}};
        for (std::size_t i = 0; i + 2 < sp.stockSolid.positions.size(); i += 3) {
            stock.min = {std::min(stock.min.x, sp.stockSolid.positions[i]),
                         std::min(stock.min.y, sp.stockSolid.positions[i + 1]),
                         std::min(stock.min.z, sp.stockSolid.positions[i + 2])};
            stock.max = {std::max(stock.max.x, sp.stockSolid.positions[i]),
                         std::max(stock.max.y, sp.stockSolid.positions[i + 1]),
                         std::max(stock.max.z, sp.stockSolid.positions[i + 2])};
        }
        for (const double cell : {0.2, 0.1, 0.05}) {
            const Result<bool> done =
                jobs()
                    .submit<bool>(
                        JobInfo{"cal", JobPriority::Normal, false},
                        [&](JobContext&) -> Result<bool> {
                            std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
                            m->reset(sp.stockSolid, stock, StockResolution{cell});
                            const sim::detail::TriDexelInfo ti = sim::detail::triDexelInfo(*m);
                            const Vec3 c{ti.cx, ti.cy, ti.cz};
                            std::shared_ptr<const IStockModel::Snapshot> keep = m->snapshot();
                            double since = 0;
                            std::size_t i = 0;
                            while (i < motions.size()) {
                                const std::uint32_t op = motions[i].op;
                                const ToolShape& shape = sp.tools[op];
                                double radius = 0;
                                for (const Vec2& v : shape.cuttingProfile) {
                                    radius = std::max(radius, v.x);
                                }
                                const double height = shape.cuttingProfile.back().y;
                                std::vector<IStockModel::CutMove> moves;
                                double rays = 0;
                                while (i < motions.size() && motions[i].op == op && moves.size() < 256) {
                                    moves.push_back({motions[i].a, motions[i].b, motions[i].rapid});
                                    rays += sim::detail::rayCost(motions[i], radius, height, stock, c);
                                    ++i;
                                }
                                // Shared with a snapshot as after a keyframe, about as often: copies are paid.
                                since += rays;
                                if (since > sim::detail::kKeyframeRays) {
                                    keep = m->snapshot();
                                    since = 0;
                                }
                                const std::uint64_t w0 = sim::detail::triDexelWrites(*m);
                                const double t0 = cpuNs();
                                m->cutBatch(shape, moves, {});
                                const double ns = cpuNs() - t0;
                                std::printf("caldata %s %.2f %u %.0f %llu %.0f\n", p.name, cell, op, rays,
                                            static_cast<unsigned long long>(sim::detail::triDexelWrites(*m) - w0), ns);
                            }
                            std::fflush(stdout);
                            return true;
                        })
                    .wait();
            REQUIRE(done);
        }
    }
    jobs().setWorkerCount(8);
}

// Not run by default ([.]): what interning costs and saves at High, on the facing plate and on S6, with the same
// keyframes either way (no budget, so none is thinned), alternately twice.
TEST_CASE("Simulator stress: interning off and on", "[.][sim][simulator][stress-ab]")
{
    StressProgram p = facingProgram();
    SimFixture s6 = benchFixture(11, 0.25);
    const SimProgram& s6p = s6.build();
    for (int rep = 0; rep < 2; ++rep) {
        for (const bool intern : {false, true}) {
            sim::detail::SimTuning tu;
            tu.internTiles = intern;
            tu.keyframeBudgetBytes = 64 * sim::detail::kKeyframeBudgetHighBytes;
            stressTier(intern ? "facing interned" : "facing plain", kTiers[2], p.f.sp, p.moves, tu);
            stressTier(intern ? "s6 interned" : "s6 plain", kTiers[2], s6p, 112126, tu);
        }
    }
}
TEST_CASE("Simulator stress: finishing", "[.][sim][simulator][stress][stress-finish]")
{
    StressProgram p = finishProgram();
    for (const Tier& t : kTiers) {
        stressTier(p.name, t, p.f.sp, p.moves);
    }
}
TEST_CASE("Simulator stress: long program", "[.][sim][simulator][stress][stress-long]")
{
    StressProgram p = finishProgram(2.0);
    for (const Tier& t : kTiers) {
        stressTier(p.name, t, p.f.sp, p.moves);
    }
}
TEST_CASE("Simulator stress: the S6 workload over every interval", "[.][sim][simulator][stress][stress-s6]")
{
    SimFixture f = benchFixture(11, 0.25);
    const SimProgram& sp = f.build();
    std::uint64_t moves = 0;
    for (const auto& tp : sp.toolpaths) {
        moves += tp->moves.size();
    }
    for (const Tier& t : kTiers) {
        stressTier("s6", t, sp, moves);
    }
}
// Not run by default ([.]): the keyframe spacing (kKeyframeRays) on S6 and the facing program at High, for the report.
TEST_CASE("Simulator stress: keyframe spacing sweep at High", "[.][sim][simulator][stress-sweep]")
{
    SimFixture s6 = benchFixture(11, 0.25);
    const SimProgram& s6p = s6.build();
    StressProgram facing = facingProgram();
    for (const double spacing : {16.0e6, 24.0e6, 32.0e6}) {
        sim::detail::SimTuning tu;
        tu.keyframeRays = spacing;
        char name[64];
        std::snprintf(name, sizeof name, "%.0fM", spacing / 1e6);
        stressTier((std::string("s6 ") + name).c_str(), kTiers[2], s6p, 112126, tu);
        stressTier((std::string("facing ") + name).c_str(), kTiers[2], facing.f.sp, facing.moves, tu);
    }
}
// Not run by default ([.]): what the programs thinned at High would pin with no budget (64 GB), and how they would
// seek: the memory the policy would need to meet the seek budget. Needs that much memory free.
TEST_CASE("Simulator stress: High without a keyframe budget", "[.][sim][simulator][stress-unbounded]")
{
    sim::detail::SimTuning tu;
    tu.keyframeBudgetBytes = std::uint64_t{64} << 30;
    {
        StressProgram p = finishProgram();
        stressTier("finish unbounded", kTiers[2], p.f.sp, p.moves, tu);
    }
    {
        StressProgram p = largeProgram();
        stressTier("large unbounded", kTiers[2], p.f.sp, p.moves, tu);
    }
    {
        StressProgram p = finishProgram(2.0);
        stressTier("long unbounded", kTiers[2], p.f.sp, p.moves, tu);
    }
}
// Not run by default ([.]): where the removal's time goes, per operation (detail::CutProbe): the batches, how their
// tasks (one per strip) spread over the workers, the kernels' time per ray reached in each grid, tiles copied on write,
// and the time outside the batches (keyframes, of which interning). Lines "profile ..." for the report.
namespace {

struct ProfileProbe final : sim::detail::CutProbe {
    std::vector<sim::detail::CutBatchRecord> batches;
    std::uint64_t now() override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    void batch(sim::detail::CutBatchRecord&& r) override { batches.push_back(std::move(r)); }
    std::vector<sim::detail::InternRecord> interns;
    void intern(const sim::detail::InternRecord& r) override { interns.push_back(r); }
};

void profileRun(const char* program, const Tier& t, const SimProgram& sp, std::uint64_t moves, int partTiles = 0)
{
    jobs().setWorkerCount(t.workers);
    std::unique_ptr<IStockModel> model = makeTriDexelCpu(jobs());
    ProfileProbe probe;
    sim::detail::setCutProbe(*model, &probe);
    sim::detail::setTriDexelPartTiles(*model, partTiles);
    auto sim = makeSimulator(jobs(), std::move(model));
    SimOptions o{};
    o.resolution = StockResolution{t.cell};
    o.safetyChecks = false;
    o.gouges = false;
    double runS = 0;
    const Result<bool> done = jobs()
                                  .submit<bool>(JobInfo{"profile", JobPriority::Normal, false},
                                                [&](JobContext& ctx) -> Result<bool> {
                                                    const auto t0 = std::chrono::steady_clock::now();
                                                    const Status s = sim->run(sp, o, ctx);
                                                    runS = seconds(t0);
                                                    return s ? Result<bool>(true) : tl::unexpected(s.error());
                                                })
                                  .wait();
    REQUIRE(done);
    const sim::detail::SimRunInfo info = sim::detail::simRunInfo(*sim);
    const std::uint64_t hash = sim::detail::triDexelHash(sim->stock());
    // Per operation (batches are in program order and cover the motions in turn).
    struct Acc {
        std::uint64_t motions = 0, batches = 0, tasks = 0, copies = 0;
        double wall = 0, pre = 0, post = 0, par = 0, taskSum = 0, maxTaskSum = 0, lowerBound = 0;
        double gridNs[3] = {0, 0, 0}, gridReached[3] = {0, 0, 0}, gridWrites[3] = {0, 0, 0};
        double gridTasks[3] = {0, 0, 0}, gridMax[3] = {0, 0, 0};
        std::vector<double> topShare; // per batch: its largest task over the batch's tasks' sum
    };
    std::vector<Acc> acc(sp.ops.size());
    std::uint64_t cursor = 0;
    const double w = static_cast<double>(t.workers);
    for (const sim::detail::CutBatchRecord& b : probe.batches) {
        const std::uint32_t op = info.motionOp[std::min<std::uint64_t>(cursor, info.motions - 1)];
        cursor += b.moves;
        Acc& a = acc[op];
        a.motions += b.moves;
        ++a.batches;
        a.wall += static_cast<double>(b.end - b.begin);
        a.pre += static_cast<double>(b.parallelBegin - b.begin);
        a.post += static_cast<double>(b.end - b.parallelEnd);
        const double par = static_cast<double>(b.parallelEnd - b.parallelBegin);
        a.par += par;
        double sum = 0, mx = 0, gmax[3] = {0, 0, 0};
        for (const sim::detail::CutTaskRecord& k : b.tasks) {
            const double ns = static_cast<double>(k.end - k.start);
            sum += ns;
            mx = std::max(mx, ns);
            a.gridNs[k.grid] += ns;
            a.gridReached[k.grid] += static_cast<double>(k.reached);
            a.gridWrites[k.grid] += static_cast<double>(k.writes);
            a.gridTasks[k.grid] += 1;
            gmax[k.grid] = std::max(gmax[k.grid], ns);
            a.copies += k.copies;
        }
        for (int g = 0; g < 3; ++g) {
            a.gridMax[g] += gmax[g];
        }
        a.tasks += b.tasks.size();
        a.taskSum += sum;
        a.maxTaskSum += mx;
        a.lowerBound += std::max(mx, sum / w);
        a.topShare.push_back(sum > 0 ? mx / sum : 0);
    }
    std::printf("profile %s %s: %d workers, parts of %d tiles, %llu moves, run %.2f s (%.2f s per 100k moves): batches "
                "%.2f s, keyframes "
                "%.2f s (interning %.2f), other %.2f s; %llu batches, %zu keyframes, %d thinnings; hash %016llx\n",
                program, t.name, t.workers, partTiles > 0 ? partTiles : sim::detail::kPartTiles,
                static_cast<unsigned long long>(moves), runS, runS * 1e5 / static_cast<double>(moves), info.cutSeconds,
                info.keyframeSeconds, info.internSeconds, runS - info.cutSeconds - info.keyframeSeconds,
                static_cast<unsigned long long>(info.batches), info.keyframeMotions.size(), info.thinnings,
                static_cast<unsigned long long>(hash));
    {
        double st[7] = {0, 0, 0, 0, 0, 0, 0}, buckets = 0, freed = 0, items = 0, replaced = 0;
        for (const sim::detail::InternRecord& r : probe.interns) {
            const std::uint64_t at[8] = {r.begin,  r.purgedScan, r.purged,   r.listed,
                                         r.hashed, r.looked,     r.compared, r.end};
            for (int k = 0; k < 7; ++k) {
                st[k] += static_cast<double>(at[k + 1] - at[k]) * 1e-9;
            }
            buckets += static_cast<double>(r.buckets);
            freed += static_cast<double>(r.freed);
            items += static_cast<double>(r.items);
            replaced += static_cast<double>(r.replaced);
        }
        const double n = std::max<double>(1.0, static_cast<double>(probe.interns.size()));
        std::printf("profile %s %s interning: %zu calls: finding the table's own tiles %.2f s, letting them go %.2f s, "
                    "listing %.2f s, hashing %.2f s, looking up %.2f s, comparing %.2f s, replacing %.2f s; per call "
                    "%.0f buckets, %.0f tiles let go, %.0f tiles changed, %.0f replaced\n",
                    program, t.name, probe.interns.size(), st[0], st[1], st[2], st[3], st[4], st[5], st[6], buckets / n,
                    freed / n, items / n, replaced / n);
    }
    for (std::size_t op = 0; op < acc.size(); ++op) {
        const Acc& a = acc[op];
        if (a.batches == 0) {
            continue;
        }
        const double s = 1e-9;
        std::printf(
            "profile %s %s op %zu: %llu motions in %llu batches (%.0f per batch, %.1f tasks per batch); wall "
            "%.2f s = before the tasks %.3f + tasks %.2f + after %.3f; tasks' sum %.2f s, efficiency %.0f %% "
            "(sum / workers / wall), bound max(largest task, sum / workers) %.2f s, largest tasks' sum %.2f s, "
            "largest task's share of a batch median %.2f p90 %.2f; copies %llu\n",
            program, t.name, op, static_cast<unsigned long long>(a.motions), static_cast<unsigned long long>(a.batches),
            static_cast<double>(a.motions) / a.batches, static_cast<double>(a.tasks) / a.batches, a.wall * s, a.pre * s,
            a.par * s, a.post * s, a.taskSum * s, 100.0 * a.taskSum / (w * a.par), a.lowerBound * s, a.maxTaskSum * s,
            percentile(a.topShare, 0.5), percentile(a.topShare, 0.9), static_cast<unsigned long long>(a.copies));
        static const char* const kGrid[3] = {"Z", "X", "Y"};
        for (int g = 0; g < 3; ++g) {
            if (a.gridTasks[g] == 0) {
                continue;
            }
            std::printf("profile %s %s op %zu grid %s: %.1f tasks per batch, tasks' time %.2f s (largest per batch "
                        "summed %.2f s), %.0f M rays reached, %.2f ns per ray reached (one thread's time), %.1f M "
                        "writes\n",
                        program, t.name, op, kGrid[g], a.gridTasks[g] / a.batches, a.gridNs[g] * s, a.gridMax[g] * s,
                        a.gridReached[g] / 1e6, a.gridNs[g] / std::max(1.0, a.gridReached[g]), a.gridWrites[g] / 1e6);
        }
    }
    std::fflush(stdout);
    jobs().setWorkerCount(8);
}

} // namespace

TEST_CASE("Simulator stress: profile of the removal", "[.][sim][simulator][stress-profile]")
{
    {
        StressProgram p = finishProgram();
        // Whole strips as tasks (as before parts), then parts of 2, 4 (the default) and 8 tiles.
        for (const int parts : {1 << 20, 2, 0, 8}) {
            profileRun(p.name, kTiers[2], p.f.sp, p.moves, parts);
        }
        profileRun(p.name, Tier{"High 4w", 0.05, 4, 15.0, 100.0}, p.f.sp, p.moves);
    }
    {
        StressProgram p = finishProgram(2.0);
        profileRun(p.name, kTiers[2], p.f.sp, p.moves);
    }
    {
        SimFixture s6 = benchFixture(11, 0.25);
        const SimProgram& sp = s6.build();
        profileRun("s6", kTiers[2], sp, 112126);
    }
}

TEST_CASE("Simulator stress: large stock at High", "[.][sim][simulator][stress][stress-large]")
{
    StressProgram p = largeProgram();
    stressTier(p.name, kTiers[2], p.f.sp, p.moves);
}
