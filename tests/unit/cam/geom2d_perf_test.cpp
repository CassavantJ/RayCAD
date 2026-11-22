// Timing of the 2D core on a part with many loops (task T-050 acceptance: "performance stated: slicing and offsetting
// a 500-loop part; no quadratic blow-up in loop count") and the latency of a cancel. [wallclock]: these run alone.
// The times are printed (WARN) for the report; the checks are a generous ceiling and the growth from 125 to 500 loops,
// which is 4 for linear work and 16 for quadratic.
#include "geom2d_parts.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace ray::cam::geom2d::test {
namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

struct Timing {
    double slice = 0, regions = 0, offsetTime = 0, silhouetteTime = 0;
    std::size_t holes = 0;
};

Timing measure(int count)
{
    const kernel::Shape plate = plateWithHoles(count);
    JobContext ctx;
    Timing t;
    // Best of three, so a busy moment on a shared machine does not decide.
    for (int round = 0; round < 3; ++round) {
        auto t0 = Clock::now();
        const auto loops = take(kernel::sliceAt(plate, Xform::identity(), 5.0, ctx));
        auto t1 = Clock::now();
        const Regions rs = take(regionsFromLoops(loops, ctx));
        auto t2 = Clock::now();
        const Regions shrunk = take(offset(rs, -1'500'000, ctx));
        auto t3 = Clock::now();
        const auto shadow = take(kernel::silhouette(plate, Xform::identity(), ctx));
        auto t4 = Clock::now();
        REQUIRE(loops.size() == static_cast<std::size_t>(count + 1));
        REQUIRE(rs.size() == 1);
        REQUIRE(rs[0].holes.size() == static_cast<std::size_t>(count));
        REQUIRE(shrunk.size() == 1);
        REQUIRE(shrunk[0].holes.size() == static_cast<std::size_t>(count));
        REQUIRE(shadow.size() == static_cast<std::size_t>(count + 1));
        const auto best = [&](double& slot, double v) {
            slot = round == 0 ? v : std::min(slot, v);
        };
        best(t.slice, seconds(t0, t1));
        best(t.regions, seconds(t1, t2));
        best(t.offsetTime, seconds(t2, t3));
        best(t.silhouetteTime, seconds(t3, t4));
        t.holes = rs[0].holes.size();
    }
    return t;
}

} // namespace

TEST_CASE("timing: slicing and offsetting a 500-loop part, and growth from 125 loops", "[geom2d][wallclock]")
{
    const Timing small = measure(124);
    const Timing large = measure(499);
    WARN("125 loops: slice " << small.slice << " s, regions " << small.regions << " s, offset " << small.offsetTime
                             << " s, silhouette " << small.silhouetteTime << " s");
    WARN("500 loops: slice " << large.slice << " s, regions " << large.regions << " s, offset " << large.offsetTime
                             << " s, silhouette " << large.silhouetteTime << " s");
    CHECK(large.slice < 10.0);
    CHECK(large.regions < 2.0);
    CHECK(large.offsetTime < 5.0);
    // Growth: 4x the loops must cost well under 16x (quadratic); 10x leaves room for noise on a shared machine.
    const auto growth = [](double a, double b) {
        return b / std::max(a, 1e-4);
    };
    CHECK(growth(small.regions, large.regions) < 10.0);
    CHECK(growth(small.offsetTime, large.offsetTime) < 10.0);
    CHECK(growth(small.slice, large.slice) < 10.0);
}

TEST_CASE("timing: a cancel stops a long offset promptly", "[geom2d][wallclock]")
{
    // 20,000 circles in rows: one offset that takes long enough to cancel in the middle.
    Regions many;
    for (int i = 0; i < 20'000; ++i)
        many.push_back(region(Path::circle({(i % 200) * 3 * kMm, (i / 200) * 3 * kMm}, kMm, true)));
    JobContext plain;
    const auto t0 = Clock::now();
    const Regions full = take(offset(many, 200'000, plain));
    const double whole = seconds(t0, Clock::now());
    REQUIRE(full.size() == 20'000);
    CancelSource source;
    JobContext ctx{source.token(), {}};
    std::atomic<bool> done{false};
    Clock::time_point cancelledAt, returnedAt;
    Result<Regions> result = Regions{};
    std::thread worker([&] {
        result = offset(many, 200'000, ctx);
        returnedAt = Clock::now();
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::duration<double>(whole * 0.3));
    cancelledAt = Clock::now();
    source.cancel();
    worker.join();
    WARN("offset of 20,000 circles: " << whole << " s whole; returned " << seconds(cancelledAt, returnedAt)
                                      << " s after the cancel");
    if (returnedAt > cancelledAt) {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::Cancelled);
        // Clipper2's own union is one call and cannot be interrupted; the rest polls every 64 loops.
        CHECK(seconds(cancelledAt, returnedAt) < whole * 0.8 + 0.05);
    }
}

} // namespace ray::cam::geom2d::test
