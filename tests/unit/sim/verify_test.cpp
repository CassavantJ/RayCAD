// The safety checks (task T-071): for every event kind a known-bad program that raises exactly the expected events and
// a known-good one that raises none. The stock is a 100 x 80 x 30 block with its top at Z0 unless a case says
// otherwise; tools stand in a cylinder holder of 40 mm diameter and 50 mm gauge length.
#include "verify_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;
using Strings = std::vector<std::string>;

namespace {

Box3 stockBox()
{
    return box(0, 0, -30, 100, 80, 0);
}
// 10 mm flat end mill, 20 mm of flute, 75 long, sticking out 40 (holder nose 40 above the tip, gauge line 90).
lib::Tool toolA()
{
    return endMill(10, 20, 75);
}
Program withStock()
{
    Program p;
    p.stock = boxSoup(stockBox());
    return p;
}
bool contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("rapid into the initial stock", "[sim][verify]")
{
    SECTION("bad: a rapid down into uncut stock")
    {
        Program p = withStock();
        p.op(Path(-20, 40, 20).rapid(50, 40, -5).done(), toolA(), holder(40, 50), 40);
        sim::detail::VerifyStats stats;
        const std::vector<SimEvent> ev = verify(p, &stats);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"RapidIntoStock@0.0"});
        CHECK(contains(ev[0].message, "no earlier move has cut"));
        CHECK(!stats.cutStockRan); // settled exactly: nothing was cut there before
        // The first contact: the tool's flat end reaches the stock's top (its side is passed above the stock).
        CHECK(std::abs(ev[0].position.z) < 0.01);
        CHECK(std::abs(ev[0].position.x - 36.0) < 0.01);
    }
    SECTION("bad: a rapid through the part")
    {
        Program p = withStock();
        p.part = boxSoup(box(20, 20, -30, 80, 60, -1));
        p.op(Path(-20, 40, -3).rapid(50, 40, -3).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"RapidIntoStock@0.0"});
        CHECK(contains(ev[0].message, "the part"));
    }
    SECTION("bad: the toolpath starts inside the stock (how the tool got there is the post's)")
    {
        Program p = withStock();
        p.op(Path(50, 40, -2).rapid(50, 40, 20).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"RapidIntoStock@0.0"});
    }
    SECTION("good: rapids 5 mm above the stock")
    {
        Program p = withStock();
        p.op(Path(-20, 40, 20).rapid(50, 40, 5).rapid(120, 40, 5).done(), toolA(), holder(40, 50), 40);
        sim::detail::VerifyStats stats;
        CHECK(verify(p, &stats).empty());
        CHECK(stats.deferred == 0);
        CHECK(!stats.cutStockRan);
    }
}

TEST_CASE("rapid into cut stock, at the fixed cell", "[sim][verify]")
{
    // A slot 5 deep along Y40, entered from outside the stock; then a rapid back at Z-4.
    const auto slot = [] {
        return Path(-20, 40, 5).feed(-20, 40, -5).feed(110, 40, -5);
    };
    SECTION("bad: the rapid runs into the slot's wall")
    {
        Program p = withStock();
        p.op(slot().rapid(50, 46, -4).done(), toolA(), holder(40, 50), 40);
        sim::detail::VerifyStats stats;
        const std::vector<SimEvent> ev = verify(p, &stats);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"RapidIntoCutStock@0.2"});
        CHECK(stats.cutStockRan);
        CHECK(stats.cutStockCellMm == sim::detail::kCutStockCellMm);
        CHECK(contains(ev[0].message, "0.1 mm cells"));
    }
    SECTION("good: the rapid stays inside the slot")
    {
        Program p = withStock();
        p.op(slot().rapid(50, 40, -4).done(), toolA(), holder(40, 50), 40);
        sim::detail::VerifyStats stats;
        const std::vector<SimEvent> ev = verify(p, &stats);
        INFO(describe(ev));
        CHECK(ev.empty());
        CHECK(stats.deferred > 0); // the initial stock was there: only the cut stock tells
        CHECK(stats.cutStockRan);
    }
}

TEST_CASE("holder against stock, and the stick-out it needs", "[sim][verify]")
{
    // 40 mm of flute, sticking out 30: all of it flute, the holder's nose 30 above the tip. Beside the stock at X-6
    // (the tool clear of it), 35 deep: the holder, 20 in radius, comes down to Z-5 over the stock.
    const auto pass = [](double z) {
        return Path(-6, -30, 20).feed(-6, -30, z).feed(-6, 110, z).feed(-6, 110, 20);
    };
    SECTION("bad")
    {
        Program p = withStock();
        p.op(pass(-35).done(), endMill(10, 40, 100), holder(40, 50), 30);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"HolderHitsStock@0.1", "ToolTooShort@0.1"});
        CHECK(contains(ev[0].message, "no earlier move has cut"));
        // 5 mm lower than the stock top, plus the 2 mm clearance, rounded up to 0.1 mm.
        CHECK(ev[1].amountMm == 7.1);
        CHECK(contains(ev[1].message, "minimum safe stick-out 37.1 mm, now 30 mm"));
        CHECK(!contains(ev[1].message, "allows at most")); // 100 - 2.5 x 10 = 75
    }
    SECTION("bad: more stick-out than the tool allows")
    {
        Program p = withStock();
        p.op(pass(-35).done(), endMill(10, 40, 50), holder(40, 50), 30);
        const std::vector<SimEvent> ev = verify(p);
        REQUIRE(summary(ev, p) == Strings{"HolderHitsStock@0.1", "ToolTooShort@0.1"});
        CHECK(contains(ev[1].message, "allows at most 25 mm")); // 50 - 2.5 x 10
    }
    SECTION("good: 15 deep")
    {
        Program p = withStock();
        p.op(pass(-15).done(), endMill(10, 40, 100), holder(40, 50), 30);
        CHECK(verify(p).empty());
    }
    SECTION("bad: in material earlier moves left (cut stock)")
    {
        // A slot 35 deep along Y40 with the same tool: the holder comes down to Z-5 over the slot's walls.
        Program p = withStock();
        p.op(Path(-30, 40, 20).feed(-30, 40, -35).feed(110, 40, -35).done(), endMill(10, 40, 100), holder(40, 50), 30);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"HolderHitsStock@0.1", "ToolTooShort@0.1"});
        CHECK(contains(ev[0].message, "0.1 mm cells"));
        CHECK(ev[1].amountMm >= 7.0);
        CHECK(ev[1].amountMm <= 7.1);
    }
}

TEST_CASE("shank against stock, and the flute length it needs", "[sim][verify]")
{
    // 20 mm of flute: a slot 25 deep leaves the shank 5 mm in the stock.
    const auto slot = [](double z) {
        return Path(-20, 40, 5).feed(-20, 40, z).feed(110, 40, z);
    };
    SECTION("bad")
    {
        Program p = withStock();
        p.op(slot(-25).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"ShankHitsStock@0.1", "ToolTooShort@0.1"});
        CHECK(ev[1].amountMm >= 5.0);
        CHECK(ev[1].amountMm <= 5.1);
        CHECK(contains(ev[1].message, "flutes are too short"));
    }
    SECTION("good: 15 deep")
    {
        Program p = withStock();
        p.op(slot(-15).done(), toolA(), holder(40, 50), 40);
        CHECK(verify(p).empty());
    }
}

TEST_CASE("shank and holder against the part", "[sim][verify]")
{
    // The part stands in the stock: X20 to 80, Y20 to 60, top at Z0.
    const auto setup = [] {
        Program p = withStock();
        p.part = boxSoup(box(20, 20, -30, 80, 60, 0));
        return p;
    };
    SECTION("bad: the shank rubs the part's wall below 20 mm of flute")
    {
        Program p = setup();
        p.op(Path(15, -10, 5).feed(15, -10, -25).feed(15, 90, -25).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"ShankHitsPart@0.1", "ToolTooShort@0.1"});
        CHECK(ev[1].amountMm == 5.1);
    }
    SECTION("good: the same wall 15 deep")
    {
        Program p = setup();
        p.op(Path(15, -10, 5).feed(15, -10, -15).feed(15, 90, -15).rapid(15, 90, 20).done(), toolA(), holder(40, 50),
             40);
        CHECK(verify(p).empty());
    }
    SECTION("bad: the holder overhangs the part")
    {
        Program p = setup();
        p.op(Path(5, -30, 5).feed(5, -30, -28).feed(5, 110, -28).done(), endMill(10, 40, 100), holder(40, 50), 25);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"HolderHitsPart@0.1", "ToolTooShort@0.1"});
        CHECK(ev[1].amountMm == 5.1); // 3 mm into it, plus 2 clearance, rounded up
    }
    SECTION("good: sticking out 35")
    {
        Program p = setup();
        p.op(Path(5, -30, 5).feed(5, -30, -28).feed(5, 110, -28).done(), endMill(10, 40, 100), holder(40, 50), 35);
        CHECK(verify(p).empty());
    }
    SECTION("good: a retract straight up along the finished wall rubs it, which is no rapid into the part")
    {
        Program p;
        p.part = boxSoup(box(20, 20, -30, 80, 60, 0));
        lib::Tool necked = toolA();
        necked.neckDiameter = 8;
        necked.neckLength = 30;
        p.op(Path(15, 10, 5).feed(15, 10, -10).feed(15, 70, -10).rapid(15, 70, 20).done(), toolA(), holder(40, 50), 40);
        p.op(Path(15, 10, 5).feed(15, 10, -10).feed(15, 70, -10).rapid(15, 70, 20).done(), necked, holder(40, 50), 40);
        sim::detail::VerifyStats stats;
        const std::vector<SimEvent> ev = verify(p, &stats);
        INFO(describe(ev));
        CHECK(ev.empty());
        CHECK(stats.skippedMotions == 1); // the plain tool's retract; the necked one is checked
    }
}

TEST_CASE("tool and holder against fixtures", "[sim][verify]")
{
    // A vise jaw: X-15 to -5, the length of the part, from Z-40 up to Z5 (5 above the stock top).
    const kernel::TriSoup jaw = boxSoup(box(-15, 0, -40, -5, 80, 5));
    SECTION("bad: a rapid through the jaw")
    {
        Program p;
        p.fixtures.push_back(jaw);
        p.op(Path(50, 40, 2).rapid(-30, 40, 2).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"ToolHitsFixture@0.0"});
        CHECK(contains(ev[0].message, "fixture 1"));
    }
    SECTION("bad: the holder passes through the jaw (sticking out 12)")
    {
        Program p;
        p.fixtures.push_back(jaw);
        p.op(Path(2, -30, 20).feed(2, -30, -10).feed(2, 110, -10).feed(2, 110, 20).done(), endMill(10, 12, 60),
             holder(40, 50), 12);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"HolderHitsFixture@0.1", "ToolTooShort@0.1"});
        CHECK(ev[1].amountMm == 5.1);
    }
    SECTION("good: sticking out 40")
    {
        Program p;
        p.fixtures.push_back(jaw);
        p.op(Path(2, -30, 20).feed(2, -30, -10).feed(2, 110, -10).feed(2, 110, 20).done(), endMill(10, 12, 60),
             holder(40, 50), 40);
        CHECK(verify(p).empty());
    }
    SECTION("arcs: a circle round a post, 0.86 mm clear and 2.3 um clear (too close to call: an event)")
    {
        for (const double half : {10.0, 10.605}) {
            Program p;
            p.fixtures.push_back(boxSoup(box(50 - half, 40 - half, -30, 50 + half, 40 + half, 0)));
            p.op(Path(70, 40, -10).arc(false, 70, 40, -10, 50, 40).done(), toolA(), holder(40, 50), 40);
            const std::vector<SimEvent> ev = verify(p);
            INFO(describe(ev));
            CHECK(summary(ev, p) == (half == 10.0 ? Strings{} : Strings{"ToolHitsFixture@0.0"}));
        }
    }
}

TEST_CASE("travel limits", "[sim][verify]")
{
    lib::MachineDef m;
    m.axes = {{'X', 0, 100, 0, 0, 0}, {'Y', 0, 80, 0, 0, 0}, {'Z', -300, 0, 0, 0, 0}};
    SECTION("bad: the program is longer in X than the machine travels")
    {
        Program p;
        p.machine = m;
        p.op(Path(0, 0, 50).rapid(150, 0, 50).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"TravelLimit@0.0"});
        CHECK(ev[0].amountMm == 50);
        CHECK(contains(ev[0].message, "no work offset fits it"));
    }
    SECTION("good: it fits")
    {
        Program p;
        p.machine = m;
        p.op(Path(0, 0, 50).rapid(90, 70, 50).rapid(0, 0, 50).done(), toolA(), holder(40, 50), 40);
        CHECK(verify(p).empty());
    }
    SECTION("bad, with the work offset: 10 mm beyond X, and the gauge line 40 mm above Z")
    {
        Program p;
        p.machine = m;
        p.wcsInMachine = Vec3{20, 0, -200};
        // Gauge line = tip + 90: Z150 puts it at machine Z40.
        p.op(Path(0, 0, 50).rapid(90, 0, 50).rapid(10, 10, 50).rapid(10, 10, 150).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        REQUIRE(summary(ev, p) == Strings{"TravelLimit@0.0", "TravelLimit@0.2"});
        CHECK(ev[0].amountMm == 10);
        CHECK(ev[1].amountMm == 40);
    }
    SECTION("good, with the work offset")
    {
        Program p;
        p.machine = m;
        p.wcsInMachine = Vec3{5, 0, -200};
        p.op(Path(0, 0, 50).rapid(90, 70, 50).rapid(0, 0, 50).done(), toolA(), holder(40, 50), 40);
        CHECK(verify(p).empty());
    }
}

TEST_CASE("plunges with a tool that does not cut at its centre", "[sim][verify]")
{
    SECTION("bad: a flat end mill straight down into the stock")
    {
        Program p = withStock();
        p.op(Path(50, 40, 5).feed(50, 40, -5).done(), toolA(), holder(40, 50), 40);
        const std::vector<SimEvent> ev = verify(p);
        INFO(describe(ev));
        CHECK(summary(ev, p) == Strings{"PlungeWithNonCentreCutting@0.0"});
    }
    SECTION("good: a drill does that")
    {
        Program p = withStock();
        p.op(Path(50, 40, 5).feed(50, 40, -5).done(), endMill(10, 30, 80, lib::ToolType::Drill), holder(40, 50), 40);
        CHECK(verify(p).empty());
    }
    SECTION("ramps: 3 degrees is a ramp, 10 degrees a plunge")
    {
        for (const double deg : {3.0, 10.0}) {
            const double run = 2.5 / dmath::tan(deg * 3.14159265358979323846 / 180.0);
            Program p = withStock();
            p.op(Path(20, 40, 5).feed(20, 40, 0.5).feed(20 + run, 40, -2).done(), toolA(), holder(40, 50), 40);
            const std::vector<SimEvent> ev = verify(p);
            INFO(deg << " degrees\n" << describe(ev));
            CHECK(summary(ev, p) == (deg < 5 ? Strings{} : Strings{"PlungeWithNonCentreCutting@0.1"}));
        }
    }
    SECTION("a tap into a drilled hole is fine; without the drill it is not")
    {
        lib::Tool drill = endMill(5, 30, 70, lib::ToolType::Drill);
        lib::Tool tap = endMill(6, 20, 70, lib::ToolType::Tap);
        for (const bool drilled : {true, false}) {
            Program p = withStock();
            if (drilled) {
                p.op(Path(50, 40, 10).cycle(cam::MoveKind::CycleDrill, 50, 40, -15, 0, 2, 10).cancel().done(), drill,
                     holder(40, 50), 40);
            }
            p.op(Path(50, 40, 10).cycle(cam::MoveKind::CycleTap, 50, 40, -12, 0, 2, 10).cancel().done(), tap,
                 holder(40, 50), 40);
            const std::vector<SimEvent> ev = verify(p);
            INFO((drilled ? "drilled\n" : "not drilled\n") << describe(ev));
            CHECK(summary(ev, p) == (drilled ? Strings{} : Strings{"PlungeWithNonCentreCutting@0.0"}));
        }
    }
}

TEST_CASE("events come in runs, one per stretch of consecutive moves", "[sim][verify]")
{
    // The holder over the stock (as in the holder case) along five short moves, then clear, then again for two.
    Program p = withStock();
    Path path(-6, -30, 20);
    path.feed(-6, -30, -35);
    for (int k = 1; k <= 5; ++k) {
        path.feed(-6, 10.0 * k, -35);
    }
    path.feed(-6, 50, -15).feed(-6, 60, -15).feed(-6, 60, -35).feed(-6, 70, -35).feed(-6, 75, -35);
    p.op(path.done(), endMill(10, 40, 100), holder(40, 50), 30);
    const std::vector<SimEvent> ev = verify(p);
    INFO(describe(ev));
    // Move 6 goes straight up (no new space: not checked), 7 is clear at -15, 8 goes down again.
    REQUIRE(summary(ev, p) == Strings{"HolderHitsStock@0.1", "ToolTooShort@0.1", "HolderHitsStock@0.8"});
    CHECK(contains(ev[0].message, "the 4 after it"));
    CHECK(contains(ev[2].message, "the 2 after it"));
}

TEST_CASE("a SimProgram reaches the checks unchanged", "[sim][verify]")
{
    const std::shared_ptr<const cam::Toolpath> tp = Path(-20, 40, 20).rapid(50, 40, -5).done();
    cam::Operation op;
    op.id.value.bytes[3] = 7;
    lib::Tool t = toolA();
    lib::Holder h = holder(40, 50);
    lib::MachineDef machine;
    SimProgram sp{};
    sp.ops = {&op};
    sp.toolpaths = {tp};
    sp.tools = {makeToolShape(t, &h, 40)};
    sp.stockSolid = boxSoup(stockBox());
    sp.modelFromWcs = Xform::identity();
    sp.machine = &machine;
    sp.toolDefs = {&t};
    const sim::detail::VerifyInput in = sim::detail::verifyInputFrom(sp);
    REQUIRE(in.ops.size() == 1);
    CHECK(in.ops[0].id == op.id);
    CHECK(in.ops[0].path == tp.get());
    CHECK(in.ops[0].shape == &sp.tools[0]);
    CHECK(in.ops[0].tool == &t);
    CHECK(in.stock == &sp.stockSolid);
    CHECK(in.machine == &machine);
    JobContext ctx;
    const Result<std::vector<SimEvent>> ev = sim::detail::verifyProgram(in, jobs(), ctx);
    REQUIRE(ev.has_value());
    REQUIRE(ev->size() == 1);
    CHECK((*ev)[0].kind == EventKind::RapidIntoStock);
    CHECK((*ev)[0].operation == op.id);
}

TEST_CASE("a cancelled check returns Cancelled", "[sim][verify]")
{
    Program p = withStock();
    p.op(Path(-20, 40, 20).rapid(50, 40, -5).done(), toolA(), holder(40, 50), 40);
    const sim::detail::VerifyInput in = p.input();
    CancelSource src;
    src.cancel();
    JobContext ctx{src.token(), {}};
    const Result<std::vector<SimEvent>> r = sim::detail::verifyProgram(in, jobs(), ctx);
    REQUIRE(!r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
}
