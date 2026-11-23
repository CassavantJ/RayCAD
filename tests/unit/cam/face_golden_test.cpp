// Golden toolpaths of the facing operation (task T-052 acceptance: zigzag and one-way facing over the stock boundary
// with stepover, overhang and multiple depths; golden plus invariants; the whole stock top covered, checked by a 2D
// sweep), on the kernel-built block of face_test_util.h and on a corpus part, and one chain from the operation through
// the backplot (sim::buildBackplot) and the post host with the test post (tests/unit/post/posts/iso-test.post.js) to a
// committed G-code golden.
//
// Each golden is tests/golden/toolpaths/<name>.rctp (Toolpath::serialize) with <name>.txt next to it (toolpath_dump.h),
// which is what a reviewer reads. The test checks that a fresh toolpath gives both files byte for byte and that the two
// agree; every golden toolpath also passes the invariant checker with facing's coverage claim. To make them again after
// a reviewed change: set RAY_UPDATE_TOOLPATH_GOLDENS=1 and run ray_cam_face_tests "[golden]", then read the .txt diff.
#include "face_test_util.h"

#include "cam/geom2d.h"

#include <ray/post.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <span>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& block()
{
    static const kernel::Shape s = faceBlock();
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

// The toolpath is the golden <name>: both files match it and each other.
void matchesGolden(const std::string& name, const Toolpath& p)
{
    const std::vector<std::byte> raw = p.serialize();
    const std::string bytes(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::string text = dumpToolpath(p);
    if (updating()) {
        writeFile(goldenPath(name + ".rctp"), bytes);
        writeFile(goldenPath(name + ".txt"), text);
    }
    const std::string storedBytes = faceReadText(goldenPath(name + ".rctp"));
    const std::string storedText = faceReadText(goldenPath(name + ".txt"));
    REQUIRE_FALSE(storedBytes.empty());
    INFO(name);
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText);
}

FaceCase onBlock(const std::string& params, lib::Tool t)
{
    FaceCase c;
    c.op = faceOp(params);
    c.tool = std::move(t);
    c.geom = stockAround(block(), 2, 1.5);
    return c;
}

// The corpus plate (plate_thru_holes_1: 120 x 80 x 10 with six 6.6 mm through holes), its stock 5 mm wider on each
// side and 1.2 mm above it, faced with the library's 2 in face mill (50.8 mm, flutes 6).
FaceCase onPlate(const std::string& params)
{
    JobContext ctx;
    auto imported =
        kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/plate_thru_holes_1.stp", {}, ctx);
    REQUIRE(imported);
    REQUIRE(imported->bodies.size() == 1);
    FaceCase c;
    c.op = faceOp(params, 1500, 900);
    c.tool = millTool(lib::ToolType::FaceMill, 50.8, 0, 6);
    c.geom = stockAround(imported->bodies[0].shape, 5, 1.2);
    return c;
}

// The Cut moves of a toolpath at height z, as 2D paths (each from where the tool was before it).
std::vector<geom2d::Path> cutsAt(const Toolpath& p, Nm z)
{
    std::vector<geom2d::Path> out;
    Nm x = p.startX, y = p.startY, zz = p.startZ;
    for (const Move& m : p.moves) {
        if (m.role == MoveRole::Cut && m.kind == MoveKind::Linear && m.z == z && zz == z) {
            geom2d::Path path;
            path.start = geom2d::Pt{x, y};
            path.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, geom2d::Pt{m.x, m.y}, {}, 0});
            out.push_back(std::move(path));
        }
        x = m.x;
        y = m.y;
        zz = m.z;
    }
    return out;
}

// The area (nm^2) of the stock box's XY that discs of radius r along `cuts` leave uncovered (geom2d: sweptArea, then
// subtract).
double uncovered(const Box3& stock, const std::vector<geom2d::Path>& cuts, Nm r)
{
    geom2d::Path rect;
    rect.start = geom2d::Pt{toNm(stock.min.x), toNm(stock.min.y)};
    rect.closed = true;
    for (const geom2d::Pt q :
         {geom2d::Pt{toNm(stock.max.x), toNm(stock.min.y)}, geom2d::Pt{toNm(stock.max.x), toNm(stock.max.y)},
          geom2d::Pt{toNm(stock.min.x), toNm(stock.max.y)}, rect.start}) {
        rect.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, q, {}, 0});
    }
    JobContext ctx;
    auto swept = geom2d::sweptArea(cuts, r, ctx);
    REQUIRE(swept);
    auto rest = geom2d::subtract(geom2d::Regions{geom2d::Region{rect, {}}}, *swept, ctx);
    REQUIRE(rest);
    return geom2d::area(*rest);
}

// The coverage check by 2D sweep, on a generated toolpath: the flat bottoms of the cuts at the faced height leave
// nothing of the stock top.
void coversStockTop(const FaceCase& c, const Toolpath& p)
{
    const ops::FacePlan plan = c.okPlan();
    const std::vector<geom2d::Path> last = cutsAt(p, plan.bottom);
    CHECK(last.size() == plan.strokesPerLevel);
    CHECK(uncovered(c.geom.stockBounds, last, plan.flatHalf) == 0.0);
}

Toolpath zigzagGolden()
{
    return onBlock(R"({"stepdown":0.6})", millTool(lib::ToolType::FlatEndMill, 10)).path();
}

} // namespace

TEST_CASE("golden: zigzag facing of the block at three depths", "[cam][face][golden]")
{
    const FaceCase c = onBlock(R"({"stepdown":0.6})", millTool(lib::ToolType::FlatEndMill, 10));
    const Toolpath p = c.path();
    matchesGolden("face_zigzag", p);
    coversStockTop(c, p);
    // Three levels of eight strokes, every one 98 mm long (from X -9 to X 89).
    std::size_t cuts = 0;
    for (const Move& m : p.moves) {
        cuts += m.role == MoveRole::Cut ? 1 : 0;
    }
    CHECK(cuts == 24);
    CHECK(p.stats.minZ == 20.0);
}

TEST_CASE("golden: one-way climb facing along Y with a bull nose", "[cam][face][golden]")
{
    const FaceCase c = onBlock(R"({"pattern":"oneWay","direction":"alongY","stepover":7,"overhang":3,"stepdown":1})",
                               millTool(lib::ToolType::BullEndMill, 12, 1.5));
    const Toolpath p = c.path();
    matchesGolden("face_oneway_y", p);
    coversStockTop(c, p);
}

TEST_CASE("golden: one-way conventional facing along X", "[cam][face][golden]")
{
    const FaceCase c = onBlock(R"({"pattern":"oneWay","cutMode":"conventional","stepover":5})",
                               millTool(lib::ToolType::FlatEndMill, 8));
    const Toolpath p = c.path();
    matchesGolden("face_oneway_conventional", p);
    coversStockTop(c, p);
}

TEST_CASE("golden: zigzag facing of a corpus plate with a face mill", "[cam][face][golden][corpus]")
{
    const FaceCase c = onPlate(R"({"stepdown":1,"overhang":5})");
    const Toolpath p = c.path();
    matchesGolden("face_plate_facemill", p);
    coversStockTop(c, p);
    // 1.2 mm in two passes of 0.6; the plate's top at Z 10.
    const ops::FacePlan plan = c.okPlan();
    CHECK(plan.levels == std::vector<Nm>{10'600'000, 10'000'000});
}

TEST_CASE("face: the coverage check by 2D sweep finds a gap", "[cam][face][golden]")
{
    // The check itself bites: the golden zigzag's last level less one stroke, or swept with a radius of 3.4 mm (its
    // neighbours are 6.857 mm apart, so they need 3.43), leaves stock uncovered.
    const FaceCase c = onBlock(R"({"stepdown":0.6})", millTool(lib::ToolType::FlatEndMill, 10));
    const Toolpath p = zigzagGolden();
    const ops::FacePlan plan = c.okPlan();
    std::vector<geom2d::Path> last = cutsAt(p, plan.bottom);
    REQUIRE(last.size() == 8);
    CHECK(uncovered(c.geom.stockBounds, last, plan.flatHalf) == 0.0);
    CHECK(uncovered(c.geom.stockBounds, last, 3'400'000) > 1e9);
    last.erase(last.begin() + 3);
    CHECK(uncovered(c.geom.stockBounds, last, plan.flatHalf) > 1e9);
    // And the plan's own check (faceUncovered) on a plan with a middle stroke of the last level moved 5 mm (the bands
    // overlap by 10 - 6.857 = 3.143 mm, so this opens a gap of 1.857 mm).
    ops::FacePlan broken = plan;
    ops::FaceStroke& moved = broken.strokes[broken.strokes.size() - 4];
    moved.from.y += 5 * kMm;
    moved.to.y += 5 * kMm;
    JobContext ctx;
    CHECK(*ops::faceUncovered(broken, ctx) > 1e9);
    auto refused = ops::faceToolpath(broken, ctx);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Internal);
}

TEST_CASE("face: the same operation gives the same bytes every time", "[cam][face][golden]")
{
    const auto first = zigzagGolden().serialize();
    for (int i = 0; i < 3; ++i) {
        CHECK(zigzagGolden().serialize() == first);
    }
}

TEST_CASE("chain: a facing toolpath through the backplot and the post host to G-code", "[cam][face][golden][post]")
{
    const FaceCase c =
        onBlock(R"({"pattern":"oneWay","stepover":7,"stepdown":0.8})", millTool(lib::ToolType::FlatEndMill, 10));
    const Toolpath faced = c.path();

    // Backplot: every move maps to vertices; the lowest point is the faced height, the highest the clearance.
    const sim::Backplot b = sim::buildBackplot(faced, Xform::identity(), 0.01);
    REQUIRE(b.moveFirstVertex.size() == faced.moves.size() + 1);
    CHECK(b.moveFirstVertex.back() == b.lineVertices.size());
    CHECK(std::abs(b.bounds.min.z - 20.0) < 1e-4);
    CHECK(std::abs(b.bounds.max.z - 36.5) < 1e-4);

    // Post with the test post.
    Setup setup;
    setup.name = "Op 1 top";
    setup.programNumber = 1052;
    setup.programComment = "T-052 facing chain";
    setup.wcs.workOffset = 1;
    lib::MachineDef mill = faceMachine();
    mill.name = "Test mill";
    mill.model = "TM-1";
    mill.controlDialect = "generic-iso";
    Operation op = c.op;
    op.name = "Face 10";
    op.tool.toolNumber = op.tool.lengthOffset = 2;
    op.cutting.coolant = CuttingData::Coolant::Flood;
    lib::Tool tool = c.tool;
    tool.name = "10 mm flat end mill";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "facing";
    job.userName = "tester";
    job.sections = {post::PostSection{&op, &faced, &tool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = goldenPath("face_chain.iso-test.nc");
    if (updating()) {
        writeFile(golden, out->text);
    }
    INFO(out->text);
    CHECK(out->text == faceReadText(golden));
    CHECK(out->text.find("G1") != std::string::npos);
    CHECK(out->text.find("G0") != std::string::npos);
}
