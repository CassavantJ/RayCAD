// The safety checks raise nothing on toolpaths the M3/M4 operation types generate (task T-071): the golden toolpaths of
// tests/golden/toolpaths whose setups are known exactly (their tests' blocks, stock = the part's box, the tools of
// tests/unit/cam/pocket2d_test_util.h and drill_test_util.h, no holder). Helices, ramps, stay-down and lifted links,
// retracts, plunges into pre-drilled holes (given here by a drilling operation before them, as a shop would run it).
#include "verify_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

std::shared_ptr<const cam::Toolpath> golden(const std::string& name)
{
    std::ifstream f(std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/" + name + ".rctp", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(bytes.empty());
    Result<cam::Toolpath> tp =
        cam::Toolpath::deserialize(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    REQUIRE(tp.has_value());
    return std::make_shared<const cam::Toolpath>(std::move(*tp));
}

// pocket2d_test_util.h's end mill: flute 25 (unless given), shank the same, 40 longer overall.
lib::Tool camEndMill(double diameter)
{
    lib::Tool t = endMill(diameter, 25, 65);
    t.shoulderLength = 25;
    // Stated not to cut at its centre (the operations plan as if it did not: pocket2d, slot, adaptive2d, ... link with
    // centreCutting false), so a straight plunge into material by any of them would still be an event here.
    t.centreCutting = false;
    return t;
}
// drill_test_util.h's drill.
lib::Tool camDrill(double diameter)
{
    lib::Tool t = endMill(diameter, 40, 80, lib::ToolType::Drill);
    t.tipAngleDeg = 118;
    return t;
}

lib::MachineDef camMachine()
{
    lib::MachineDef m;
    m.axes = {{'X', 0, 762, 0, 0, 0}, {'Y', 0, 406, 0, 0, 0}, {'Z', -508, 0, 0, 0, 0}};
    return m;
}

struct GoldenCase {
    const char* name;
    Box3 stock;
    double toolDiameter;
    // A pre-drilled hole the toolpath enters by: drilled first at (x, y) down to z with a drill of this diameter.
    double drillX = 0, drillY = 0, drillZ = 0, drillDiameter = 0;
};

} // namespace

TEST_CASE("generated toolpaths raise no events", "[sim][verify]")
{
    const GoldenCase cases[] = {
        {"adaptive2d_islands", box(0, 0, 0, 40, 30, 20), 6, 0, 0, 0, 0},
        {"adaptive2d_ramp", box(0, 0, 0, 50, 20, 10), 6, 0, 0, 0, 0},
        {"adaptive2d_open", box(0, 0, 0, 40, 30, 12), 6, 0, 0, 0, 0},
        {"adaptive2d_predrill", box(0, 0, 0, 60, 40, 15), 6, 20, 15, 11, 9},
        {"pocket2d_finish", box(0, 0, 0, 60, 40, 15), 10, 0, 0, 0, 0},
        {"pocket2d_predrill", box(0, 0, 0, 60, 40, 15), 8, 30, 20, 7, 9},
    };
    for (const GoldenCase& c : cases) {
        for (const bool withHolder : {false, true}) {
            Program p;
            p.stock = boxSoup(c.stock);
            p.machine = camMachine();
            if (c.drillDiameter > 0) {
                p.op(Path(c.drillX, c.drillY, 40)
                         .cycle(cam::MoveKind::CycleDrill, c.drillX, c.drillY, c.drillZ, c.stock.max.z,
                                c.stock.max.z + 2, 40)
                         .cancel()
                         .done(),
                     camDrill(c.drillDiameter));
            }
            // With a holder: a 30 mm one with the tool's 25 mm of flute and 5 mm of shank out of it.
            if (withHolder) {
                p.op(golden(c.name), camEndMill(c.toolDiameter), holder(30, 40), 30);
            } else {
                p.op(golden(c.name), camEndMill(c.toolDiameter));
            }
            sim::detail::VerifyStats stats;
            const std::vector<SimEvent> ev = verify(p, &stats);
            INFO(c.name << (withHolder ? " in a holder" : "") << ": " << stats.motions << " motions, " << stats.deferred
                        << " deferred\n"
                        << describe(ev));
            CHECK(ev.empty());
        }
    }
}

TEST_CASE("without the pre-drilled hole, the plunge into it is an event", "[sim][verify]")
{
    Program p;
    p.stock = boxSoup(box(0, 0, 0, 60, 40, 15));
    p.op(golden("pocket2d_predrill"), camEndMill(8));
    const std::vector<SimEvent> ev = verify(p);
    INFO(describe(ev));
    REQUIRE(!ev.empty());
    for (const SimEvent& e : ev) {
        CHECK(e.kind == EventKind::PlungeWithNonCentreCutting);
        CHECK(std::abs(e.position.x - 30) < 1e-6);
        CHECK(std::abs(e.position.y - 20) < 1e-6);
    }
}
