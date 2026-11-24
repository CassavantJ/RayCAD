// Stock model behaviour other than volumes of cuts (task T-070): grids, stock from triangle soups and kernel solids,
// snapshots, clones, versions, the non-cutting part and intersects.
#include "tridexel_test_util.h"

#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// A closed, outward triangle soup of a cylinder about +Z with `n` facets (a prism).
kernel::TriSoup cylinderSoup(double cx, double cy, double z0, double z1, double r, int n, bool inward = false)
{
    kernel::TriSoup s;
    const auto add = [&](double x, double y, double z) {
        s.positions.insert(s.positions.end(), {x, y, z});
        return static_cast<std::uint32_t>(s.positions.size() / 3 - 1);
    };
    const std::uint32_t b = add(cx, cy, z0), t = add(cx, cy, z1);
    std::vector<std::uint32_t> lo, hi;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n;
        lo.push_back(add(cx + r * std::cos(a), cy + r * std::sin(a), z0));
        hi.push_back(add(cx + r * std::cos(a), cy + r * std::sin(a), z1));
    }
    const auto tri = [&](std::uint32_t a, std::uint32_t c, std::uint32_t d) {
        if (inward) {
            s.indices.insert(s.indices.end(), {a, d, c});
        } else {
            s.indices.insert(s.indices.end(), {a, c, d});
        }
    };
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        tri(b, lo[static_cast<std::size_t>(j)], lo[static_cast<std::size_t>(i)]);
        tri(t, hi[static_cast<std::size_t>(i)], hi[static_cast<std::size_t>(j)]);
        tri(lo[static_cast<std::size_t>(i)], lo[static_cast<std::size_t>(j)], hi[static_cast<std::size_t>(j)]);
        tri(lo[static_cast<std::size_t>(i)], hi[static_cast<std::size_t>(j)], hi[static_cast<std::size_t>(i)]);
    }
    return s;
}

double cellLayer(const IStockModel& m, double area)
{
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(m);
    return std::max({in.cx, in.cy, in.cz}) * area;
}

} // namespace

TEST_CASE("The cell is snapped per axis so the stock box is spanned exactly", "[sim][tridexel][stock]")
{
    auto m = blockStock({1.1, -2.3, 0.7}, {153.5, 99.3, 26.1}, 0.2);
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(*m);
    CHECK(in.nx == static_cast<int>(std::ceil(152.4 / 0.2 - 1e-9)));
    CHECK(std::abs(in.nx * in.cx - 152.4) < 1e-9);
    CHECK(std::abs(in.ny * in.cy - 101.6) < 1e-9);
    CHECK(std::abs(in.nz * in.cz - 25.4) < 1e-9);
    CHECK(in.cx <= 0.2);
    CHECK(in.cy <= 0.2);
    CHECK(in.cz <= 0.2);
    const double box = 152.4 * 101.6 * 25.4;
    for (int g = 0; g < 3; ++g) {
        CHECK(std::abs(in.volume[g] - box) < 1e-4 * box * 1e-3);
    }
    CHECK(m->version() == 1);
    CHECK(in.dexels == static_cast<std::uint64_t>(in.nx) * in.ny + static_cast<std::uint64_t>(in.ny) * in.nz +
                           static_cast<std::uint64_t>(in.nx) * in.nz);
    // An empty or broken box gives a model without material that ignores cuts.
    auto e = makeTriDexelCpu(jobs());
    e->reset({}, Box3{{0, 0, 0}, {10, 0, 5}}, StockResolution{0.2});
    CHECK(e->cut(flat(6), {5, 0, -1}, {5, 0, -1}, false) == 0.0);
    CHECK_FALSE(e->intersects(flat(6), {0, 0, 0}, {1, 1, 1}));
    CHECK(e->toMesh(0, nullptr, 1000).indices.empty());
}

TEST_CASE("Stock from a triangle soup: a cylinder, either orientation", "[sim][tridexel][stock]")
{
    const double r = 30.0, z0 = -20.0, z1 = 0.0;
    const int n = 256;
    const double prism = 0.5 * n * r * r * std::sin(2 * kPi / n) * (z1 - z0);
    const double area = 2 * kPi * r * (z1 - z0) + 2 * kPi * r * r;
    for (bool inward : {false, true}) {
        for (double cell : {0.5, 0.2}) {
            auto m = makeTriDexelCpu(jobs());
            // Bounds a little larger than the cylinder and off the grid.
            m->reset(cylinderSoup(50.0, 40.0, z0, z1, r, n, inward), Box3{{18.7, 8.3, -20.0}, {81.9, 71.1, 0.0}},
                     StockResolution{cell});
            const Volumes v = volumes(*m);
            INFO("inward " << inward << ", cell " << cell << ": prism " << prism << ", grids " << v.v[0] << " / "
                           << v.v[1] << " / " << v.v[2]);
            for (double g : v.v) {
                CHECK(std::abs(g - prism) <= cellLayer(*m, area));
            }
            // A ray through the axis has the whole height; one outside has nothing.
            ToolShape probe; // wider than a cell, so it meets at least one ray
            probe.cuttingProfile = {{0, 0}, {cell, 0}, {cell, 1}};
            CHECK(m->intersects(probe, {50.0, 40.0, -10.0}, {50.0, 40.0, -10.0}));
            CHECK_FALSE(m->intersects(probe, {19.5, 9.5, -10.0}, {19.5, 9.5, -10.0}));
        }
    }
}

TEST_CASE("Stock from a kernel solid: a block with a through hole", "[sim][tridexel][stock]")
{
    JobContext ctx;
    auto block = kernel::makeBox({0, 0, -25}, {60, 40, 0});
    if (!block) {
        FAIL("makeBox: " << block.error().message);
    }
    auto hole = kernel::makeCylinder({30, 20, -30}, {0, 0, 1}, 8.0, 40);
    REQUIRE(hole);
    auto cut = kernel::boolean(kernel::BoolOp::Cut, *block, *hole, ctx);
    REQUIRE(cut);
    auto soup = kernel::triangulateExact(cut->shape, 0.01, ctx);
    REQUIRE(soup);
    auto props = kernel::massProps(cut->shape);
    REQUIRE(props);
    for (double cell : {0.5, 0.2}) {
        auto m = makeTriDexelCpu(jobs());
        m->reset(*soup, Box3{{0, 0, -25}, {60, 40, 0}}, StockResolution{cell});
        const Volumes v = volumes(*m);
        INFO("cell " << cell << ": kernel volume " << props->volume << ", grids " << v.v[0] << " / " << v.v[1] << " / "
                     << v.v[2]);
        for (double g : v.v) {
            CHECK(std::abs(g - props->volume) <= cellLayer(*m, props->area));
        }
        // The hole holds no material; the block beside it does.
        CHECK_FALSE(m->intersects(flat(4.0), {30, 20, -10}, {30, 20, -10}));
        CHECK(m->intersects(flat(4.0), {10, 10, -10}, {10, 10, -10}));
    }
}

TEST_CASE("Rays through soup edges and vertices are counted once", "[sim][tridexel][stock]")
{
    // An octahedron whose six vertices and many edges lie exactly on ray centres of a 1 mm grid (centres at k + 0.5),
    // and a box whose faces lie on ray centres: parity glitches would show as wrong volumes or stray material.
    kernel::TriSoup oct;
    oct.positions = {10.5, 10.5, 0.5,  10.5, 10.5, 20.5, 0.5,  10.5, 10.5,
                     20.5, 10.5, 10.5, 10.5, 0.5,  10.5, 10.5, 20.5, 10.5};
    // 0 bottom, 1 top, 2 -x, 3 +x, 4 -y, 5 +y
    oct.indices = {0, 3, 4, 0, 5, 3, 0, 2, 5, 0, 4, 2, 1, 4, 3, 1, 3, 5, 1, 5, 2, 1, 2, 4};
    auto m = makeTriDexelCpu(jobs());
    m->reset(oct, Box3{{0, 0, 0}, {21, 21, 21}}, StockResolution{1.0});
    const Volumes v = volumes(*m);
    const double exact = 4.0 / 3.0 * 10 * 10 * 10;
    for (double g : v.v) {
        CHECK(std::abs(g - exact) <= 1.0 * 4 * std::sqrt(3.0) * 100); // one cell over its surface (8 faces)
    }
    // Every dexel holds at most one interval (the solid is convex).
    CHECK(sim::detail::triDexelInfo(*m).maxIntervals[0] <= 1);
    CHECK(sim::detail::triDexelInfo(*m).maxIntervals[1] <= 1);
    CHECK(sim::detail::triDexelInfo(*m).maxIntervals[2] <= 1);
}

TEST_CASE("Snapshots and clones: cheap, independent, exact", "[sim][tridexel][stock]")
{
    const Workload w = benchmarkWorkload(3, 2.0);
    auto m = makeTriDexelCpu(jobs());
    m->reset({}, benchmarkStock(), StockResolution{0.5});
    std::vector<double> v(400);
    m->cutBatch(benchmarkTool(0), std::span<const IStockModel::CutMove>(w.passes[0].moves.data(), 400), v);
    const std::uint64_t h1 = sim::detail::triDexelHash(*m);
    const std::uint32_t v1 = m->version();
    auto snap = m->snapshot();
    auto copy = m->clone();
    CHECK(sim::detail::triDexelHash(*copy) == h1);
    CHECK(sim::detail::snapshotPrivateBytes(*m, *snap) == 0);
    m->cutBatch(benchmarkTool(0), std::span<const IStockModel::CutMove>(w.passes[0].moves.data() + 400, 400), v);
    const std::uint64_t h2 = sim::detail::triDexelHash(*m);
    CHECK(h2 != h1);
    CHECK(sim::detail::triDexelHash(*copy) == h1); // the clone did not move
    CHECK(sim::detail::snapshotPrivateBytes(*m, *snap) > 0);
    // The clone cut the same way reaches the same stock.
    copy->cutBatch(benchmarkTool(0), std::span<const IStockModel::CutMove>(w.passes[0].moves.data() + 400, 400), v);
    CHECK(sim::detail::triDexelHash(*copy) == h2);
    m->restore(*snap);
    CHECK(sim::detail::triDexelHash(*m) == h1);
    CHECK(m->version() > v1);
    CHECK(sim::detail::triDexelHash(*copy) == h2);
    // Cutting after a restore does not touch the snapshot: restore it again.
    m->cutBatch(benchmarkTool(1), std::span<const IStockModel::CutMove>(w.passes[1].moves.data(), 200), v);
    m->restore(*snap);
    CHECK(sim::detail::triDexelHash(*m) == h1);
    // A snapshot outlives its model; one of another grid is ignored.
    auto other = blockStock({0, 0, 0}, {10, 10, 10}, 0.5);
    auto otherSnap = other->snapshot();
    other.reset();
    m->restore(*otherSnap);
    CHECK(sim::detail::triDexelHash(*m) == h1);
}

TEST_CASE("Versions count every call that can change the stock", "[sim][tridexel][stock]")
{
    auto m = blockStock({0, 0, -10}, {50, 50, 0}, 0.5);
    std::uint32_t v = m->version();
    CHECK(v == 1);
    m->cut(flat(6), {10, 10, 5}, {10, 10, 4}, false); // nothing removed
    CHECK(m->version() == ++v);
    m->cutArc(flat(6), {20, 10, -1}, {10, 20, -1}, {10, 10, -1}, false, cam::ArcPlane::XY);
    CHECK(m->version() == ++v);
    m->cutBatch(flat(6), {}, {});
    CHECK(m->version() == ++v);
    auto s = m->snapshot();
    CHECK(m->version() == v);
    m->restore(*s);
    CHECK(m->version() == ++v);
    m->reset({}, Box3{{0, 0, -10}, {50, 50, 0}}, StockResolution{0.5});
    CHECK(m->version() == ++v);
}

TEST_CASE("The shank and holder meet stock: per move in a batch, and intersects", "[sim][tridexel][stock]")
{
    // Flutes 6 mm long, shank 8 mm (wider than the 6 mm cutter), no holder.
    lib::Tool t = tool(lib::ToolType::FlatEndMill, 6.0, 6.0);
    t.shankDiameter = 8.0;
    t.overallLength = 50;
    const ToolShape ts = makeToolShape(t, nullptr, 0);
    auto m = blockStock({0, 0, -20}, {60, 60, 0}, 0.25);
    using CM = IStockModel::CutMove;
    // 1. Down 4 mm into fresh stock: the shank's 1 mm wider ring starts at 6 + 1 mm above the tip (45 degree step):
    //    above the top. No hit.
    // 2. Down to -10: the step to the shank comes 3 mm below the top, around the flutes' hole: a hit (the shank is
    //    wider than the hole the flutes cut).
    // 3. Along at -10 into fresh stock: the shank meets the stock above the flutes: a hit.
    // 4. Straight up out of the slot's end: the shank starts in the stock above the slot (the flutes cut only the 6 mm
    //    under it): a hit, checked before the move since it rises.
    // 5. Above everything: no hit.
    std::vector<CM> moves = {{{10, 10, 5}, {10, 10, -4}, false},
                             {{10, 10, -4}, {10, 10, -10}, false},
                             {{10, 10, -10}, {30, 10, -10}, false},
                             {{30, 10, -10}, {30, 10, 5}, false},
                             {{30, 10, 5}, {50, 50, 5}, true}};
    std::vector<double> v(moves.size());
    std::vector<std::uint8_t> hit(moves.size());
    m->cutBatch(ts, moves, v, hit);
    CHECK(hit[0] == 0);
    CHECK(hit[1] == 1);
    CHECK(hit[2] == 1);
    CHECK(hit[3] == 1);
    CHECK(hit[4] == 0);
    CHECK(v[4] == 0.0);
    // The same flags with the moves in batches of one: per move, in order.
    auto m2 = blockStock({0, 0, -20}, {60, 60, 0}, 0.25);
    for (std::size_t i = 0; i < moves.size(); ++i) {
        double vv = 0;
        std::uint8_t hh = 0;
        m2->cutBatch(ts, std::span<const CM>(&moves[i], 1), std::span<double>(&vv, 1), std::span<std::uint8_t>(&hh, 1));
        CHECK(hh == hit[i]);
        CHECK(vv == v[i]);
    }
    CHECK(sim::detail::triDexelHash(*m2) == sim::detail::triDexelHash(*m));
    // intersects: the shank alone over the slot meets the material left above the flutes; the whole tool too.
    ToolShape shank = ts;
    shank.cuttingProfile.clear();
    CHECK(m->intersects(shank, {20, 10, -10}, {20, 10, -10}));
    CHECK(m->intersects(ts, {20, 10, -10}, {20, 10, -10}));
    CHECK_FALSE(m->intersects(ts, {0, 0, 1}, {60, 60, 1}));
    // A descending plunge whose own flutes clear the way, with a shank no wider than the cutter: no hit.
    const ToolShape straight = flat(6.0, 6.0);
    auto m3 = blockStock({0, 0, -20}, {60, 60, 0}, 0.25);
    const CM plunge{{40, 40, 5}, {40, 40, -15}, false};
    std::uint8_t ph = 9;
    double pv = 0;
    m3->cutBatch(straight, std::span<const CM>(&plunge, 1), std::span<double>(&pv, 1), std::span<std::uint8_t>(&ph, 1));
    CHECK(ph == 0);
    CHECK(pv > 0);
    // Rising out through material that a rising move's flutes would clear only later: the shank meets it first.
    auto m4 = blockStock({0, 0, -20}, {60, 60, 0}, 0.25);
    const CM rise{{20, 20, -12}, {40, 20, -2}, false};
    m4->cutBatch(straight, std::span<const CM>(&rise, 1), std::span<double>(&pv, 1), std::span<std::uint8_t>(&ph, 1));
    CHECK(ph == 1);
}

TEST_CASE("intersects is resolution-dependent: a wall thinner than a cell between rays is missed",
          "[sim][tridexel][stock]")
{
    // A 0.1 mm wall left between two slots, on a 1 mm grid whose rays miss it, and on a 0.05 mm grid that sees it.
    for (double cell : {1.0, 0.05}) {
        auto m = blockStock({0, 0, -10}, {40, 20, 0}, cell);
        const ToolShape t = flat(10.0);
        m->cut(t, {0, 5.0, -5}, {40, 5.0, -5}, false);   // y in [0, 10.0]
        m->cut(t, {0, 15.1, -5}, {40, 15.1, -5}, false); // y in [10.1, 20.1]
        ToolShape probe;
        probe.cuttingProfile = {{0, 0}, {3, 0}, {3, 3}};
        const bool seen = m->intersects(probe, {20, 10.05, -4}, {30, 10.05, -4});
        INFO("cell " << cell);
        CHECK(seen == (cell < 0.1));
    }
}

TEST_CASE("Memory of the 6 x 4 x 1 in stock: under 20 MB at 0.2 mm, under 300 MB at 0.05 mm", "[sim][tridexel][stock]")
{
    // The model's own accounting (dexel arrays, pools, tables). Cut by the full S6 workload these are checked again in
    // the benchmark (tridexel_bench_test.cpp); here fresh, and at 0.2 mm after a thinned workload.
    const auto mb = [](const IStockModel& m) {
        return static_cast<double>(sim::detail::triDexelInfo(m).bytes) / (1024.0 * 1024.0);
    };
    auto low = makeTriDexelCpu(jobs());
    low->reset({}, benchmarkStock(), StockResolution{0.2});
    CHECK(mb(*low) < 20.0);
    runWorkload(*low, benchmarkWorkload(3, 2.0), 512);
    INFO("0.2 mm after the thinned workload: " << mb(*low) << " MB");
    CHECK(mb(*low) < 20.0);
    auto high = makeTriDexelCpu(jobs());
    high->reset({}, benchmarkStock(), StockResolution{0.05});
    INFO("0.05 mm fresh: " << mb(*high) << " MB");
    CHECK(mb(*high) < 300.0);
    // 20 bytes a dexel: 8.77 M dexels at 0.05 mm.
    CHECK(sim::detail::triDexelInfo(*high).dexels == 3048ull * 2032 + 2032ull * 508 + 3048ull * 508);
}
