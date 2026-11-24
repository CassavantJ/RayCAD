// The stock model's display mesh and carried solid (task T-070): dirty regions, the triangle budget, closed surfaces,
// splicing a region into an earlier mesh as a viewer does (T-073), and re-orienting stock through toSolid (T-075).
#include "tridexel_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

using P3 = std::array<float, 3>;

// Every directed edge (by exact vertex position) is matched by as many edges the other way.
bool closedByPosition(const std::vector<float>& pos, const std::vector<std::uint32_t>& idx)
{
    std::map<std::pair<P3, P3>, int> edges;
    const auto at = [&](std::uint32_t i) {
        return P3{pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]};
    };
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        for (int k = 0; k < 3; ++k) {
            const P3 a = at(idx[t + k]), b = at(idx[t + (k + 1) % 3]);
            if (a < b) {
                ++edges[{a, b}];
            } else {
                --edges[{b, a}];
            }
        }
    }
    for (const auto& [e, n] : edges) {
        if (n != 0) {
            return false;
        }
    }
    return true;
}

// Volume enclosed by a closed, outward triangle surface (divergence theorem).
double enclosed(const std::vector<float>& pos, const std::vector<std::uint32_t>& idx, const Vec3& ref)
{
    double v = 0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        double p[3][3];
        for (int k = 0; k < 3; ++k) {
            for (int a = 0; a < 3; ++a) {
                p[k][a] = static_cast<double>(pos[3 * idx[t + k] + a]) - (a == 0 ? ref.x : a == 1 ? ref.y : ref.z);
            }
        }
        v += p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
             p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
    }
    return v / 6.0;
}

std::vector<float> toFloat(const std::vector<double>& d)
{
    std::vector<float> f;
    f.reserve(d.size());
    for (double v : d) {
        f.push_back(static_cast<float>(v));
    }
    return f;
}

// Inside the box in XY, to float rounding of the vertices.
bool inBoxXY(const Box3& b, double x, double y)
{
    const double e = 1e-4;
    return x >= b.min.x - e && x <= b.max.x + e && y >= b.min.y - e && y <= b.max.y + e;
}

} // namespace

TEST_CASE("toMesh: the whole stock, then only what changed", "[sim][tridexel][mesh]")
{
    auto m = blockStock({-5.0, 2.0, -20.0}, {75.0, 52.0, 0.0}, 0.2);
    const std::uint32_t v0 = m->version();
    Box3 box;
    const kernel::Mesh full = m->toMesh(0, &box, 10'000'000);
    CHECK(box.min.x == -5.0);
    CHECK(box.min.y == 2.0);
    CHECK(std::abs(box.max.x - 75.0) < 1e-9);
    CHECK(std::abs(box.max.y - 52.0) < 1e-9);
    CHECK(box.min.z == -20.0);
    CHECK(box.max.z == 0.0);
    CHECK(closedByPosition(full.positions, full.indices));
    CHECK(std::abs(enclosed(full.positions, full.indices, {0, 0, 0}) - 80.0 * 50.0 * 20.0) < 1.0);
    CHECK(full.normals.size() == full.positions.size());
    // Nothing changed since v0: an empty mesh and an empty box.
    const kernel::Mesh none = m->toMesh(v0, &box, 1000);
    CHECK(none.indices.empty());
    CHECK(box.min.x > box.max.x);
    // A pocket corner: only the tiles it touched (64 cells = 12.8 mm tiles here).
    m->cut(flat(8.0), {3.0, 8.0, -6.0}, {9.0, 8.0, -6.0}, false);
    m->cutArc(flat(8.0), {9.0, 8.0, -6.0}, {9.0, 14.0, -6.0}, {9.0, 11.0, -6.0}, false, cam::ArcPlane::XY);
    const kernel::Mesh part = m->toMesh(v0, &box, 10'000'000);
    INFO("dirty box x " << box.min.x << " .. " << box.max.x << ", y " << box.min.y << " .. " << box.max.y);
    CHECK(box.min.x == -5.0);
    CHECK(std::abs(box.max.x - (-5.0 + 2 * 12.8)) < 1e-9);
    CHECK(box.min.y == 2.0);
    CHECK(std::abs(box.max.y - (2.0 + 2 * 12.8)) < 1e-9);
    CHECK(closedByPosition(part.positions, part.indices));
    CHECK(part.indices.size() < full.indices.size() * 2);
    for (std::size_t i = 0; i < part.positions.size(); i += 3) {
        CHECK(inBoxXY(box, part.positions[i], part.positions[i + 1]));
    }
    // The region holds less than its box: the cut is in it.
    const double region = enclosed(part.positions, part.indices, {0, 0, 0});
    CHECK(region < 25.6 * 25.6 * 20.0 - 100.0);
    CHECK(region > 0);
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(*m);
    const kernel::Mesh after = m->toMesh(0, nullptr, 10'000'000);
    CHECK(std::abs(enclosed(after.positions, after.indices, {0, 0, 0}) - in.volume[0]) < 1e-3 * in.volume[0]);
}

TEST_CASE("toMesh honours the triangle budget by merging columns", "[sim][tridexel][mesh]")
{
    auto m = blockStock({0, 0, -25.4}, {152.4, 101.6, 0}, 0.2);
    const Workload w = benchmarkWorkload(3, 2.0);
    runWorkload(*m, w, 512);
    const kernel::Mesh full = m->toMesh(0, nullptr, 100'000'000);
    const std::size_t fullTris = full.indices.size() / 3;
    INFO("full resolution: " << fullTris << " triangles");
    CHECK(closedByPosition(full.positions, full.indices));
    for (std::uint32_t budget : {500'000u, 100'000u, 20'000u}) {
        const kernel::Mesh small = m->toMesh(0, nullptr, budget);
        const std::size_t tris = small.indices.size() / 3;
        INFO("budget " << budget << ": " << tris << " triangles");
        CHECK(tris <= budget);
        CHECK(tris > 0);
        CHECK(closedByPosition(small.positions, small.indices));
    }
    // Below one column per tile, the coarsest mesh is returned (more than the budget).
    const kernel::Mesh tiny = m->toMesh(0, nullptr, 10);
    CHECK(tiny.indices.size() / 3 > 10);
    CHECK(closedByPosition(tiny.positions, tiny.indices));
}

TEST_CASE("A dirty region spliced into the earlier mesh gives the new stock", "[sim][tridexel][mesh]")
{
    // What a viewer does with updateMesh(dirty): drop the old triangles of the tiles the new mesh brings (its face
    // ranges, one per tile), add the new ones.
    auto m = blockStock({0, 0, -15}, {60, 40, 0}, 0.25);
    m->cut(ball(6.0), {5, 5, -3}, {55, 35, -5}, false);
    std::uint32_t shown = m->version();
    kernel::Mesh view = m->toMesh(0, nullptr, 50'000'000);
    const int tiles = sim::detail::triDexelInfo(*m).tilesX * sim::detail::triDexelInfo(*m).tilesY;
    CHECK(static_cast<int>(view.faces.size()) == tiles);
    for (int step = 0; step < 3; ++step) {
        m->cut(flat(5.0), {10.0 + 15 * step, 30, -4}, {12.0 + 15 * step, 8, -8}, false);
        Box3 box;
        const kernel::Mesh patch = m->toMesh(shown, &box, 50'000'000);
        shown = m->version();
        REQUIRE(!patch.faces.empty());
        CHECK(static_cast<int>(patch.faces.size()) < tiles);
        std::set<std::uint32_t> replaced;
        std::size_t covered = 0;
        for (const kernel::FaceRange& f : patch.faces) {
            replaced.insert(f.face.index);
            covered += f.indexCount;
            // Every triangle of a tile lies in the box.
            for (std::uint32_t i = f.firstIndex; i < f.firstIndex + f.indexCount; ++i) {
                const std::uint32_t v = patch.indices[i];
                CHECK(inBoxXY(box, patch.positions[3 * v], patch.positions[3 * v + 1]));
            }
        }
        CHECK(covered == patch.indices.size());
        kernel::Mesh next;
        const auto append = [&next](const kernel::Mesh& from, const kernel::FaceRange& f) {
            const std::uint32_t first = static_cast<std::uint32_t>(next.indices.size());
            for (std::uint32_t i = f.firstIndex; i < f.firstIndex + f.indexCount; ++i) {
                const std::uint32_t v = from.indices[i];
                next.indices.push_back(static_cast<std::uint32_t>(next.positions.size() / 3));
                next.positions.insert(next.positions.end(), from.positions.begin() + 3 * v,
                                      from.positions.begin() + 3 * v + 3);
            }
            next.faces.push_back({f.face, first, f.indexCount});
        };
        for (const kernel::FaceRange& f : view.faces) {
            if (replaced.count(f.face.index) == 0) {
                append(view, f);
            }
        }
        for (const kernel::FaceRange& f : patch.faces) {
            append(patch, f);
        }
        view = std::move(next);
        INFO("step " << step);
        CHECK(static_cast<int>(view.faces.size()) == tiles);
        CHECK(closedByPosition(view.positions, view.indices));
        const double vol = sim::detail::triDexelInfo(*m).volume[0];
        CHECK(std::abs(enclosed(view.positions, view.indices, {0, 0, 0}) - vol) < 1e-3 * vol);
    }
}

TEST_CASE("toSolid is a closed, welded solid that re-orients into a new stock", "[sim][tridexel][mesh]")
{
    auto m = blockStock({0, 0, -20}, {50, 30, 0}, 0.25);
    m->cut(flat(10.0), {10, 15, -8}, {40, 15, -8}, false);
    m->cut(flat(4.0), {25, 15, 2}, {25, 15, -25}, false); // a through hole
    const kernel::TriSoup solid = m->toSolid();
    REQUIRE(!solid.indices.empty());
    // Closed by index: every directed edge has its reverse.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
    for (std::size_t t = 0; t < solid.indices.size(); t += 3) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = solid.indices[t + k], b = solid.indices[t + (k + 1) % 3];
            if (a < b) {
                ++edges[{a, b}];
            } else {
                --edges[{b, a}];
            }
        }
    }
    bool closed = true;
    for (const auto& [e, n] : edges) {
        closed = closed && n == 0;
    }
    CHECK(closed);
    const std::vector<float> pf = toFloat(solid.positions);
    const double vz = sim::detail::triDexelInfo(*m).volume[0];
    CHECK(std::abs(enclosed(pf, solid.indices, {0, 0, 0}) - vz) < 1e-3 * vz);
    // Setup 2: flip the part over (rotate 180 degrees about X) and start a new stock from it.
    kernel::TriSoup flipped = solid;
    for (std::size_t i = 0; i < flipped.positions.size(); i += 3) {
        flipped.positions[i + 1] = 30.0 - flipped.positions[i + 1];
        flipped.positions[i + 2] = -20.0 - flipped.positions[i + 2];
    }
    auto m2 = makeTriDexelCpu(jobs());
    m2->reset(flipped, Box3{{0, 0, -20}, {50, 30, 0}}, StockResolution{0.25});
    const Volumes v2 = volumes(*m2);
    const double area = 2 * (50 * 30 + 50 * 20 + 30 * 20) + 2 * 30 * 8 + 2 * kPi * 2 * 20;
    for (double g : v2.v) {
        CHECK(std::abs(g - vz) <= 0.25 * area);
    }
    // The slot is now at the bottom: a 1 mm probe finds material at the top beside the hole, none in the through hole
    // and none inside the slot (now under the stock).
    ToolShape probe;
    probe.cuttingProfile = {{0, 0}, {0.5, 0}, {0.5, 1}};
    const auto takes = [&](double x, double y, double z) {
        auto c = m2->clone();
        return c->cut(probe, {x, y, z}, {x, y, z}, false);
    };
    CHECK(takes(15, 15, -2) > 0.5);
    CHECK(takes(25, 15, -2) == 0.0);
    CHECK(takes(15, 15, -19.5) == 0.0);
    CHECK(takes(15, 15, -11.0) > 0.5);
}
