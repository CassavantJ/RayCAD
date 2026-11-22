// The drilling order (task T-034): orderHolesShortestPath is a permutation, deterministic (ties to the lower index,
// whole nanometres), never longer than the nearest-neighbour path from the same start (computed here independently)
// and so never worse than 1.25 times it, on random point sets, grids, clusters and the hole sets of the labelled corpus
// parts; and fast for thousands of holes.
#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam;

namespace {

// splitmix64: the same numbers on every platform (the standard distributions are not).
struct Random {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    double uniform(double lo, double hi) { return lo + (hi - lo) * static_cast<double>(next() >> 11) * 0x1p-53; }
};

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

double pathLength(std::span<const Vec2> xy, Vec2 start, const std::vector<std::size_t>& order)
{
    double len = 0;
    Vec2 at = start;
    for (const std::size_t i : order) {
        len += dist(at, xy[i]);
        at = xy[i];
    }
    return len;
}

// Nearest neighbour from `start`, ties to the lower index: the reference the order is measured against.
std::vector<std::size_t> nearestNeighbour(std::span<const Vec2> xy, Vec2 start)
{
    std::vector<std::size_t> order;
    std::vector<bool> used(xy.size(), false);
    Vec2 at = start;
    for (std::size_t step = 0; step < xy.size(); ++step) {
        std::size_t best = xy.size();
        for (std::size_t i = 0; i < xy.size(); ++i) {
            if (!used[i] && (best == xy.size() || dist(at, xy[i]) < dist(at, xy[best]))) {
                best = i;
            }
        }
        used[best] = true;
        order.push_back(best);
        at = xy[best];
    }
    return order;
}

bool isPermutation(std::vector<std::size_t> order, std::size_t n)
{
    std::sort(order.begin(), order.end());
    std::vector<std::size_t> all(n);
    std::iota(all.begin(), all.end(), std::size_t{0});
    return order == all;
}

// Checks one point set and returns the order's length over the nearest-neighbour length.
double checkSet(const std::vector<Vec2>& xy, Vec2 start)
{
    const std::vector<std::size_t> order = orderHolesShortestPath(xy, start);
    REQUIRE(isPermutation(order, xy.size()));
    CHECK(order == orderHolesShortestPath(xy, start)); // deterministic
    const double nn = pathLength(xy, start, nearestNeighbour(xy, start));
    const double len = pathLength(xy, start, order);
    CHECK(len <= nn + 1e-6);
    CHECK(len <= 1.25 * nn + 1e-6);
    return nn > 0 ? len / nn : 1.0;
}

std::vector<Vec2> randomSet(Random& r, std::size_t n, double size)
{
    std::vector<Vec2> xy(n);
    for (Vec2& p : xy) {
        p = Vec2{r.uniform(0, size), r.uniform(0, size)};
    }
    return xy;
}

} // namespace

TEST_CASE("small cases and the tie rule", "[cam][holes][order]")
{
    CHECK(orderHolesShortestPath({}, Vec2{0, 0}).empty());
    const std::vector<Vec2> one{{5, 5}};
    CHECK(orderHolesShortestPath(one, Vec2{0, 0}) == std::vector<std::size_t>{0});
    const std::vector<Vec2> two{{10, 0}, {1, 0}};
    CHECK(orderHolesShortestPath(two, Vec2{0, 0}) == std::vector<std::size_t>{1, 0});
    // Equal distances go to the lower index: two holes at one place, and four around the start.
    const std::vector<Vec2> twins{{3, 0}, {3, 0}, {1, 0}};
    CHECK(orderHolesShortestPath(twins, Vec2{0, 0}) == std::vector<std::size_t>{2, 0, 1});
    const std::vector<Vec2> square{{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    CHECK(orderHolesShortestPath(square, Vec2{0, 0}) == std::vector<std::size_t>{0, 1, 2, 3});
    // A path that nearest neighbour gets wrong: 2-opt finds the shorter one.
    const std::vector<Vec2> zig{{0, 5}, {5, 0}, {-5, 0}};
    const auto order = orderHolesShortestPath(zig, Vec2{0, 0});
    CHECK(pathLength(zig, Vec2{0, 0}, order) < pathLength(zig, Vec2{0, 0}, nearestNeighbour(zig, Vec2{0, 0})) - 1);
    // Whole nanometres: points less than half a nanometre apart order the same.
    std::vector<Vec2> a{{10, 3}, {2, 7}, {5, 5}, {8, 1}};
    std::vector<Vec2> b = a;
    for (Vec2& p : b) {
        p.x += 2e-7;
        p.y -= 1e-7;
    }
    CHECK(orderHolesShortestPath(a, Vec2{0, 0}) == orderHolesShortestPath(b, Vec2{0, 0}));
}

TEST_CASE("never worse than nearest neighbour on random sets, grids and clusters", "[cam][holes][order]")
{
    Random r{0x5eed};
    double worst = 0, sum = 0;
    int sets = 0;
    for (const std::size_t n : {3u, 4u, 7u, 12u, 30u, 100u, 400u}) {
        for (int seed = 0; seed < 12; ++seed) {
            INFO(n << " points, set " << seed);
            const std::vector<Vec2> xy = randomSet(r, n, 200);
            const double ratio = checkSet(xy, Vec2{r.uniform(-20, 220), r.uniform(-20, 220)});
            worst = std::max(worst, ratio);
            sum += ratio;
            ++sets;
        }
    }
    // A grid of holes (many equal distances) and clusters of holes far apart.
    std::vector<Vec2> grid;
    for (int i = 0; i < 17; ++i) {
        for (int j = 0; j < 11; ++j) {
            grid.push_back(Vec2{12.5 * i, 9.0 * j});
        }
    }
    sum += checkSet(grid, Vec2{-10, -10});
    std::vector<Vec2> clusters;
    for (int c = 0; c < 6; ++c) {
        const Vec2 centre{r.uniform(0, 1000), r.uniform(0, 1000)};
        for (int k = 0; k < 25; ++k) {
            clusters.push_back(Vec2{centre.x + r.uniform(-8, 8), centre.y + r.uniform(-8, 8)});
        }
    }
    sum += checkSet(clusters, Vec2{0, 0});
    sets += 2;
    std::cout << "drilling order over nearest neighbour: mean " << sum / sets << ", worst " << worst << " (" << sets
              << " sets)\n";
}

TEST_CASE("never worse than nearest neighbour on the labelled corpus hole sets", "[cam][holes][order]")
{
    // The holes of each labelled part (holes_labels.txt, model frame), from the corner of the part's box.
    std::ifstream in(RAY_HOLES_LABELS);
    REQUIRE(in.good());
    std::vector<std::vector<Vec2>> sets;
    std::string line;
    bool topFrame = false;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string word;
        s >> word;
        if (word == "part") {
            std::string id, frame;
            s >> id >> frame;
            topFrame = frame.rfind("tilt", 0) != 0; // every frame but the tilted copies of z+
            sets.emplace_back();
        } else if (word == "hole" && topFrame) {
            Vec2 p;
            s >> p.x >> p.y;
            sets.back().push_back(p);
        }
    }
    int checked = 0;
    for (const std::vector<Vec2>& xy : sets) {
        if (xy.size() >= 2) {
            Vec2 corner = xy[0];
            for (const Vec2& p : xy) {
                corner = Vec2{std::min(corner.x, p.x), std::min(corner.y, p.y)};
            }
            checkSet(xy, Vec2{corner.x - 1, corner.y - 1});
            ++checked;
        }
    }
    CHECK(checked >= 20);
}

TEST_CASE("ordering thousands of holes is fast", "[cam][holes][order][wallclock]")
{
    Random r{42};
    for (const std::size_t n : {2000u, 10000u}) {
        const std::vector<Vec2> xy = randomSet(r, n, 1000);
        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::size_t> order = orderHolesShortestPath(xy, Vec2{0, 0});
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REQUIRE(isPermutation(order, n));
        const double nn = pathLength(xy, Vec2{0, 0}, nearestNeighbour(xy, Vec2{0, 0}));
        const double len = pathLength(xy, Vec2{0, 0}, order);
        std::cout << "orderHolesShortestPath: " << n << " holes in " << seconds << " s, " << len / nn
                  << " of nearest neighbour\n";
        CHECK(len <= nn + 1e-6);
        CHECK(seconds < (n <= 2000 ? 1.0 : 5.0));
    }
}
