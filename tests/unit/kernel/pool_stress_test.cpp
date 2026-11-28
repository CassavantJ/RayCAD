// Many kernel calls at once on shared shapes, for minutes (task T-080 round 5; the slow tier, ctest
// ray_kernel_pool_stress). Guards two things:
//
//  - OCCT's default thread pool is never resized while other calls can read it. The kernel used to re-initialise it to
//    the mesh team on every mesh call whose team differed (OSD_ThreadPool::Init frees and reallocates its thread
//    array), assuming only mesh calls use it. In OCCT 8.0.1 these read its size from any thread, single-threaded:
//      BRepCheck_Analyzer::Perform       (validity checks: validateSolids, isValid, checkedValid at import, healing)
//      BRepExtrema_DistShapeShape         (DistanceVertVert, DistanceMapMap, SolidTreatment: minDistance, the
//                                          direct edits' point tests, and inside OCCT IntTools_EdgeEdge (every
//                                          boolean), BOPAlgo_ArgumentAnalyzer, ShapeAnalysis_Edge (healing),
//                                          BRepOffset_Tool, BRepFill)
//    and these launch on it: BRepMesh (tessellate, triangulateExact, STL export; under the mesh lock),
//    GeomLib_CheckCurveOnSurface when asked to run in parallel (never in the kernel). ThreadSanitizer, with OCCT itself
//    instrumented, caught BRepCheck_Analyzer::Perform reading the pool while a mesh call resized it, and the naming
//    blocks crashed once in 19 runs inside minDistance while another thread meshed. Here a monitor samples the pool's
//    size the whole time and the mesh calls switch between their own team and a team of one (testing::MeshTeamCap, so
//    the teams differ on any machine with 2 or more processors), which used to resize it on every switch: the size must
//    never change. On one processor the workload still runs, with a warning that nothing could resize the pool.
//  - Concurrent calls on the SAME Shape give the results they give alone (kernel.h: a Shape is only read): one shape is
//    meshed, measured (minDistance between its own faces and edges), checked and cut from at the same time.
//
// RAY_POOL_STRESS_S sets the duration (default 120 s); RAY_POOL_STRESS_THREADS=<n> runs it as on n processors. Under
// ThreadSanitizer with an instrumented OCCT it also shows any race in those paths.
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <mesh_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

using Clock = std::chrono::steady_clock;

std::chrono::seconds stressDuration()
{
    const char* text = std::getenv("RAY_POOL_STRESS_S");
    return std::chrono::seconds(text != nullptr ? std::max(1, std::atoi(text)) : 120);
}

// The first answer of each task, and every later answer compared with it.
class Answers {
public:
    void check(const std::string& key, const std::string& answer)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto [it, fresh] = first_.emplace(key, answer);
        ++calls_;
        if (!fresh && it->second != answer && mismatches_.size() < 20)
            mismatches_.push_back(key + ": " + answer + " (first " + it->second + ")");
    }
    void fail(const std::string& what)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (failures_.size() < 20)
            failures_.push_back(what);
    }
    long calls() const { return calls_; }
    std::vector<std::string> mismatches() const { return mismatches_; }
    std::vector<std::string> failures() const { return failures_; }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> first_;
    std::vector<std::string> mismatches_, failures_;
    long calls_ = 0;
};

std::string distanceText(const Distance& d)
{
    return std::to_string(d.value);
}

} // namespace

TEST_CASE("meshing, distances, checks, booleans and imports at once on shared shapes", "[stress][pool_stress]")
{
    // RAY_POOL_STRESS_THREADS=<n>: OCCT's pool as on a machine with n processors (only if nothing used it yet: the
    // test alone in its process, as ctest runs it).
    if (const char* threads = std::getenv("RAY_POOL_STRESS_THREADS"))
        testing::createMeshPool(std::max(1, std::atoi(threads)));
    const Shape shared = take(testing::makeFilletedBody()); // meshed, measured, checked and cut from at once
    const Shape plate = take(testing::makeHolePlate(8, 8));
    const Shape sphere = take(testing::makeSphere({200, 0, 0}, 15));
    const Shape tool = take(makeBox({30, 15, -5}, {50, 35, 30}));
    const std::string stepFile = std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/manifold_block_2.stp";
    // Mesh calls alternate between their own team and a team of one (testing::MeshTeamCap): the old kernel
    // re-initialised the pool on every switch between two team sizes. Capping, not the tolerances, makes the teams
    // differ: on 4 processors every tolerance from 0.2 to 0.01 mm gets the whole pool.
    const int pool = testing::meshPoolThreads();
    REQUIRE(pool >= 1);
    std::printf("OCCT's default pool: %d threads; mesh teams %d (0.2 mm), %d (0.01 mm) and 1 (capped)\n", pool,
                testing::meshConcurrency(0.2), testing::meshConcurrency(0.01));
    const bool teamsDiffer = pool >= 2;
    if (!teamsDiffer)
        WARN("one processor: every mesh team is the whole pool, so the concurrent workload runs but nothing here could "
             "have resized the pool");
    {
        // The team of one holds back every other pool thread, and the pool keeps its size.
        const testing::MeshTeamCap one(1);
        JobContext ctx;
        REQUIRE(tessellate(plate, MeshOptions{0.2, 0.35}, ctx).has_value());
        CHECK(testing::lastMeshTeam() == 1);
        CHECK(testing::lastMeshHeldThreads() == pool - 1);
        CHECK(testing::meshPoolThreads() == pool);
    }
    std::atomic<bool> stop{false};
    std::atomic<int> poolMin{pool}, poolMax{pool};
    Answers answers;
    const auto worker = [&](const char* name, std::function<void(int)> step) {
        return std::thread([&, name, step] {
            for (int i = 0; !stop.load(); ++i) {
                try {
                    step(i);
                } catch (const std::exception& e) {
                    answers.fail(std::string(name) + ": " + e.what());
                }
            }
        });
    };
    const auto faceId = [](std::uint32_t i) {
        return SubId{SubKind::Face, i};
    };
    const auto edgeId = [](std::uint32_t i) {
        return SubId{SubKind::Edge, i};
    };

    std::vector<std::thread> threads;
    // The pool's size, sampled the whole time.
    threads.emplace_back([&] {
        while (!stop.load()) {
            const int n = testing::meshPoolThreads();
            if (n < poolMin.load())
                poolMin.store(n);
            if (n > poolMax.load())
                poolMax.store(n);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    // Meshing the shared body and the plate at two tolerances, every other call with a team of one.
    for (int t = 0; t < 2; ++t)
        threads.push_back(worker("tessellate", [&, t](int i) {
            const double tol = (i + t) % 2 == 0 ? 0.2 : 0.01;
            const testing::MeshTeamCap cap((i / 2 + t) % 2 == 0 ? 0 : 1);
            JobContext ctx;
            const Shape& s = t == 0 ? shared : plate;
            const Result<Mesh> m = tessellate(s, MeshOptions{tol, 0.35}, ctx);
            if (!m)
                answers.fail("tessellate: " + m.error().message);
            else
                answers.check("tessellate " + std::to_string(t) + " " + std::to_string(tol),
                              std::to_string(m->indices.size()));
        }));
    threads.push_back(worker("triangulateExact", [&](int i) {
        const testing::MeshTeamCap cap(i % 3 == 0 ? 1 : 0);
        JobContext ctx;
        const double tol = i % 2 == 0 ? 0.05 : 0.01;
        const Result<TriSoup> m = triangulateExact(shared, tol, ctx);
        if (!m)
            answers.fail("triangulateExact: " + m.error().message);
        else
            answers.check("exact " + std::to_string(tol), std::to_string(m->indices.size()));
    }));
    // Distances on the shape being meshed (faces and edges of the same body) and between bodies.
    for (int t = 0; t < 3; ++t)
        threads.push_back(worker("minDistance", [&, t](int i) {
            const std::uint32_t faces = shared.count(SubKind::Face), edges = shared.count(SubKind::Edge);
            const std::uint32_t a = static_cast<std::uint32_t>(i * 7 + t) % faces;
            const std::uint32_t b = static_cast<std::uint32_t>(i * 13 + 3 * t + 1) % faces;
            const Result<Distance> ff = minDistance(shared, faceId(a), shared, faceId(b));
            const std::uint32_t e = static_cast<std::uint32_t>(i * 5 + t) % edges;
            const std::uint32_t g = static_cast<std::uint32_t>(i * 11 + t + 2) % edges;
            const Result<Distance> ee = minDistance(shared, edgeId(e), shared, edgeId(g));
            const Result<Distance> fs = minDistance(shared, faceId(a), sphere, faceId(0));
            if (!ff || !ee || !fs) {
                answers.fail("minDistance failed");
                return;
            }
            answers.check("ff " + std::to_string(a) + " " + std::to_string(b), distanceText(*ff));
            answers.check("ee " + std::to_string(e) + " " + std::to_string(g), distanceText(*ee));
            answers.check("fs " + std::to_string(a), distanceText(*fs));
        }));
    // Validity checks (BRepCheck_Analyzer) of the shared body and of a boolean result made from it.
    threads.push_back(worker("check", [&](int) {
        const Result<bool> valid = testing::isValidShape(shared);
        if (!valid || !*valid)
            answers.fail("isValidShape");
        answers.check("valid", valid && *valid ? "1" : "0");
    }));
    threads.push_back(worker("boolean", [&](int) {
        JobContext ctx;
        const Result<OpResult> cut = boolean(BoolOp::Cut, shared, tool, ctx);
        const Result<MassProps> props = cut ? massProps(cut->shape) : Result<MassProps>(tl::unexpected(cut.error()));
        if (!props)
            answers.fail("boolean: " + props.error().message);
        else
            answers.check("cut", std::to_string(cut->shape.count(SubKind::Face)) + " faces, volume " +
                                     std::to_string(std::llround(props->volume * 1000.0)));
    }));
    // STEP imports (healing and the import's own check).
    threads.push_back(worker("import", [&](int) {
        JobContext ctx;
        const Result<ImportResult> r = importStep(stepFile, {}, ctx);
        if (!r || r->bodies.empty())
            answers.fail("importStep: " + (r ? std::string("no body") : r.error().message));
        else
            answers.check("import", std::to_string(r->bodies.size()) + " bodies, " +
                                        std::to_string(r->bodies.front().shape.count(SubKind::Face)) + " faces");
    }));

    const auto duration = stressDuration();
    const Clock::time_point start = Clock::now();
    while (Clock::now() - start < duration && answers.failures().empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    for (std::thread& t : threads)
        t.join();

    INFO(answers.calls() << " calls in " << std::chrono::duration<double>(Clock::now() - start).count() << " s");
    for (const std::string& f : answers.failures())
        UNSCOPED_INFO("failure: " << f);
    for (const std::string& m : answers.mismatches())
        UNSCOPED_INFO("different answer: " << m);
    CHECK(answers.failures().empty());
    CHECK(answers.mismatches().empty());
    CHECK(answers.calls() > 100);
    // The pool kept its size throughout.
    CHECK(poolMin.load() == pool);
    CHECK(poolMax.load() == pool);
    CHECK(testing::structuredExceptions() == 0);
}
