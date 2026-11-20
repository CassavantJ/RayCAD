// Which features a rebuild runs again after an edit (pre-M2 performance work), on a realistic part: a plate (sketch,
// extrude) whose width is a parameter, a datum plane on its top, four pockets (sketch on the datum, cut, fillet on a
// pocket edge), two hole features at sketch points and a chamfer: 20 features on one body.
//   - a late edit (the second hole's diameter) rebuilds that hole and what follows it on the body (the chamfer) only
//   - an early edit (the plate width) rebuilds the plate and everything that modifies the body after it, but none of
//     the sketches on the datum plane, the datum plane or the hole point sketches
//   - a rebuild of an unchanged state rebuilds nothing; every feature has exactly one report per build
//   - committing a command that was previewed rebuilds nothing more: the preview's rebuild did the work
#include "parametric_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::doc;
using namespace ray::doc::test;

namespace {

constexpr int kPockets = 4;

std::uint64_t pocketSketch(int i)
{
    return 10 + 3 * static_cast<std::uint64_t>(i);
}
constexpr std::uint64_t kHolePoints1 = 30, kHole1 = 31, kHolePoints2 = 32, kHole2 = 33, kChamfer = 34;

std::string holeProps(std::uint64_t sketch, double diameter)
{
    return R"({"sketch":")" + uuid('f', sketch) + R"(","target":")" + uuid('b', 1) +
           R"(","kind":"simple","diameter":)" + num(diameter) + R"(,"end":"through"})";
}

// Applies commands to a state and builds it with one rebuilder (its cache persists between builds).
struct Part {
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

// The features a build ran (not cache hits), as feature numbers; checks one report per feature, all of them good.
std::set<std::uint64_t> rebuilt(const DocState& state, const BuildResult& build)
{
    REQUIRE(build.reports.size() == state.features.size());
    std::set<std::uint64_t> out;
    for (std::size_t i = 0; i < build.reports.size(); ++i) {
        const FeatureReport& r = build.reports[i];
        CHECK(r.id == state.features[i].id);
        INFO(state.features[i].name << ": " << r.message);
        CHECK((r.status == FeatureStatus::Ok || r.status == FeatureStatus::Warning));
        if (!r.cacheHit) {
            for (std::uint64_t n = 1; n < 100; ++n) {
                if (featureId(n) == r.id) {
                    out.insert(n);
                }
            }
        }
    }
    return out;
}

void makePart(Part& p)
{
    p.run(
        cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","name":"plate_w","expression":"120 mm","kind":"length"})"));
    SketchJson plate;
    plate.rect(1, 0, 0, 120, 80)
        .constraint(1, "fix", {1})
        .constraint(2, "horizontal", {5})
        .constraint(3, "horizontal", {7})
        .constraint(4, "vertical", {6})
        .constraint(5, "vertical", {8})
        .constraint(6, "distanceX", {1, 2}, "plate_w")
        .constraint(7, "distanceY", {1, 4}, "80 mm");
    p.run(addFeature("sketch", 1, sketchProps(R"({"principal":"xy"})", plate)));
    p.run(addFeature("extrude", 2, extrudeProps(1, "15", "new", 'b')));
    p.run(addFeature("datum_plane", 3, R"({"base":{"principal":"xy"},"offset":"15 mm"})"));
    const std::string top = R"({"datum":")" + uuid('f', 3) + R"("})";
    for (int i = 0; i < kPockets; ++i) {
        const std::uint64_t s = pocketSketch(i);
        const double x0 = 10 + 25 * i, y0 = 10;
        p.run(addFeature("sketch", s, sketchProps(top, SketchJson().rect(1, x0, y0, x0 + 18, y0 + 20))));
        p.run(addFeature("extrude", s + 1, extrudeProps(s, "\"5 mm\"", "cut", 'b', R"(,"direction":"reverse")")));
        const BuildResultPtr b = p.build();
        REQUIRE(report(*b, s + 1).status == FeatureStatus::Ok);
        const auto edge = findEdge(bodyOf(*b, 'b').shape, [&](const kernel::CurveInfo& c) {
            return c.type == kernel::CurveType::Line && near(c.start.x, x0) && near(c.end.x, x0) &&
                   near(c.start.y, y0) && near(c.end.y, y0);
        });
        REQUIRE(edge);
        p.run(addFeature("fillet", s + 2, R"({"edges":[)" + refJson(refTo(*b, 'b', *edge)) + R"(],"radius":2})"));
    }
    p.run(addFeature("sketch", kHolePoints1, sketchProps(top, SketchJson().point(1, 20, 60).point(2, 40, 60))));
    p.run(addFeature("hole", kHole1, holeProps(kHolePoints1, 5)));
    p.run(addFeature("sketch", kHolePoints2, sketchProps(top, SketchJson().point(1, 70, 60).point(2, 90, 60))));
    p.run(addFeature("hole", kHole2, holeProps(kHolePoints2, 6)));
    const BuildResultPtr b = p.build();
    const auto front = findEdge(bodyOf(*b, 'b').shape, [](const kernel::CurveInfo& c) {
        return c.type == kernel::CurveType::Line && near(c.start.y, 0) && near(c.end.y, 0) && near(c.start.z, 15) &&
               near(c.end.z, 15);
    });
    REQUIRE(front);
    p.run(addFeature("chamfer", kChamfer, R"({"edges":[)" + refJson(refTo(*b, 'b', *front)) + R"(],"distance":1})"));
    REQUIRE(p.state->features.size() == 20);
}

} // namespace

TEST_CASE("an edit rebuilds exactly the features downstream of it on the same body", "[doc][rebuild][reuse]")
{
    Part p;
    makePart(p);
    const BuildResultPtr full = p.build();
    REQUIRE(rebuilt(*p.state, *full).size() <= 20);
    const DocStatePtr original = p.state;
    // Nothing changed: nothing is built again.
    CHECK(rebuilt(*p.state, *p.build()).empty());

    // Late: the second hole's diameter. The hole and the chamfer after it run; nothing before the hole does.
    p.run(editFeature("hole", kHole2, R"({"diameter":7})"));
    CHECK(rebuilt(*p.state, *p.build()) == std::set<std::uint64_t>{kHole2, kChamfer});

    // Early: the plate width. The plate sketch and extrude run, then every feature that modifies the body after
    // it (cuts, fillets, holes, chamfer); the datum plane and the sketches on it (pockets, hole points) do not.
    p.run(cmd("param.set", R"({"id":")" + uuid('a', 1) + R"(","expression":"125 mm"})"));
    std::set<std::uint64_t> early{1, 2, kHole1, kHole2, kChamfer};
    for (int i = 0; i < kPockets; ++i) {
        early.insert(pocketSketch(i) + 1);
        early.insert(pocketSketch(i) + 2);
    }
    CHECK(rebuilt(*p.state, *p.build()) == early);

    // Undo: the state from before the edits (an undo step restores that snapshot) is all in the cache. (Typing the
    // old width again is not: the plate sketch is solved again and stored with coordinates a few ulps away from the
    // first ones, so its props differ and it, with everything after it on the body, runs again.)
    p.state = original;
    const std::set<std::uint64_t> back = rebuilt(*p.state, *p.build());
    for (std::uint64_t n : back) {
        UNSCOPED_INFO("rebuilt " << n);
    }
    CHECK(back.empty());
}

TEST_CASE("committing a previewed edit rebuilds nothing the preview built", "[doc][rebuild][reuse]")
{
    Part p;
    makePart(p);
    DocEnv env(4, p.state);
    REQUIRE(env.settle());
    CHECK(rebuilt(*env.doc->state(), *env.doc->build()).size() == 20); // a document of its own: a fresh cache

    const Command edit = editFeature("hole", kHole1, R"({"diameter":5.5})");
    env.doc->beginPreview(edit);
    REQUIRE(env.ui.runUntil([&] { return env.doc->previewBuild() != nullptr; }));
    CHECK(rebuilt(*env.doc->previewState(), *env.doc->previewBuild()) ==
          std::set<std::uint64_t>{kHole1, kHole2, kChamfer});
    env.run(edit);
    REQUIRE(env.settle());
    CHECK(rebuilt(*env.doc->state(), *env.doc->build()).empty());
}
