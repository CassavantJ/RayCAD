// The safety checks do not depend on the tier preset or the thread count (task T-071). The tiers differ in the
// simulation cell (Low 0.2, Medium 0.1, High 0.05 mm, PLAN.md 6.2) and in threads; neither reaches the checks.
#include "verify_impl.h"
#include "verify_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstring>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// Two slots along X, 5 deep, leaving between them a wall 0.15 mm thick at Y45.12 to Y45.27; then a rapid across the
// wall at Z-4 (move 8). The Low tier's Z rays (every 0.2 mm: Y45.1 and Y45.3) both fall in the slots, so a model at
// that cell does not hold the wall; the checker's fixed 0.1 mm cell does (Y45.15 and Y45.25).
Program thinWall()
{
    Program p;
    p.stock = boxSoup(box(0, 0, -30, 100, 80, 0));
    p.op(Path(-20, 40.12, 5)
             .feed(-20, 40.12, -5)
             .feed(110, 40.12, -5)
             .rapid(110, 40.12, 5)
             .rapid(-20, 50.27, 5)
             .feed(-20, 50.27, -5)
             .feed(110, 50.27, -5)
             .rapid(110, 50.27, 5)
             .rapid(50, 50.27, -4)
             .rapid(50, 40.12, -4)
             .done(),
         endMill(10, 20, 75), holder(40, 50), 40);
    return p;
}

// Every known-bad program of verify_test.cpp's kinds in one setup, as separate operations.
Program mixed()
{
    Program p;
    p.stock = boxSoup(box(0, 0, -30, 100, 80, 0));
    // A vise jaw along the stock's front (Y-15 to -5), 5 above its top.
    p.fixtures.push_back(boxSoup(box(0, -15, -40, 100, -5, 5)));
    // 180 mm of X travel: the last operation's rapid to X160 makes the program 190 long (it starts at X-30).
    lib::MachineDef m;
    m.axes = {{'X', 0, 180}, {'Y', 0, 80}, {'Z', -300, 0}};
    p.machine = m;
    p.op(Path(-20, 40, 20).rapid(50, 40, -5).rapid(50, 40, 20).done(), endMill(10, 20, 75), holder(40, 50), 40);
    p.op(Path(-20, 40, 5).feed(-20, 40, -5).feed(110, 40, -5).rapid(50, 46, -4).rapid(50, 46, 20).done(),
         endMill(10, 20, 75), holder(40, 50), 40);
    p.op(Path(-30, 40, 20).feed(-30, 40, -35).feed(110, 40, -35).feed(110, 40, 20).done(), endMill(10, 40, 100),
         holder(40, 50), 30);
    p.op(Path(-30, 2, 20).feed(-30, 2, -10).feed(130, 2, -10).feed(130, 2, 20).done(), endMill(10, 12, 60),
         holder(40, 50), 12);
    p.op(Path(50, 20, 5).feed(50, 20, -5).feed(50, 20, 20).rapid(160, 20, 20).done(), endMill(10, 20, 75),
         holder(40, 50), 40);
    return p;
}

// Bit-exact equality of two event lists.
bool same(const std::vector<SimEvent>& a, const std::vector<SimEvent>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const SimEvent& x = a[i];
        const SimEvent& y = b[i];
        if (x.kind != y.kind || x.severity != y.severity || x.operation != y.operation || x.moveIndex != y.moveIndex ||
            x.message != y.message || std::memcmp(&x.position, &y.position, sizeof(Vec3)) != 0 ||
            std::bit_cast<std::uint64_t>(x.amountMm) != std::bit_cast<std::uint64_t>(y.amountMm)) {
            return false;
        }
    }
    return true;
}

struct Tier {
    const char* name;
    double cellMm; // the preset's simulation cell (PLAN.md 6.2)
    int workers;
};
constexpr Tier kTiers[] = {{"Low", 0.2, 2}, {"Medium", 0.1, 4}, {"High", 0.05, 16}};

} // namespace

TEST_CASE("the checks give the same events on every tier; the tier's own stock model would not", "[sim][verify]")
{
    Program p = thinWall();
    std::vector<std::vector<SimEvent>> perTier;
    for (const Tier& t : kTiers) {
        // What a tier changes: the worker count of the job system, and the cell its simulator runs at.
        jobs().setWorkerCount(t.workers);
        std::vector<SimEvent> ev = verify(p);
        INFO(t.name << "\n" << describe(ev));
        CHECK(summary(ev, p) == std::vector<std::string>{"RapidIntoCutStock@0.8"});
        perTier.push_back(std::move(ev));

        // The tier's own stock model, cut through the same moves, asked the same question.
        const sim::detail::VerifyInput in = p.input();
        const std::vector<sim::detail::Motion> motions = sim::detail::expandMotions(in);
        std::unique_ptr<IStockModel> model = makeTriDexelCpu(jobs());
        model->reset(*p.stock, box(0, 0, -30, 100, 80, 0), StockResolution{t.cellMm});
        const ToolShape& shape = p.ops[0].shape;
        for (const sim::detail::Motion& m : motions) {
            if (m.move == 8) {
                const bool seen = model->intersects(shape, m.a, m.b);
                // Low's cell is coarser than the wall's thickness and misses it; that is why the checks have their own.
                CHECK(seen == (t.cellMm < 0.15));
                break;
            }
            model->cut(shape, m.a, m.b, m.rapid);
        }
    }
    jobs().setWorkerCount(8);
    CHECK(same(perTier[0], perTier[1]));
    CHECK(same(perTier[0], perTier[2]));
}

TEST_CASE("the same events, bit for bit, for any thread count and on a repeat", "[sim][verify]")
{
    Program p = mixed();
    std::vector<SimEvent> first;
    for (const int workers : {1, 3, 16, 1}) {
        jobs().setWorkerCount(workers);
        sim::detail::VerifyStats stats;
        const std::vector<SimEvent> ev = verify(p, &stats);
        INFO(workers << " workers\n" << describe(ev));
        CHECK(stats.cutStockRan);
        if (first.empty()) {
            first = ev;
            // Each operation's fault: rapid into stock; rapid into cut stock; holder over the slot's walls; holder
            // through the jaw; a plunge into the stock and travel beyond X.
            CHECK(summary(ev, p) == std::vector<std::string>{"RapidIntoStock@0.0", "RapidIntoCutStock@1.2",
                                                             "HolderHitsStock@2.1", "ToolTooShort@2.1",
                                                             "HolderHitsFixture@3.1", "ToolTooShort@3.1",
                                                             "PlungeWithNonCentreCutting@4.0", "TravelLimit@4.2"});
        } else {
            CHECK(same(first, ev));
        }
    }
    jobs().setWorkerCount(8);
}
