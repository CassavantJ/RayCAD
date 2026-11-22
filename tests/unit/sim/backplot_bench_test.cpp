// Backplot speed (task T-037, PLAN.md section 9: 1 M moves in under 150 ms on the Low reference CPU). Timed against the
// wall clock, so it runs alone (ray_sim_tests_wallclock, tests/tiers.cmake), and the budget is checked only in an
// optimised build without sanitizers.
#include "backplot_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

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

double uni(std::mt19937_64& rng)
{
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

constexpr double kPi = 3.14159265358979323846;

// A continuous toolpath shaped like a mix of adaptive clearing, contours and drilling: 55% short cuts, 10% rapids,
// 25% XY arcs (radius 0.5 to 20 mm, 5 to 180 degrees), 3% full helix turns, 2% XZ/YZ arcs, 3% drill and peck cycles,
// 2% dwells.
cam::Toolpath mixedToolpath(std::size_t count)
{
    std::mt19937_64 rng(42);
    std::vector<Move> moves;
    moves.reserve(count);
    double x = 0, y = 0, z = 5;
    for (std::size_t i = 0; i < count; ++i) {
        const double p = uni(rng);
        if (std::abs(x) > 150 || std::abs(y) > 150) {
            x = 0, y = 0, z = 5;
            moves.push_back(rapid(x, y, z));
        } else if (p < 0.55) {
            const double a = uni(rng) * 2 * kPi, len = 0.1 + 1.9 * uni(rng);
            x += len * std::cos(a), y += len * std::sin(a);
            moves.push_back(line(x, y, z, 1500));
        } else if (p < 0.65) {
            x = (uni(rng) - 0.5) * 200, y = (uni(rng) - 0.5) * 200;
            moves.push_back(rapid(x, y, z));
        } else if (p < 0.90) {
            const double r = 0.5 + 19.5 * uni(rng), a = uni(rng) * 2 * kPi;
            const double cx = x + r * std::cos(a), cy = y + r * std::sin(a);
            const bool ccw = (rng() & 1) != 0;
            const double sweep = (5.0 + 175.0 * uni(rng)) * kPi / 180.0;
            const double b = a + kPi + (ccw ? sweep : -sweep);
            x = cx + r * std::cos(b), y = cy + r * std::sin(b);
            moves.push_back(arc(ccw, ArcPlane::XY, x, y, z, cx, cy, z));
        } else if (p < 0.93) {
            const double r = 2.0 + 8.0 * uni(rng);
            z -= 0.5;
            moves.push_back(arc(true, ArcPlane::XY, x, y, z, x + r, y, z));
        } else if (p < 0.95) {
            const double r = 1.0 + 4.0 * uni(rng);
            if ((rng() & 1) != 0) { // XZ: from +X of the centre to +Z of it
                const double cx = x - r, cz = z;
                x = cx, z = cz + r;
                moves.push_back(arc(true, ArcPlane::XZ, x, y, z, cx, y, cz));
            } else { // YZ: from +Y of the centre to +Z of it
                const double cy = y - r, cz = z;
                y = cy, z = cz + r;
                moves.push_back(arc(true, ArcPlane::YZ, x, y, z, x, cy, cz));
            }
        } else if (p < 0.98) {
            const bool peck = (rng() & 1) != 0;
            x += 5.0, z = 2.0;
            moves.push_back(
                cycle(peck ? MoveKind::CyclePeck : MoveKind::CycleDrill, x, y, -12.0, 2.0, peck ? 3.0 : 0.0));
        } else {
            moves.push_back(blankMove(MoveKind::Dwell, MoveRole::Cut));
        }
    }
    return toolpath(0, 0, 5, std::move(moves));
}

} // namespace

TEST_CASE("1 M mixed moves build in under 150 ms", "[sim][backplot][bench][wallclock]")
{
    const std::size_t kMoves = 1'000'000;
    const cam::Toolpath tp = mixedToolpath(kMoves);
    REQUIRE(tp.moves.size() == kMoves);
    // A setup's WCS turned and shifted in the model, as a real one is.
    const Xform modelFromWcs{{0.0, -1.0, 0.0, 120.5, 1.0, 0.0, 0.0, -40.25, 0.0, 0.0, 1.0, 35.0}};
    // PLAN.md section 9 budgets the build per tier; each tier draws at its own display tolerance (TierPreset::
    // meshChordTolMm: Low 0.2, Medium 0.05, High 0.01). The acceptance bullet of T-037 is the Low one, checked here;
    // the others and 1 um are printed.
    struct Level {
        const char* tier;
        double tol, budgetMs;
        bool checked;
    };
    const Level levels[] = {{"Low", 0.2, 150.0, true},
                            {"Medium", 0.05, 100.0, false},
                            {"High", 0.01, 60.0, false},
                            {"none", 0.001, 0, false}};
    for (const Level& level : levels) {
        double best = 1e9;
        std::size_t vertices = 0, bytes = 0;
        for (int run = 0; run < 4; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            const Backplot bp = buildBackplot(tp, modelFromWcs, level.tol);
            const auto t1 = std::chrono::steady_clock::now();
            if (run > 0) { // the first run warms the allocator and the caches
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            vertices = bp.lineVertices.size();
            bytes = bp.lineVertices.capacity() * sizeof(BackplotVertex) +
                    bp.moveFirstVertex.capacity() * sizeof(std::uint32_t);
            if (run == 0) {
                checkLayout(tp, bp);
            }
        }
        std::cout << "[sim] backplot of " << kMoves << " mixed moves at " << level.tol << " mm (tier " << level.tier
                  << ", budget " << level.budgetMs << " ms on its reference machine): " << best << " ms (best of 3), "
                  << vertices << " vertices, " << (static_cast<double>(bytes) / 1e6) << " MB ("
                  << sizeof(BackplotVertex) << " bytes per vertex)\n";
        if (level.checked && optimisedBuild()) {
            CHECK(best < level.budgetMs);
        }
    }
}
