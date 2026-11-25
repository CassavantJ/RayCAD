// Golden toolpaths of the slot and chamfer2d operations (task T-055 acceptance: goldens plus invariants), on the
// kernel-built blocks (slot_chamfer_test_util.h) and on corpus parts, every one also through the invariant checker,
// and one chain from both operations through the backplot (sim::buildBackplot) and the post host with the test post
// (tests/unit/post/posts/iso-test.post.js) to a committed G-code golden.
//
// Each golden is tests/golden/toolpaths/<name>.rctp with <name>.txt next to it (toolpath_dump.h). The test checks that
// a fresh toolpath gives both files byte for byte and that the two agree. To make them again after a reviewed change:
// set RAY_UPDATE_TOOLPATH_GOLDENS=1 and run ray_cam_slot_chamfer_tests "[golden]", then read the .txt diff.
#include "slot_chamfer_test_util.h"

#include <ray/post.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <optional>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

const kernel::Shape& slots()
{
    static const kernel::Shape s = slotBlock();
    return s;
}
const kernel::Shape& chamfers()
{
    static const kernel::Shape s = chamferBlock();
    return s;
}

kernel::Shape corpus(const std::string& file)
{
    JobContext ctx;
    auto imported = kernel::importStep(std::string(RAY_CORPUS_DIR) + "/raycad-synthetic/" + file, {}, ctx);
    REQUIRE(imported);
    REQUIRE(imported->bodies.size() == 1);
    return imported->bodies[0].shape;
}

Case slotCase(const kernel::Shape& part, kernel::SubId floor, double d, const std::string& params)
{
    Case c;
    c.op = millOp("slot", params);
    c.tool = endMill(d);
    c.geom = geometryOn(part, {floor});
    return c;
}

const auto kSlotClaims = [](double rampAngle) {
    return [rampAngle](invariants::Claims& k) {
        k.maxRampAngleDeg = rampAngle;
    };
};

Toolpath straightSlot()
{
    return slotCase(slots(), floorAt(slots(), 35, 12, 14), 8, R"({"stepDown":2.5,"rampAngle":4})").path(kSlotClaims(4));
}

std::vector<kernel::SubId> blockEdges()
{
    // The outside, the rounded pocket and the hole of the chamfer block.
    std::vector<kernel::SubId> out;
    for (const auto& box : {std::array<double, 4>{0, 0, 80, 0}, std::array<double, 4>{80, 0, 80, 50},
                            std::array<double, 4>{0, 50, 80, 50}, std::array<double, 4>{0, 0, 0, 50},
                            std::array<double, 4>{10, 10, 40, 40}, std::array<double, 4>{54.9, 9.9, 65.1, 20.1}}) {
        const auto e = edgesAt(chamfers(), 20, box[0], box[1], box[2], box[3]);
        out.insert(out.end(), e.begin(), e.end());
    }
    return out;
}

Toolpath blockChamfer()
{
    const std::vector<kernel::SubId> edges = blockEdges();
    Case c;
    c.op = millOp("chamfer2d", R"({"width":0.6,"tipOffset":0.4})", 9000, 900);
    c.tool = chamferMill(6.35, 90, 0.1);
    c.geom = geometryOn(chamfers(), edges);
    return c.path([&](invariants::Claims& k) {
        chamferClaims(k, chamfers(), edges, 0.6, 1.0);
        k.initialStock = invariants::InitialStock::Part;
        k.direction = invariants::Direction::Climb;
    });
}

} // namespace

TEST_CASE("golden: a straight slot in depths of 2 mm, ramped at 4 degrees", "[cam][slot][golden]")
{
    const Toolpath p = straightSlot();
    matchesGolden("slot_straight", p);
    CHECK(movesOf(p, MoveRole::Ramp).size() >= 6); // two legs (or more) per depth, three depths
}

TEST_CASE("golden: an arc slot", "[cam][slot][golden]")
{
    const Toolpath p =
        slotCase(slots(),
                 floorAt(slots(), 70 + 15 * std::cos(145 * kPi / 180), 25 + 15 * std::sin(145 * kPi / 180), 16), 6,
                 R"({"stepDown":2})")
            .path(kSlotClaims(3));
    matchesGolden("slot_arc", p);
    bool arcs = false;
    for (const Move& m : movesOf(p, MoveRole::Cut)) {
        arcs = arcs || m.kind != MoveKind::Linear;
    }
    CHECK(arcs);
}

TEST_CASE("golden: a slot open at one end, run out past it", "[cam][slot][golden]")
{
    const Toolpath p = slotCase(slots(), floorAt(slots(), 90, 50, 15), 10, R"({"stepDown":2.5,"floorStock":0.2})")
                           .path(kSlotClaims(3));
    matchesGolden("slot_open", p);
    Nm lowest = 0;
    for (const Move& m : p.moves) {
        lowest = std::min(lowest == 0 ? m.z : lowest, m.z);
    }
    CHECK(lowest == 15'200'000); // the floor, plus the floor stock
}

TEST_CASE("golden: a slot too short for a ramp, plunged into at each depth (after M3)", "[cam][slot][golden]")
{
    // shortSlotBlock: a centre line of 4 mm, shorter than the ramp's run, with an 8 mm end mill that cuts at its centre
    // (its type's default): a straight plunge at each of Z 10, 8 and 6 (plunge_entry.h), with a warning.
    static const kernel::Shape part = shortSlotBlock();
    const Toolpath p = slotCase(part, floorAt(part, 17, 15, 6), 8, R"({"stepDown":2.5})").path(kSlotClaims(3));
    matchesGolden("slot_plunge", p);
    CHECK(movesOf(p, MoveRole::Plunge).size() == 3);
    CHECK(movesOf(p, MoveRole::Ramp).empty());
    REQUIRE(p.warnings.size() == 1);
    CHECK_THAT(p.warnings[0], ContainsSubstring("Plunges straight down at 3 places"));
}

TEST_CASE("golden: the keyway of a corpus shaft", "[cam][slot][golden][corpus]")
{
    // keyway_tslot_1: a 25 mm shaft along X, 80 long, with a blind keyway 8 wide and 32 long at X 40, its floor at
    // Z 8.5, 4 mm below the top of the shaft (tools/corpus_gen/corpus_gen.cpp).
    const kernel::Shape shaft = corpus("keyway_tslot_1.stp");
    const Toolpath p = slotCase(shaft, floorAt(shaft, 40, 0, 8.5), 8, R"({"stepDown":1.5})").path(kSlotClaims(3));
    matchesGolden("slot_keyway_corpus", p);
    // Depths 11.167, 9.833, 8.5 (4 mm in ceil(4 / 1.5) = 3 steps from the stock top, Z 12.5).
    std::vector<Nm> depths;
    for (const Move& m : movesOf(p, MoveRole::Cut)) {
        if (depths.empty() || depths.back() != m.z) {
            depths.push_back(m.z);
        }
    }
    CHECK(depths == std::vector<Nm>{11'166'667, 9'833'333, 8'500'000});
}

TEST_CASE("corpus: the foot of a T-slot is refused (its throat stands over it)", "[cam][slot][corpus]")
{
    // keyway_tslot_2: a block with a T-slot through along X. Its foot's floor, cut as a centre-line slot of its own
    // width, would cut the throat's shoulders.
    const kernel::Shape block = corpus("keyway_tslot_2.stp");
    const Box3 b = kernel::bounds(block);
    // The lowest up-facing floor inside the block: the foot's.
    std::optional<kernel::SubId> foot;
    double footZ = 1e9;
    for (std::uint32_t i = 0; i < block.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(block, f);
        const auto sample = kernel::faceSample(block, f);
        if (info && sample && info->type == kernel::SurfaceType::Plane && sample->normal.z > 0.999 &&
            info->bounds.min.z > b.min.z + 1e-6 && info->bounds.min.z < footZ) {
            foot = f;
            footZ = info->bounds.min.z;
        }
    }
    REQUIRE(foot);
    const auto info = kernel::surfaceInfo(block, *foot);
    REQUIRE(info);
    Case c;
    c.op = millOp("slot", "{}");
    c.tool = endMill(info->bounds.max.y - info->bounds.min.y);
    c.geom = geometryOn(block, {*foot});
    const auto gen = c.generate();
    REQUIRE_FALSE(gen);
    CHECK_THAT(gen.error().message, ContainsSubstring("the slot is narrower there than at its floor, or something "
                                                      "stands over it"));
}

TEST_CASE("golden: chamfering the outside, a pocket and a hole of the block", "[cam][chamfer][golden]")
{
    matchesGolden("chamfer2d_block", blockChamfer());
}

TEST_CASE("golden: a spot drill chamfering the outside and the counterbores of a corpus plate",
          "[cam][chamfer][golden][corpus]")
{
    // plate_counterbore_1: four counterbored through holes, 11 mm counterbores from the top at Z 15
    // (tests/unit/cam/holes_labels.txt).
    const kernel::Shape plate = corpus("plate_counterbore_1.stp");
    const Box3 b = kernel::bounds(plate);
    std::vector<kernel::SubId> edges = edgesAt(plate, b.max.z, b.min.x, b.min.y, b.max.x, b.max.y);
    REQUIRE(edges.size() >= 8); // the four sides and the four counterbores (circles may come in two halves)
    lib::Tool spot = chamferMill(10, 90);
    spot.type = lib::ToolType::SpotDrill;
    Case c;
    c.op = millOp("chamfer2d", R"({"width":0.5,"tipOffset":0.5})", 5000, 500);
    c.tool = spot;
    c.geom = geometryOn(plate, edges);
    const Toolpath p = c.path([&](invariants::Claims& k) {
        chamferClaims(k, plate, edges, 0.5, 1.0);
        k.initialStock = invariants::InitialStock::Part;
    });
    matchesGolden("chamfer2d_spot_corpus", p);
    CHECK(movesOf(p, MoveRole::Plunge).size() == 5); // one per loop: the outside and the four counterbores
}

TEST_CASE("chain: a slot and a chamfer through the backplot and the post host to G-code",
          "[cam][slot][chamfer][golden][post]")
{
    const Toolpath slotted = straightSlot();
    const Toolpath chamfered = blockChamfer();
    for (const Toolpath* p : {&slotted, &chamfered}) {
        const sim::Backplot b = sim::buildBackplot(*p, Xform::identity(), 0.01);
        REQUIRE(b.moveFirstVertex.size() == p->moves.size() + 1);
        CHECK(b.moveFirstVertex.back() == b.lineVertices.size());
        float lowest = 1e9F;
        for (const sim::BackplotVertex& v : b.lineVertices) {
            lowest = std::min(lowest, v.z);
        }
        CHECK(std::abs(static_cast<double>(lowest) - p->stats.minZ) < 1e-4);
    }
    Setup setup;
    setup.name = "Op 1 top";
    setup.programNumber = 1055;
    setup.programComment = "T-055 slot and chamfer chain";
    setup.wcs.workOffset = 1;
    lib::MachineDef mill = machine();
    mill.name = "Test mill";
    mill.model = "TM-1";
    mill.controlDialect = "generic-iso";
    Operation slotOp = millOp("slot", R"({"stepDown":2.5,"rampAngle":4})");
    slotOp.name = "Slot 8";
    slotOp.tool.toolNumber = slotOp.tool.lengthOffset = 4;
    slotOp.cutting.coolant = CuttingData::Coolant::Flood;
    Operation chamferOp = millOp("chamfer2d", R"({"width":0.6,"tipOffset":0.4})", 9000, 900);
    chamferOp.name = "Chamfer";
    chamferOp.tool.toolNumber = chamferOp.tool.lengthOffset = 9;
    lib::Tool slotTool = endMill(8);
    slotTool.name = "8 mm end mill";
    lib::Tool chamferTool = chamferMill(6.35, 90, 0.1);
    chamferTool.name = "90 degree chamfer mill";
    const lib::Holder holder{{}, "ER32", "", {}, 60, "", ""};
    post::PostJob job;
    job.setup = &setup;
    job.machine = &mill;
    job.outputUnits = UnitSystem::Metric;
    job.documentName = "slot and chamfer";
    job.userName = "tester";
    job.sections = {post::PostSection{&slotOp, &slotted, &slotTool, &holder},
                    post::PostSection{&chamferOp, &chamfered, &chamferTool, &holder}};
    auto engine = post::makeQuickJsPostEngine();
    REQUIRE(engine);
    JobContext ctx;
    auto out = engine->run(std::string(RAY_POST_TEST_DIR) + "/posts/iso-test.post.js", job, ctx);
    if (!out) {
        FAIL(out.error().message);
    }
    const std::string golden = std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/slot_chamfer_chain.iso-test.nc";
    if (updatingGoldens()) {
        writeFile(golden, out->text);
    }
    INFO(out->text);
    CHECK(out->text == readText(golden));
    CHECK(out->text.find("G2") != std::string::npos); // the chamfer's corners and the hole
}
