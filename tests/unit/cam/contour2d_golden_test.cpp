// Golden toolpaths of the 2D contour operation (task T-053 acceptance: golden plus invariants), on the kernel-built
// contour part (contour2d_test_util.h contourPart) and on a corpus part, each also through the independent invariant
// checker; and one chain from the operation with compensation in the control through the backplot and the post host
// with the test post (tests/unit/post/posts/iso-test.post.js) to a committed G-code golden.
//
// Each golden is tests/golden/toolpaths/<name>.rctp (Toolpath::serialize) with <name>.txt next to it (toolpath_dump.h),
// which is what a reviewer reads. To make them again after a reviewed change: set RAY_UPDATE_TOOLPATH_GOLDENS=1 and
// run ray_cam_contour2d_tests "[golden]", then read the .txt diff.
#include "contour2d_test_util.h"

#include <ray/post.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& part()
{
    static const kernel::Shape s = contourPart();
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
    INFO(name << ":\n" << text);
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText);
}

ContourCase on(const std::vector<kernel::SubId>& picks, const std::string& params, double bottom, lib::Tool t)
{
    ContourCase c;
    c.op = contourOp(params, bottom);
    c.tool = std::move(t);
    c.geom = geometryOn(part(), picks);
    return c;
}

kernel::SubId bossTop()
{
    return lineEdgeAt(part(), Vec3{50, 15, 25});
}
kernel::SubId plateFront()
{
    return lineEdgeAt(part(), Vec3{50, 0, 10});
}
kernel::SubId pocketFloorFace()
{
    return flatFaceAt(part(), 18, 50, 40);
}

ContourCase controlCase()
{
    return on({plateFront()}, R"({"compensation":"control","leadLength":4,"stockToLeave":0.2,"maxStepdown":5})", -10,
              endMill(10));
}

bool hasComp(const Toolpath& p)
{
    for (const Move& m : p.moves) {
        if (m.compensation != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("golden: the outside of the boss, climb, three depths, arc leads", "[cam][contour2d][golden]")
{
    const Toolpath p =
        on({bossTop()}, R"({"stockToLeave":0.2,"maxStepdown":5,"leadLength":2})", -15, endMill(10)).path();
    matchesGolden("contour2d_boss_climb", p);
    CHECK_FALSE(hasComp(p));
}

TEST_CASE("golden: the pocket wall, conventional, two roughing passes, finish and spring", "[cam][contour2d][golden]")
{
    const Toolpath p = on({pocketFloorFace()},
                          R"({"direction":"conventional","maxStepdown":3.5,"roughPasses":2,"roughStepover":1,)"
                          R"("finishPass":true,"finishAllowance":0.25,"springPass":true,"leadRadius":3})",
                          0, endMill(6))
                           .path();
    matchesGolden("contour2d_pocket_finish", p);
}

TEST_CASE("golden: an open chain with line leads", "[cam][contour2d][golden]")
{
    const Toolpath p =
        on({plateFront()}, R"({"followTangent":false,"leadType":"line","maxStepdown":4})", -10, endMill(8)).path();
    matchesGolden("contour2d_open_line", p);
}

TEST_CASE("golden: the plate's outline through with rectangular tabs", "[cam][contour2d][golden]")
{
    const Toolpath p =
        on({plateFront()}, R"({"maxStepdown":4,"tabs":true,"tabCount":4,"tabWidth":6,"tabHeight":3,"rampAngle":10})",
           -10, endMill(10))
            .path();
    matchesGolden("contour2d_tabs_rect", p);
}

TEST_CASE("golden: the plate's outline through with triangular tabs", "[cam][contour2d][golden]")
{
    const Toolpath p = on({plateFront()},
                          R"({"tabs":true,"tabShape":"triangular","tabCount":3,"tabWidth":10,)"
                          R"("tabHeight":2,"leadType":"line"})",
                          -10, endMill(6, 30, lib::ToolType::BallEndMill))
                           .path();
    matchesGolden("contour2d_tabs_tri", p);
}

TEST_CASE("golden: compensation in the control", "[cam][contour2d][golden][compensation]")
{
    auto p = controlCase().generate();
    REQUIRE(p);
    matchesGolden("contour2d_control", *p);
    CHECK(hasComp(*p));
}

TEST_CASE("golden: compensation for wear", "[cam][contour2d][golden][compensation]")
{
    const Toolpath p =
        on({plateFront()}, R"({"compensation":"wear","leadType":"line","maxStepdown":5})", -10, endMill(10)).path();
    matchesGolden("contour2d_wear", p);
    CHECK(hasComp(p));
}

TEST_CASE("golden: the outline of a corpus part", "[cam][contour2d][golden][corpus]")
{
    // fillet_block_1: a block with filleted vertical edges; its top face's outer loop, 6 mm down, outside.
    JobContext ctx;
    auto imported = kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/fillet_block_1.stp", {}, ctx);
    REQUIRE(imported);
    REQUIRE(imported->bodies.size() == 1);
    const kernel::Shape shape = imported->bodies[0].shape;
    // The up-facing plane highest up, the largest of them.
    std::optional<kernel::SubId> top;
    double topZ = -1e300, topArea = 0;
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto s = kernel::surfaceInfo(shape, f);
        auto sample = kernel::faceSample(shape, f);
        if (!s || !sample || s->type != kernel::SurfaceType::Plane || sample->normal.z < 1 - 1e-9) {
            continue;
        }
        if (s->bounds.max.z > topZ + 1e-9 || (std::abs(s->bounds.max.z - topZ) <= 1e-9 && s->area > topArea)) {
            top = f;
            topZ = s->bounds.max.z;
            topArea = s->area;
        }
    }
    REQUIRE(top);
    ContourCase c;
    c.op = contourOp(R"({"maxStepdown":3,"stockToLeave":0.1})", -6);
    c.tool = endMill(8);
    c.geom = geometryOn(shape, {*top}, 20); // the stock 20 mm wider than the part each way
    const Toolpath p = c.path();
    matchesGolden("contour2d_corpus", p);
}

TEST_CASE("chain: a contour with compensation in the control through the backplot and the post host",
          "[cam][contour2d][golden][post]")
{
    const ContourCase c = controlCase();
    auto made = c.generate();
    REQUIRE(made);
    const Toolpath& p = *made;
    const sim::Backplot b = sim::buildBackplot(p, Xform::identity(), 0.01);
    REQUIRE(b.moveFirstVertex.size() == p.moves.size() + 1);
    CHECK(b.moveFirstVertex.back() == b.lineVertices.size());

    Setup setup;
    setup.name = "Op 1 outline";
    setup.programNumber = 1053;
    setup.programComment = "T-053 contour chain";
    setup.wcs.workOffset = 1;
    lib::MachineDef mill = c.machine;
    mill.name = "Test mill";
    mill.model = "TM-1";
    mill.controlDialect = "generic-iso";
    Operation op = c.op;
    op.name = "Contour outline";
    op.tool.toolNumber = op.tool.lengthOffset = op.tool.diameterOffset = 5;
    op.cutting.coolant = CuttingData::Coolant::Flood;
    lib::Tool tool = c.tool;
    tool.name = "10 mm end mill";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "contour";
    job.userName = "tester";
    job.sections = {post::PostSection{&op, &p, &tool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = goldenPath("contour2d_chain.iso-test.nc");
    if (updating()) {
        writeFile(golden, out->text);
    }
    INFO(out->text);
    CHECK(out->text == readText(golden));
    CHECK(out->text.find("G41") != std::string::npos);
    CHECK(out->text.find("G40") != std::string::npos);
    // Every G41 is on a straight move (G1), followed by the lead arc; every G40 too.
    CHECK(out->text.find("G41 Y") != std::string::npos);
    CHECK(out->text.find("G40 G1") != std::string::npos);
}
