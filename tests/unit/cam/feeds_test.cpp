// Feeds and speeds (cam.h computeFeeds, task T-048).
//
//  - 39 hand-calculated cases. Each states its inputs and the arithmetic, step by step, with the rules of
//    src/cam/feeds.cpp: n = 1000 vc / (pi Deff) rounded down to a whole rpm; vf = n fz z x thinning, rounded down to a
//    whole um/min; Q = ae ap vf / 1000 (drilling: pi/4 D^2 vf / 1000); Pc = Q kc W / 60000 with the tool wear factor
//    W = 1.2; motor power P = Pc / 0.8; torque T = Pc 60000 / (2 pi n). The expected values were worked out by hand
//    (checked with a calculator), not with computeFeeds, and must match to 0.1 %.
//  - Property tests: feed is linear in chip load until a clamp; no known machine limit is ever exceeded (a
//    deterministic sweep of 4000 random-looking inputs); the radial chip thinning factor is 1 at and above half the
//    diameter and grows monotonically below it.
//  - Input validation: errors, documented fallbacks, never NaN or infinity.
//  - The shipped library (resources/library) on its three machines, and a pinned hash of every combination's rpm
//    and feed (the values that reach toolpaths), the same on every platform.
#include <ray/cam.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

const MaterialId kMat{Uuid::fromString("11111111-1111-4111-8111-111111111111")};
const MaterialId kOtherMat{Uuid::fromString("22222222-2222-4222-8222-222222222222")};

lib::Material material(double kc)
{
    lib::Material m{};
    m.id = kMat;
    m.name = "test steel";
    m.group = "Steel";
    m.hardnessHb = 200;
    m.specificCuttingForceNmm2 = kc;
    m.densityGcm3 = 7.85;
    return m;
}

lib::ToolPreset preset(std::string opClass, double vc, double fz, double radial = 1, double axial = 1)
{
    return lib::ToolPreset{kMat, std::move(opClass), vc, fz, radial, axial, 0};
}

lib::Tool tool(lib::ToolType type, double d, int flutes)
{
    lib::Tool t{};
    t.id = ToolId{Uuid::fromString("33333333-3333-4333-8333-333333333333")};
    t.name = "test tool";
    t.type = type;
    t.flutes = flutes;
    t.diameter = d;
    t.shankDiameter = d;
    t.fluteLength = 4 * d;
    t.shoulderLength = 4 * d;
    t.overallLength = 8 * d;
    t.bodyMaterial = "carbide";
    return t;
}

// A tool with one preset of the given class.
lib::Tool tool(lib::ToolType type, double d, int flutes, const std::string& opClass, double vc, double fz)
{
    lib::Tool t = tool(type, d, flutes);
    t.presets.push_back(preset(opClass, vc, fz));
    return t;
}

lib::Tool tapTool(double d, double pitch, double vc)
{
    lib::Tool t = tool(lib::ToolType::Tap, d, 3);
    t.threadPitch = pitch;
    t.presets.push_back(preset("tap", vc, 0));
    return t;
}

struct Limits {
    double maxRpm = 0, minRpm = 0, powerKw = 0, torqueNm = 0, feedXY = 0, feedZ = 0;
    bool rigidTapping = true;
};
// Known limits that the synthetic cases never reach.
const Limits kBig{60000, 0, 500, 5000, 100000, 100000};

lib::MachineDef machine(const Limits& l)
{
    lib::MachineDef m{};
    m.name = "test machine";
    m.kind = lib::MachineDef::Kind::Mill3Axis;
    m.axes = {lib::AxisDef{'X', 0, 500, 0, l.feedXY, 0}, lib::AxisDef{'Y', 0, 400, 0, l.feedXY, 0},
              lib::AxisDef{'Z', 0, 300, 0, l.feedZ, 0}};
    m.maxRpm = l.maxRpm;
    m.minRpm = l.minRpm;
    m.spindlePowerKw = l.powerKw;
    m.spindleTorqueNm = l.torqueNm;
    m.rigidTapping = l.rigidTapping;
    return m;
}

FeedsResult run(const lib::Tool& t, const lib::Material& mat, const lib::MachineDef& m, double ae, double ap,
                std::string_view opType, std::string_view opClass = {})
{
    auto r = computeFeeds(FeedsInput{.tool = t,
                                     .material = mat,
                                     .machine = m,
                                     .radialEngagementMm = ae,
                                     .axialDepthMm = ap,
                                     .opType = opType,
                                     .opClass = opClass});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    // Invariants of every result.
    CHECK(r->rpm == std::floor(r->rpm));
    CHECK(static_cast<double>(r->feedMicronPerMin) / 1000 == r->feedMmMin);
    return *r;
}

bool hasNote(const FeedsResult& r, std::string_view text)
{
    return std::any_of(r.notes.begin(), r.notes.end(),
                       [&](const std::string& n) { return n.find(text) != std::string::npos; });
}

#define CHECK_REL(actual, expected) CHECK_THAT((actual), WithinRel((expected), 1e-3))

} // namespace

// =====================================================================================================================
// Hand-calculated cases. Material kc1.1 = 2000 N/mm2 unless stated. Machine kBig (limits known, never reached) unless
// stated. W = 1.2, eta = 0.8.
// =====================================================================================================================

TEST_CASE("feeds hand 1: flat end mill slotting", "[cam][feeds][hand]")
{
    // 10 mm 4FL flat, slot ae = 10, ap = 5, vc = 100 m/min, fz = 0.05 mm.
    // n = 100000 / (pi 10) = 3183.0989 -> 3183 rpm. ae >= D/2: thinning 1.
    // vf = 3183 x 0.05 x 4 = 636.6 mm/min. Q = 10 x 5 x 636.6 / 1000 = 31.83 cm3/min.
    // Pc = 31.83 x 2000 x 1.2 / 60000 = 1.2732 kW; P = 1.2732 / 0.8 = 1.5915 kW;
    // T = 1.2732 x 60000 / (2 pi 3183) = 3.81972 N m.
    auto r =
        run(tool(lib::ToolType::FlatEndMill, 10, 4, "slot", 100, 0.05), material(2000), machine(kBig), 10, 5, "slot");
    CHECK(r.mode == FeedsMode::Milling);
    CHECK(r.opClass == "slot");
    CHECK(r.rpm == 3183);
    CHECK_REL(r.feedMmMin, 636.6);
    CHECK(r.feedMicronPerMin == 636600);
    CHECK_REL(r.chipLoadMm, 0.05);
    CHECK_REL(r.effectiveChipLoadMm, 0.05);
    CHECK(r.chipThinningFactor == 1);
    CHECK_REL(r.mrrCm3Min, 31.83);
    CHECK_REL(r.cuttingPowerKw, 1.2732);
    CHECK_REL(r.powerKw, 1.5915);
    CHECK_REL(r.torqueNm, 3.81972);
    CHECK_REL(r.actualSurfaceSpeedMMin, 99.997); // pi x 10 x 3183 / 1000
    CHECK_REL(r.feedPerRevMm, 0.2);
    CHECK(r.powerModel == PowerModel::MillingKc11);
    CHECK_FALSE((r.limitedByRpm || r.limitedByFeed || r.limitedByPower || r.limitedByTorque || r.limitedByMinRpm));
    CHECK(r.limits.maxRpm.state == LimitState::NotReached);
    CHECK(r.limits.power.state == LimitState::NotReached);
}

TEST_CASE("feeds hand 2: light radial engagement, ae = 0.2 D", "[cam][feeds][hand]")
{
    // 10 mm 4FL, ae = 2, ap = 10, vc 100, fz 0.05. n = 3183.
    // thinning = D / (2 sqrt(ae (D - ae))) = 10 / (2 sqrt(2 x 8)) = 10 / 8 = 1.25.
    // fz programmed = 0.05 x 1.25 = 0.0625; vf = 3183 x 0.0625 x 4 = 795.75 mm/min; hex = 0.05.
    // Q = 2 x 10 x 795.75 / 1000 = 15.915; Pc = 15.915 x 2000 x 1.2 / 60000 = 0.6366 kW; P = 0.79575 kW;
    // T = 0.6366 x 60000 / (2 pi 3183) = 1.909859 N m.
    auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05), material(2000), machine(kBig), 2, 10,
                 "pocket2d");
    CHECK(r.rpm == 3183);
    CHECK_REL(r.radialThinningFactor, 1.25);
    CHECK_REL(r.feedMmMin, 795.75);
    CHECK_REL(r.chipLoadMm, 0.0625);
    CHECK_REL(r.effectiveChipLoadMm, 0.05);
    CHECK_REL(r.mrrCm3Min, 15.915);
    CHECK_REL(r.powerKw, 0.79575);
    CHECK_REL(r.torqueNm, 1.909859);
}

TEST_CASE("feeds hand 3: ae = 0.1 D", "[cam][feeds][hand]")
{
    // 10 mm 4FL, ae = 1, ap = 10. thinning = 10 / (2 sqrt(1 x 9)) = 10 / 6 = 1.666667.
    // vf = 3183 x 0.05 x 1.666667 x 4 = 1061.0 mm/min. Q = 1 x 10 x 1061 / 1000 = 10.61;
    // P = 10.61 x 2000 x 1.2 / 60000 / 0.8 = 0.5305 kW.
    auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05), material(2000), machine(kBig), 1, 10,
                 "pocket2d");
    CHECK_REL(r.radialThinningFactor, 1.666667);
    CHECK_REL(r.feedMmMin, 1061.0);
    CHECK_REL(r.chipLoadMm, 0.083333);
    CHECK_REL(r.mrrCm3Min, 10.61);
    CHECK_REL(r.powerKw, 0.5305);
}

TEST_CASE("feeds hand 4: ae exactly half the diameter has no thinning", "[cam][feeds][hand]")
{
    // 10 mm 4FL, ae = 5 = D/2: hex = fz, thinning 1. vf = 3183 x 0.05 x 4 = 636.6.
    auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05), material(2000), machine(kBig), 5, 10,
                 "pocket2d");
    CHECK(r.radialThinningFactor == 1);
    CHECK_REL(r.feedMmMin, 636.6);
}

TEST_CASE("feeds hand 5: very light engagement is capped at 5", "[cam][feeds][hand]")
{
    // 10 mm 4FL, ae = 0.1 (0.01 D). thinning = 10 / (2 sqrt(0.1 x 9.9)) = 10 / 1.989975 = 5.025189 -> capped 5.
    // vf = 3183 x 0.05 x 5 x 4 = 3183.0 mm/min; hex = 0.25 / 5 = 0.05.
    auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05), material(2000), machine(kBig), 0.1, 10,
                 "contour2d");
    CHECK_REL(r.radialThinningFactor, 5.025189);
    CHECK(r.chipThinningFactor == 5);
    CHECK(r.thinningCapped);
    CHECK_REL(r.feedMmMin, 3183.0);
    CHECK_REL(r.chipLoadMm, 0.25);
    CHECK_REL(r.effectiveChipLoadMm, 0.05);
    CHECK(hasNote(r, "capped"));
}

TEST_CASE("feeds hand 6: ball end mill at shallow depth and light stepover", "[cam][feeds][hand]")
{
    // 6 mm 2FL ball, ap = 0.5, ae = 0.3, vc 150, fz 0.04.
    // Deff = 2 sqrt(ap (D - ap)) = 2 sqrt(0.5 x 5.5) = 3.316625 mm. lead factor = D / Deff = 1.809068.
    // radial (on Deff): 0.3 < 1.658: 3.316625 / (2 sqrt(0.3 x 3.016625)) = 3.316625 / 1.902619 = 1.743191.
    // total = 1.809068 x 1.743191 = 3.153552.
    // n = 150000 / (pi 3.316625) = 14396.106 -> 14396. vf = 14396 x 0.04 x 3.153552 x 2 = 3631.882 mm/min.
    // fz = 3631.882 / (14396 x 2) = 0.126142; hex = 0.04.
    lib::Tool t = tool(lib::ToolType::BallEndMill, 6, 2, "finish", 150, 0.04);
    t.cornerRadius = 3;
    auto r = run(t, material(2000), machine(kBig), 0.3, 0.5, "contour2d", "finish");
    CHECK_REL(r.effectiveDiameterMm, 3.316625);
    CHECK_REL(r.leadThinningFactor, 1.809068);
    CHECK_REL(r.radialThinningFactor, 1.743191);
    CHECK_REL(r.chipThinningFactor, 3.153552);
    CHECK(r.rpm == 14396);
    CHECK_REL(r.feedMmMin, 3631.882);
    CHECK_REL(r.chipLoadMm, 0.126142);
    CHECK_REL(r.effectiveChipLoadMm, 0.04);
    CHECK_REL(r.actualSurfaceSpeedMMin, 149.9989); // pi x 3.316625 x 14396 / 1000
}

TEST_CASE("feeds hand 7: ball end mill at depth R uses the full diameter", "[cam][feeds][hand]")
{
    // 6 mm 2FL ball, ap = 3 = R, slot ae = 6. Deff = D = 6, lead 1, radial 1.
    // n = 150000 / (pi 6) = 7957.747 -> 7957; vf = 7957 x 0.04 x 2 = 636.56. Q = 6 x 3 x 636.56 / 1000 = 11.45808.
    lib::Tool t = tool(lib::ToolType::BallEndMill, 6, 2, "slot", 150, 0.04);
    t.cornerRadius = 3;
    auto r = run(t, material(2000), machine(kBig), 6, 3, "slot");
    CHECK(r.effectiveDiameterMm == 6);
    CHECK(r.chipThinningFactor == 1);
    CHECK(r.rpm == 7957);
    CHECK_REL(r.feedMmMin, 636.56);
    CHECK_REL(r.mrrCm3Min, 11.45808);
}

TEST_CASE("feeds hand 8: corner-radius end mill cutting with its corner", "[cam][feeds][hand]")
{
    // 12.7 mm (1/2 in) 4FL, r = 0.762 (0.030 in), ap = 0.3 < r, ae = 6.35, vc 200, fz 0.05.
    // s = sqrt(ap (2r - ap)) = sqrt(0.3 x 1.224) = 0.605970. Deff = D - 2r + 2s = 12.7 - 1.524 + 1.211940 = 12.387940.
    // lead = r / s = 0.762 / 0.605970 = 1.257487. radial: ae 6.35 >= Deff/2 = 6.19397: 1.
    // n = 200000 / (pi 12.38794) = 5139.028 -> 5139. vf = 5139 x 0.05 x 1.257487 x 4 = 1292.445 mm/min.
    lib::Tool t = tool(lib::ToolType::BullEndMill, 12.7, 4, "finish", 200, 0.05);
    t.cornerRadius = 0.762;
    auto r = run(t, material(2000), machine(kBig), 6.35, 0.3, "face", "finish");
    CHECK_REL(r.effectiveDiameterMm, 12.387941);
    CHECK_REL(r.leadThinningFactor, 1.257487);
    CHECK(r.radialThinningFactor == 1);
    CHECK(r.rpm == 5139);
    CHECK_REL(r.feedMmMin, 1292.445);
}

TEST_CASE("feeds hand 9: corner-radius end mill below its corner, light engagement", "[cam][feeds][hand]")
{
    // 12.7 mm 4FL, r = 0.762, ap = 5 >= r: Deff = 12.7, lead 1. ae = 1.27 (0.1 D):
    // radial = 12.7 / (2 sqrt(1.27 x 11.43)) = 12.7 / (2 x 3.81) = 1.666667.
    // n = 200000 / (pi 12.7) = 5012.754 -> 5012. vf = 5012 x 0.05 x 1.666667 x 4 = 1670.666 mm/min.
    lib::Tool t = tool(lib::ToolType::BullEndMill, 12.7, 4, "rough", 200, 0.05);
    t.cornerRadius = 0.762;
    auto r = run(t, material(2000), machine(kBig), 1.27, 5, "contour2d");
    CHECK(r.effectiveDiameterMm == 12.7);
    CHECK(r.leadThinningFactor == 1);
    CHECK_REL(r.radialThinningFactor, 1.666667);
    CHECK(r.rpm == 5012);
    CHECK_REL(r.feedMmMin, 1670.666);
}

TEST_CASE("feeds hand 10: face mill", "[cam][feeds][hand]")
{
    // 50.8 mm (2 in) 4-insert face mill, ae = 38.1, ap = 2, vc 180, fz 0.1, kc 1700.
    // n = 180000 / (pi 50.8) = 1127.870 -> 1127. Centred cut: no thinning. vf = 1127 x 0.1 x 4 = 450.8.
    // Q = 38.1 x 2 x 450.8 / 1000 = 34.35096; Pc = 34.35096 x 1700 x 1.2 / 60000 = 1.167933 kW; P = 1.459916;
    // T = 1.167933 x 60000 / (2 pi 1127) = 9.896127 N m.
    auto r =
        run(tool(lib::ToolType::FaceMill, 50.8, 4, "face", 180, 0.1), material(1700), machine(kBig), 38.1, 2, "face");
    CHECK(r.rpm == 1127);
    CHECK(r.chipThinningFactor == 1);
    CHECK_REL(r.feedMmMin, 450.8);
    CHECK_REL(r.mrrCm3Min, 34.35096);
    CHECK_REL(r.powerKw, 1.459916);
    CHECK_REL(r.torqueNm, 9.896127);
    CHECK(hasNote(r, "Face mill"));
}

TEST_CASE("feeds hand 11: twist drill", "[cam][feeds][hand]")
{
    // 10 mm 2FL drill, vc 24, fz 0.075 per flute (f = 0.15 mm/rev), kc 1700.
    // n = 24000 / (pi 10) = 763.944 -> 763. vf = 763 x 0.075 x 2 = 114.45 mm/min.
    // Q = pi/4 x 10^2 x 114.45 / 1000 = 8.988882 cm3/min. Pc = 8.988882 x 1700 x 1.2 / 60000 = 0.305622 kW
    // (Sandvik's vc f D kc / 240000 with vc = pi 10 763 / 1000 = 23.970: 23.970 x 0.15 x 10 x 1700 / 240000 = 0.254685,
    // x W 1.2 = 0.305622). P = 0.382027; T = 0.305622 x 60000 / (2 pi 763) = 3.825 N m.
    auto r = run(tool(lib::ToolType::Drill, 10, 2, "drill", 24, 0.075), material(1700), machine(kBig), 0, 0, "drill");
    CHECK(r.mode == FeedsMode::Drilling);
    CHECK(r.powerModel == PowerModel::DrillingKc11);
    CHECK(r.rpm == 763);
    CHECK_REL(r.feedMmMin, 114.45);
    CHECK_REL(r.feedPerRevMm, 0.15);
    CHECK_REL(r.chipLoadMm, 0.075);
    CHECK_REL(r.mrrCm3Min, 8.988882);
    CHECK_REL(r.cuttingPowerKw, 0.305622);
    CHECK_REL(r.powerKw, 0.382027);
    CHECK_REL(r.torqueNm, 3.825);
}

TEST_CASE("feeds hand 12: reamer opening a hole", "[cam][feeds][hand]")
{
    // 6 mm 6FL reamer from a 5.8 mm hole: ae = 0.1 per side. vc 20, fz 0.03.
    // n = 20000 / (pi 6) = 1061.033 -> 1061. vf = 1061 x 0.03 x 6 = 190.98 mm/min.
    // section = pi/4 (6^2 - 5.8^2) = pi/4 x 2.36 = 1.853540 mm2. Q = 1.853540 x 190.98 / 1000 = 0.353989 cm3/min.
    // Pc = 0.353989 x 2000 x 1.2 / 60000 = 0.0141596 kW.
    auto r = run(tool(lib::ToolType::Reamer, 6, 6, "ream", 20, 0.03), material(2000), machine(kBig), 0.1, 0, "drill");
    CHECK(r.rpm == 1061);
    CHECK_REL(r.feedMmMin, 190.98);
    CHECK_REL(r.mrrCm3Min, 0.353989);
    CHECK_REL(r.cuttingPowerKw, 0.0141596);
}

TEST_CASE("feeds hand 13: metric tap", "[cam][feeds][hand]")
{
    // M8x1.25 tap, vc 10. n = 10000 / (pi 8) = 397.887 -> 397. 1250000 nm: gcd with 1000 is 1000, step 1.
    // feed = 1.25 x 397 = 496.25 mm/min exactly (496250 um/min = 1250000 nm x 397 / 1000).
    // D1 = 8 - 1.0825318 x 1.25 = 6.646835; D^2 - D1^2 = 64 - 44.180420 = 19.819580;
    // V = 7/16 x pi/4 x 19.819580 x 1.25 = 8.512800 mm3/rev; T = 2000 x 1.2 x 8.5128 / (2 pi) / 1000 = 3.251650 N m;
    // P = 3.251650 x 2 pi x 397 / 60000 / 0.8 = 0.168979 kW.
    auto r = run(tapTool(8, 1.25, 10), material(2000), machine(kBig), 0, 0, "drill");
    CHECK(r.mode == FeedsMode::Tapping);
    CHECK(r.powerModel == PowerModel::TappingKc11);
    CHECK(r.rpm == 397);
    CHECK(r.feedMicronPerMin == 496250);
    CHECK(r.tapFeedExact);
    CHECK(r.feedPerRevMm == 1.25);
    CHECK(r.chipLoadMm == 0);
    CHECK_REL(r.torqueNm, 3.251650);
    CHECK_REL(r.powerKw, 0.168979);
}

TEST_CASE("feeds hand 14: inch tap with a whole-micrometre pitch", "[cam][feeds][hand]")
{
    // 1/4-20 UNC: D = 6.35, P = 25.4 / 20 = 1.27 mm. vc 15: n = 15000 / (pi 6.35) = 751.913 -> 751.
    // feed = 1.27 x 751 = 953.77 mm/min exactly (1270000 nm x 751 / 1000 = 953770 um/min).
    // D1 = 6.35 - 1.0825318 x 1.27 = 4.975185; V = 7/16 x pi/4 x 15.570037 x 1.27 = 6.794560; T = 2.595331 N m.
    auto r = run(tapTool(6.35, 1.27, 15), material(2000), machine(kBig), 0, 0, "drill");
    CHECK(r.rpm == 751);
    CHECK(r.feedMicronPerMin == 953770);
    CHECK(r.tapFeedExact);
    CHECK_REL(r.torqueNm, 2.595331);
}

TEST_CASE("feeds hand 15: 32 TPI tap rounds rpm to a step of 4 for an exact feed", "[cam][feeds][hand]")
{
    // #8-32 UNC: D = 4.1656, P = 25.4 / 32 = 0.79375 mm = 793750 nm. gcd(793750, 1000) = 250: step 1000/250 = 4.
    // vc 9: n = 9000 / (pi 4.1656) = 687.725 -> 687 -> down to a multiple of 4: 684.
    // feed = 793750 x 684 / 1000 = 542925 um/min = 542.925 mm/min exactly.
    auto r = run(tapTool(4.1656, 0.79375, 9), material(2000), machine(kBig), 0, 0, "drill");
    CHECK(r.rpm == 684);
    CHECK(r.feedMicronPerMin == 542925);
    CHECK(r.tapFeedExact);
}

TEST_CASE("feeds hand 16: 24 TPI tap has no exact step and rounds the feed", "[cam][feeds][hand]")
{
    // 3/8-24 UNF: D = 9.525, P = 1.058333 mm = 1058333 nm; gcd(1058333, 1000) = 1, step 1000 > 10: plain rpm.
    // vc 10: n = 10000 / (pi 9.525) = 334.184 -> 334. feed = 1058333 x 334 / 1000 = 353483.222 -> 353483 um/min.
    auto r = run(tapTool(9.525, 1.058333, 10), material(2000), machine(kBig), 0, 0, "drill");
    CHECK(r.rpm == 334);
    CHECK(r.feedMicronPerMin == 353483);
    CHECK_FALSE(r.tapFeedExact);
    CHECK(hasNote(r, "rounded to 1 um/min"));
}

TEST_CASE("feeds hand 17: thread mill", "[cam][feeds][hand]")
{
    // 6 mm 3FL thread mill, ae = 0.6 (thread depth), ap = 12, vc 100, fz 0.02.
    // n = 100000 / (pi 6) = 5305.165 -> 5305. radial = 6 / (2 sqrt(0.6 x 5.4)) = 6 / 3.6 = 1.666667.
    // vf = 5305 x 0.02 x 1.666667 x 3 = 530.5 mm/min.
    auto r = run(tool(lib::ToolType::ThreadMill, 6, 3, "thread", 100, 0.02), material(2000), machine(kBig), 0.6, 12,
                 "threadmill");
    CHECK(r.rpm == 5305);
    CHECK_REL(r.radialThinningFactor, 1.666667);
    CHECK_REL(r.feedMmMin, 530.5);
}

TEST_CASE("feeds hand 18: maximum rpm keeps the chip load", "[cam][feeds][hand][limits]")
{
    // 3 mm 2FL, slot ae = 3, ap = 3, vc 250, fz 0.02; machine 24000 rpm.
    // n = 250000 / (pi 3) = 26525.82 > 24000 -> 24000. vf = 24000 x 0.02 x 2 = 960 mm/min: fz stays 0.02.
    // actual vc = pi x 3 x 24000 / 1000 = 226.195 m/min.
    Limits l = kBig;
    l.maxRpm = 24000;
    auto r = run(tool(lib::ToolType::FlatEndMill, 3, 2, "slot", 250, 0.02), material(2000), machine(l), 3, 3, "slot");
    CHECK(r.limitedByRpm);
    CHECK(r.limits.maxRpm.state == LimitState::Applied);
    CHECK_REL(r.limits.maxRpm.requested, 26525.82);
    CHECK(r.limits.maxRpm.machine == 24000);
    CHECK(r.rpm == 24000);
    CHECK_REL(r.feedMmMin, 960);
    CHECK_REL(r.chipLoadMm, 0.02);
    CHECK_REL(r.actualSurfaceSpeedMMin, 226.195);
    CHECK_FALSE(r.limitedByFeed);
}

TEST_CASE("feeds hand 19: minimum rpm raises the speed and keeps the chip load", "[cam][feeds][hand][limits]")
{
    // 50.8 mm 4-insert face mill, ae = 38.1, ap = 1, vc 20, fz 0.1; machine minimum 200 rpm.
    // n = 20000 / (pi 50.8) = 125.32 < 200 -> 200. vf = 200 x 0.1 x 4 = 80 mm/min. vc actual = pi 50.8 200 / 1000
    // = 31.919.
    Limits l = kBig;
    l.minRpm = 200;
    auto r = run(tool(lib::ToolType::FaceMill, 50.8, 4, "face", 20, 0.1), material(2000), machine(l), 38.1, 1, "face");
    CHECK(r.limitedByMinRpm);
    CHECK(r.limits.minRpm.state == LimitState::Applied);
    CHECK_FALSE(r.limitedByRpm);
    CHECK(r.rpm == 200);
    CHECK_REL(r.feedMmMin, 80);
    CHECK_REL(r.actualSurfaceSpeedMMin, 31.919);
    CHECK(hasNote(r, "raised to the machine's minimum"));
}

TEST_CASE("feeds hand 20: rpm and feed limits together", "[cam][feeds][hand][limits]")
{
    // 12 mm 4FL slot ae = 12, ap = 6, vc 600, fz 0.1; machine 10000 rpm, 3000 mm/min.
    // n = 600000 / (pi 12) = 15915.49 -> 10000 (rpm limit). vf = 10000 x 0.1 x 4 = 4000 > 3000 -> 3000 (feed limit).
    // fz = 3000 / (10000 x 4) = 0.075: below the 0.1 asked for, and flagged.
    Limits l = kBig;
    l.maxRpm = 10000;
    l.feedXY = 3000;
    auto r = run(tool(lib::ToolType::FlatEndMill, 12, 4, "slot", 600, 0.1), material(2000), machine(l), 12, 6, "slot");
    CHECK(r.limitedByRpm);
    CHECK(r.limitedByFeed);
    CHECK(r.rpm == 10000);
    CHECK_REL(r.limits.maxFeed.requested, 4000);
    CHECK(r.feedMicronPerMin == 3000000);
    CHECK_REL(r.chipLoadMm, 0.075);
    CHECK_REL(r.effectiveChipLoadMm, 0.075);
    CHECK(r.targetChipLoadMm == 0.1);
    CHECK(hasNote(r, "chip load is below"));
}

TEST_CASE("feeds hand 21: power limit lowers the feed", "[cam][feeds][hand][limits]")
{
    // 20 mm 4FL slot ae = 20, ap = 20, vc 150, fz 0.1, kc 2000; machine 10 kW, torque not known.
    // n = 150000 / (pi 20) = 2387.32 -> 2387. vf = 2387 x 0.1 x 4 = 954.8.
    // motor power per mm/min = 20 x 20 / 1000 x 2000 x 1.2 / 60000 / 0.8 = 0.02 kW: P = 19.096 kW > 10.
    // vf = 954.8 x 10 / 19.096 = 500 mm/min. Q = 400 x 500 / 1000 = 200 cm3/min; Pc = 8 kW; P = 10 kW;
    // T = 8 x 60000 / (2 pi 2387) = 32.004 N m.
    Limits l = kBig;
    l.powerKw = 10;
    l.torqueNm = 0;
    auto r = run(tool(lib::ToolType::FlatEndMill, 20, 4, "slot", 150, 0.1), material(2000), machine(l), 20, 20, "slot");
    CHECK(r.limitedByPower);
    CHECK_FALSE(r.limitedByTorque);
    CHECK(r.limits.torque.state == LimitState::Unknown);
    CHECK_REL(r.limits.power.requested, 19.096);
    CHECK_REL(r.feedMmMin, 500);
    CHECK(r.powerKw <= 10);
    CHECK_REL(r.powerKw, 10);
    CHECK_REL(r.torqueNm, 32.004);
    CHECK_REL(r.chipLoadMm, 0.052367); // 500 / (2387 x 4)
    CHECK(hasNote(r, "spindle torque is not known"));
}

TEST_CASE("feeds hand 22: torque limit lowers the feed", "[cam][feeds][hand][limits]")
{
    // As 21, power not known, torque 30 N m. T per mm/min = 0.016 x 60000 / (2 pi 2387) = 0.0640094;
    // T at 954.8 = 61.1155 > 30. vf = 30 / 0.0640094 = 468.686 mm/min.
    Limits l = kBig;
    l.powerKw = 0;
    l.torqueNm = 30;
    auto r = run(tool(lib::ToolType::FlatEndMill, 20, 4, "slot", 150, 0.1), material(2000), machine(l), 20, 20, "slot");
    CHECK(r.limitedByTorque);
    CHECK_FALSE(r.limitedByPower);
    CHECK(r.limits.power.state == LimitState::Unknown);
    CHECK_REL(r.limits.torque.requested, 61.1155);
    CHECK_REL(r.feedMmMin, 468.686);
    CHECK(r.torqueNm <= 30);
}

TEST_CASE("feeds hand 23: power and torque both over, the tighter one sets the feed", "[cam][feeds][hand][limits]")
{
    // As 21 with 15 kW and 50 N m. Power: 954.8 x 15 / 19.096 = 750.0; torque: 954.8 x 50 / 61.1155 = 781.13.
    // The smaller, 750 mm/min, applies; both limits were over and both are flagged. T at 750 = 48.0065 N m.
    Limits l = kBig;
    l.powerKw = 15;
    l.torqueNm = 50;
    auto r = run(tool(lib::ToolType::FlatEndMill, 20, 4, "slot", 150, 0.1), material(2000), machine(l), 20, 20, "slot");
    CHECK(r.limitedByPower);
    CHECK(r.limitedByTorque);
    CHECK_REL(r.feedMmMin, 750);
    CHECK_REL(r.torqueNm, 48.0065);
}

TEST_CASE("feeds hand 24: unknown machine values skip every limit", "[cam][feeds][hand][limits]")
{
    // As 18 on a machine with nothing known: n = 26525.82 -> 26525, vf = 26525 x 0.02 x 2 = 1061.0 mm/min.
    auto r =
        run(tool(lib::ToolType::FlatEndMill, 3, 2, "slot", 250, 0.02), material(2000), machine(Limits{}), 3, 3, "slot");
    CHECK(r.rpm == 26525);
    CHECK_REL(r.feedMmMin, 1061.0);
    for (const FeedsLimit* lim :
         {&r.limits.maxRpm, &r.limits.minRpm, &r.limits.maxFeed, &r.limits.power, &r.limits.torque}) {
        CHECK(lim->state == LimitState::Unknown);
        CHECK(lim->machine == 0);
    }
    CHECK_FALSE((r.limitedByRpm || r.limitedByFeed || r.limitedByPower || r.limitedByTorque || r.limitedByMinRpm));
    CHECK(hasNote(r, "maximum spindle speed is not known"));
    CHECK(hasNote(r, "maximum cutting feed is not known"));
    CHECK(hasNote(r, "spindle power is not known"));
    CHECK(hasNote(r, "spindle torque is not known"));
}

TEST_CASE("feeds hand 25: unknown kc1.1 gives no power or torque", "[cam][feeds][hand][limits]")
{
    // As 1 with kc = 0 and a 0.1 kW machine: rpm and feed as in 1 (3183, 636.6), power and torque 0 and not checked.
    Limits l = kBig;
    l.powerKw = 0.1;
    auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "slot", 100, 0.05), material(0), machine(l), 10, 5, "slot");
    CHECK(r.rpm == 3183);
    CHECK_REL(r.feedMmMin, 636.6);
    CHECK(r.powerModel == PowerModel::Unknown);
    CHECK(r.powerKw == 0);
    CHECK(r.torqueNm == 0);
    CHECK(r.limits.power.state == LimitState::Unknown);
    CHECK_FALSE(r.limitedByPower);
    CHECK(hasNote(r, "specific cutting force is not known"));
}

TEST_CASE("feeds hand 26: inch tool and inch engagement", "[cam][feeds][hand]")
{
    // 1/2 in 3FL (12.7 mm), ae = 0.05 in (1.27 mm), ap = 0.5 in (12.7 mm), vc 300 m/min, fz 0.002 in (0.0508 mm).
    // n = 300000 / (pi 12.7) = 7519.131 -> 7519. radial = 12.7 / (2 sqrt(1.27 x 11.43)) = 1.666667.
    // vf = 7519 x 0.0508 x 1.666667 x 3 = 1909.826 mm/min (75.19 in/min). Q = 1.27 x 12.7 x 1909.826 / 1000 = 30.80358.
    auto r = run(tool(lib::ToolType::FlatEndMill, 12.7, 3, "rough", 300, 0.0508), material(2000), machine(kBig), 1.27,
                 12.7, "contour2d");
    CHECK(r.rpm == 7519);
    CHECK_REL(r.feedMmMin, 1909.826);
    CHECK_REL(r.mrrCm3Min, 30.80358);
}

TEST_CASE("feeds hand 27: tap feed limit lowers the rpm", "[cam][feeds][hand][limits]")
{
    // M10x1.5, vc 15: n = 15000 / (pi 10) = 477.46 -> 477, feed 1.5 x 477 = 715.5 > Z limit 600.
    // n = floor(600 / 1.5) = 400; feed = 600.0 mm/min exactly.
    Limits l = kBig;
    l.feedZ = 600;
    auto r = run(tapTool(10, 1.5, 15), material(2000), machine(l), 0, 0, "drill");
    CHECK(r.limitedByFeed);
    CHECK(r.limits.maxFeed.state == LimitState::Applied);
    CHECK_REL(r.limits.maxFeed.requested, 715.5);
    CHECK(r.rpm == 400);
    CHECK(r.feedMicronPerMin == 600000);
}

TEST_CASE("feeds hand 28: chamfer mill near its tip", "[cam][feeds][hand]")
{
    // 12.7 mm 90 deg 4FL chamfer mill, tip diameter 0, ap = 2 from the tip, ae = 1, vc 150, fz 0.03.
    // Deff = 0 + 2 x 2 x tan 45 = 4.0. lead = 1 / cos 45 = 1.414214. radial (on Deff): 4 / (2 sqrt(1 x 3)) = 1.154701.
    // total = 1.632993. n = 150000 / (pi 4) = 11936.62 -> 11936. vf = 11936 x 0.03 x 1.632993 x 4 = 2338.968.
    lib::Tool t = tool(lib::ToolType::ChamferMill, 12.7, 4, "chamfer", 150, 0.03);
    t.tipAngleDeg = 90;
    auto r = run(t, material(2000), machine(kBig), 1, 2, "chamfer2d");
    CHECK_REL(r.effectiveDiameterMm, 4.0);
    CHECK_REL(r.leadThinningFactor, 1.414214);
    CHECK_REL(r.radialThinningFactor, 1.154701);
    CHECK(r.rpm == 11936);
    CHECK_REL(r.feedMmMin, 2338.968);
}

TEST_CASE("feeds hand 29: spot drill at a shallow depth", "[cam][feeds][hand]")
{
    // 10 mm 90 deg 2FL spot drill, ap = 1.5: Deff = 2 x 1.5 x tan 45 = 3.0. vc 40, fz 0.025.
    // n = 40000 / (pi 3) = 4244.13 -> 4244. vf = 4244 x 0.025 x 2 = 212.2. Q = pi/4 x 9 x 212.2 / 1000 = 1.499953.
    lib::Tool t = tool(lib::ToolType::SpotDrill, 10, 2, "spot", 40, 0.025);
    t.tipAngleDeg = 90;
    auto r = run(t, material(2000), machine(kBig), 0, 1.5, "drill");
    CHECK(r.mode == FeedsMode::Drilling);
    CHECK_REL(r.effectiveDiameterMm, 3.0);
    CHECK(r.rpm == 4244);
    CHECK_REL(r.feedMmMin, 212.2);
    CHECK_REL(r.mrrCm3Min, 1.499953);
}

TEST_CASE("feeds hand 30: tap power limit lowers the rpm", "[cam][feeds][hand][limits]")
{
    // M12x1.75, vc 10, kc 2400; machine 0.15 kW. n = 10000 / (pi 12) = 265.26 -> 265.
    // D1 = 12 - 1.0825318 x 1.75 = 10.105569; D^2 - D1^2 = 41.877467; V = 7/16 x pi/4 x 41.877467 x 1.75 = 25.181778;
    // T = 2400 x 1.2 x 25.181778 / (2 pi) / 1000 = 11.542477 N m; P(265) = 11.542477 x 2 pi 265 / 60000 / 0.8 =
    // 0.400390. n = floor(265 x 0.15 / 0.400390) = floor(99.28) = 99; P(99) = 0.149580; feed = 1.75 x 99 = 173.25.
    Limits l = kBig;
    l.powerKw = 0.15;
    auto r = run(tapTool(12, 1.75, 10), material(2400), machine(l), 0, 0, "drill");
    CHECK(r.limitedByPower);
    CHECK_REL(r.limits.power.requested, 0.400390);
    CHECK(r.rpm == 99);
    CHECK(r.feedMicronPerMin == 173250);
    CHECK_REL(r.powerKw, 0.149580);
    CHECK_REL(r.torqueNm, 11.542477);
}

TEST_CASE("feeds hand 31: tap torque over the machine is exceeded, not hidden", "[cam][feeds][hand][limits]")
{
    // As 30 with 10 N m and no power limit: T = 11.542477 > 10 and no rpm lowers it: Exceeded, rpm stays 265,
    // feed 1.75 x 265 = 463.75.
    Limits l = kBig;
    l.torqueNm = 10;
    auto r = run(tapTool(12, 1.75, 10), material(2400), machine(l), 0, 0, "drill");
    CHECK(r.limitedByTorque);
    CHECK(r.limits.torque.state == LimitState::Exceeded);
    CHECK(r.rpm == 265);
    CHECK(r.feedMicronPerMin == 463750);
    CHECK(hasNote(r, "tapping torque is above"));
}

TEST_CASE("feeds hand 32: tap below the minimum rpm", "[cam][feeds][hand][limits]")
{
    // M3x0.5, vc 4: n = 4000 / (pi 3) = 424.41 < minimum 500 -> 500. feed = 0.5 x 500 = 250 mm/min.
    Limits l = kBig;
    l.minRpm = 500;
    auto r = run(tapTool(3, 0.5, 4), material(2000), machine(l), 0, 0, "drill");
    CHECK(r.limitedByMinRpm);
    CHECK(r.rpm == 500);
    CHECK(r.feedMicronPerMin == 250000);
}

TEST_CASE("feeds hand 33: ball nose cutting with its tip is bounded", "[cam][feeds][hand]")
{
    // 6 mm 2FL ball, ap = 0.05, slot. Deff = 2 sqrt(0.05 x 5.95) = 1.090871 < D/5 = 1.2 -> 1.2.
    // lead = 6 / 1.090871 = 5.500191 -> total capped at 5 (radial 1: ae 6 >= 0.6).
    // n = 150000 / (pi 1.2) = 39788.736 -> 39788. vf = 39788 x 0.04 x 5 x 2 = 15915.2 mm/min.
    lib::Tool t = tool(lib::ToolType::BallEndMill, 6, 2, "finish", 150, 0.04);
    t.cornerRadius = 3;
    auto r = run(t, material(2000), machine(kBig), 6, 0.05, "contour2d", "finish");
    CHECK_REL(r.effectiveDiameterMm, 1.2);
    CHECK_REL(r.leadThinningFactor, 5.500191);
    CHECK(r.chipThinningFactor == 5);
    CHECK(r.rpm == 39788);
    CHECK_REL(r.feedMmMin, 15915.2);
    CHECK(hasNote(r, "a fifth of the tool diameter"));
}

// ---------- Shipped library on shipped machines ----------
namespace {

struct Shipped {
    std::vector<lib::Tool> tools;
    std::vector<lib::Material> materials;
    std::vector<lib::MachineDef> machines;
    const lib::Tool& tool(std::string_view id) const
    {
        for (const auto& t : tools)
            if (t.id.value == Uuid::fromString(id))
                return t;
        FAIL("no shipped tool " << id);
        return tools.front();
    }
    const lib::Material& material(std::string_view id) const
    {
        for (const auto& m : materials)
            if (m.id.value == Uuid::fromString(id))
                return m;
        FAIL("no shipped material " << id);
        return materials.front();
    }
    const lib::MachineDef& machine(std::string_view id) const
    {
        for (const auto& m : machines)
            if (m.id.value == Uuid::fromString(id))
                return m;
        FAIL("no shipped machine " << id);
        return machines.front();
    }
};

const Shipped& shipped()
{
    static const Shipped s = [] {
        auto lib = lib::ILibrary::open(std::string(RAY_SOURCE_DIR) + "/resources/library");
        REQUIRE(lib);
        return Shipped{(*lib)->tools(), (*lib)->materials(), (*lib)->machines()};
    }();
    return s;
}

// Ids from tools/library_ids.json.
constexpr std::string_view kHaas = "b903ede7-5123-4c26-bc15-466e92fba4c6";  // machine.vmc.haas-ngc
constexpr std::string_view kFanuc = "79733809-d5a1-4b1d-9502-d9ccd73a1777"; // machine.vmc.fanuc
constexpr std::string_view kMazak = "1647afea-3110-4e8e-a439-fa1082f98a5d"; // machine.vmc.mazak-eia
constexpr std::string_view k6061 = "b72c5171-c258-44d2-91ae-ee049b636621";
constexpr std::string_view k7075 = "4dfd9a48-f386-449e-a847-47d763e5a0cf";
constexpr std::string_view k1018 = "000dafba-65e0-4ceb-8fc2-284c79449e1a";
constexpr std::string_view k4140 = "89571a2d-9732-405b-9534-7b44020c53d8"; // 4140 annealed
constexpr std::string_view k304 = "84d2a88a-bf96-428c-958e-9d9ae6363ce2";

} // namespace

TEST_CASE("feeds hand 34 (library): 1/2 in 3FL rough in 6061 on the Haas", "[cam][feeds][hand][shipped]")
{
    // tool.em.flat.1-2.3fl: D 12.7, 3FL; 6061 rough preset vc 250, fz 0.1016, maxRadial 0.15, maxAxial 2. Haas: 8100
    // rpm, 16510 mm/min, 22.4 kW, 122 N m. Engagement not given: ae = 0.15 x 12.7 = 1.905, ap = 2 x 12.7 = 25.4.
    // n = 250000 / (pi 12.7) = 6265.94 -> 6265. radial = 12.7 / (2 sqrt(1.905 x 10.795)) = 12.7 / 9.069500 = 1.400280.
    // vf = 6265 x 0.1016 x 1.400280 x 3 = 2673.935. Q = 1.905 x 25.4 x 2673.935 / 1000 = 129.3837; kc 700:
    // Pc = 129.3837 x 700 x 1.2 / 60000 = 1.811372; P = 2.264215 kW; T = 1.811372 x 60000 / (2 pi 6265) = 2.760946 N m.
    const auto& s = shipped();
    auto r = run(s.tool("3fb7624c-615b-4f07-8f57-ecc8432f07df"), s.material(k6061), s.machine(kHaas), 0, 0, "pocket2d");
    CHECK(r.opClass == "rough");
    CHECK_REL(r.radialEngagementMm, 1.905);
    CHECK_REL(r.axialDepthMm, 25.4);
    CHECK(r.rpm == 6265);
    CHECK_REL(r.radialThinningFactor, 1.400280);
    CHECK_REL(r.feedMmMin, 2673.935);
    CHECK_REL(r.mrrCm3Min, 129.3837);
    CHECK_REL(r.powerKw, 2.264215);
    CHECK_REL(r.torqueNm, 2.760946);
    CHECK(r.limits.maxRpm.state == LimitState::NotReached);
    CHECK(r.limits.power.state == LimitState::NotReached);
    CHECK(r.limits.torque.state == LimitState::NotReached);
    CHECK(hasNote(r, "No width of cut given"));
}

TEST_CASE("feeds hand 35 (library): 1/8 in 2FL rough in 6061 on the Fanuc", "[cam][feeds][hand][shipped]")
{
    // tool.em.flat.1-8.2fl: D 3.175, 2FL; rough vc 250, fz 0.0254. Fanuc: 24000 rpm, 30000 mm/min, power and torque 0.
    // ae = 0.47625, ap = 6.35. n = 250000 / (pi 3.175) = 25063.77 -> 24000 (rpm limit). radial 1.400280 (same ae/D).
    // vf = 24000 x 0.0254 x 1.400280 x 2 = 1707.221 mm/min.
    const auto& s = shipped();
    auto r =
        run(s.tool("bd50ea0a-cb02-4e9c-8223-3a18c9d59197"), s.material(k6061), s.machine(kFanuc), 0, 0, "pocket2d");
    CHECK(r.limitedByRpm);
    CHECK(r.rpm == 24000);
    CHECK_REL(r.feedMmMin, 1707.221);
    CHECK(r.limits.power.state == LimitState::Unknown);
    CHECK(r.limits.torque.state == LimitState::Unknown);
    CHECK(r.limits.maxFeed.state == LimitState::NotReached);
}

TEST_CASE("feeds hand 36 (library): 10 mm drill in 4140 on the Mazak", "[cam][feeds][hand][shipped]")
{
    // tool.drill.10mm: 2FL; 4140 annealed drill vc 18, fz 0.06 (0.12 mm/rev). Mazak: 12000 rpm, 18 kW, feed and torque
    // 0. n = 18000 / (pi 10) = 572.96 -> 572. vf = 572 x 0.06 x 2 = 68.64. Q = pi/4 x 100 x 68.64 / 1000 = 5.390973; kc
    // 2000: Pc = 0.215639; P = 0.269549 kW.
    const auto& s = shipped();
    auto r = run(s.tool("5a0addb3-3dab-40bc-bc10-1871e0c0fbd8"), s.material(k4140), s.machine(kMazak), 0, 0, "drill");
    CHECK(r.rpm == 572);
    CHECK_REL(r.feedMmMin, 68.64);
    CHECK_REL(r.mrrCm3Min, 5.390973);
    CHECK_REL(r.powerKw, 0.269549);
    CHECK(r.limits.maxFeed.state == LimitState::Unknown);
    CHECK(r.limits.power.state == LimitState::NotReached);
    CHECK(r.limits.torque.state == LimitState::Unknown);
}

TEST_CASE("feeds hand 37 (library): M6x1 tap in 304 on the Haas", "[cam][feeds][hand][shipped]")
{
    // tool.tap.m6x1: D 6, P 1; 304 tap vc 4. n = 4000 / (pi 6) = 212.21 -> 212; feed = 212 mm/min.
    // D1 = 6 - 1.0825318 = 4.917468; V = 7/16 x pi/4 x 11.818506 x 1 = 4.060977; kc 2300:
    // T = 2300 x 1.2 x 4.060977 / (2 pi) / 1000 = 1.783856 N m; P = 1.783856 x 2 pi 212 / 60000 / 0.8 = 0.049503 kW.
    const auto& s = shipped();
    auto r = run(s.tool("8c62b95b-6890-46f4-a990-cacd8715acf8"), s.material(k304), s.machine(kHaas), 0, 0, "drill");
    CHECK(r.rpm == 212);
    CHECK(r.feedMicronPerMin == 212000);
    CHECK_REL(r.torqueNm, 1.783856);
    CHECK_REL(r.powerKw, 0.049503);
    CHECK(r.limits.torque.state == LimitState::NotReached);
}

TEST_CASE("feeds hand 38 (library): 2 in face mill in 1018 on the Haas", "[cam][feeds][hand][shipped]")
{
    // tool.face.2in.45.4z: D 50.8, 4 inserts; 1018 face vc 180, fz 0.1, maxRadial 0.75, maxAxial 0.04.
    // ae = 38.1, ap = 2.032. n = 1127; vf = 450.8. Q = 38.1 x 2.032 x 450.8 / 1000 = 34.900575; kc 1700:
    // Pc = 1.186620; P = 1.483274 kW; T = 1.186620 x 60000 / (2 pi 1127) = 10.054465 N m.
    const auto& s = shipped();
    auto r = run(s.tool("91e50525-1a88-4f40-a282-2997b0fb4852"), s.material(k1018), s.machine(kHaas), 0, 0, "face");
    CHECK(r.rpm == 1127);
    CHECK_REL(r.feedMmMin, 450.8);
    CHECK_REL(r.mrrCm3Min, 34.900575);
    CHECK_REL(r.powerKw, 1.483274);
    CHECK_REL(r.torqueNm, 10.054465);
}

TEST_CASE("feeds hand 39 (library): 6 mm ball finishing 7075 on the Mazak", "[cam][feeds][hand][shipped]")
{
    // tool.em.ball.6mm.2fl: D 6, 2FL; 7075 finish vc 264, fz 0.0336. ae = 0.3, ap = 0.2.
    // Deff = 2 sqrt(0.2 x 5.8) = 2.154066; lead = 6 / 2.154066 = 2.785430;
    // radial = 2.154066 / (2 sqrt(0.3 x 1.854066)) = 1.444128; total = 4.022518.
    // n = 264000 / (pi 2.154066) = 39011.7 -> 12000 (rpm limit). vf = 12000 x 0.0336 x 4.022518 x 2 = 3243.758.
    const auto& s = shipped();
    auto r = run(s.tool("2573ab3d-4f81-404b-848e-4ae05bee75ca"), s.material(k7075), s.machine(kMazak), 0.3, 0.2,
                 "contour2d", "finish");
    CHECK_REL(r.effectiveDiameterMm, 2.154066);
    CHECK_REL(r.chipThinningFactor, 4.022518);
    CHECK(r.limitedByRpm);
    CHECK(r.rpm == 12000);
    CHECK_REL(r.feedMmMin, 3243.758);
    CHECK(r.limits.maxFeed.state == LimitState::Unknown);
}

// =====================================================================================================================
// Properties
// =====================================================================================================================

TEST_CASE("feeds: feed is linear in chip load until the feed limit", "[cam][feeds][property]")
{
    // 10 mm 4FL, ae = 2 (thinning 1.25), n = 3183: vf = 3183 x fz x 1.25 x 4 = 15915 fz. Feed limit 2000 mm/min,
    // reached at fz = 0.12567.
    Limits l = kBig;
    l.feedXY = 2000;
    const auto m = machine(l);
    const auto mat = material(2000);
    double base = 0;
    bool clamped = false;
    for (int k = 1; k <= 40; ++k) {
        const double fz = 0.005 * k;
        auto r = run(tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, fz), mat, m, 2, 10, "pocket2d");
        CHECK(r.rpm == 3183);
        if (k == 1)
            base = r.feedMmMin;
        const double linear = base * k;
        if (linear <= 2000 - 0.001 * k) {
            CHECK_FALSE(r.limitedByFeed);
            CHECK(std::fabs(r.feedMmMin - linear) <= 0.001 * k); // one um/min of rounding per step
            CHECK_REL(r.effectiveChipLoadMm, fz);
        } else if (linear > 2000 + 0.001 * k) {
            clamped = true;
            CHECK(r.limitedByFeed);
            CHECK(r.feedMmMin == 2000);
            CHECK(r.effectiveChipLoadMm < fz);
        }
    }
    CHECK(clamped);
}

namespace {

// A small portable generator (no std:: distributions, whose output differs between standard libraries).
struct Lcg {
    std::uint64_t s;
    double next() // [0, 1)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
    bool chance(double p) { return next() < p; }
};

} // namespace

TEST_CASE("feeds: no known machine limit is ever exceeded", "[cam][feeds][property]")
{
    using T = lib::ToolType;
    const T types[] = {T::FlatEndMill, T::BallEndMill, T::BullEndMill, T::ChamferMill, T::FaceMill,   T::SpotDrill,
                       T::Drill,       T::Tap,         T::Reamer,      T::BoringBar,   T::ThreadMill, T::EngraveCutter};
    Lcg g{12345};
    int checked = 0;
    for (int i = 0; i < 4000; ++i) {
        const T type = types[static_cast<std::size_t>(g.next() * 12)];
        const double d = g.range(0.5, 80);
        lib::Tool t = tool(type, d, 1 + static_cast<int>(g.next() * 8));
        t.cornerRadius = type == T::BallEndMill ? d / 2 : g.range(0, d / 2);
        t.tipAngleDeg = g.chance(0.8) ? g.range(30, 150) : 0;
        t.tipDiameter = g.chance(0.5) ? g.range(0, d / 2) : 0;
        t.threadPitch = g.range(0.2, 4);
        t.presets.push_back(preset("p", g.range(2, 800), g.range(0.002, 0.4), g.range(0.01, 1), g.range(0.01, 3)));
        Limits l;
        l.maxRpm = g.chance(0.8) ? g.range(3000, 40000) : 0;
        l.minRpm = g.chance(0.4) ? g.range(0, 200) : 0;
        l.powerKw = g.chance(0.7) ? g.range(0.05, 40) : 0;
        l.torqueNm = g.chance(0.7) ? g.range(0.5, 300) : 0;
        l.feedXY = g.chance(0.7) ? g.range(500, 40000) : 0;
        l.feedZ = g.chance(0.7) ? g.range(500, 40000) : 0;
        const auto m = machine(l);
        const auto mat = material(g.chance(0.85) ? g.range(300, 4000) : 0);
        const double ae = g.chance(0.2) ? 0 : g.range(0.001, 1.2) * d;
        const double ap = g.chance(0.2) ? 0 : g.range(0.001, 3) * d;
        const char* opTypes[] = {"pocket2d", "contour2d", "slot", "face", "drill", "chamfer2d"};
        auto r = computeFeeds(FeedsInput{.tool = t,
                                         .material = mat,
                                         .machine = m,
                                         .radialEngagementMm = ae,
                                         .axialDepthMm = ap,
                                         .opType = opTypes[static_cast<std::size_t>(g.next() * 6)],
                                         .opClass = "p"});
        if (!r) {
            // Only the documented edge cases: a feed below 1 um/min or above the toolpath range.
            CHECK(r.error().code == ErrorCode::InvalidArgument);
            CHECK_THAT(r.error().message, ContainsSubstring("feed"));
            continue;
        }
        ++checked;
        INFO("case " << i);
        const bool tapping = r->mode == FeedsMode::Tapping;
        CHECK(r->rpm >= 1);
        CHECK(r->feedMicronPerMin >= 1);
        CHECK(r->rpm == std::floor(r->rpm));
        if (l.maxRpm > 0)
            CHECK(r->rpm <= l.maxRpm);
        if (l.minRpm > 0)
            CHECK(r->rpm >= l.minRpm);
        const double feedLimit = r->mode == FeedsMode::Milling ? l.feedXY : l.feedZ;
        if (feedLimit > 0 && r->limits.maxFeed.state != LimitState::Exceeded)
            CHECK(r->feedMmMin <= feedLimit);
        if (l.powerKw > 0 && r->limits.power.state != LimitState::Exceeded)
            CHECK(r->powerKw <= l.powerKw);
        if (l.torqueNm > 0 && r->limits.torque.state != LimitState::Exceeded)
            CHECK(r->torqueNm <= l.torqueNm);
        // Exceeded only where the function cannot help it: taps.
        if (!tapping) {
            CHECK(r->limits.maxFeed.state != LimitState::Exceeded);
            CHECK(r->limits.power.state != LimitState::Exceeded);
            CHECK(r->limits.torque.state != LimitState::Exceeded);
            CHECK(r->effectiveChipLoadMm <= r->targetChipLoadMm * (1 + 1e-9));
            CHECK(r->chipThinningFactor <= 5);
            CHECK(r->chipThinningFactor >= 1);
        }
        for (double v : {r->rpm, r->feedMmMin, r->chipLoadMm, r->effectiveChipLoadMm, r->mrrCm3Min, r->powerKw,
                         r->torqueNm, r->effectiveDiameterMm, r->chipThinningFactor, r->actualSurfaceSpeedMMin})
            CHECK(std::isfinite(v));
    }
    CHECK(checked > 3900);
}

TEST_CASE("feeds: radial chip thinning is 1 from half the diameter and grows below it", "[cam][feeds][property]")
{
    const auto m = machine(Limits{});
    const auto mat = material(2000);
    const lib::Tool t = tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05);
    double previousRadial = 0, previousTotal = 0;
    // ae from the full diameter down to 0.0005 D.
    for (int k = 2000; k >= 1; --k) {
        const double ae = 10.0 * k / 2000;
        auto r = run(t, mat, m, ae, 10, "pocket2d", "rough");
        if (ae >= 5) {
            CHECK(r.radialThinningFactor == 1);
            CHECK(r.chipThinningFactor == 1);
        } else {
            CHECK(r.radialThinningFactor > previousRadial); // strictly larger as ae falls
            CHECK(r.chipThinningFactor >= previousTotal);   // monotonic, capped at 5
            CHECK(r.chipThinningFactor <= 5);
        }
        previousRadial = r.radialThinningFactor;
        previousTotal = r.chipThinningFactor;
    }
    CHECK(previousTotal == 5);
}

// =====================================================================================================================
// Validation
// =====================================================================================================================

TEST_CASE("feeds: bad tools and engagements are errors, never NaN", "[cam][feeds][validation]")
{
    const auto m = machine(kBig);
    const auto mat = material(2000);
    auto fails = [&](const lib::Tool& t, double ae, double ap, ErrorCode code, std::string_view text,
                     std::string_view opType = "pocket2d") {
        auto r = computeFeeds(FeedsInput{
            .tool = t, .material = mat, .machine = m, .radialEngagementMm = ae, .axialDepthMm = ap, .opType = opType});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code);
        CHECK_THAT(r.error().message, ContainsSubstring(std::string(text)));
    };
    const lib::Tool good = tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05);
    for (double d : {0.0, -5.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        lib::Tool t = good;
        t.diameter = d;
        fails(t, 1, 1, ErrorCode::InvalidArgument, "diameter");
    }
    lib::Tool noFlutes = good;
    noFlutes.flutes = 0;
    fails(noFlutes, 1, 1, ErrorCode::InvalidArgument, "flute");
    fails(good, -1, 1, ErrorCode::InvalidArgument, "width of cut");
    fails(good, 1, std::nan(""), ErrorCode::InvalidArgument, "depth of cut");
    lib::Tool noPitch = tapTool(6, 0, 10);
    fails(noPitch, 0, 0, ErrorCode::InvalidArgument, "pitch", "drill");
    lib::Tool zeroChip = tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0);
    fails(zeroChip, 1, 1, ErrorCode::InvalidArgument, "chip load");

    // A tap ignores flutes: 0 flutes is fine there.
    lib::Tool tapNoFlutes = tapTool(6, 1, 10);
    tapNoFlutes.flutes = 0;
    CHECK(computeFeeds(FeedsInput{.tool = tapNoFlutes,
                                  .material = mat,
                                  .machine = m,
                                  .radialEngagementMm = 0,
                                  .axialDepthMm = 0,
                                  .opType = "drill"}));

    // Machine with minimum above maximum.
    Limits bad = kBig;
    bad.minRpm = 9000;
    bad.maxRpm = 8000;
    auto r = computeFeeds(FeedsInput{.tool = good,
                                     .material = mat,
                                     .machine = machine(bad),
                                     .radialEngagementMm = 1,
                                     .axialDepthMm = 1,
                                     .opType = "pocket2d"});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);

    // Below 1 rpm, and a feed beyond the toolpath's int32 um/min range.
    lib::Tool huge = tool(lib::ToolType::FaceMill, 2000, 4, "face", 1, 0.1);
    fails(huge, 1000, 1, ErrorCode::InvalidArgument, "below 1 rpm", "face");
    lib::Tool fast = tool(lib::ToolType::FlatEndMill, 1, 8, "rough", 10000, 5);
    auto fr = computeFeeds(FeedsInput{.tool = fast,
                                      .material = mat,
                                      .machine = machine(Limits{}),
                                      .radialEngagementMm = 1,
                                      .axialDepthMm = 1,
                                      .opType = "slot",
                                      .opClass = "rough"});
    REQUIRE_FALSE(fr);
    CHECK_THAT(fr.error().message, ContainsSubstring("largest toolpath feed"));

    // Degenerate but valid inputs stay finite.
    for (double tiny : {1e-300, 1e-12, 5e-324}) {
        lib::Tool ball = tool(lib::ToolType::BallEndMill, 6, 2, "rough", 100, 0.05);
        ball.cornerRadius = 3;
        auto br = run(ball, mat, m, tiny, tiny, "contour2d");
        CHECK(br.chipThinningFactor == 5);
        CHECK(std::isfinite(br.feedMmMin));
        lib::Tool bull = tool(lib::ToolType::BullEndMill, 12, 4, "rough", 100, 0.05);
        bull.cornerRadius = 1;
        auto ur = run(bull, mat, m, tiny, tiny, "contour2d");
        CHECK(std::isfinite(ur.leadThinningFactor));
    }
}

TEST_CASE("feeds: preset selection, missing presets and overrides", "[cam][feeds][validation]")
{
    const auto m = machine(kBig);
    const auto mat = material(2000);
    lib::Tool t = tool(lib::ToolType::FlatEndMill, 10, 4);
    t.presets = {preset("rough", 100, 0.05, 0.2, 1.5), preset("slot", 80, 0.04, 1, 0.5),
                 preset("finish", 120, 0.04, 0.05, 1.5)};
    t.presets.push_back(lib::ToolPreset{kOtherMat, "rough", 999, 0.9, 1, 1, 0});

    // Default class: rough for a pocket, slot for a slot op or a full-width cut, finish only when asked for.
    CHECK(run(t, mat, m, 2, 5, "pocket2d").opClass == "rough");
    CHECK(run(t, mat, m, 2, 5, "slot").opClass == "slot");
    CHECK(run(t, mat, m, 10, 5, "pocket2d").opClass == "slot");
    CHECK(run(t, mat, m, 0.5, 5, "contour2d", "finish").opClass == "finish");
    // Plunging with an end mill: drilling mode with its slot data.
    auto plunge = run(t, mat, m, 0, 0, "drill");
    CHECK(plunge.mode == FeedsMode::Drilling);
    CHECK(plunge.opClass == "slot");

    // Engagement defaults: ae = 0.2 x 10, ap = 1.5 x 10.
    auto def = run(t, mat, m, 0, 0, "pocket2d");
    CHECK(def.radialEngagementMm == 2);
    CHECK(def.axialDepthMm == 15);
    CHECK(hasNote(def, "No width of cut given"));
    CHECK(hasNote(def, "No depth of cut given"));
    // Over the preset's maxima and the flute length (40 mm): notes, values unchanged.
    auto deep = run(t, mat, m, 3, 45, "pocket2d");
    CHECK(deep.radialEngagementMm == 3);
    CHECK(hasNote(deep, "above the cutting data's largest radial"));
    CHECK(hasNote(deep, "deeper than the tool's flute length"));
    // Wider than the tool: computed as a slot.
    auto wide = run(t, mat, m, 12, 5, "pocket2d", "rough");
    CHECK(wide.radialEngagementMm == 10);
    CHECK(hasNote(wide, "wider than the tool"));

    // No preset for the material: NotFound, naming what the tool has.
    lib::Material other = material(2000);
    other.id = MaterialId{Uuid::fromString("44444444-4444-4444-8444-444444444444")};
    other.name = "Inconel 718";
    auto missing = computeFeeds(FeedsInput{
        .tool = t, .material = other, .machine = m, .radialEngagementMm = 2, .axialDepthMm = 5, .opType = "pocket2d"});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK_THAT(missing.error().message, ContainsSubstring("Inconel 718"));
    // A named class the tool does not have.
    auto noClass = computeFeeds(FeedsInput{.tool = t,
                                           .material = mat,
                                           .machine = m,
                                           .radialEngagementMm = 2,
                                           .axialDepthMm = 5,
                                           .opType = "pocket2d",
                                           .opClass = "engrave"});
    REQUIRE_FALSE(noClass);
    CHECK(noClass.error().code == ErrorCode::NotFound);
    CHECK_THAT(noClass.error().message, ContainsSubstring("finish, rough, slot"));

    // Both values given: no preset needed (fallback for a material without data). Same arithmetic as hand case 2.
    auto own = computeFeeds(FeedsInput{.tool = t,
                                       .material = other,
                                       .machine = m,
                                       .radialEngagementMm = 2,
                                       .axialDepthMm = 10,
                                       .opType = "pocket2d",
                                       .surfaceSpeedMMin = 100.0,
                                       .chipLoadMm = 0.05});
    REQUIRE(own);
    CHECK(own->opClass.empty());
    CHECK(own->rpm == 3183);
    CHECK_REL(own->feedMmMin, 795.75);
    // ...but then the engagement must be given.
    auto noAe = computeFeeds(FeedsInput{.tool = t,
                                        .material = other,
                                        .machine = m,
                                        .radialEngagementMm = 0,
                                        .axialDepthMm = 10,
                                        .opType = "pocket2d",
                                        .surfaceSpeedMMin = 100.0,
                                        .chipLoadMm = 0.05});
    REQUIRE_FALSE(noAe);
    CHECK(noAe.error().code == ErrorCode::InvalidArgument);
    // One override with the preset: vc 50 instead of 100 halves rpm (1591.5 -> 1591) at the preset's chip load.
    auto half = computeFeeds(FeedsInput{.tool = t,
                                        .material = mat,
                                        .machine = m,
                                        .radialEngagementMm = 5,
                                        .axialDepthMm = 10,
                                        .opType = "pocket2d",
                                        .surfaceSpeedMMin = 50.0});
    REQUIRE(half);
    CHECK(half->rpm == 1591);
    CHECK(half->surfaceSpeedMMin == 50);
    CHECK(half->targetChipLoadMm == 0.05);
    auto badOverride = computeFeeds(FeedsInput{.tool = t,
                                               .material = mat,
                                               .machine = m,
                                               .radialEngagementMm = 5,
                                               .axialDepthMm = 10,
                                               .opType = "pocket2d",
                                               .surfaceSpeedMMin = -1.0});
    REQUIRE_FALSE(badOverride);
    CHECK(badOverride.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("feeds: the result is a value the operation editor compares, never a write", "[cam][feeds]")
{
    // computeFeeds takes the tool, material and machine by const reference and returns everything the panel shows;
    // an Operation's CuttingData is untouched until "use calculated" runs a command. The same input gives the same
    // result every time (no hidden state).
    const auto m = machine(kBig);
    const auto mat = material(2000);
    const lib::Tool t = tool(lib::ToolType::FlatEndMill, 10, 4, "rough", 100, 0.05);
    auto a = run(t, mat, m, 2, 10, "pocket2d");
    auto b = run(t, mat, m, 2, 10, "pocket2d");
    CHECK(a.feedMicronPerMin == b.feedMicronPerMin);
    CHECK(a.rpm == b.rpm);
    CHECK(a.notes == b.notes);
    // Everything the panel needs to explain the number is there.
    CHECK(a.surfaceSpeedMMin == 100);
    CHECK(a.targetChipLoadMm == 0.05);
    CHECK(a.limits.maxRpm.machine == 60000);
    CHECK_REL(a.limits.maxRpm.requested, 3183.0989);
    CHECK_FALSE(a.notes.empty());
}

TEST_CASE("feeds: a machine without rigid tapping gets a note", "[cam][feeds]")
{
    Limits l = kBig;
    l.rigidTapping = false;
    auto r = run(tapTool(6, 1, 10), material(2000), machine(l), 0, 0, "drill");
    CHECK(hasNote(r, "rigid tapping"));
}

// =====================================================================================================================
// The shipped library: every tool, preset and machine; determinism
// =====================================================================================================================

namespace {

std::string_view opTypeFor(lib::ToolType type)
{
    using T = lib::ToolType;
    switch (type) {
    case T::FaceMill:
        return "face";
    case T::ChamferMill:
        return "chamfer2d";
    case T::ThreadMill:
        return "threadmill";
    case T::EngraveCutter:
        return "engrave";
    case T::Drill:
    case T::SpotDrill:
    case T::Tap:
    case T::Reamer:
    case T::BoringBar:
        return "drill";
    default:
        return "pocket2d";
    }
}

} // namespace

TEST_CASE("feeds: every shipped preset on every shipped machine, pinned", "[cam][feeds][shipped]")
{
    const auto& s = shipped();
    REQUIRE(s.machines.size() == 3);
    Hasher h;
    int count = 0;
    for (const auto& t : s.tools) {
        for (const auto& p : t.presets) {
            const lib::Material* mat = nullptr;
            for (const auto& m : s.materials)
                if (m.id == p.material)
                    mat = &m;
            REQUIRE(mat);
            for (const auto& m : s.machines) {
                INFO(t.name << " / " << mat->name << " / " << p.opClass << " / " << m.name);
                auto r = computeFeeds(FeedsInput{.tool = t,
                                                 .material = *mat,
                                                 .machine = m,
                                                 .radialEngagementMm = 0,
                                                 .axialDepthMm = 0,
                                                 .opType = opTypeFor(t.type),
                                                 .opClass = p.opClass});
                REQUIRE(r);
                if (m.maxRpm > 0)
                    CHECK(r->rpm <= m.maxRpm);
                if (r->limits.maxFeed.machine > 0)
                    CHECK(r->feedMmMin <= r->limits.maxFeed.machine);
                if (m.spindlePowerKw > 0)
                    CHECK(r->powerKw <= m.spindlePowerKw);
                if (m.spindleTorqueNm > 0)
                    CHECK(r->torqueNm <= m.spindleTorqueNm);
                h.i64(static_cast<std::int64_t>(r->rpm)).i64(r->feedMicronPerMin);
                h.i64((r->limitedByRpm ? 1 : 0) | (r->limitedByFeed ? 2 : 0) | (r->limitedByPower ? 4 : 0) |
                      (r->limitedByTorque ? 8 : 0) | (r->limitedByMinRpm ? 16 : 0) | (r->tapFeedExact ? 32 : 0));
                ++count;
            }
        }
    }
    CHECK(count == 2088); // 696 presets x 3 machines
    // The rpm and feed of every combination, the values that reach toolpaths: identical on every platform. Changes
    // only when the library, the formulas or the constants of feeds.cpp change (then review and update this).
    CHECK(h.finish().hex() == "99be74e907961782b72e9d9b7cf47a14d434554a7d7bf02b4d2044fc62edb42a");
}

// Not a test: a table of the shipped presets that hit a limit on all three shipped machines (run by hand with
// "[.survey]" to review the starter library's cutting data).
TEST_CASE("feeds: survey of shipped presets limited on every machine", "[.survey]")
{
    const auto& s = shipped();
    std::map<std::string, int> limitedEverywhere, perMachine;
    for (const auto& t : s.tools) {
        for (const auto& p : t.presets) {
            const lib::Material* mat = nullptr;
            for (const auto& m : s.materials)
                if (m.id == p.material)
                    mat = &m;
            std::string what;
            int limitedCount = 0;
            for (const auto& m : s.machines) {
                auto r = computeFeeds(FeedsInput{.tool = t,
                                                 .material = *mat,
                                                 .machine = m,
                                                 .radialEngagementMm = 0,
                                                 .axialDepthMm = 0,
                                                 .opType = opTypeFor(t.type),
                                                 .opClass = p.opClass});
                REQUIRE(r);
                std::string flags;
                const std::string machineKey = m.controlDialect;
                perMachine[machineKey + " total"]++;
                if (r->limitedByRpm)
                    perMachine[machineKey + " rpm"]++;
                if (r->limitedByFeed)
                    perMachine[machineKey + " feed"]++;
                if (r->limitedByPower)
                    perMachine[machineKey + " power"]++;
                if (r->limitedByTorque)
                    perMachine[machineKey + " torque"]++;
                if (r->thinningCapped)
                    perMachine[machineKey + " thinning capped"]++;
                if (r->mode == FeedsMode::Tapping && !r->tapFeedExact)
                    perMachine[machineKey + " tap feed inexact"]++;
                if (r->powerKw > 0 && m.spindlePowerKw > 0 && r->powerKw > 0.5 * m.spindlePowerKw)
                    perMachine[machineKey + " power over half"]++;
                if (r->limitedByRpm)
                    flags += " rpm";
                if (r->limitedByMinRpm)
                    flags += " minrpm";
                if (r->limitedByFeed)
                    flags += " feed";
                if (r->limitedByPower)
                    flags += " power";
                if (r->limitedByTorque)
                    flags += " torque";
                if (r->thinningCapped)
                    flags += " thin-cap";
                if (!flags.empty())
                    ++limitedCount;
                char buf[200];
                std::snprintf(buf, sizeof buf, " | %s: %.0f rpm (asked %.0f) %.1f mm/min%s",
                              m.model.empty() ? m.controlDialect.c_str() : m.model.c_str(), r->rpm,
                              r->limits.maxRpm.requested, r->feedMmMin, flags.c_str());
                what += buf;
            }
            if (limitedCount == 3) {
                std::printf("ALL3 %s / %s / %s%s\n", t.name.c_str(), mat->name.c_str(), p.opClass.c_str(),
                            what.c_str());
                ++limitedEverywhere[t.name];
            }
        }
    }
    for (const auto& [name, n] : limitedEverywhere)
        std::printf("tool %s: %d presets limited on all three\n", name.c_str(), n);
    for (const auto& [key, n] : perMachine)
        std::printf("%s: %d\n", key.c_str(), n);
    SUCCEED();
}
