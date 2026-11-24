// What the safety checks cost on spike S6's benchmark workload (task T-071): 112k moves on the 6 x 4 x 1 in stock.
// Hidden (not a pass criterion: PLAN.md section 9 has no row for it); run with  ray_sim_tests "[verify-bench]".
#include "verify_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// The workload's passes as two operations' toolpaths (feeds; a rapid up and across between disjoint moves).
std::shared_ptr<const cam::Toolpath> asToolpath(const Workload::Pass& pass)
{
    const IStockModel::CutMove& first = pass.moves.front();
    Path p(first.from.x, first.from.y, 40);
    p.feed(first.from.x, first.from.y, first.from.z);
    Vec3 cur = first.from;
    for (const IStockModel::CutMove& m : pass.moves) {
        if (m.from.x != cur.x || m.from.y != cur.y || m.from.z != cur.z) {
            p.rapid(cur.x, cur.y, 40).rapid(m.from.x, m.from.y, 40).feed(m.from.x, m.from.y, m.from.z);
        }
        p.feed(m.to.x, m.to.y, m.to.z);
        cur = m.to;
    }
    p.rapid(cur.x, cur.y, 40);
    return p.done();
}

void run(double fluteFlat, double stickOut, double holderDiameter, const char* label)
{
    const Workload w = benchmarkWorkload();
    Program p;
    p.stock = boxSoup(benchmarkStock());
    lib::Tool flat = endMill(12.7, fluteFlat, fluteFlat + 50);
    lib::Tool ball = endMill(6.35, 25, 75, lib::ToolType::BallEndMill);
    p.op(asToolpath(w.passes[0]), flat, holder(holderDiameter, 50), stickOut);
    p.op(asToolpath(w.passes[1]), ball, holder(30, 40), 35);
    for (const bool cutStock : {false, true}) {
        sim::detail::VerifyStats stats;
        sim::detail::VerifyOptions o;
        o.cutStock = cutStock;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<SimEvent> ev = verify(p, &stats, o);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s, %s: %.2f s, %llu motions, %llu deferred, cut stock %s (%.2f mm), %zu events\n", label,
                    cutStock ? "both layers" : "analytic only", s, static_cast<unsigned long long>(stats.motions),
                    static_cast<unsigned long long>(stats.deferred), stats.cutStockRan ? "ran" : "not run",
                    stats.cutStockCellMm, ev.size());
        for (std::size_t i = 0; i < ev.size() && i < 3; ++i) {
            std::printf("  %s move %u: %s\n", kindName(ev[i].kind).c_str(), ev[i].moveIndex, ev[i].message.c_str());
        }
    }
}

} // namespace

TEST_CASE("the checks on the S6 benchmark workload", "[.][verify-bench]")
{
    jobs().setWorkerCount(8);
    // 30 mm of flute out of the holder: shank and holder above the stock throughout; nothing to defer.
    run(30, 40, 40, "flute 30, stick-out 40");
    // Stick-out 12 in a 30 mm holder: the holder runs inside the pocket (it fits): every pocket move deferred, none
    // a hit.
    run(30, 12, 30, "stick-out 12, holder 30");
    // 8 mm of flute: the pocket's lower levels (to 18 deep) run the shank into the walls the levels above left.
    run(8, 18, 40, "flute 8");
}
