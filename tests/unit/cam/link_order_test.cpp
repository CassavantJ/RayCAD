// Linking (task T-051), the order: on passes shaped like the benchmark parts' operations and on seeded random small
// cases, the rapid distance of the linked toolpath is within 10% of the best order found by brute force (every
// permutation that respects the sequence and level constraints, each with its best orientation). The linked rapid
// length equals the order's travelCost, so the comparison is of real toolpaths.
#include "link_parts.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace ray::cam::link::test {
namespace {

struct Best {
    double cost = std::numeric_limits<double>::infinity();
    std::vector<std::size_t> order;
    std::size_t permutations = 0;
};

Best bruteForce(const std::vector<Pass>& passes, const Options& o)
{
    Best b;
    std::vector<std::size_t> idx(passes.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    do {
        if (!validOrder(passes, idx))
            continue;
        ++b.permutations;
        const std::vector<Placement> pl = orient(passes, idx, o);
        const double c = travelCost(passes, pl, o);
        if (c < b.cost) {
            b.cost = c;
            b.order = idx;
        }
    } while (std::next_permutation(idx.begin(), idx.end()));
    return b;
}

struct Compared {
    double best = 0, ours = 0, linked = 0;
};

Compared compare(const std::vector<Pass>& passes, const Options& o, const Stock& stock)
{
    JobContext ctx;
    const Best best = bruteForce(passes, o);
    REQUIRE(best.permutations > 0);
    const std::vector<Placement> ours = take(orderPasses(passes, o, ctx));
    std::vector<std::size_t> idx;
    for (const Placement& p : ours)
        idx.push_back(p.pass);
    CHECK(validOrder(passes, idx));
    const Linked l = take(linkPasses(passes, o, stock, ctx));
    Compared c{best.cost, travelCost(passes, ours, o), l.rapidLength};
    // The linked toolpath's rapids are exactly what the cost function counts (plunge or free entries, no stay-down).
    CHECK(std::abs(c.linked - c.ours) <= 1e-9 * c.ours + 2.0);
    return c;
}

} // namespace

TEST_CASE("link order: rapid distance on the benchmark parts within 10% of the brute-force best", "[link][order]")
{
    for (const PartCase& pc : benchmarkCases()) {
        INFO(pc.name << ", " << pc.passes.size() << " passes");
        const Compared c = compare(pc.passes, pc.options, pc.stock);
        std::printf("link order: %-22s %zu passes: linked rapids %.3f mm, brute-force best %.3f mm, ratio %.4f\n",
                    pc.name.c_str(), pc.passes.size(), c.linked / 1e6, c.best / 1e6, c.linked / c.best);
        CHECK(c.linked <= 1.10 * c.best);
        CHECK(c.ours >= c.best - 1e-6 * c.best); // brute force is the best there is
    }
}

TEST_CASE("link order: seeded random small cases within 10% of the brute-force best", "[link][order]")
{
    double worst = 1.0;
    std::uint64_t worstSeed = 0;
    int exact = 0;
    constexpr std::uint64_t kSeeds = 300;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng{seed * 7919};
        const int n = static_cast<int>(rng.between(3, 8));
        std::vector<Pass> passes;
        for (int k = 0; k < n; ++k) {
            const Pt a{rng.between(0, 100 * kMm), rng.between(0, 80 * kMm)};
            const Nm z = -kMm * rng.between(1, 3);
            Pass p;
            const double kind = rng.uniform();
            if (kind < 0.4) {
                const Pt b{a.x + rng.between(-30 * kMm, 30 * kMm), a.y + rng.between(-30 * kMm, 30 * kMm)};
                p = passFromPath(openPath(a, {line(b == a ? Pt{a.x + kMm, a.y} : b)}), z, kCutFeed);
                p.reversible = rng.chance(0.6);
            } else if (kind < 0.7) {
                const Coord w = rng.between(4 * kMm, 25 * kMm), h = rng.between(4 * kMm, 25 * kMm);
                p = passFromPath(rect(a.x, a.y, a.x + w, a.y + h), z, kCutFeed);
                p.rotatable = rng.chance(0.7);
                p.reversible = rng.chance(0.3);
            } else {
                p = passFromPath(Path::circle(a, rng.between(2 * kMm, 12 * kMm), rng.chance(0.5)), z, kCutFeed);
                p.rotatable = rng.chance(0.7);
            }
            if (rng.chance(0.25)) {
                p.leadIn = Lead{LeadKind::Arc, rng.between(kMm, 4 * kMm), 90, 0, Side::Left, 0};
                p.leadOut = p.leadIn;
                p.room = unbounded();
            }
            if (rng.chance(0.4)) {
                p.cluster = static_cast<int>(rng.between(0, 1));
                p.level = static_cast<int>(rng.between(0, 2));
            }
            p.sequence = rng.chance(0.15) ? 1 : 0;
            passes.push_back(p);
        }
        Options o = baseOptions();
        o.entryOrder = {EntryKind::Plunge};
        o.centreCutting = true;
        o.rapidRetract = rng.chance(0.5);
        INFO("seed " << seed << ", " << n << " passes");
        const Compared c = compare(passes, o, Stock{});
        const double ratio = c.linked / c.best;
        if (ratio > 1.10 && std::getenv("RAY_LINK_DEBUG") != nullptr) {
            const Best b = bruteForce(passes, o);
            JobContext ctx;
            const std::vector<Placement> ours = take(orderPasses(passes, o, ctx));
            std::printf("seed %llu: ours %.3f best %.3f\n", static_cast<unsigned long long>(seed), c.ours / 1e6,
                        c.best / 1e6);
            for (const Placement& p : ours)
                std::printf("  ours %zu rev %d s %.3f\n", p.pass, p.reversed ? 1 : 0, p.startS / 1e6);
            for (const Placement& p : orient(passes, b.order, o))
                std::printf("  best %zu rev %d s %.3f\n", p.pass, p.reversed ? 1 : 0, p.startS / 1e6);
            for (std::size_t i = 0; i < passes.size(); ++i)
                std::printf("  pass %zu start (%.3f %.3f %.3f) moves %zu rev %d rot %d cl %d lv %d seq %d lead %d\n", i,
                            passes[i].start.x / 1e6, passes[i].start.y / 1e6, passes[i].start.z / 1e6,
                            passes[i].moves.size(), passes[i].reversible, passes[i].rotatable, passes[i].cluster,
                            passes[i].level, passes[i].sequence, static_cast<int>(passes[i].leadIn.kind));
        }
        CHECK(ratio <= 1.10);
        if (ratio <= 1.0 + 1e-9)
            ++exact;
        if (ratio > worst) {
            worst = ratio;
            worstSeed = seed;
        }
    }
    std::printf("link order: %d of %d random cases at the brute-force best; worst ratio %.4f (seed %llu)\n", exact,
                static_cast<int>(kSeeds), worst, static_cast<unsigned long long>(worstSeed));
}

TEST_CASE("link order: a start point is part of the cost", "[link][order]")
{
    std::vector<Pass> passes;
    for (int k = 0; k < 4; ++k)
        passes.push_back(
            passFromPath(openPath({k * 20 * kMm, 0}, {line({k * 20 * kMm + 5 * kMm, 0})}), -kMm, kCutFeed));
    Options o = baseOptions();
    o.from = Pt{100 * kMm, 0};
    JobContext ctx;
    const std::vector<Placement> order = take(orderPasses(passes, o, ctx));
    REQUIRE(order.size() == 4);
    CHECK(order[0].pass == 3); // nearest the start point
    CHECK(order[3].pass == 0);
}

} // namespace ray::cam::link::test
