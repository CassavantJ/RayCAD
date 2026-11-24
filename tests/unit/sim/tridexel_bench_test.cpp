// Stock model speed, keyframes and memory on the S6 benchmark (task T-070; PLAN.md section 9 "Material-removal sim,
// 100k moves at tier resolution": Low 0.2 mm 20 s, Medium 0.1 mm 15 s, High 0.05 mm 15 s on 4 threads and 10 s on 8
// or more; "Timeline seek" needs keyframes that are not full copies: snapshot under 50 ms). Timed against the wall
// clock, so these run alone (ray_sim_tests_wallclock, tests/tiers.cmake); the time limits are checked only in an
// optimised build without sanitizers. The memory limits (6 x 4 x 1 in stock under 20 MB at 0.2 mm, under 300 MB at
// 0.05 mm) are the model's own accounting and are checked everywhere.
#include "tridexel_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>

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

struct TierRun {
    double cell;
    int workers;
    double budget100k; // s per 100k moves
    double memoryMb;   // limit of the model's own memory, 0: none
    bool extras;       // also measure meshes, the carried solid and a reset from a solid
};

// The benchmark stock as a closed soup of 12 triangles.
kernel::TriSoup boxSoup(const Box3& b)
{
    kernel::TriSoup s;
    for (int i = 0; i < 8; ++i) {
        s.positions.push_back((i & 1) != 0 ? b.max.x : b.min.x);
        s.positions.push_back((i & 2) != 0 ? b.max.y : b.min.y);
        s.positions.push_back((i & 4) != 0 ? b.max.z : b.min.z);
    }
    s.indices = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
    return s;
}

void runTier(const TierRun& t)
{
    const Workload w = benchmarkWorkload();
    const std::size_t moves = w.moves();
    REQUIRE(moves > 100000);
    jobs().setWorkerCount(t.workers);
    std::unique_ptr<IStockModel> m = makeTriDexelCpu(jobs());
    m->reset({}, benchmarkStock(), StockResolution{t.cell});
    const double fresh = static_cast<double>(sim::detail::triDexelInfo(*m).bytes) / (1024.0 * 1024.0);
    // Run as the simulator will: inside a job, so the job's own worker is one of the `workers` threads (a thread
    // outside the pool calling cutBatch would add one more).
    const std::size_t batch = 512;
    double run = 0;
    const Result<double> done = jobs()
                                    .submit<double>(JobInfo{"bench", JobPriority::Normal, false},
                                                    [&](JobContext&) -> Result<double> {
                                                        const auto t0 = std::chrono::steady_clock::now();
                                                        const double v = runWorkload(*m, w, batch);
                                                        run = seconds(t0);
                                                        return v;
                                                    })
                                    .wait();
    REQUIRE(done);
    const double removed = *done;
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(*m);
    const double mb = static_cast<double>(in.bytes) / (1024.0 * 1024.0);
    // Keyframes: take one, cut a further 1000 moves (which copy the strips they touch), restore, compare.
    const std::uint64_t h0 = sim::detail::triDexelHash(*m);
    const auto t1 = std::chrono::steady_clock::now();
    std::shared_ptr<const IStockModel::Snapshot> snap = m->snapshot();
    const double snapMs = seconds(t1) * 1e3;
    const Workload::Pass& pocket = w.passes[0];
    const auto t2 = std::chrono::steady_clock::now();
    std::vector<double> v(1000);
    m->cutBatch(benchmarkTool(0), std::span<const IStockModel::CutMove>(pocket.moves.data() + 200, 1000), v);
    const double afterMs = seconds(t2) * 1e3;
    const double privateMb = static_cast<double>(sim::detail::snapshotPrivateBytes(*m, *snap)) / (1024.0 * 1024.0);
    const auto t3 = std::chrono::steady_clock::now();
    m->restore(*snap);
    const double restoreMs = seconds(t3) * 1e3;
    const bool restored = sim::detail::triDexelHash(*m) == h0;
    const auto t4 = std::chrono::steady_clock::now();
    std::unique_ptr<IStockModel> copy = m->clone();
    const double cloneMs = seconds(t4) * 1e3;
    const double per100k = run * 100000.0 / static_cast<double>(moves);
    char line[1024];
    std::snprintf(
        line, sizeof line,
        "T-070 bench: cell %.2f mm, %d workers: grid %d x %d x %d, %zu moves in %.2f s (%.0f moves/s, %.2f s "
        "per 100k), removed %.0f mm3, hash %016llx; model %.1f MB fresh, %.1f MB cut; intervals Z %llu X %llu Y %llu, "
        "most in one dexel %u / %u / %u; snapshot %.3f ms, 1000 more moves %.1f ms holding %.1f MB in the "
        "snapshot, restore %.3f ms, clone %.3f ms; process peak %.0f MB",
        t.cell, t.workers, in.nx, in.ny, in.nz, moves, run, static_cast<double>(moves) / run, per100k, removed,
        static_cast<unsigned long long>(h0), fresh, mb, static_cast<unsigned long long>(in.intervals[0]),
        static_cast<unsigned long long>(in.intervals[1]), static_cast<unsigned long long>(in.intervals[2]),
        in.maxIntervals[0], in.maxIntervals[1], in.maxIntervals[2], snapMs, afterMs, privateMb, restoreMs, cloneMs,
        processPeakMb());
    std::cout << line << std::endl;
    if (t.extras) {
        // Display meshes: the whole stock at full resolution (counted), within a 2 M triangle budget, and the dirty
        // region of one more 20 mm move; the carried solid; a reset from a solid of the same box.
        const std::uint64_t fullTris = sim::detail::triDexelMeshTriangles(*m, 1);
        const auto t5 = std::chrono::steady_clock::now();
        const kernel::Mesh budget = m->toMesh(0, nullptr, 2'000'000);
        const double budgetMs = seconds(t5) * 1e3;
        const std::uint32_t before = m->version();
        m->cut(benchmarkTool(0), {120.0, 40.0, 20.0}, {140.0, 40.0, 20.0}, false);
        Box3 dirty;
        const auto t6 = std::chrono::steady_clock::now();
        const kernel::Mesh patch = m->toMesh(before, &dirty, 2'000'000);
        const double patchMs = seconds(t6) * 1e3;
        double solidMs = 0;
        std::size_t solidTris = 0;
        if (t.cell >= 0.1) {
            const auto t7 = std::chrono::steady_clock::now();
            const kernel::TriSoup solid = m->toSolid();
            solidMs = seconds(t7) * 1e3;
            solidTris = solid.indices.size() / 3;
        }
        auto fromSoup = makeTriDexelCpu(jobs());
        const auto t8 = std::chrono::steady_clock::now();
        fromSoup->reset(boxSoup(benchmarkStock()), benchmarkStock(), StockResolution{t.cell});
        const double soupMs = seconds(t8) * 1e3;
        std::snprintf(
            line, sizeof line,
            "T-070 bench: cell %.2f mm meshes: full resolution %llu triangles; budget 2 M: %zu triangles in "
            "%.1f ms; dirty region %.1f x %.1f mm after one 20 mm move: %zu triangles in %.1f ms; toSolid %zu "
            "triangles in %.1f ms (not run below 0.1 mm); reset from a 12-triangle box soup %.1f ms",
            t.cell, static_cast<unsigned long long>(fullTris), budget.indices.size() / 3, budgetMs,
            dirty.max.x - dirty.min.x, dirty.max.y - dirty.min.y, patch.indices.size() / 3, patchMs, solidTris, solidMs,
            soupMs);
        std::cout << line << std::endl;
        CHECK(budget.indices.size() / 3 <= 2'000'000);
        // The dirty region alone fits the budget at a finer column size than the whole stock does.
        CHECK(patch.indices.size() / 3 <= 2'000'000);
        CHECK(dirty.max.x - dirty.min.x < 152.4);
        CHECK(dirty.max.y - dirty.min.y < 101.6);
        CHECK(sim::detail::triDexelInfo(*fromSoup).volume[0] > 0);
    }
    CHECK(restored);
    if (t.memoryMb > 0) {
        CHECK(mb < t.memoryMb);
    }
    if (optimisedBuild()) {
        CHECK(per100k <= t.budget100k);
        CHECK(snapMs < 50.0);
        CHECK(restoreMs < 50.0);
    }
    jobs().setWorkerCount(8);
}

} // namespace

TEST_CASE("Benchmark at the Low tier resolution, 0.2 mm, 4 workers", "[sim][tridexel][bench][bench-low][wallclock]")
{
    runTier({0.2, 4, 20.0, 20.0, true});
}

TEST_CASE("Benchmark at the Medium tier resolution, 0.1 mm, 4 workers",
          "[sim][tridexel][bench][bench-medium][wallclock]")
{
    runTier({0.1, 4, 15.0, 0.0, true});
}

TEST_CASE("Benchmark at the High tier resolution, 0.05 mm, 4 and 8 workers",
          "[sim][tridexel][bench][bench-high][wallclock]")
{
    runTier({0.05, 4, 15.0, 300.0, true});
    runTier({0.05, 8, 10.0, 300.0, false});
}
