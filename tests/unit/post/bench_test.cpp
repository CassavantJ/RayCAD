// Post 1,000,000 moves (PLAN.md section 9: 2 s High, 3 s Medium, 5 s Low; spike S8 measured 0.68 s for a toy post).
// Tagged [wallclock]: tests/tiers.cmake runs it alone. The budget is checked in optimised builds only.
#include "engine.h"
#include "post_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

using namespace ray;
using namespace ray::post;
using namespace ray::post::test;

namespace {

// A fixed-seed toolpath in the shape of spike S8's: cuts at feed with axis-only and two-axis moves, quarter arcs and
// full circles, plunges and rapids between pockets; a quarter of the coordinates on a rounding tie.
Program benchProgram(std::size_t count)
{
    Program p;
    cam::Toolpath& path = p.section("bench", "pocket2d", 4, makeTool("8 mm end mill", lib::ToolType::FlatEndMill, 8, 3));
    auto& moves = path.moves;
    moves.reserve(count);
    std::uint64_t state = 0x5EED5EED5EED5EEDull;
    auto next = [&] {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    auto range = [&](std::int64_t lo, std::int64_t hi) { return lo + static_cast<std::int64_t>(next() % (hi - lo + 1)); };
    auto snap = [&](Nm v) {
        switch (next() & 3) {
        case 0:
            return (v / 1000) * 1000 + (v < 0 ? -500 : 500);
        case 1:
            return (v / 1000) * 1000;
        default:
            return v;
        }
    };
    Nm x = 0, y = 0, z = mm(25);
    while (moves.size() < count) {
        x = snap(range(0, mm(500)));
        y = snap(range(0, mm(500)));
        moves.push_back(rapid(x, y, mm(25)));
        moves.push_back(rapid(x, y, mm(2)));
        z = -snap(range(mm(1), mm(10)));
        moves.push_back(linear(x, y, z, mmPerMin(300), cam::MoveRole::Plunge));
        for (int i = 0; i < 96 && moves.size() < count; ++i) {
            const std::int32_t feed = next() % 10 == 0 ? mmPerMin(800) : mmPerMin(1200);
            if (i % 8 == 7) {
                const Nm rx = snap(range(mm(1) / 2, mm(5))) * ((next() & 1) ? 1 : -1);
                const Nm ry = snap(range(mm(1) / 2, mm(5))) * ((next() & 1) ? 1 : -1);
                const Nm cx = x + rx, cy = y + ry;
                const bool cw = (next() & 1) != 0;
                if (i % 32 != 31) { // a quarter turn, exact in integers; else a full circle
                    if (cw) {
                        x = cx - ry;
                        y = cy + rx;
                    } else {
                        x = cx + ry;
                        y = cy - rx;
                    }
                }
                moves.push_back(arc(cw, x, y, z, cx, cy, 0, feed));
            } else {
                const auto r = next() % 16;
                if (r >= 1 && r < 6) {
                    x = snap(x + range(-mm(5), mm(5)));
                } else if (r >= 6 && r < 11) {
                    y = snap(y + range(-mm(5), mm(5)));
                } else if (r >= 11) {
                    x = snap(x + range(-mm(5), mm(5)));
                    y = snap(y + range(-mm(5), mm(5)));
                }
                moves.push_back(linear(x, y, z, feed)); // r == 0: a zero-length move
            }
        }
        if (moves.size() < count) {
            moves.push_back(rapid(x, y, mm(25)));
        }
    }
    moves.resize(count);
    return p;
}

} // namespace

TEST_CASE("post 1,000,000 moves", "[post][bench][wallclock]")
{
    constexpr std::size_t kMoves = 1'000'000;
    const Program program = benchProgram(kMoves);
    const PostJob job = program.job();
    QuickJsPostEngine engine;
    JobContext ctx;
    double best = 1e9;
    Hash256 hash{};
    for (int run = 0; run < 3; ++run) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = engine.run(postPath("iso-test.post.js"), job, ctx);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(out.has_value());
        if (run == 0) {
            hash = out->hash();
            std::fprintf(stderr, "post bench: %zu moves -> %zu bytes, %zu lines mapped, JS heap peak %.2f MB\n", kMoves,
                         out->text.size(), out->lineMap.size(),
                         static_cast<double>(engine.lastRun().heapPeakBytes) / (1024.0 * 1024.0));
        }
        CHECK(out->hash() == hash);
        CHECK(engine.lastRun().heapAfterFreeBytes == 0);
        std::fprintf(stderr, "post bench run %d: %.3f s wall, %.3f s CPU, %llu interrupt polls\n", run + 1, seconds,
                     engine.lastRun().cpuSeconds, static_cast<unsigned long long>(engine.lastRun().interruptPolls));
        best = std::min(best, seconds);
    }
#if defined(NDEBUG) && !defined(RAY_SANITIZED_BUILD)
    // The Low-tier budget; the High-tier one (2 s) is checked on the reference machine.
    CHECK(best < 5.0);
#endif
    std::fprintf(stderr, "post bench: best of 3: %.3f s for %zu moves\n", best, kMoves);
}
