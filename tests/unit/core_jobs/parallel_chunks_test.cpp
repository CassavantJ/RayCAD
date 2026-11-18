// IJobSystem::parallelChunks (task T-003): fixed chunking, merge in chunk order, deterministic errors.
#include <catch2/catch_test_macros.hpp>

#include "test_dispatcher.h"

#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using ray::test::TestDispatcher;

namespace {

struct ChunkOut {
    std::size_t begin = 0;
    std::size_t end = 0;
    double sum = 0.0; // floating-point: order of addition matters, so only per-chunk sums are deterministic
    std::uint64_t mix = 0;
};

// Runs a floating-point reduction with fixed chunks and merges the slots in chunk order. Returns nothing if
// the call failed or the chunks did not tile [0, count) in order. No Catch2 assertion in here: it also runs
// on worker threads.
std::optional<std::vector<double>> reduce(ray::IJobSystem& jobs, ray::JobContext& ctx, std::size_t count,
                                          std::size_t grain)
{
    const std::size_t chunks = (count + grain - 1) / grain;
    std::vector<ChunkOut> slots(chunks);
    const ray::Status status = jobs.parallelChunks(
        count, grain, ctx, [&](std::size_t chunk, std::size_t begin, std::size_t end) -> ray::Status {
            ChunkOut& out = slots[chunk];
            out.begin = begin;
            out.end = end;
            for (std::size_t i = begin; i < end; ++i) {
                const double x = static_cast<double>(i) * 0.1 + 1.0 / static_cast<double>(i + 1);
                out.sum += x * x / 3.0;
                out.mix = out.mix * 1'000'003u + i;
            }
            return {};
        });
    if (!status) {
        return std::nullopt;
    }
    std::vector<double> merged;
    double total = 0.0;
    std::size_t expectedBegin = 0;
    for (const ChunkOut& out : slots) {
        if (out.begin != expectedBegin || out.end <= out.begin || out.end - out.begin > grain) {
            return std::nullopt;
        }
        expectedBegin = out.end;
        total += out.sum;
        merged.push_back(out.sum);
        merged.push_back(static_cast<double>(out.mix % 1'000'000'007u));
    }
    if (expectedBegin != count) {
        return std::nullopt;
    }
    merged.push_back(total);
    return merged;
}

bool bitIdentical(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

} // namespace

TEST_CASE("parallelChunks gives identical merged output with 1, 2 and 16 workers", "[jobs][chunks]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1);
    ray::JobContext ctx;
    for (const std::size_t grain : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{100'000}}) {
        INFO("grain " << grain);
        constexpr std::size_t kCount = 100'003;
        jobs->setWorkerCount(1);
        const std::optional<std::vector<double>> one = reduce(*jobs, ctx, kCount, grain);
        REQUIRE(one.has_value());
        for (const int workers : {2, 16, 1, 16}) {
            jobs->setWorkerCount(workers);
            INFO("workers " << workers);
            const std::optional<std::vector<double>> other = reduce(*jobs, ctx, kCount, grain);
            REQUIRE(other.has_value());
            CHECK(bitIdentical(*other, *one));
        }
    }
}

TEST_CASE("parallelChunks inside a job gives the same output as outside", "[jobs][chunks]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    ray::JobContext outside;
    const std::optional<std::vector<double>> expected = reduce(*jobs, outside, 50'000, 33);
    REQUIRE(expected.has_value());
    for (const ray::JobPriority priority :
         {ray::JobPriority::Interactive, ray::JobPriority::Normal, ray::JobPriority::Background}) {
        auto handle = jobs->submit<std::vector<double>>(
            {"chunks in a job", priority}, [&](ray::JobContext& ctx) -> ray::Result<std::vector<double>> {
                std::optional<std::vector<double>> merged = reduce(*jobs, ctx, 50'000, 33);
                if (!merged) {
                    return tl::unexpected<ray::Error>(ray::Error{ray::ErrorCode::Internal, "reduce failed", {}});
                }
                return std::move(*merged);
            });
        const auto result = handle.wait();
        REQUIRE(result.has_value());
        CHECK(bitIdentical(*result, *expected));
    }
}

TEST_CASE("parallelChunks returns the error of the lowest failing chunk", "[jobs][chunks]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 1);
    ray::JobContext ctx;
    for (const int workers : {1, 2, 16}) {
        jobs->setWorkerCount(workers);
        for (int round = 0; round < 20; ++round) {
            INFO("workers " << workers << ", round " << round);
            std::atomic<int> ran{0};
            const ray::Status status =
                jobs->parallelChunks(1000, 10, ctx, [&](std::size_t chunk, std::size_t, std::size_t) -> ray::Status {
                    ++ran;
                    if (chunk == 3 || chunk == 7 || chunk == 70) {
                        // Later chunks fail sooner, to tempt a first-come-first-served implementation.
                        if (chunk == 3) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        return tl::unexpected<ray::Error>(
                            ray::Error{ray::ErrorCode::ToolpathFailed, "chunk " + std::to_string(chunk), {}});
                    }
                    return {};
                });
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ray::ErrorCode::ToolpathFailed);
            CHECK(status.error().message == "chunk 3");
            CHECK(ran.load() >= 4); // chunks 0 to 3 always run
        }
    }
}

TEST_CASE("an exception in a chunk becomes that chunk's Internal error", "[jobs][chunks]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    ray::JobContext ctx;
    const ray::Status status =
        jobs->parallelChunks(100, 1, ctx, [](std::size_t chunk, std::size_t, std::size_t) -> ray::Status {
            if (chunk == 40) {
                throw std::runtime_error("chunk exploded");
            }
            if (chunk == 60) {
                return tl::unexpected<ray::Error>(ray::Error{ray::ErrorCode::InvalidArgument, "later", {}});
            }
            return {};
        });
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ray::ErrorCode::Internal);
    CHECK(status.error().message.find("chunk exploded") != std::string::npos);
}

TEST_CASE("parallelChunks stops on cancel and returns Cancelled", "[jobs][chunks][cancel]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);

    SECTION("cancelled before the call: no chunk runs")
    {
        ray::CancelSource source;
        source.cancel();
        ray::JobContext ctx{source.token(), {}};
        std::atomic<int> ran{0};
        const ray::Status status = jobs->parallelChunks(1000, 1, ctx, [&](std::size_t, std::size_t, std::size_t) {
            ++ran;
            return ray::Status{};
        });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ray::ErrorCode::Cancelled);
        CHECK(ran.load() == 0);
    }
    SECTION("cancelled from inside a chunk: the rest is skipped")
    {
        ray::CancelSource source;
        ray::JobContext ctx{source.token(), {}};
        std::atomic<int> ran{0};
        const ray::Status status =
            jobs->parallelChunks(100'000, 1, ctx, [&](std::size_t chunk, std::size_t, std::size_t) {
                ++ran;
                if (chunk == 10) {
                    source.cancel();
                }
                return ray::Status{};
            });
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ray::ErrorCode::Cancelled);
        CHECK(ran.load() < 100'000);
    }
}

TEST_CASE("parallelChunks argument edge cases", "[jobs][chunks]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    ray::JobContext ctx;
    int calls = 0;
    auto body = [&](std::size_t, std::size_t, std::size_t) {
        ++calls;
        return ray::Status{};
    };
    CHECK(jobs->parallelChunks(0, 10, ctx, body).has_value());
    CHECK(calls == 0);
    const ray::Status zeroGrain = jobs->parallelChunks(10, 0, ctx, body);
    REQUIRE_FALSE(zeroGrain.has_value());
    CHECK(zeroGrain.error().code == ray::ErrorCode::InvalidArgument);
    CHECK(calls == 0);
    CHECK(jobs->parallelChunks(5, 10, ctx, body).has_value()); // one short chunk
    CHECK(calls == 1);
}

TEST_CASE("parallelChunks reports completed chunks as progress", "[jobs][chunks][progress]")
{
    TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    std::atomic<int> reports{0};
    std::atomic<bool> sawFinal{false};
    ray::JobContext ctx{{}, ray::Progress([&](double fraction, std::string_view) {
                            ++reports;
                            if (fraction == 1.0) {
                                sawFinal = true;
                            }
                        })};
    CHECK(jobs->parallelChunks(10'000, 1, ctx, [](std::size_t, std::size_t, std::size_t) { return ray::Status{}; })
              .has_value());
    CHECK(sawFinal.load());
    CHECK(reports.load() >= 1);
}
