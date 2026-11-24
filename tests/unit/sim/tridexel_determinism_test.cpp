// Stock model determinism (task T-070; PLAN.md section 8): the stock, the removed volumes and the non-cutting hits are
// bit-identical for 1, 4 and 16 workers, for any batch size, with snapshots and clones taken on the way, and match a
// golden hash that a Debug and a Release build (and other platforms) must all reproduce.
#include "tridexel_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// The S6 workload thinned out (3 pocket lanes, 2 mm scallops), plus arcs in the three planes and a short-fluted tool
// whose shank meets the stock, so every path of the model runs.
struct Run {
    std::uint64_t hash = 0;
    std::vector<double> removed;
    std::vector<std::uint8_t> hits;
};

std::vector<Workload::Pass> workload()
{
    Workload w = benchmarkWorkload(3, 2.0);
    Workload::Pass arcs{2, {}};
    using CM = IStockModel::CutMove;
    const auto arc = [&](Vec3 a, Vec3 b, Vec3 c, bool cw, cam::ArcPlane pl) {
        CM m{a, b, false};
        m.kind = cw ? cam::MoveKind::ArcCW : cam::MoveKind::ArcCCW;
        m.centre = c;
        m.plane = pl;
        arcs.moves.push_back(m);
    };
    for (int i = 0; i < 40; ++i) {
        const double x = 110 + (i % 8) * 4.0, y = 10 + (i / 8) * 8.0;
        arc({x + 3, y, 20.0 - i * 0.1}, {x + 3, y, 19.0 - i * 0.1}, {x, y, 0}, i % 2 == 0, cam::ArcPlane::XY);
        arc({x, y, 21.0}, {x + 4, y + 2, 21.0}, {x + 2, y + 1, 21.0}, true, cam::ArcPlane::XZ);
        arc({x, y, 21.0}, {x + 1, y + 4, 21.0}, {x + 0.5, y + 2, 21.0}, false, cam::ArcPlane::YZ);
    }
    w.passes.push_back(std::move(arcs));
    return w.passes;
}

const ToolShape& toolOf(int i)
{
    static const ToolShape shortFlutes = [] {
        lib::Tool t = tool(lib::ToolType::BullEndMill, 8.0, 3.0);
        t.cornerRadius = 1.0;
        t.shankDiameter = 10.0;
        return makeToolShape(t, nullptr, 0);
    }();
    return i < 2 ? benchmarkTool(i) : shortFlutes;
}

Run runAll(int workers, std::size_t batch, double cell, bool keyframes)
{
    jobs().setWorkerCount(workers);
    auto m = makeTriDexelCpu(jobs());
    m->reset({}, benchmarkStock(), StockResolution{cell});
    Run r;
    std::vector<std::shared_ptr<const IStockModel::Snapshot>> snaps;
    std::vector<std::unique_ptr<IStockModel>> clones;
    for (const Workload::Pass& p : workload()) {
        for (std::size_t i = 0; i < p.moves.size(); i += batch) {
            const std::size_t n = std::min(batch, p.moves.size() - i);
            std::vector<double> v(n);
            std::vector<std::uint8_t> h(n);
            m->cutBatch(toolOf(p.tool), std::span<const IStockModel::CutMove>(p.moves.data() + i, n), v, h);
            r.removed.insert(r.removed.end(), v.begin(), v.end());
            r.hits.insert(r.hits.end(), h.begin(), h.end());
            if (keyframes && (i / batch) % 7 == 3) {
                snaps.push_back(m->snapshot()); // kept alive: later cuts copy the strips they share
                clones.push_back(m->clone());
            }
        }
    }
    r.hash = sim::detail::triDexelHash(*m);
    jobs().setWorkerCount(8);
    return r;
}

bool same(const Run& a, const Run& b)
{
    return a.hash == b.hash && a.removed.size() == b.removed.size() &&
           std::memcmp(a.removed.data(), b.removed.data(), a.removed.size() * sizeof(double)) == 0 && a.hits == b.hits;
}

} // namespace

TEST_CASE("Identical stock, volumes and hits for 1, 4 and 16 workers and any batch size",
          "[sim][tridexel][determinism]")
{
    const double cell = 0.5;
    const Run ref = runAll(4, 512, cell, false);
    std::size_t hits = 0;
    for (std::uint8_t h : ref.hits) {
        hits += h;
    }
    INFO(ref.removed.size() << " moves, " << hits << " with the shank in the stock");
    CHECK(hits > 0);
    for (int workers : {1, 4, 16}) {
        for (std::size_t batch : {1u, 7u, 64u, 512u, 100000u}) {
            if (batch == 1 && workers != 4) {
                continue; // batch 1 is slow: once is enough
            }
            const Run r = runAll(workers, batch, cell, batch == 64);
            INFO(workers << " workers, batch " << batch);
            CHECK(same(r, ref));
        }
    }
}

TEST_CASE("The stock hash of a fixed program is the golden value", "[sim][tridexel][determinism]")
{
    // A Debug and a Release build must both produce this value (the module is built with strict floating point; its
    // kernels drop only FENV_ACCESS: see tridexel_cut.cpp). Regenerate it only for a deliberate change of the model.
    constexpr std::uint64_t kGolden = 0x4c979f4be80cb980ull;
    const Run r = runAll(4, 512, 0.5, false);
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(r.hash));
    std::cout << "T-070 golden stock hash: 0x" << hex << std::endl;
    CHECK(r.hash == kGolden);
}
