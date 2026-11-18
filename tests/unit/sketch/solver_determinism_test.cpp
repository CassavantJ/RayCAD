// Determinism (task T-090, PLAN.md section 8): results are bit-identical over 100 runs, across threads and job-system
// worker counts, between a P-core and an E-core (Windows, by a probe process pinned to each), and the cancellable
// copy of PlaneGCS's iteration gives the same bits as PlaneGCS's own System::solve.
#include "drag_runner.h"
#include "sketch_det_hash.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;

namespace {

class QueueDispatcher final : public IUiDispatcher {
public:
    void post(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(fn));
    }

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
};

bool sameBits(const Sketch& a, const Sketch& b)
{
    const auto ca = coordinates(a), cb = coordinates(b);
    return ca.size() == cb.size() && std::memcmp(ca.data(), cb.data(), ca.size() * sizeof(double)) == 0;
}

} // namespace

TEST_CASE("100 runs give the same bits", "[sketch][determinism]")
{
    const Grid g = makeGrid(1, GridVariant::Nearly, 0.3);
    const std::uint64_t first = detHash(g, 5);
    // 100 in an optimised build; 10 in a debug or sanitizer build, where one run takes seconds.
    const int runs = optimisedBuild() ? 100 : 10;
    for (int run = 1; run < runs; ++run) {
        INFO("run " << run);
        REQUIRE(detHash(g, 5) == first);
    }
}

TEST_CASE("The cancellable iteration gives the same bits as PlaneGCS's System::solve", "[sketch][determinism]")
{
    const std::vector<std::pair<int, GridVariant>> cases = {{1, GridVariant::Nearly}, {1, GridVariant::Loose},
                                                            {2, GridVariant::Nearly}, {2, GridVariant::Redundant},
                                                            {2, GridVariant::Fully},  {1, GridVariant::Decoupled}};
    for (const auto& [rows, variant] : cases) {
        INFO("rows " << rows << ", variant " << static_cast<int>(variant));
        const Grid g = makeGrid(rows, variant, 0.3);
        JobContext ctx;
        const SolveResult ours = take(sketch::detail::solveSketch(g.sketch, ctx));
        sketch::detail::SolveOptions pg;
        pg.planeGcsSolve = true;
        const SolveResult theirs = take(sketch::detail::solveSketch(g.sketch, ctx, pg));
        REQUIRE(ours.status == theirs.status);
        CHECK(ours.status != SolveStatus::Failed);
        CHECK(sameBits(ours.solved, theirs.solved));
        CHECK(ours.redundant == theirs.redundant);
    }
}

TEST_CASE("Threads and job-system worker counts do not change the bits", "[sketch][determinism]")
{
    // 200 curves: the size at which spike S7 saw Eigen's cache blocking change the bits between core types (50 in a
    // debug or sanitizer build, where the dense reference drag in the fingerprint is too slow at 200).
    const Grid g = makeGrid(optimisedBuild() ? 4 : 1, GridVariant::Nearly, 0.3);
    const std::uint64_t base = detHash(g, 3);
    {
        std::vector<std::uint64_t> hashes(4, 0);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            threads.emplace_back([&, i] { hashes[i] = detHash(g, 3); });
        }
        for (auto& t : threads) {
            t.join();
        }
        for (std::uint64_t h : hashes) {
            CHECK(h == base);
        }
    }
    QueueDispatcher dispatcher;
    for (int workers : {1, 8}) {
        auto jobs = makeJobSystem(dispatcher, workers);
        std::vector<JobHandle<std::uint64_t>> handles;
        for (int i = 0; i < 3; ++i) {
            handles.push_back(
                jobs->submit<std::uint64_t>(JobInfo{"sketch determinism", JobPriority::Normal, false},
                                            [&g](JobContext&) -> Result<std::uint64_t> { return detHash(g, 3); }));
        }
        for (auto& h : handles) {
            INFO("workers " << workers);
            CHECK(take(h.wait()) == base);
        }
    }
}

#ifdef RAY_SKETCH_DET_PROBE
TEST_CASE("A P-core and an E-core give the same bits", "[sketch][determinism]")
{
    // Each probe is its own process pinned to one core class before any Eigen code runs, so Eigen's cache-size
    // detection runs on that core (spike S7: without pinned cache sizes the 200-curve result differed).
    auto run = [](const char* coreClass) {
        const std::string cmd = std::string("\"") + RAY_SKETCH_DET_PROBE + "\" " + coreClass;
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        REQUIRE(pipe);
        std::string out;
        char buf[256];
        while (std::fgets(buf, sizeof buf, pipe)) {
            out += buf;
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return out;
    };
    const std::string p = run("p");
    const std::string e = run("e");
    INFO("P-core probe: " << p);
    INFO("E-core probe: " << e);
    if (p.find("no-hybrid") != std::string::npos || e.find("no-hybrid") != std::string::npos) {
        WARN("This CPU has one core class (or pinning is not available here); P-core against E-core not tested");
        return;
    }
    REQUIRE(p.rfind("hash ", 0) == 0);
    REQUIRE(e.rfind("hash ", 0) == 0);
    CHECK(p.substr(0, 21) == e.substr(0, 21));
    CHECK(p != e); // the probes report which core they ran on, so the lines differ after the hash
}
#endif
