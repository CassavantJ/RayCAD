// Golden toolpaths of the 2D pocket operation (task T-054): islands with a helix entry, an open pocket with ramps and
// conventional cutting, wall and floor finishing with stock to leave, pre-drilled entries, and a corpus part; and one
// chain from the operation through the backplot (sim::buildBackplot) and the post host with the test post
// (tests/unit/post/posts/iso-test.post.js) to a committed G-code golden. Each toolpath also passes the independent
// invariant checker with coverage claims on its floors (Case::path).
//
// Each golden is tests/golden/toolpaths/<name>.rctp (Toolpath::serialize) with <name>.txt next to it
// (toolpath_dump.h), which is what a reviewer reads. The test checks that a fresh toolpath gives both files byte for
// byte and that the two files agree. To make them again after a reviewed change: set RAY_UPDATE_TOOLPATH_GOLDENS=1
// and run ray_cam_pocket2d_tests "[golden]", then read the .txt diff.
#include "pocket2d_test_util.h"

#include <ray/post.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = goldenBlock();
    return s;
}

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

// The toolpath is the golden <name>: both files match it and each other.
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
    CHECK(dumpToolpath(*back) == storedText); // the .txt next to the .rctp says what the .rctp holds
}

std::vector<std::vector<Vec2>> pocketAFloor()
{
    return {rectPoly(8, 8, 52, 62, 5), circlePoly(30, 22, 5), rectPoly(22, 40, 38, 50, 1)};
}

Case islandsCase()
{
    Case c;
    c.op = pocketOp(R"({"stepdown":4,"stepover":45})");
    c.tool = endMill(6);
    c.geom = geometryOn(block(), {upFace(block(), 12)});
    c.clears = {clearClaim(pocketAFloor(), 12), clearClaim({rectPoly(22, 40, 38, 50, 1)}, 16)};
    return c;
}

std::size_t helixMoves(const Toolpath& p)
{
    return static_cast<std::size_t>(std::count_if(p.moves.begin(), p.moves.end(), [](const Move& m) {
        return (m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW) && m.role == MoveRole::Ramp;
    }));
}

} // namespace

TEST_CASE("golden: a closed pocket with a tall and a low island, entered by helices", "[cam][pocket2d][golden]")
{
    const Case c = islandsCase();
    const Toolpath p = c.path();
    matchesGolden("pocket2d_islands", p);
    CHECK(helixMoves(p) > 0);
    CHECK(countRole(p, MoveRole::Plunge) == 0);
    CHECK(p.stats.minZ == 12.0);
}

TEST_CASE("golden: an open step, ramped into, conventional, with stock left on the walls", "[cam][pocket2d][golden]")
{
    Case c;
    c.op = pocketOp(R"({"stepdown":3,"stepover":55,"cutMode":"conventional","entry":"ramp","rampAngle":3,)"
                    R"("stockToLeaveWall":0.3})");
    c.tool = endMill(8);
    c.geom = geometryOn(block(), {upFace(block(), 14)}, 1);
    c.clears = {clearClaim({rectPoly(62, 0, 95, 25)}, 14)};
    const Toolpath p = c.path(c.claims(0.3, 0, true, 3));
    matchesGolden("pocket2d_open", p);
    CHECK(countRole(p, MoveRole::Plunge) == 0);
}

TEST_CASE("golden: wall and floor finishing with stock to leave", "[cam][pocket2d][golden]")
{
    const kernel::Shape part = pocketBlock(10, 8, 48, 32, 4, 6);
    Case c;
    c.op = pocketOp(R"({"stepdown":4.5,"stepover":40,"stockToLeaveWall":0.1,"stockToLeaveFloor":0.05,)"
                    R"("finishWalls":true,"wallAllowance":0.25,"finishFloor":true,"floorAllowance":0.2})");
    c.tool = endMill(10);
    c.geom = geometryOn(part, {upFace(part, 6)});
    c.clears = {clearClaim({rectPoly(10, 8, 48, 32, 4)}, 6)};
    c.walls = {invariants::WallClaim{invariants::Region::of({rectPoly(10, 8, 48, 32, 4)}), 6.05, 15.0, 0.05}};
    const Toolpath p = c.path(c.claims(0.1, 0.05));
    matchesGolden("pocket2d_finish", p);
    CHECK(countRole(p, MoveRole::LeadIn) > 0);
    CHECK(p.stats.minZ == 6.05);
}

TEST_CASE("golden: entered at pre-drilled holes", "[cam][pocket2d][golden]")
{
    const kernel::Shape part = pocketBlock(6, 6, 54, 34, 3, 8);
    Case c;
    c.op = pocketOp(R"({"stepdown":3.5,"stepover":50,"entry":"preDrill","preDrillPoints":"30 20",)"
                    R"("preDrillDiameter":9})");
    c.tool = endMill(8);
    c.geom = geometryOn(part, {upFace(part, 8)});
    c.clears = {clearClaim({rectPoly(6, 6, 54, 34, 3)}, 8)};
    invariants::Claims k = c.claims();
    k.centreCutting = true; // the checker cannot know the hole is drilled (pocket2d_test.cpp, pre-drilled holes)
    const Toolpath p = c.path(k);
    matchesGolden("pocket2d_predrill", p);
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Plunge) {
            CHECK(m.x == 30'000'000);
            CHECK(m.y == 20'000'000);
        }
    }
    CHECK(countRole(p, MoveRole::Plunge) == 2);
}

TEST_CASE("golden: the pocket of a corpus part", "[cam][pocket2d][golden][corpus]")
{
    // closed_pocket_1 (tools/corpus_gen): a block with a closed rectangular pocket with corner radii.
    JobContext ctx;
    auto imported = kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/closed_pocket_1.stp", {}, ctx);
    REQUIRE(imported);
    REQUIRE(imported->bodies.size() == 1);
    const kernel::Shape part = imported->bodies[0].shape;
    const Box3 b = kernel::bounds(part);
    // The floor: the up-facing planar face below the top.
    kernel::SubId floor{};
    double floorZ = b.max.z;
    for (std::uint32_t i = 0; i < part.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(part, f);
        const auto s = kernel::faceSample(part, f);
        if (info && s && info->type == kernel::SurfaceType::Plane && s->normal.z > 0.999 &&
            info->bounds.max.z < b.max.z - 1e-6 && info->bounds.max.z < floorZ) {
            floor = f;
            floorZ = info->bounds.max.z;
        }
    }
    REQUIRE(floorZ < b.max.z);
    Case c;
    c.op = pocketOp(R"({"stepdown":5,"stepover":50,"finishWalls":true})");
    c.tool = endMill(8, 30);
    c.geom = geometryOn(part, {floor});
    auto plan = c.plan();
    REQUIRE(plan);
    REQUIRE(plan->pockets.size() == 1);
    CHECK_FALSE(plan->pockets[0].open);
    // The floor face's own outline, as the checker's coverage region.
    auto fb = geom2d::faceBoundary(part, floor, Xform::identity());
    REQUIRE(fb);
    std::vector<std::vector<Vec2>> loops;
    for (const geom2d::Path* loop : {&fb->region.outer}) {
        std::vector<Vec2> poly;
        for (const geom2d::Pt& q : geom2d::polygonize(*loop, 1000)) {
            poly.push_back(geom2d::toVec2(q));
        }
        loops.push_back(std::move(poly));
    }
    c.clears = {clearClaim(loops, floorZ)};
    const Toolpath p = c.path();
    matchesGolden("pocket2d_corpus", p);
    CHECK(std::abs(p.stats.minZ - floorZ) < 1e-6);
}

TEST_CASE("the same pocket gives the same bytes every time", "[cam][pocket2d][golden]")
{
    const auto first = islandsCase().generate();
    REQUIRE(first);
    for (int i = 0; i < 3; ++i) {
        auto again = islandsCase().generate();
        REQUIRE(again);
        CHECK(again->serialize() == first->serialize());
    }
}

TEST_CASE("chain: a pocket toolpath through the backplot and the post host to G-code", "[cam][pocket2d][golden][post]")
{
    const kernel::Shape part = pocketBlock(10, 10, 40, 28, 3, 9);
    Case c;
    c.op = pocketOp(R"({"stepdown":3,"stepover":50,"finishWalls":true})");
    c.tool = endMill(6);
    c.geom = geometryOn(part, {upFace(part, 9)});
    const Toolpath p = c.path();

    // Backplot: every move maps to vertices; it reaches the floor and the clearance.
    const sim::Backplot bp = sim::buildBackplot(p, Xform::identity(), 0.01);
    REQUIRE(bp.moveFirstVertex.size() == p.moves.size() + 1);
    CHECK(bp.moveFirstVertex.back() == bp.lineVertices.size());
    CHECK(std::abs(bp.bounds.min.z - 9.0) < 1e-4);
    CHECK(std::abs(bp.bounds.max.z - 30.0) < 1e-4); // stock top 15 + clearance 15

    Setup setup;
    setup.name = "Op 1 top";
    setup.programNumber = 1054;
    setup.programComment = "T-054 pocket chain";
    setup.wcs.workOffset = 1;
    lib::MachineDef mill = machine();
    mill.name = "Test mill";
    mill.model = "TM-1";
    mill.controlDialect = "generic-iso";
    Operation op = c.op;
    op.name = "Pocket 6";
    op.tool.toolNumber = op.tool.lengthOffset = 4;
    op.cutting.coolant = CuttingData::Coolant::Flood;
    lib::Tool tool = c.tool;
    tool.name = "6 mm flat end mill";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "pocket";
    job.userName = "tester";
    job.sections = {post::PostSection{&op, &p, &tool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = goldenPath("pocket2d_chain.iso-test.nc");
    if (updating()) {
        writeFile(golden, out->text);
    }
    CHECK(out->text == readText(golden));
    CHECK(out->text.find("G3") != std::string::npos); // helices and climb rings round the corners: counter-clockwise
}
