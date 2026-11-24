// In-process stock (task T-075), the src/cam half (src/cam/stock.h): a carried stock put into a later setup's frame
// (a flip about X, a turn onto a side), its box (snapped to the earlier box, a cell outward where the earlier setup cut
// a side away), and its slices: exact levels on 2.5D steps, a cell of growth, the material above every listed level
// inside its slice (seeded random column soups, never aligned with the grid), more than kCarriedExactLevels tops
// rounded down to whole cells, loops that never touch and that the 2D core (geom2d) reads, and the same bits every
// time.
#include "cam/geom2d.h"
#include "cam/stock.h"

#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace ray;
using namespace ray::cam;
namespace cd = ray::cam::detail;

namespace {

struct Box {
    Vec3 lo, hi;
};

// A closed, outward box soup appended to `soup` (12 triangles, counter-clockwise seen from outside).
void addBox(kernel::TriSoup& soup, const Box& b)
{
    const auto base = static_cast<std::uint32_t>(soup.positions.size() / 3);
    for (int k = 0; k < 8; ++k) {
        soup.positions.push_back((k & 1) ? b.hi.x : b.lo.x);
        soup.positions.push_back((k & 2) ? b.hi.y : b.lo.y);
        soup.positions.push_back((k & 4) ? b.hi.z : b.lo.z);
    }
    const std::uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                        {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& f : faces) {
        for (const std::uint32_t v : f) {
            soup.indices.push_back(base + v);
        }
    }
}

kernel::TriSoup soupOf(const std::vector<Box>& boxes)
{
    kernel::TriSoup s;
    for (const Box& b : boxes) {
        addBox(s, b);
    }
    return s;
}

Xform translation(Vec3 t)
{
    return Xform{{1, 0, 0, t.x, 0, 1, 0, t.y, 0, 0, 1, t.z}};
}

// The rotation of a setup whose Z face is the part's bottom: 180 degrees about X (rows X, -Y, -Z).
const Xform kFlip{{1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0}};
// A side setup: WCS Z = model -Y (the front face), X = model X, Y = Z x X = model +Z.
const Xform kFront{{1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0}};

CarriedStock carriedOf(kernel::TriSoup soup, Box3 bounds, Xform modelFromWcs)
{
    CarriedStock c;
    c.solid = std::make_shared<const kernel::TriSoup>(std::move(soup));
    c.bounds = bounds;
    c.modelFromWcs = modelFromWcs;
    return c;
}

// Even-odd point in the loops of one slice (lines only, as carriedSlices makes them).
bool inside(const std::vector<kernel::ProfileLoop>& loops, double x, double y)
{
    bool in = false;
    for (const kernel::ProfileLoop& loop : loops) {
        for (const kernel::ProfileCurve& c : loop.curves) {
            const Vec2 a = c.pts.at(0), b = c.pts.at(1);
            if ((a.y > y) != (b.y > y) && x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y)) {
                in = !in;
            }
        }
    }
    return in;
}

// The slice the consumers read at depth z: the highest level at or below it (adaptive2d op.h STOCK SLICES).
const std::vector<kernel::ProfileLoop>* sliceAt(const cd::StockSlices& slices, double z)
{
    const std::vector<kernel::ProfileLoop>* out = nullptr;
    for (const auto& [level, loops] : slices) {
        if (level <= z) {
            out = &loops;
        }
    }
    return out;
}

Hash256 hashOf(const cd::StockSlices& slices)
{
    Hasher h;
    for (const auto& [level, loops] : slices) {
        h.f64(level).i64(static_cast<std::int64_t>(loops.size()));
        for (const kernel::ProfileLoop& l : loops) {
            h.i64(static_cast<std::int64_t>(l.curves.size()));
            for (const kernel::ProfileCurve& c : l.curves) {
                h.f64(c.pts[0].x).f64(c.pts[0].y).f64(c.pts[1].x).f64(c.pts[1].y);
            }
        }
    }
    return h.finish();
}

cd::StockSlices sliced(const kernel::TriSoup& soup, const Box3& box, double cell, double pad = 0)
{
    JobContext ctx;
    auto s = cd::carriedSlices(soup, box, cell, pad, ctx);
    REQUIRE(s);
    return *s;
}

// Every loop of every slice reads as regions in the 2D core: no crossing, no touching.
void checkReadable(const cd::StockSlices& slices)
{
    for (const auto& [level, loops] : slices) {
        JobContext ctx;
        auto regions = geom2d::regionsFromLoops(loops, ctx);
        INFO("level " << level);
        REQUIRE(regions);
        CHECK(regions->size() <= loops.size());
    }
}

} // namespace

TEST_CASE("a carried stock flipped about X keeps its box, snapped to the earlier stock box", "[cam][carried]")
{
    // Setup 1's stock box, in its WCS (origin at its top front left corner, model (10, 5, 20)): 50 x 30 x 20. Its end:
    // faced 5 mm over x < 25, untouched beyond (a step).
    const Box3 bounds{{0, 0, -20}, {50, 30, 0}};
    const kernel::TriSoup end = soupOf({{{0, 0, -20}, {50, 30, -5}}, {{25, 0, -5}, {50, 30, 0}}});
    const CarriedStock c = carriedOf(end, bounds, translation({10, 5, 20}));
    JobContext ctx;
    auto o = cd::orientCarried(c, kFlip, ctx);
    REQUIRE(o);
    // Model box (10..60, 5..35, 0..20); flipped: x 10..60, y -35..-5, z -20..0. Every side lies on the earlier box.
    CHECK(o->lo == std::array<Nm, 3>{toNm(10), toNm(-35), toNm(-20)});
    CHECK(o->hi == std::array<Nm, 3>{toNm(60), toNm(-5), toNm(0)});
    // The soup moved exactly: the faced top (z -5 in setup 1, model 15) is z -15 in the flipped orientation.
    bool seen = false;
    for (std::size_t i = 2; i < o->soup.positions.size(); i += 3) {
        seen = seen || o->soup.positions[i] == -15.0;
    }
    CHECK(seen);
    CHECK(cd::carriedTopPad(c, kFlip) == 0.0);
    CHECK(cd::carriedTopPad(c, kFront) == c.cellMm);
}

TEST_CASE("a side the earlier setup cut away is moved out by one cell, never past the earlier box; tops and bottoms "
          "are not",
          "[cam][carried]")
{
    // The earlier setup contoured the outside 2 mm in from x = 0 and from y = 30, and faced 5 mm off the top.
    const Box3 bounds{{0, 0, -20}, {50, 30, 0}};
    const CarriedStock c = carriedOf(soupOf({{{2, 0, -20}, {50, 28, -5}}}), bounds, translation({0, 0, 0}));
    JobContext ctx;
    auto o = cd::orientCarried(c, kFlip, ctx);
    REQUIRE(o);
    // x: 2 - 0.25 (cut: one cell out), 50 (on the box). y (flipped): -28 + ... -> -(28) - 0.25 below, 0 on the box.
    // z (flipped, across the earlier Z): 5 as it is (the faced top, exact), 20 on the box.
    CHECK(o->lo == std::array<Nm, 3>{toNm(1.75), toNm(-28.25), toNm(5)});
    CHECK(o->hi == std::array<Nm, 3>{toNm(50), toNm(0), toNm(20)});
    // A cut 0.1 mm in: one cell out would pass the earlier box, so it stops there.
    const CarriedStock thin = carriedOf(soupOf({{{0.1, 0, -20}, {50, 30, 0}}}), bounds, translation({0, 0, 0}));
    auto t = cd::orientCarried(thin, kFlip, ctx);
    REQUIRE(t);
    CHECK(t->lo[0] == 0);
    // Turned onto its front: the earlier X and Z sides become this setup's X and Y, the earlier Y side its Z.
    auto side = cd::orientCarried(c, kFront, ctx);
    REQUIRE(side);
    // WCS Y = model +Z: -20..-5 (the faced top is across the earlier Z: exact); WCS Z = -model Y: -28 cut (out by a
    // cell: -28.25), 0 on the box.
    CHECK(side->lo == std::array<Nm, 3>{toNm(1.75), toNm(-20), toNm(-28.25)});
    CHECK(side->hi == std::array<Nm, 3>{toNm(50), toNm(-5), toNm(0)});
}

TEST_CASE("the slices of a 2.5D step: exact levels, the material above each, one cell of growth", "[cam][carried]")
{
    // 10 x 4, 2 high, with a 5 x 4 block 1 higher on the left; cell 0.25.
    const kernel::TriSoup soup = soupOf({{{0, 0, 0}, {10, 4, 2}}, {{0, 0, 2}, {5, 4, 3}}});
    const Box3 box{{0, 0, 0}, {10, 4, 3}};
    const cd::StockSlices s = sliced(soup, box, 0.25);
    REQUIRE(s.size() == 3);
    CHECK(s[0].first == 0.0); // the bottom: everything
    CHECK(s[1].first == 2.0); // above the lower top: the block, grown by a cell
    CHECK(s[2].first == 3.0); // above the highest top: nothing
    REQUIRE(s[0].second.size() == 1);
    REQUIRE(s[1].second.size() == 1);
    CHECK(s[2].second.empty());
    const auto corners = [](const kernel::ProfileLoop& l) {
        std::vector<std::pair<double, double>> out;
        for (const kernel::ProfileCurve& c : l.curves) {
            out.emplace_back(c.pts[0].x, c.pts[0].y);
        }
        return out;
    };
    // Counter-clockwise from the lowest corner.
    CHECK(corners(s[0].second[0]) == std::vector<std::pair<double, double>>{{0, 0}, {10, 0}, {10, 4}, {0, 4}});
    CHECK(corners(s[1].second[0]) == std::vector<std::pair<double, double>>{{0, 0}, {5.25, 0}, {5.25, 4}, {0, 4}});
    checkReadable(s);
}

TEST_CASE("the slices hold all the material above every level, on seeded random column soups", "[cam][carried]")
{
    std::mt19937_64 rng(0x7075);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int trial = 0; trial < 40; ++trial) {
        INFO("trial " << trial);
        const double cell = 0.2 + 0.3 * u(rng);
        // Columns of random size and height on a 40 x 25 x 12 stock, never on the grid.
        std::vector<Box> boxes;
        const int n = 3 + static_cast<int>(u(rng) * 12);
        for (int i = 0; i < n; ++i) {
            const double x = u(rng) * 36, y = u(rng) * 21, w = 0.3 + u(rng) * 8, d = 0.3 + u(rng) * 6;
            const double z0 = u(rng) * 6, z1 = z0 + 0.2 + u(rng) * 6;
            boxes.push_back(Box{{x, y, z0}, {std::min(40.0, x + w), std::min(25.0, y + d), z1}});
        }
        const kernel::TriSoup soup = soupOf(boxes);
        const Box3 box{{0, 0, 0}, {40, 25, 12.2}};
        const double pad = (trial % 4 == 0) ? cell : 0.0;
        const cd::StockSlices s = sliced(soup, box, cell, pad);
        REQUIRE(!s.empty());
        CHECK(s.front().first == 0.0);
        for (std::size_t i = 1; i < s.size(); ++i) {
            CHECK(s[i].first > s[i - 1].first);
        }
        // Every material point above a depth lies in (or on) the slice read at that depth.
        for (int k = 0; k < 400; ++k) {
            const Box& b = boxes[static_cast<std::size_t>(u(rng) * static_cast<double>(boxes.size()))];
            const double x = b.lo.x + u(rng) * (b.hi.x - b.lo.x), y = b.lo.y + u(rng) * (b.hi.y - b.lo.y);
            const double zPoint = b.lo.z + u(rng) * (b.hi.z - b.lo.z);
            const double depth = zPoint * u(rng); // any depth below the point
            const auto* loops = sliceAt(s, depth);
            REQUIRE(loops != nullptr);
            // A point within 1e-9 of a loop edge counts as on it: probe a tiny cross around it.
            bool in = inside(*loops, x, y);
            for (const auto& [dx, dy] : {std::pair{1e-7, 0.0}, {-1e-7, 0.0}, {0.0, 1e-7}, {0.0, -1e-7}}) {
                in = in || inside(*loops, x + dx, y + dy);
            }
            if (!in) {
                FAIL_CHECK("material at (" << x << ", " << y << ", " << zPoint << ") is not in the slice read at "
                                           << depth);
            }
        }
        checkReadable(s);
        // The same bits again.
        CHECK(hashOf(sliced(soup, box, cell, pad)) == hashOf(s));
    }
}

TEST_CASE("more distinct tops than kCarriedExactLevels are listed rounded down to whole cells", "[cam][carried]")
{
    // 100 columns 0.5 wide, each 0.013 higher than the one before (a ramp).
    std::vector<Box> boxes;
    for (int i = 0; i < 100; ++i) {
        boxes.push_back(Box{{0.5 * i, 0, 0}, {0.5 * (i + 1), 4, 1.0 + 0.013 * i}});
    }
    const Box3 box{{0, 0, 0}, {50, 4, 1.0 + 0.013 * 99}};
    const double cell = 0.25;
    const cd::StockSlices s = sliced(soupOf(boxes), box, cell);
    // Levels: the bottom, then each whole cell some top reaches (tops 1.0 to 2.287): 1, 1.25, ... 2.25.
    REQUIRE(s.size() == 7);
    for (std::size_t i = 1; i < s.size(); ++i) {
        CHECK(s[i].first == 1.0 + 0.25 * static_cast<double>(i - 1));
    }
    // Above 1.25 lie the columns from 1.25 / 0.013 on (i >= 20: x >= 10), grown by a cell: from x = 9.75.
    REQUIRE(s[2].second.size() == 1);
    CHECK(s[2].second[0].curves[0].pts[0].x == 9.75);
    checkReadable(s);
}

TEST_CASE("cells that meet only at a corner become one loop, so no two loops touch", "[cam][carried]")
{
    // Two blocks touching at the corner (5, 2), far enough apart that the growth leaves the corner pattern.
    const kernel::TriSoup soup = soupOf({{{0, 0, 0}, {5, 2, 1}}, {{5.5, 2.5, 0}, {10, 4, 1}}});
    const Box3 box{{0, 0, 0}, {10, 4, 1}};
    for (const double cell : {0.25, 0.5}) {
        const cd::StockSlices s = sliced(soup, box, cell);
        checkReadable(s);
        // Every vertex is a corner of exactly one loop.
        std::vector<std::pair<double, double>> all;
        for (const kernel::ProfileLoop& l : s[0].second) {
            for (const kernel::ProfileCurve& c : l.curves) {
                all.emplace_back(c.pts[0].x, c.pts[0].y);
            }
        }
        std::sort(all.begin(), all.end());
        CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    }
}

TEST_CASE("the carried box refuses what is not a stock", "[cam][carried]")
{
    JobContext ctx;
    CarriedStock none;
    CHECK(cd::orientCarried(none, kFlip, ctx).error().code == ErrorCode::InvalidArgument);
    const CarriedStock empty = carriedOf(kernel::TriSoup{}, Box3{{0, 0, 0}, {1, 1, 1}}, Xform::identity());
    CHECK(cd::orientCarried(empty, kFlip, ctx).error().code == ErrorCode::InvalidArgument);
    kernel::TriSoup bad = soupOf({{{0, 0, 0}, {1, 1, 1}}});
    bad.indices.push_back(99);
    bad.indices.push_back(0);
    bad.indices.push_back(1);
    const CarriedStock broken = carriedOf(bad, Box3{{0, 0, 0}, {1, 1, 1}}, Xform::identity());
    CHECK(cd::orientCarried(broken, kFlip, ctx).error().code == ErrorCode::InvalidArgument);
    CHECK(cd::carriedSlices(soupOf({{{0, 0, 0}, {1, 1, 1}}}), Box3{{0, 0, 0}, {0, 1, 1}}, 0.25, 0, ctx).error().code ==
          ErrorCode::InvalidArgument);
}
