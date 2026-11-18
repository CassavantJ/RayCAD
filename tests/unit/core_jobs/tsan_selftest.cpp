// ThreadSanitizer self-test for the job system. Hidden (not run by default): it races on purpose. Under the
// linux-clang-tsan preset CTest runs it as ray_core_jobs_tsan_selftest and passes only if TSan reports the
// race, which shows that a race in a parallelChunks body is still seen with oneTBB on the stack (oneTBB is
// built with TSan there, so no suppression of TBB frames is needed).
#include <catch2/catch_test_macros.hpp>

#include "test_dispatcher.h"

#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <cstddef>

using ray::test::TestDispatcher;

TEST_CASE("TSan reports a data race in a parallelChunks body", "[.tsan-selftest]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    ray::JobContext ctx;
    long unguarded = 0; // written by every chunk without synchronisation: the race
    std::atomic<long> guarded{0};
    const ray::Status status = jobs->parallelChunks(256, 1, ctx, [&](std::size_t, std::size_t, std::size_t) {
        ++unguarded;
        ++guarded;
        // Keep each chunk busy for a moment so that the chunks spread over several workers.
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
        while (std::chrono::steady_clock::now() < until) {
        }
        return ray::Status{};
    });
    CHECK(status.has_value());
    CHECK(guarded.load() == 256);
}
