// Golden toolpaths of the drilling operation (task T-035 acceptance: a golden toolpath for each cycle; the invariant
// checker passes), on the kernel-built golden block (drill_test_util.h goldenBlock) and on a corpus part, and one
// chain from the operation through the backplot (sim::buildBackplot) and the post host with the test post
// (tests/unit/post/posts/iso-test.post.js) to a committed G-code golden.
//
// Each golden is tests/golden/toolpaths/<name>.rctp (Toolpath::serialize, the RCTP format) with <name>.txt next to it
// (toolpath_dump.h), which is what a reviewer reads. The test checks that a fresh toolpath gives both files byte for
// byte and that the two files agree. To make them again after a reviewed change: set RAY_UPDATE_TOOLPATH_GOLDENS=1
// and run ray_cam_drill_tests "[golden]", then read the .txt diff.
#include "drill_test_util.h"

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
    INFO(name << ":\n" << text);
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText); // the .txt next to the .rctp says what the .rctp holds
}

Case on(const std::vector<std::pair<double, double>>& holes, const std::string& params, lib::Tool t, double rpm,
        double feed)
{
    Case c;
    c.op = drillOp(params, rpm, feed);
    c.tool = std::move(t);
    std::vector<kernel::SubId> faces;
    for (const auto& [x, y] : holes) {
        faces.push_back(holeFace(block(), x, y));
    }
    c.geom = geometryOn(block(), faces);
    return c;
}

const std::vector<std::pair<double, double>> kAllHoles = {{15, 15}, {35, 15}, {55, 15}, {15, 35},
                                                          {35, 35}, {70, 38}, {52, 38}, {70, 12}};

// The cycle moves of a toolpath: their kind and return level, in order.
std::vector<std::pair<MoveKind, CycleReturn>> cyclesOf(const Toolpath& p)
{
    std::vector<std::pair<MoveKind, CycleReturn>> out;
    for (const Move& m : p.moves) {
        if (isCycleMove(m.kind)) {
            out.emplace_back(m.kind, m.cycleReturn);
        }
    }
    return out;
}

Toolpath drillGolden()
{
    return on(kAllHoles, R"({"dwell":0.3})", drill(5, 118), 2000, 160).path();
}
Toolpath boreGolden()
{
    return on({{15, 15}, {15, 35}}, R"({"cycle":"bore","dwell":0.2})", tool(lib::ToolType::BoringBar, 6, 0), 900, 60)
        .path();
}

} // namespace

TEST_CASE("golden: spot drilling every hole of the block", "[cam][drill][golden]")
{
    const Toolpath p = on(kAllHoles, R"({"cycle":"spot"})", spotDrill(10, 90), 1500, 100).path();
    matchesGolden("drill_spot", p);
    // One run of G81; G99 between holes, except after H6 on the step (its R plane, Z 16, is below the block's top)
    // and after the last hole, which return to the initial level.
    const auto cycles = cyclesOf(p);
    REQUIRE(cycles.size() == 8);
    for (std::size_t i = 0; i < cycles.size(); ++i) {
        const Move& m = p.moves[i + 1];
        const bool onStep = m.x == 70'000'000 && m.y == 38'000'000;
        CHECK(cycles[i].first == MoveKind::CycleDrill);
        CHECK(cycles[i].second == (onStep || i + 1 == cycles.size() ? CycleReturn::Initial : CycleReturn::RPlane));
        CHECK(m.cycleClearance == 35'000'000); // the highest hole top (Z 20) + 15
        CHECK(m.cycleRetract == (onStep ? 16'000'000 : 22'000'000));
    }
    CHECK(p.moves.back().kind == MoveKind::CycleCancel);
}

TEST_CASE("golden: drilling with a dwell (G82) through, blind and stepped holes", "[cam][drill][golden]")
{
    const Toolpath p = drillGolden();
    matchesGolden("drill_drill", p);
    CHECK(p.moves.at(1).cycleDwellMs == 300);
    CHECK(p.moves.at(1).feedMicronPerMin == 160'000);
}

TEST_CASE("golden: peck drilling the through holes of a corpus part", "[cam][drill][golden][corpus]")
{
    // plate_counterbore_1: four counterbored through holes, 6.6 mm from Z 8.5 to Z 0 under 11 mm counterbores from
    // Z 15 (tests/unit/cam/holes_labels.txt).
    JobContext ctx;
    auto imported =
        kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/plate_counterbore_1.stp", {}, ctx);
    REQUIRE(imported);
    REQUIRE(imported->bodies.size() == 1);
    const kernel::Shape part = imported->bodies[0].shape;
    std::vector<kernel::SubId> faces;
    for (const RecognizedHole& h : holesOf(part)) {
        faces.push_back(h.axisSub);
    }
    REQUIRE(faces.size() == 4);
    Case c;
    c.op = drillOp(R"({"cycle":"peck","peck":3})", 2200, 180);
    c.tool = drill(6.6, 118);
    c.geom = geometryOn(part, faces);
    const Toolpath p = c.path();
    matchesGolden("drill_peck_corpus", p);
    for (const Move& m : p.moves) {
        if (isCycleMove(m.kind)) {
            CHECK(m.kind == MoveKind::CyclePeck);
            CHECK(m.cyclePeck == 3'000'000);
            CHECK(m.cyclePeckClearance == 0);     // the control's own
            CHECK(m.z == -1'000'000 - 1'982'840); // through: 1 mm breakthrough + 3.3 / tan 59
        }
    }
}

TEST_CASE("golden: chip-break drilling", "[cam][drill][golden]")
{
    const Toolpath p = on({{35, 35}, {52, 38}, {70, 12}}, R"({"cycle":"chipBreak","peck":1.5,"peckClearance":0.3})",
                          drill(5), 2400, 200)
                           .path();
    matchesGolden("drill_chipbreak", p);
    CHECK(p.moves.at(1).kind == MoveKind::CycleChipBreak);
    CHECK(p.moves.at(1).cyclePeckClearance == 300'000);
}

TEST_CASE("golden: tapping M6x1", "[cam][drill][golden]")
{
    const Toolpath p = on({{52, 38}, {70, 12}}, R"({"cycle":"tap"})", tap(6, 1), 500, 0.1).path();
    matchesGolden("drill_tap", p);
    for (const Move& m : p.moves) {
        if (isCycleMove(m.kind)) {
            CHECK(m.kind == MoveKind::CycleTap);
            CHECK(m.feedMicronPerMin == 500'000);
            CHECK(m.threadPitchNm == 1'000'000);
            CHECK(m.cycleLeftHand == 0);
        }
    }
}

TEST_CASE("golden: boring with a dwell (G89)", "[cam][drill][golden]")
{
    const Toolpath p = boreGolden();
    matchesGolden("drill_bore", p);
    CHECK(p.moves.at(1).kind == MoveKind::CycleBore);
}

TEST_CASE("golden: reaming", "[cam][drill][golden]")
{
    const Toolpath p =
        on({{15, 15}, {15, 35}}, R"({"cycle":"ream"})", tool(lib::ToolType::Reamer, 6.6, 0), 400, 80).path();
    matchesGolden("drill_ream", p);
    CHECK(p.moves.at(1).kind == MoveKind::CycleReam);
}

TEST_CASE("golden: hole order follows the selection when asked", "[cam][drill][golden]")
{
    const std::vector<std::pair<double, double>> picked = {{70, 12}, {15, 15}, {52, 38}};
    const Toolpath shortest = on(picked, "{}", drill(5), 2000, 160).path();
    const Toolpath selection = on(picked, R"({"holeOrder":"selection"})", drill(5), 2000, 160).path();
    const auto xs = [](const Toolpath& p) {
        std::vector<Nm> out;
        for (const Move& m : p.moves) {
            if (isCycleMove(m.kind)) {
                out.push_back(m.x);
            }
        }
        return out;
    };
    CHECK(xs(selection) == std::vector<Nm>{70'000'000, 15'000'000, 52'000'000});
    CHECK(xs(shortest) == std::vector<Nm>{15'000'000, 52'000'000, 70'000'000}); // from the work origin
}

TEST_CASE("the same operation gives the same bytes every time", "[cam][drill][golden]")
{
    const auto first = drillGolden().serialize();
    for (int i = 0; i < 3; ++i) {
        CHECK(drillGolden().serialize() == first);
    }
}

TEST_CASE("chain: a drilling and a boring toolpath through the backplot and the post host to G-code",
          "[cam][drill][golden][post]")
{
    const Toolpath drilled = drillGolden();
    const Toolpath bored = boreGolden();

    // Backplot: every move maps to vertices, and the cycles reach their bottoms (expanded into rapids and feeds).
    for (const Toolpath* p : {&drilled, &bored}) {
        const sim::Backplot b = sim::buildBackplot(*p, Xform::identity(), 0.01);
        REQUIRE(b.moveFirstVertex.size() == p->moves.size() + 1);
        CHECK(b.moveFirstVertex.back() == b.lineVertices.size());
        float lowest = 1e9F;
        for (const sim::BackplotVertex& v : b.lineVertices) {
            lowest = std::min(lowest, v.z);
        }
        Nm deepest = 0;
        for (const Move& m : p->moves) {
            deepest = isCycleMove(m.kind) ? std::min(deepest, m.z) : deepest;
        }
        CHECK(std::abs(static_cast<double>(lowest) - toMm(deepest)) < 1e-4);
        CHECK(std::abs(b.bounds.max.z - 35.0) < 1e-4); // the initial level
    }

    // Post: both sections with the test post, which writes G81/G82 natively and leaves bore to the host's expansion.
    Setup setup;
    setup.name = "Op 1 top";
    setup.programNumber = 1035;
    setup.programComment = "T-035 drilling chain";
    setup.wcs.workOffset = 1;
    const lib::MachineDef mill = [] {
        lib::MachineDef m = machine();
        m.name = "Test mill";
        m.model = "TM-1";
        m.controlDialect = "generic-iso";
        return m;
    }();
    Operation drillOperation = drillOp(R"({"dwell":0.3})", 2000, 160);
    drillOperation.name = "Drill 5";
    drillOperation.tool.toolNumber = drillOperation.tool.lengthOffset = 3;
    drillOperation.cutting.coolant = CuttingData::Coolant::Flood;
    Operation boreOperation = drillOp(R"({"cycle":"bore","dwell":0.2})", 900, 60);
    boreOperation.name = "Bore 6";
    boreOperation.tool.toolNumber = boreOperation.tool.lengthOffset = 7;
    lib::Tool drillTool = drill(5, 118);
    drillTool.name = "5 mm drill";
    lib::Tool boreTool = tool(lib::ToolType::BoringBar, 6, 0);
    boreTool.name = "6 mm boring bar";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "drilling";
    job.userName = "tester";
    job.sections = {post::PostSection{&drillOperation, &drilled, &drillTool, &holder},
                    post::PostSection{&boreOperation, &bored, &boreTool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = goldenPath("drill_chain.iso-test.nc");
    if (updating()) {
        writeFile(golden, out->text);
    }
    INFO(out->text);
    CHECK(out->text == readText(golden));
    CHECK(out->text.find("G82") != std::string::npos);
    CHECK(out->text.find("G98") != std::string::npos);
    CHECK(out->text.find("G99") != std::string::npos);
}
