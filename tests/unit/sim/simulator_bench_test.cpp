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
#include "simulator_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>

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
