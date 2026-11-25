// Golden toolpaths of the 2D adaptive roughing operation (task T-061), one per entry kind: a closed pocket with two
// islands entered by a helix,
// a slot too narrow for a helix entered by a ramp, a pocket entered at a pre-drilled hole, an open step entered from
// outside the stock at two depths (one engine run for both); and one chain from the operation through the backplot
// (sim::buildBackplot) and the post host with the test post (tests/unit/post/posts/iso-test.post.js) to a committed
// G-code golden. Each toolpath also passes the independent invariant checker with coverage claims on its floors
// (AdaptiveCase::path).
//
// Each golden is tests/golden/toolpaths/<name>.rctp (Toolpath::serialize) with <name>.txt next to it
// (toolpath_dump.h), which is what a reviewer reads. To make them again after a reviewed change: set
// RAY_UPDATE_TOOLPATH_GOLDENS=1 and run ray_cam_adaptive2d_tests "[golden]", then read the .txt diff.
#include "adaptive2d_test_util.h"

#include <ray/post.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_TOOLPATH_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

std::string goldenPath(const std::string& file)
{
    return std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/" + file;
}

void writeFile(const std::string& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string asText(const std::vector<std::byte>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void matchesGolden(const std::string& name, const Toolpath& p)
{
    const std::string bytes = asText(p.serialize());
    const std::string text = dumpToolpath(p);
    if (updating()) {
        writeFile(goldenPath(name + ".rctp"), bytes);
        writeFile(goldenPath(name + ".txt"), text);
    }
    const std::string storedBytes = readText(goldenPath(name + ".rctp"));
    const std::string storedText = readText(goldenPath(name + ".txt"));
    REQUIRE_FALSE(storedBytes.empty());
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText);
}

std::size_t countKind(const Toolpath& p, MoveRole role, bool arcs)
{
    return static_cast<std::size_t>(std::count_if(p.moves.begin(), p.moves.end(), [&](const Move& m) {
        return m.role == role && (m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW) == arcs;
    }));
}

} // namespace

TEST_CASE("golden: a pocket with two islands, entered by a helix", "[cam][adaptive2d][golden]")
{
    const AdaptiveCase c = islandsGoldenCase();
    const Toolpath p = c.path();
    matchesGolden("adaptive2d_islands", p);
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->depths.size() == 1);
    REQUIRE(plan->areas.size() == 1);
    REQUIRE(plan->areas[0].region[0].size() == 1);
    CHECK(plan->areas[0].region[0].front().holes.size() == 2);
    CHECK(plan->engineCalls == 1);
    for (const ops::Adaptive2dDepth& d : plan->depths) {
        CHECK(d.entries == std::vector<adaptive2d::EntryMode>{adaptive2d::EntryMode::Helix});
    }
    CHECK(countKind(p, MoveRole::Ramp, true) > 0); // the helices
    CHECK(countRole(p, MoveRole::Plunge) == 0);
    CHECK(p.stats.minZ == 15.0);
}

TEST_CASE("golden: a slot too narrow for a helix, entered by a ramp along it", "[cam][adaptive2d][golden]")
{
    const AdaptiveCase c = slotGoldenCase();
    const Toolpath p = c.path();
    matchesGolden("adaptive2d_ramp", p);
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->depths.size() == 1);
    CHECK(plan->depths[0].entries == std::vector<adaptive2d::EntryMode>{adaptive2d::EntryMode::Ramp});
    CHECK(countKind(p, MoveRole::Ramp, false) > 0); // the zigzag
    CHECK(countKind(p, MoveRole::Ramp, true) == 0); // no helix
    // The fall-back is said: the helix did not fit.
    REQUIRE_FALSE(p.warnings.empty());
    CHECK(p.warnings.front().find("is entered by a ramp") != std::string::npos);
    CHECK(p.warnings.front().find("helix") != std::string::npos);
}

TEST_CASE("golden: a pocket entered at a pre-drilled hole", "[cam][adaptive2d][golden]")
{
    const AdaptiveCase c = preDrillGoldenCase();
    invariants::Claims k = c.claims();
    k.centreCutting = true; // the checker cannot know the hole is drilled (pocket2d_test.cpp, pre-drilled holes)
    const Toolpath p = c.path(k);
    matchesGolden("adaptive2d_predrill", p);
    std::size_t plunges = 0;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Plunge) {
            ++plunges;
            CHECK(m.x == 20'000'000);
            CHECK(m.y == 15'000'000);
        }
    }
    CHECK(plunges == 1);
    CHECK(countKind(p, MoveRole::Ramp, true) == 0);
}

TEST_CASE("golden: an open step entered from outside the stock, at two depths", "[cam][adaptive2d][golden]")
{
    const AdaptiveCase c = openGoldenCase();
    const Toolpath p = c.path();
    matchesGolden("adaptive2d_open", p);
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->areas.size() == 1);
    CHECK(plan->areas[0].open);
    // Two depths of the same area: one engine run, its passes at both.
    REQUIRE(plan->depths.size() == 2);
    CHECK(plan->depths[0].z == 10'000'000);
    CHECK(plan->depths[1].z == 8'000'000);
    CHECK_FALSE(plan->depths[0].reused);
    CHECK(plan->depths[1].reused);
    CHECK(plan->engineCalls == 1);
    CHECK(p.stats.minZ == 8.0);
    for (const ops::Adaptive2dDepth& d : plan->depths) {
        CHECK(d.entries == std::vector<adaptive2d::EntryMode>{adaptive2d::EntryMode::FromCleared});
    }
    CHECK(countRole(p, MoveRole::Ramp) == 0); // no helix and no ramp: a descent outside the stock
    CHECK(countRole(p, MoveRole::Plunge) == 0);
}

TEST_CASE("golden: a round pocket no helix or ramp fits, plunged into at two depths (after M3)",
          "[cam][adaptive2d][golden]")
{
    const AdaptiveCase c = plungeGoldenCase();
    REQUIRE(c.tool.cutsAtCentre());
    const Toolpath p = c.path();
    matchesGolden("adaptive2d_plunge", p);
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->depths.size() == 2);
    CHECK(plan->engineCalls == 1); // the second depth reuses the first's plan
    for (const ops::Adaptive2dDepth& d : plan->depths) {
        CHECK(d.entries == std::vector<adaptive2d::EntryMode>{adaptive2d::EntryMode::Plunge});
    }
    CHECK(countRole(p, MoveRole::Plunge) == 2);
    CHECK(countRole(p, MoveRole::Ramp) == 0);
    REQUIRE(p.warnings.size() == 2);
    CHECK(p.warnings[1].find("Plunges straight down at 2 places") == 0);
}

TEST_CASE("the same adaptive operation gives the same bytes every time", "[cam][adaptive2d][golden]")
{
    const auto first = islandsGoldenCase().generate();
    REQUIRE(first);
    for (int i = 0; i < 2; ++i) {
        auto again = islandsGoldenCase().generate();
        REQUIRE(again);
        CHECK(again->serialize() == first->serialize());
    }
}

TEST_CASE("chain: an adaptive toolpath through the backplot and the post host to G-code",
          "[cam][adaptive2d][golden][post]")
{
    const AdaptiveCase c = slotGoldenCase();
    const Toolpath p = c.path();

    const sim::Backplot bp = sim::buildBackplot(p, Xform::identity(), 0.01);
    REQUIRE(bp.moveFirstVertex.size() == p.moves.size() + 1);
    CHECK(bp.moveFirstVertex.back() == bp.lineVertices.size());
    CHECK(std::abs(bp.bounds.min.z - 7.0) < 1e-4);

    Setup setup;
    setup.name = "Op 1 top";
    setup.programNumber = 1061;
    setup.programComment = "T-061 adaptive chain";
    setup.wcs.workOffset = 1;
    lib::MachineDef mill = machine();
    mill.name = "Test mill";
    mill.model = "TM-1";
    mill.controlDialect = "generic-iso";
    Operation op = c.op;
    op.name = "Adaptive slot";
    op.tool.toolNumber = op.tool.lengthOffset = 5;
    op.cutting.coolant = CuttingData::Coolant::Flood;
    lib::Tool tool = c.tool;
    tool.name = "6 mm flat end mill";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "adaptive";
    job.userName = "tester";
    job.sections = {post::PostSection{&op, &p, &tool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = goldenPath("adaptive2d_chain.iso-test.nc");
    if (updating()) {
        writeFile(golden, out->text);
    }
    CHECK(out->text == readText(golden));
    CHECK(out->text.find("G2") != std::string::npos); // fitted arcs both ways
    CHECK(out->text.find("G3") != std::string::npos);
}
