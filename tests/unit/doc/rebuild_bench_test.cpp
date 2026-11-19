// A 100-feature synthetic part: incremental rebuild times (PLAN.md section 9: "Incremental rebuild, 100-feature
// part, edit feature 50": Low 1.0 s, Medium 0.7 s, High 0.5 s), and the geometry goldens of small reference parts
// (task T-096).
//
// The part: a 300 x 200 x 20 plate whose width is the parameter plate_w (feature 1, sketch; 2, extrude), a datum plane
// on its top (3), 24 pockets each made of a sketch, a cut and a fillet on one of its corners (4 to 75), 12 holes
// (sketch and cut, 76 to 99) and a chamfer on the plate's front top edge (100).
#include "golden_geometry.h"
#include "parametric_test_util.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <vector>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

namespace {

struct Part {
    explicit Part(int workers = 4) : env(workers) {}
    DocStatePtr state = std::make_shared<const DocState>();
    RebuildEnv env;

    void run(const Command& c)
    {
        const ICommandHandler* handler = builtinCommandRegistry()->find(c.name);
        REQUIRE(handler != nullptr);
        auto next = handler->apply(state, c.argsJson);
        if (!next) {
            FAIL(c.name << " failed: " << next.error().message);
        }
        state = *next;
    }
    BuildResultPtr build() { return env.rebuild(state); }
};

constexpr int kPockets = 24;
constexpr int kHoles = 12;

double pocketX(int i)
{
    return 15 + (i % 6) * 47;
}
double pocketY(int i)
{
    return 15 + (i / 6) * 45;
}

// Feature number n of the part (uuid('f', n)).
std::uint64_t pocketSketch(int i)
{
    return 4 + 3 * static_cast<std::uint64_t>(i);
}
std::uint64_t holeSketch(int j)
{
    return 4 + 3 * kPockets + 2 * static_cast<std::uint64_t>(j);
}

std::string topDatum()
{
    return R"({"datum":")" + uuid('f', 3) + R"("})";
}

void makePart(Part& p)
{
    p.run(
        cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"plate_w","expression":"300 mm","kind":"length"})"));
    SketchJson plate;
    plate.rect(1, 0, 0, 300, 200)
        .constraint(1, "fix", {1})
        .constraint(2, "horizontal", {5})
        .constraint(3, "horizontal", {7})
        .constraint(4, "vertical", {6})
        .constraint(5, "vertical", {8})
        .constraint(6, "distanceX", {1, 2}, "plate_w")
        .constraint(7, "distanceY", {1, 4}, "200 mm");
    p.run(addFeature("sketch", 1, sketchProps(R"({"principal":"xy"})", plate)));
    p.run(addFeature("extrude", 2, extrudeProps(1, "20", "new", 'b')));
    p.run(addFeature("datum_plane", 3, R"({"base":{"principal":"xy"},"offset":"20 mm"})"));
    for (int i = 0; i < kPockets; ++i) {
        const std::uint64_t s = pocketSketch(i);
        const double x0 = pocketX(i), y0 = pocketY(i);
        p.run(addFeature("sketch", s, sketchProps(topDatum(), SketchJson().rect(1, x0, y0, x0 + 30, y0 + 25))));
        p.run(addFeature("extrude", s + 1, extrudeProps(s, "\"6 mm\"", "cut", 'b', R"(,"direction":"reverse")")));
        const BuildResultPtr b = p.build();
        REQUIRE(report(*b, s + 1).status == FeatureStatus::Ok);
        const auto edge = findEdge(bodyOf(*b, 'b').shape, [&](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && near(c.start.x, x0) && near(c.end.x, x0) &&
                   near(c.start.y, y0) && near(c.end.y, y0);
        });
        REQUIRE(edge);
        p.run(addFeature("fillet", s + 2, R"({"edges":[)" + refJson(refTo(*b, 'b', *edge)) + R"(],"radius":3})"));
    }
    for (int j = 0; j < kHoles; ++j) {
        const std::uint64_t s = holeSketch(j);
        p.run(addFeature("sketch", s, sketchProps(topDatum(), SketchJson().circleAt(1, 22 + j * 23, 188, 4))));
        p.run(addFeature("extrude", s + 1, extrudeProps(s, "20", "cut", 'b', R"(,"direction":"reverse")")));
    }
    const BuildResultPtr b = p.build();
    const auto front = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.y, 0) && near(c.end.y, 0) && near(c.start.z, 20) &&
               near(c.end.z, 20);
    });
    REQUIRE(front);
    p.run(addFeature("chamfer", 100, R"({"edges":[)" + refJson(refTo(*b, 'b', *front)) + R"(],"distance":1})"));
    REQUIRE(p.state->features.size() == 100);
}

struct Timed {
    double seconds;
    int rebuilt;
};

Timed timedBuild(Part& p)
{
    const auto start = std::chrono::steady_clock::now();
    const BuildResultPtr b = p.build();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    int rebuilt = 0;
    std::map<std::string, std::pair<int, double>> byType;
    for (std::size_t i = 0; i < b->reports.size(); ++i) {
        const FeatureReport& r = b->reports[i];
        CHECK((r.status == FeatureStatus::Ok || r.status == FeatureStatus::Warning));
        rebuilt += r.cacheHit ? 0 : 1;
        if (!r.cacheHit) {
            auto& t = byType[p.state->features[i].type];
            ++t.first;
            t.second += r.seconds;
        }
    }
    if (std::getenv("RAYCAD_BENCH_DETAIL") != nullptr) {
        for (const auto& [type, t] : byType) {
            std::printf("    %s: %d built, %.3f s\n", type.c_str(), t.first, t.second);
        }
    }
    return Timed{s, rebuilt};
}

} // namespace

TEST_CASE("a 100-feature part rebuilds incrementally after an early, a middle and a late edit", "[rebuild-bench]")
{
    Part p;
    const auto start = std::chrono::steady_clock::now();
    makePart(p);
    const double authoring = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // A cold build on a fresh rebuilder (nothing cached).
    Part cold;
    cold.state = p.state;
    const Timed full = timedBuild(cold);
    CHECK(full.rebuilt == 100);
    const Timed noop = timedBuild(cold);
    CHECK(noop.rebuilt == 0);

    // Each run sets a value never used before, so every rebuild is a real (cache-missing) incremental rebuild.
    struct Edit {
        const char* name;
        std::function<Command(int run)> make;
        int expectRebuilt;
    };
    const std::uint64_t mid = pocketSketch(15) + 1; // feature 50: the cut of pocket 15
    REQUIRE(mid == 50);
    const std::uint64_t lastHole = holeSketch(kHoles - 1);
    std::vector<Edit> edits = {
        {"early: plate width (parameter plate_w, a dimension of sketch 1)",
         [](int run) {
             return cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":")" + num(301 + run) + R"( mm"})");
         },
         63},
        {"middle: depth of the cut that is feature 50",
         [mid](int run) { return editFeature("extrude", mid, R"({"distance":)" + num(6.1 + 0.1 * run) + "}"); }, 31},
        {"late: radius of the last hole (sketch 98)",
         [lastHole](int run) {
             return cmd("sketch.set",
                        R"({"id":")" + uuid('f', lastHole) + R"(","sketch":)" +
                            SketchJson().circleAt(1, 22 + (kHoles - 1) * 23, 188, 4.1 + 0.1 * run).json() + "}");
         },
         3},
    };
    std::printf("\n100-feature part: authored in %.2f s (with %d incremental builds); cold full build %.3f s; no-op "
                "rebuild %.4f s\n",
                authoring, kPockets + 1, full.seconds, noop.seconds);
    std::printf("| edit | features rebuilt | rebuild s (runs) | median s | PLAN 9 budget Low / Medium / High |\n"
                "|-|-|-|-|-|\n");
    for (const Edit& e : edits) {
        std::vector<double> times;
        int rebuilt = 0;
        for (int run = 0; run < 5; ++run) {
            cold.run(e.make(run));
            const Timed t = timedBuild(cold);
            times.push_back(t.seconds);
            rebuilt = t.rebuilt;
            CHECK(t.rebuilt == e.expectRebuilt);
        }
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        std::string runs;
        for (const double t : times) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%s%.3f", runs.empty() ? "" : " ", t);
            runs += buf;
        }
        std::printf("| %s | %d | %s | %.3f | 1.0 / 0.7 / 0.5 |\n", e.name, rebuilt, runs.c_str(),
                    sorted[sorted.size() / 2]);
    }
}

TEST_CASE("geometry goldens: small reference parts", "[golden-geometry]")
{
    SECTION("block with a fillet and a chamfer")
    {
        Part p;
        p.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xy"})", SketchJson().rect(1, 0, 0, 40, 20))));
        p.run(addFeature("extrude", 3, extrudeProps(2, "10", "new", 'b')));
        const BuildResultPtr b = p.build();
        const kernel::Shape& shape = bodyOf(*b, 'b').shape;
        const auto vertical = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && near(c.start.x, 40) && near(c.end.x, 40) &&
                   near(c.start.y, 20) && near(c.end.y, 20);
        });
        const auto top = findEdge(shape, [](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && near(c.start.y, 0) && near(c.end.y, 0) && near(c.start.z, 10) &&
                   near(c.end.z, 10);
        });
        REQUIRE(vertical);
        REQUIRE(top);
        p.run(addFeature("fillet", 4, R"({"edges":[)" + refJson(refTo(*b, 'b', *vertical)) + R"(],"radius":3})"));
        p.run(addFeature("chamfer", 5, R"({"edges":[)" + refJson(refTo(*b, 'b', *top)) + R"(],"distance":2})"));
        const BuildResultPtr done = p.build();
        checkGoldenGeometry("block_fillet_chamfer", bodyOf(*done, 'b').shape, bodyOf(*done, 'b').map);
    }
    SECTION("revolved tube with a cut")
    {
        Part p;
        SketchJson s;
        s.rect(1, 10, 0, 15, 20).point(20, 0, 0).point(21, 0, 10).line(22, 20, 21, true);
        p.run(addFeature("sketch", 2, sketchProps(R"({"principal":"xz"})", s)));
        p.run(addFeature("revolve", 3,
                         R"({"sketch":")" + uuid('f', 2) + R"(","axis":{"entity":")" + ent(22) +
                             R"("},"operation":"new","target":")" + uuid('b', 1) + R"("})"));
        p.run(addFeature("sketch", 4, sketchProps(R"({"principal":"xy"})", SketchJson().rect(1, -20, -2, 20, 2))));
        p.run(addFeature("extrude", 5, extrudeProps(4, "5", "cut", 'b')));
        const BuildResultPtr done = p.build();
        REQUIRE(report(*done, 5).status == FeatureStatus::Ok);
        checkGoldenGeometry("revolve_tube_slot", bodyOf(*done, 'b').shape, bodyOf(*done, 'b').map);
    }
    SECTION("the 100-feature synthetic part")
    {
        Part p;
        makePart(p);
        const BuildResultPtr done = p.build();
        checkGoldenGeometry("synthetic_100", bodyOf(*done, 'b').shape, bodyOf(*done, 'b').map);
    }
}
