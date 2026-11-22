// The drilling operation's depths and feeds against hand-computed values (task T-035 acceptance: tip-angle depth
// compensation and through-hole breakthrough correct to 1 um; tapping feed equal to pitch times rpm exactly in IR
// units). The tangents are exact (tan 45 = 1, tan 60 = sqrt 3, tan 67.5 = 1 + sqrt 2) or taken to 20 digits from a
// 50-digit Taylor series (tan 59 = 1.66427948235051791103, tan 70 = 2.74747741945462227876); every expected value is
// written out with its arithmetic and rounded to whole nanometres by hand.
#include "drill_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

constexpr Nm kUm = 1000;

const kernel::Shape& block()
{
    static const kernel::Shape s = goldenBlock();
    return s;
}

Case on(double x, double y, const std::string& params, lib::Tool t)
{
    Case c;
    c.op = drillOp(params);
    c.tool = std::move(t);
    c.geom = geometryOn(block(), {holeFace(block(), x, y)});
    return c;
}

// The tip Z the operation gives the hole at (x, y), which must be within 1 um of `expected` (and is, to the nm).
void tipIs(double x, double y, const std::string& params, const lib::Tool& t, Nm expected)
{
    const Toolpath p = on(x, y, params, t).path();
    const Nm tip = firstTip(p);
    INFO("tip " << mmOf(tip) << " expected " << mmOf(expected));
    CHECK(std::abs(tip - expected) <= kUm);
    CHECK(std::abs(tip - expected) <= 1); // in fact to the nanometre: the rule rounds once
}

bool warns(const Toolpath& p, const std::string& text)
{
    for (const std::string& w : p.warnings) {
        if (w.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("the point length of a tool matches hand-computed values to the nanometre", "[cam][drill][depth]")
{
    // L = ((D - d) / 2) / tan(a / 2), rounded half to even to nm.
    struct Row {
        double d, angle, flat;
        Nm expected;
    };
    const Row rows[] = {
        {6, 118, 0, 1'802'582},   // 3 / tan 59 = 3 / 1.66427948235051791 = 1.80258185708268124 mm
        {10, 90, 0, 5'000'000},   // 5 / tan 45 = 5 / 1 = 5 mm exactly
        {8, 135, 0, 1'656'854},   // 4 / tan 67.5 = 4 / (1 + sqrt 2) = 4 (sqrt 2 - 1) = 1.65685424949238020 mm
        {12, 140, 0, 2'183'821},  // 6 / tan 70 = 6 / 2.74747741945462228 = 2.18382140559721417 mm
        {6.8, 118, 0, 2'042'926}, // 3.4 / 1.66427948235051791 = 2.04292610469370541 mm
        {5, 118, 0, 1'502'152},   // 2.5 / 1.66427948235051791 = 1.50215154756890104 mm
        {3.3, 118, 0, 991'420},   // 1.65 / 1.66427948235051791 = 0.99142002139547468 mm
        {10, 90, 0.5, 4'750'000}, // (10 - 0.5) / 2 / 1 = 4.75 mm: a spot drill with a flat at its tip
        {6, 120, 0, 1'732'051},   // 3 / tan 60 = 3 / sqrt 3 = sqrt 3 = 1.73205080756887729 mm
        {6, 180, 0, 0},           // a flat-bottom drill has no point
        {6, 0, 0, 0},             // nor has a tool without a tip angle (an end mill)
    };
    for (const Row& r : rows) {
        INFO("D " << r.d << " angle " << r.angle << " flat " << r.flat);
        CHECK(ops::pointLengthNm(r.d, r.angle, r.flat) == r.expected);
    }
}

TEST_CASE("a through hole is drilled past its exit by the breakthrough and the point length, to 1 um",
          "[cam][drill][depth]")
{
    // H1 (15, 15) runs from Z 20 out at Z 0. tip = 0 - breakthrough - L.
    tipIs(15, 15, R"({"breakthrough":1})", drill(6, 118), -1'000'000 - 1'802'582); // -2.802582: 1 + 1.802582
    tipIs(15, 15, R"({"breakthrough":0.5})", drill(6, 90), -500'000 - 3'000'000);  // -3.5: 0.5 + 3 / tan 45
    tipIs(15, 15, R"({"breakthrough":2})", drill(6, 135), -2'000'000 - 1'242'641); // 3 (sqrt 2 - 1) = 1.24264069
    tipIs(15, 15, R"({"breakthrough":1})", drill(6, 140), -1'000'000 - 1'091'911); // 3 / tan 70 = 1.09191070
    tipIs(15, 15, R"({"breakthrough":0})", drill(6, 118), -1'802'582);             // the full diameter at the exit
    tipIs(15, 15, R"({"breakthrough":1})", drill(6, 180), -1'000'000);             // a flat-bottom drill: no point
    // The default breakthrough is 1 mm (schemas/ops/drill.schema.json).
    tipIs(15, 15, "{}", drill(6, 118), -2'802'582);
    // A counterbored through hole (H4): the bore breaks out at Z 0 as well.
    tipIs(15, 35, R"({"breakthrough":1})", drill(6.6, 118), -1'000'000 - 1'982'840); // 3.3 / tan 59 = 1.98283997
}

TEST_CASE("a blind hole with a drill point is drilled to its point or to its full-diameter depth",
          "[cam][drill][depth]")
{
    // H2 (35, 15): 6 mm, full diameter down to Z 8, a 118 degree point to Z 8 - 3 / tan 59 = 6.197418 (6.19741814).
    tipIs(35, 15, "{}", drill(6, 118), 6'197'418);             // the same angle: the tip goes to the modelled point
    tipIs(35, 15, "{}", drill(5, 118), 6'197'418);             // a smaller drill of that angle, too
    tipIs(35, 15, "{}", drill(6, 140), 8'000'000 - 1'091'911); // a flatter point: full diameter at Z 8, tip 6.908089
    // A sharper point reaches the full diameter only below the modelled point: 8 - 3 / tan 45 = 5; said in a warning.
    const Toolpath p = on(35, 15, "{}", drill(6, 90)).path();
    CHECK(firstTip(p) == 5'000'000);
    CHECK(warns(p, "below the hole's modelled 118 degree point"));
}

TEST_CASE("a flat-floored blind hole stops the tip at the floor unless allowed below it", "[cam][drill][depth]")
{
    // H3 (55, 15): 8 mm to a flat floor at Z 10.
    const Toolpath stops = on(55, 15, "{}", drill(8, 118)).path();
    CHECK(firstTip(stops) == 10'000'000);
    CHECK(warns(stops, "keeps a 2.403 mm deep drill point"));                            // 4 / tan 59 = 2.40344248
    tipIs(55, 15, R"({"allowBelowFloor":true})", drill(8, 118), 10'000'000 - 2'403'442); // 7.596558
    const Toolpath flat = on(55, 15, "{}", drill(8, 180)).path(); // a flat-bottom drill fills the floor, no warning
    CHECK(firstTip(flat) == 10'000'000);
    CHECK(flat.warnings.empty());
}

TEST_CASE("an explicit depth is of the full diameter or of the tip", "[cam][drill][depth]")
{
    // H1's top is Z 20.
    tipIs(15, 15, R"({"depthFrom":"value","depth":5})", drill(6, 118), 15'000'000 - 1'802'582); // 13.197418
    tipIs(15, 15, R"({"depthFrom":"value","depth":5,"depthReference":"tip"})", drill(6, 118), 15'000'000);
    tipIs(15, 15, R"({"depthFrom":"value","depth":2.5,"cycle":"bore"})", tool(lib::ToolType::BoringBar, 6, 0),
          17'500'000);
}

TEST_CASE("a spot is as deep as its diameter needs with the spot drill's angle", "[cam][drill][depth]")
{
    // Plain H1 (6.6 mm): a 90 degree spot drill makes a 6.6 mm spot 3.3 / tan 45 = 3.3 deep.
    tipIs(15, 15, R"({"cycle":"spot"})", spotDrill(10, 90), 20'000'000 - 3'300'000);
    // Countersunk H5: the countersink's 10 mm, 5 deep.
    tipIs(35, 35, R"({"cycle":"spot"})", spotDrill(12, 90), 15'000'000);
    // A given 4 mm spot with a 120 degree spot drill: 2 / tan 60 = 2 / sqrt 3 = 1.15470054 deep.
    tipIs(15, 15, R"({"cycle":"spot","spotDiameter":4})", spotDrill(10, 120), 20'000'000 - 1'154'701);
    // A spot drill with a 0.5 mm flat: (6.6 - 0.5) / 2 / tan 45 = 3.05.
    tipIs(15, 15, R"({"cycle":"spot"})", spotDrill(10, 90, 0.5), 20'000'000 - 3'050'000);
}

TEST_CASE("a tap goes to the thread depth plus its lead, or to the drilled depth", "[cam][drill][depth][tap]")
{
    // H7 (52, 38): 5 mm blind, drilled full diameter to Z 6, no thread in the model: the tap goes to the drilled depth.
    tipIs(52, 38, R"({"cycle":"tap"})", tap(6, 1), 6'000'000);
    // H8 (70, 12) through: 0 - 1 (breakthrough) - 2 x 1 (two pitches of lead) = -3.
    tipIs(70, 12, R"({"cycle":"tap"})", tap(6, 1), -3'000'000);
    tipIs(70, 12, R"({"cycle":"tap","tapLeadPitches":3,"breakthrough":0.5})", tap(6, 1), -3'500'000);
    // A given full-thread depth of 10 from the top (Z 20) and the lead: 20 - 10 - 2 = 8.
    tipIs(52, 38, R"({"cycle":"tap","depthFrom":"value","depth":10})", tap(6, 1), 8'000'000);
    // Deeper than drilled is refused.
    const Case deep = on(52, 38, R"({"cycle":"tap","depthFrom":"value","depth":13})", tap(6, 1));
    auto v = deep.validate();
    REQUIRE_FALSE(v);
    CHECK_THAT(v.error().message, ContainsSubstring("below the drilled depth"));
}

TEST_CASE("the tapping feed is the pitch times the spindle speed exactly", "[cam][drill][tap]")
{
    // feedMicronPerMin = threadPitchNm x rpm / 1000, rounded half to even (cam.h).
    struct Row {
        Nm pitch;
        std::int64_t rpm;
        std::int64_t feed;
    };
    const Row rows[] = {
        {1'000'000, 500, 500'000},   // M6x1 at 500 rpm: 500 mm/min
        {1'250'000, 800, 1'000'000}, // M8x1.25
        {500'000, 1234, 617'000},    // M3x0.5
        {1'270'000, 600, 762'000},   // 1/4-20 UNC: 25.4 / 20 = 1.27 mm exactly
        {907'143, 1000, 907'143},    // 1/4-28 UNF: 25.4 / 28 = 0.907142857 mm, not a whole um: 907143 nm
        {907'143, 700, 635'000},     // 907143 x 700 / 1000 = 635000.1 -> 635000
        {793'750, 1001, 794'544},    // 10-32 UNF: 25.4 / 32 = 0.79375 mm; 793750 x 1001 / 1000 = 794543.75
        {793'750, 1234, 979'488},    // 979487.5: the tie goes to the even 979488
        {793'750, 1238, 982'662},    // 982662.5: the tie goes to the even 982662
        {1'411'111, 450, 635'000},   // 5/16-18 UNC: 25.4 / 18 = 1.4111111 mm; 634999.95 -> 635000
    };
    for (const Row& r : rows) {
        INFO("pitch " << r.pitch << " nm at " << r.rpm << " rpm");
        CHECK(ops::tapFeedMicronPerMin(r.pitch, r.rpm) == r.feed);
    }

    // Through the operation: the cycle carries the tap's pitch in nm and that feed, whatever the cutting feed says.
    struct Tap {
        double diameter, pitch;
        double rpm;
        Nm pitchNm;
        std::int32_t feed;
    };
    const Tap taps[] = {
        {6, 1, 500, 1'000'000, 500'000},
        {6.35, 25.4 / 20, 600, 1'270'000, 762'000},
        {6.35, 25.4 / 28, 700, 907'143, 635'000},
    };
    for (const Tap& t : taps) {
        INFO("tap " << t.diameter << " x " << t.pitch);
        Case c = on(70, 12, R"({"cycle":"tap"})", tap(t.diameter, t.pitch));
        c.op.cutting.spindleRpm = t.rpm;
        c.op.cutting.feedCut = 999;  // not used for tapping
        const Toolpath p = c.path(); // the invariant checker checks the tap feed rule as well (tapFeed)
        const Move& m = p.moves.at(1);
        CHECK(m.kind == MoveKind::CycleTap);
        CHECK(m.threadPitchNm == t.pitchNm);
        CHECK(m.feedMicronPerMin == t.feed);
    }
}
