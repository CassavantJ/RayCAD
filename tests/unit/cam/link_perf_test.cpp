// Linking (task T-051), the cases timed against the wall clock ([wallclock]: tests/tiers.cmake runs them alone): the
// order of 2000 passes, and how soon a cancelled link of a large pocket returns.
#include "link_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace ray::cam::link::test {

TEST_CASE("link timing: 2000 strokes ordered and linked in under 10 s", "[link][wallclock]")
{
    Rng rng{42};
    std::vector<Pass> passes;
    for (int k = 0; k < 2000; ++k) {
        const Pt a{rng.between(0, 300 * kMm), rng.between(0, 200 * kMm)};
        Pass p =
            passFromPath(openPath(a, {line({a.x + rng.between(kMm, 20 * kMm), a.y + rng.between(-5 * kMm, 5 * kMm)})}),
                         -kMm, kCutFeed);
        p.reversible = k % 2 == 0;
        passes.push_back(p);
    }
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Plunge};
    o.centreCutting = true;
    JobContext ctx;
    const auto t0 = std::chrono::steady_clock::now();
    const Linked l = take(linkPasses(passes, o, Stock{}, ctx));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("link timing: 2000 strokes in %.3f s, %zu moves\n", s, l.moves.size());
    CHECK(l.order.size() == 2000);
    CHECK(s < 10.0);
}

TEST_CASE("link timing: a cancelled link returns within 250 ms", "[link][wallclock]")
{
    // A 120 x 80 mm pocket in a block: offset rings 2 mm apart at four depths, every link tested against the cut area.
    const Stock stock = boxStock(0, 0, 140 * kMm, 100 * kMm, 0);
    JobContext setup;
    const Regions room = take(geom2d::offset(regionOf(rect(10 * kMm, 10 * kMm, 130 * kMm, 90 * kMm)), -3 * kMm, setup));
    std::vector<Pass> passes;
    for (int level = 0; level < 4; ++level)
        for (int ring = 0; ring < 30; ++ring) {
            const Regions rs = take(geom2d::offset(room, -2 * kMm * ring, setup));
            if (rs.empty())
                break;
            Pass p = passFromPath(rs[0].outer, -kMm * (level + 1), kCutFeed);
            p.rotatable = true;
            p.cluster = 0;
            p.level = level;
            p.room = room;
            passes.push_back(p);
        }
    Options o = baseOptions();
    o.helixDiameter = 5 * kMm;
    o.rampAngleDeg = 2;
    o.maxStayDown = 6 * kMm;
    o.stepIn = true;
    {
        JobContext ctx;
        const auto t0 = std::chrono::steady_clock::now();
        const Linked l = take(linkPasses(passes, o, stock, ctx));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("link timing: the pocket's %zu rings linked in %.3f s (%zu moves)\n", passes.size(), s,
                    l.moves.size());
        CHECK(s < 60.0);
    }
    double worst = 0;
    for (const int delayMs : {5, 40, 150}) {
        CancelSource src;
        JobContext ctx{src.token(), {}};
        std::atomic<bool> done{false};
        std::chrono::steady_clock::time_point end;
        Result<Linked> result = tl::unexpected(Error{ErrorCode::Internal, "not run", {}});
        std::thread worker([&] {
            result = linkPasses(passes, o, stock, ctx);
            end = std::chrono::steady_clock::now();
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        const auto cancelled = std::chrono::steady_clock::now();
        src.cancel();
        worker.join();
        if (result.has_value()) {
            std::printf("link timing: finished before the cancel at %d ms\n", delayMs);
            continue;
        }
        CHECK(result.error().code == ErrorCode::Cancelled);
        const double ms = std::chrono::duration<double, std::milli>(end - cancelled).count();
        worst = std::max(worst, ms);
        std::printf("link timing: cancelled at %d ms, returned %.1f ms later\n", delayMs, ms);
    }
    CHECK(worst < 250.0);
}

} // namespace ray::cam::link::test
