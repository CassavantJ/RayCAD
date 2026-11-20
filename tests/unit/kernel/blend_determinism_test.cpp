// A fillet or chamfer gives the same result every time it runs on the same input in one process, whatever the heap
// and the stack look like (PLAN.md section 8). OCCT 8.0.1 read an uninitialised flag in ChFi3d (IntersectMoreCorner,
// fixed by ports/opencascade/0008-chfi3d-initialise-prol.patch): when the stack slot was not 0, the corner of a blend
// ending at a vertex on a cylindrical wall was built on the unbounded wall and the seam lost one of its pcurves, so in
// about 1 of 70 runs these blends came out as an invalid solid (and then as "could not be built"). This test repeats a
// few blends many times with live heap blocks of random sizes allocated between the runs, so that each run sees other
// addresses and other leftovers, and checks that every run gives the expected kind of result and the bytes of the
// first one.
//
// The default count runs in a few seconds. RAYCAD_BLEND_REPEATS=<n> sets another count; the hidden test case
// [.blendrepeat] prints how many runs of each case differ instead of stopping at the first one.
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <shape_testing.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

// The outcome of a blend: the content hash of the result, or the error with its context lines.
std::string outcome(const Result<OpResult>& r)
{
    if (r)
        return "ok " + r->shape.contentHash().hex();
    std::string out = "error " + r.error().message;
    for (const std::string& line : r.error().context)
        out += " | " + line;
    return out;
}

Plane planeOnFace(const Shape& shape, const SubId& face)
{
    const SurfaceInfo s = take(surfaceInfo(shape, face));
    const Vec3 n = s.reversed ? Vec3{-s.axis.x, -s.axis.y, -s.axis.z} : s.axis;
    const Vec3 e = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 x = cross(e, n);
    const double len = length(x);
    x = Vec3{x.x / len, x.y / len, x.z / len};
    return Plane{s.centroid, n, x};
}

// The planar faces of `shape` by decreasing area (ties by SubId).
std::vector<std::pair<SubId, double>> planarFacesBySize(const Shape& shape)
{
    std::vector<std::pair<SubId, double>> out;
    for (std::uint32_t i = 0; i < shape.count(SubKind::Face); ++i) {
        const SubId id{SubKind::Face, i};
        auto s = surfaceInfo(shape, id);
        if (s && s->type == SurfaceType::Plane && s->area > 1.0)
            out.emplace_back(id, s->area);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

// The body of the "cut body" chain of immutability_test.cpp at its blend steps: the cut body with a round pocket and
// a rectangular boss. `lines` and `circles` are its blend candidates as picked there.
struct Body {
    Shape shape;
    double size = 0.0;
    std::vector<SubId> lines;
    std::vector<SubId> circles;
};
Body cutBodyWithPocketAndBoss()
{
    JobContext ctx;
    const Shape base = take(testing::makeCutBody());
    const Box3 box = bounds(base);
    Body out;
    out.size = length(sub(box.max, box.min));
    const auto faces = planarFacesBySize(base);
    REQUIRE(faces.size() > 1);
    const double r1 = std::clamp(0.15 * std::sqrt(faces[0].second), 0.5, 12.0);
    const Shape pocket = take(extrude(profile(planeOnFace(base, faces[0].first), {circleLoop({0, 0}, r1, 1, 100)}),
                                      -0.2 * out.size, 0.0, ctx))
                             .op.shape;
    Shape current = take(boolean(BoolOp::Cut, base, pocket, ctx)).shape;
    const double w = std::clamp(0.2 * std::sqrt(faces[1].second), 0.5, 15.0);
    const Shape boss =
        take(extrude(profile(planeOnFace(base, faces[1].first), {rectangle(-w, -w / 2, w, w / 2, 1, 100)}),
                     0.1 * out.size, 0.0, ctx))
            .op.shape;
    out.shape = take(boolean(BoolOp::Union, current, boss, ctx)).shape;
    for (std::uint32_t i = 0; i < out.shape.count(SubKind::Edge); ++i) {
        const SubId id{SubKind::Edge, i};
        const std::vector<SubId> adj = adjacent(out.shape, id, SubKind::Face);
        if (adj.size() != 2)
            continue;
        auto info = curveInfo(out.shape, id);
        auto a = surfaceInfo(out.shape, adj[0]);
        auto b = surfaceInfo(out.shape, adj[1]);
        if (!info || !a || !b)
            continue;
        if (info->type == CurveType::Line && a->type == SurfaceType::Plane && b->type == SurfaceType::Plane &&
            std::abs(std::abs(dot(a->axis, b->axis)) - 1.0) > 1e-6 && info->length > 2.0)
            out.lines.push_back(id);
        const bool planeCyl = (a->type == SurfaceType::Plane && b->type == SurfaceType::Cylinder) ||
                              (a->type == SurfaceType::Cylinder && b->type == SurfaceType::Plane);
        if (info->type == CurveType::Circle && planeCyl && info->radius > 0.5)
            out.circles.push_back(id);
    }
    REQUIRE(out.lines.size() > 4);
    REQUIRE(!out.circles.empty());
    return out;
}

struct Case {
    std::string name;
    std::function<Result<OpResult>()> run;
    std::string expect; // "ok " for a result, else a part of the error message
};

bool expected(const Case& c, const std::string& got)
{
    return c.expect == "ok " ? got.rfind("ok ", 0) == 0 : got.find(c.expect) != std::string::npos;
}

std::vector<Case> cases(const Body& body)
{
    const std::vector<SubId> line{body.lines[body.lines.size() / 2]};
    const std::vector<SubId> circle{body.circles.front()};
    const std::vector<SubId> four(body.lines.begin(), body.lines.begin() + 4);
    const Shape& s = body.shape;
    const double large = 0.15 * body.size;
    auto blend = [&s](bool isFillet, std::vector<SubId> edges, double value) {
        return [&s, isFillet, edges, value]() {
            JobContext ctx;
            return isFillet ? fillet(s, edges, value, ctx) : chamfer(s, edges, value, ctx);
        };
    };
    return {
        // Step "fillet 1" of the cut body chain in immutability_test.cpp: too large, so the size search runs.
        {"fillet 1 of the cut body chain (" + std::to_string(large) + ")", blend(true, line, large),
         "is too large for"},
        // The build of that size search that gave an invalid solid now and then.
        {"fillet of the same edge at a tenth of that", blend(true, line, 0.1 * large), "ok "},
        {"fillet 0.4 of a straight edge", blend(true, line, 0.4), "ok "},
        {"chamfer 0.4 of a straight edge", blend(false, line, 0.4), "ok "},
        {"fillet 0.4 of a circular edge", blend(true, circle, 0.4), "ok "},
        {"chamfer 0.4 of a circular edge", blend(false, circle, 0.4), "ok "},
        {"fillet 0.4 of four straight edges", blend(true, four, 0.4), "ok "},
        {"chamfer 0.4 of four straight edges", blend(false, four, 0.4), "ok "},
    };
}

int repeats(int fallback)
{
    if (const char* text = std::getenv("RAYCAD_BLEND_REPEATS"))
        if (const int n = std::atoi(text); n > 0)
            return n;
    return fallback;
}

// Heap blocks of random sizes that stay alive until the end of the test, a random number of them before each run,
// so that the objects OCCT allocates in a run land at other addresses than in the run before. Deterministic (a fixed
// LCG), so a failure repeats.
class HeapShuffle {
public:
    void next()
    {
        const std::size_t n = 1 + random() % 64;
        for (std::size_t k = 0; k < n; ++k) {
            auto block = std::make_unique<char[]>(8 + random() % 1024);
            block[0] = 1;
            if (random() % 4 != 0)
                keep_.push_back(std::move(block)); // the others are freed again, leaving holes
        }
    }

private:
    std::size_t random()
    {
        seed_ = seed_ * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::size_t>(seed_ >> 33);
    }
    std::uint64_t seed_ = 1;
    std::vector<std::unique_ptr<char[]>> keep_;
};

} // namespace

TEST_CASE("a fillet or chamfer repeated in one process gives the same result whatever the heap looks like",
          "[kernel][blend][determinism]")
{
    const Body body = cutBodyWithPocketAndBoss();
    const std::vector<Case> all = cases(body);
    const int runs = repeats(50);
    HeapShuffle heap;
    std::vector<std::string> first;
    for (const Case& c : all) {
        first.push_back(outcome(c.run()));
        INFO(c.name);
        CHECK(expected(c, first.back()));
    }
    for (int i = 0; i < runs; ++i) {
        for (std::size_t c = 0; c < all.size(); ++c) {
            heap.next();
            const std::string now = outcome(all[c].run());
            INFO(all[c].name << ", run " << i + 2);
            REQUIRE(now == first[c]);
        }
    }
}

// Counts instead of stopping: RAYCAD_BLEND_REPEATS=2000 ray_kernel_tests "[.blendrepeat]"
TEST_CASE("blend repetition count", "[.blendrepeat]")
{
    const Body body = cutBodyWithPocketAndBoss();
    const std::vector<Case> all = cases(body);
    const int runs = repeats(500);
    HeapShuffle heap;
    bool same = true;
    for (const Case& c : all) {
        std::map<std::string, int> seen;
        std::string first;
        for (int i = 0; i < runs; ++i) {
            heap.next();
            const std::string now = outcome(c.run());
            if (i == 0)
                first = now;
            ++seen[now];
        }
        std::printf("%s: %d runs, %d differ from the first, %zu distinct outcomes\n", c.name.c_str(), runs,
                    runs - seen[first], seen.size());
        for (const auto& [what, count] : seen)
            std::printf("    %5d x %s\n", count, what.substr(0, 160).c_str());
        same = same && seen.size() == 1 && expected(c, first);
    }
    std::fflush(stdout);
    CHECK(same);
}
