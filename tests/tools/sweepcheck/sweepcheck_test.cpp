// The sweep checker's own tests (task T-060): S5's self-test (spikes/s5-adaptive/src/check.cpp selfTest) on the
// library, and toolpaths that are wrong on purpose, each of which the checker must report, beside a correct twin.
#include "s5_cases.h"
#include "sweepcheck.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace ray::test::sweep;

namespace {

constexpr double kTool = 12.7;
constexpr double kR = kTool / 2;

Path line(Kind kind, double x0, double y0, double x1, double y1, bool measured = true)
{
    Path p;
    p.kind = kind;
    p.points = {{x0, y0}, {x1, y1}};
    p.measured = measured;
    return p;
}

// A slot along y = 20 (not measured), then a pass at y = 20 + ae measured over x in [30, 90] (S5's window), in a
// pocket 120 x 50 of solid material.
Report straightPass(double ae)
{
    Input in;
    in.pocket = {s5::rect(0, 0, 120, 50)};
    in.stock = {s5::rect(-5, -5, 125, 55)};
    in.toolDiameter = kTool;
    in.targetDeg = straightEngagementDeg(ae, kR);
    Region rg;
    rg.start = {10, 20};
    rg.paths.push_back(line(Kind::Cut, 10, 20, 110, 20, false));
    rg.paths.push_back(line(Kind::Retract, 110, 20, 10, 20 + ae));
    rg.paths.push_back(line(Kind::Cut, 10, 20 + ae, 30, 20 + ae, false));
    rg.paths.push_back(line(Kind::Cut, 30, 20 + ae, 90, 20 + ae, true));
    rg.paths.push_back(line(Kind::Cut, 90, 20 + ae, 110, 20 + ae, false));
    in.regions.push_back(rg);
    return check(in);
}

// A hole of radius 15 already cleared, then the tool around it at centre radius 15 - r + ae, measured over 3/4 turn.
Report insideArc(double ae, double& expected)
{
    const double R0 = 15, cx = 30, cy = 30, rho = R0 - kR + ae;
    expected = std::acos((R0 * R0 - rho * rho - kR * kR) / (2 * rho * kR)) * 180 / kPi;
    Input in;
    in.pocket = {s5::rect(0, 0, 60, 60)};
    in.stock = {s5::rect(-5, -5, 65, 65)};
    in.cleared = {s5::circle(cx, cy, R0)};
    in.toolDiameter = kTool;
    in.targetDeg = expected;
    Region rg;
    rg.start = {cx + rho, cy};
    const int n = 2000;
    for (int part = 0; part < 3; part++) {
        Path p;
        p.kind = Kind::Cut;
        p.measured = part == 1;
        int k0 = part == 0 ? 0 : part == 1 ? n / 8 : n * 7 / 8;
        int k1 = part == 0 ? n / 8 : part == 1 ? n * 7 / 8 : n;
        for (int k = k0; k <= k1; k++) {
            double a = 2 * kPi * k / n;
            p.points.push_back({cx + rho * std::cos(a), cy + rho * std::sin(a)});
        }
        rg.paths.push_back(p);
    }
    in.regions.push_back(rg);
    return check(in);
}

} // namespace

TEST_CASE("S5 self-test: straight passes read their engagement within 3 degrees", "[sweepcheck]")
{
    for (double ae : {6.35, 2.54, 1.27, 0.635}) {
        Report r = straightPass(ae);
        double expected = straightEngagementDeg(ae, kR);
        INFO("ae " << ae << " expected " << expected << ": " << summary(r));
        CHECK(std::fabs(r.median - expected) <= 3.0);
        CHECK(std::fabs(r.cutLength - 200.0) < 1e-9); // every cut counts in the length, measured or not
    }
    // A full slot reads 180 degrees.
    Input in;
    in.pocket = {s5::rect(0, 0, 120, 50)};
    in.stock = {s5::rect(-5, -5, 125, 55)};
    in.toolDiameter = kTool;
    in.targetDeg = 180;
    Region rg;
    rg.start = {10, 20};
    rg.paths.push_back(line(Kind::Cut, 10, 20, 30, 20, false));
    rg.paths.push_back(line(Kind::Cut, 30, 20, 90, 20, true));
    in.regions.push_back(rg);
    Report r = check(in);
    INFO(summary(r));
    CHECK(std::fabs(r.median - 180.0) <= 3.0);
}

TEST_CASE("S5 self-test: passes inside a cleared hole read their engagement within 3 degrees", "[sweepcheck]")
{
    for (double ae : {2.54, 1.27}) {
        double expected = 0;
        Report r = insideArc(ae, expected);
        INFO("ae " << ae << " expected " << expected << ": " << summary(r));
        CHECK(std::fabs(r.median - expected) <= 3.0);
    }
}

TEST_CASE("A pass too close to the wall is a gouge; its twin at the tool radius is not", "[sweepcheck]")
{
    auto run = [](double offset) {
        Input in;
        in.pocket = {s5::rect(0, 0, 60, 40)};
        in.stock = {s5::rect(-10, -10, 70, 50)};
        in.toolDiameter = kTool;
        in.targetDeg = 90;
        in.cell = 0.05;
        Region rg;
        rg.start = {kR + 5, kR - offset};
        rg.paths.push_back(line(Kind::Cut, kR + 5, kR - offset, 55 - kR, kR - offset));
        in.regions.push_back(rg);
        return check(in);
    };
    Report bad = run(0.050);
    Report good = run(0.0);
    INFO("bad: " << summary(bad) << "\ngood: " << summary(good));
    CHECK(std::fabs(bad.gougeAnalytic - 0.050) < 1e-6);
    CHECK(bad.gougeArea > 0);
    CHECK(good.gougeAnalytic < 1e-9);
    CHECK(good.gougeArea == 0);
}

TEST_CASE("A band left between passes is uncut area; with the pass that clears it there is none", "[sweepcheck]")
{
    auto run = [](bool middle) {
        Input in;
        in.pocket = {s5::rect(0, 0, 60, 30)};
        in.stock = {s5::rect(-10, -10, 70, 40)};
        in.toolDiameter = kTool;
        in.targetDeg = 90;
        in.cell = 0.05;
        Region rg;
        rg.start = {kR, kR};
        rg.paths.push_back(line(Kind::Cut, kR, kR, 60 - kR, kR));
        rg.paths.push_back(line(Kind::Retract, 60 - kR, kR, kR, 30 - kR));
        rg.paths.push_back(line(Kind::Cut, kR, 30 - kR, 60 - kR, 30 - kR));
        if (middle) {
            rg.paths.push_back(line(Kind::Retract, 60 - kR, 30 - kR, kR, 15));
            rg.paths.push_back(line(Kind::Cut, kR, 15, 60 - kR, 15));
            // Along both ends, so no cusp is left between the rows where they end.
            rg.paths.push_back(line(Kind::Cut, 60 - kR, 15, 60 - kR, kR));
            rg.paths.push_back(line(Kind::Cut, 60 - kR, kR, 60 - kR, 30 - kR));
            rg.paths.push_back(line(Kind::Retract, 60 - kR, 30 - kR, kR, 30 - kR));
            rg.paths.push_back(line(Kind::Cut, kR, 30 - kR, kR, kR));
        }
        in.regions.push_back(rg);
        return check(in);
    };
    Report bad = run(false);
    Report good = run(true);
    INFO("bad: " << summary(bad) << "\ngood: " << summary(good));
    CHECK(bad.uncutReachable > 100); // a band about 4.6 x 47 mm
    CHECK(bad.uncutMaxThickness > 1.5);
    CHECK(good.uncutReachable == 0);
}

TEST_CASE("A pass at twice the stepover reads as overload against the target", "[sweepcheck]")
{
    // Target 10% load (36.9 degrees, ae 1.27); the pass takes ae 2.54 (53.1 degrees) over its measured length.
    const double target = straightEngagementDeg(1.27, kR);
    Input in;
    in.pocket = {s5::rect(0, 0, 120, 50)};
    in.stock = {s5::rect(-5, -5, 125, 55)};
    in.toolDiameter = kTool;
    in.targetDeg = target;
    Region rg;
    rg.start = {10, 20};
    rg.paths.push_back(line(Kind::Cut, 10, 20, 110, 20, false));
    rg.paths.push_back(line(Kind::Retract, 110, 20, 10, 22.54));
    rg.paths.push_back(line(Kind::Cut, 10, 22.54, 30, 22.54, false));
    rg.paths.push_back(line(Kind::Cut, 30, 22.54, 90, 22.54, true));
    in.regions.push_back(rg);
    Report over = check(in);
    INFO(summary(over));
    CHECK(over.median > target + 10);
    CHECK(over.pctAboveTargetPlus10 > 90);
    CHECK(over.p99 > target + 6);
}

TEST_CASE("A tool-down link through material removes area; through a cleared slot it removes none", "[sweepcheck]")
{
    auto run = [](bool throughMaterial) {
        Input in;
        in.pocket = {s5::rect(0, 0, 80, 40)};
        in.stock = {s5::rect(-10, -10, 90, 50)};
        in.toolDiameter = kTool;
        in.targetDeg = 53.13;
        Region rg;
        rg.start = {10, 10};
        rg.paths.push_back(line(Kind::Cut, 10, 10, 70, 10)); // a slot along y = 10
        // Back along the slot, or across solid material to y = 30.
        rg.paths.push_back(throughMaterial ? line(Kind::LinkDown, 70, 10, 10, 30)
                                           : line(Kind::LinkDown, 70, 10, 10, 10));
        in.regions.push_back(rg);
        return check(in);
    };
    Report bad = run(true);
    Report good = run(false);
    INFO("bad: " << summary(bad) << "\ngood: " << summary(good));
    CHECK(bad.linkRemovedMm2 > 100);
    CHECK(bad.linkMaxDeg > 90);
    CHECK(good.linkRemovedMm2 == 0);
    CHECK(good.linkPctEngaged == 0);
}

TEST_CASE("A cut through cleared area is air; a retract removes nothing", "[sweepcheck]")
{
    Input in;
    in.pocket = {s5::rect(0, 0, 80, 40)};
    in.stock = {s5::rect(-10, -10, 90, 50)};
    in.cleared = {s5::rect(0, 0, 80, 20)};
    in.toolDiameter = kTool;
    in.targetDeg = 53.13;
    Region rg;
    rg.start = {10, 10};
    rg.paths.push_back(line(Kind::Cut, 10, 10, 70, 10));     // the disc stays in the cleared area
    rg.paths.push_back(line(Kind::Retract, 70, 10, 10, 33)); // across material, not swept
    in.regions.push_back(rg);
    Report r = check(in);
    INFO(summary(r));
    CHECK(r.pctAir == 100);
    CHECK(r.median < 2);
    CHECK(r.retractLength > 60);
    CHECK(r.uncutReachable > 500); // the upper half is still there
}

TEST_CASE("A helix entry sweeps its circle and a gap between paths is a swept join", "[sweepcheck]")
{
    Input in;
    in.pocket = {s5::circle(30, 30, 20)};
    in.stock = {s5::rect(0, 0, 60, 60)};
    in.toolDiameter = kTool;
    in.targetDeg = 53.13;
    in.cell = 0.05;
    Region rg;
    rg.helixCentre = {30, 30};
    rg.helixRadius = 4;
    rg.start = {30, 26};
    rg.paths.push_back(line(Kind::Cut, 31, 26, 34, 26)); // 1 mm gap from the start
    in.regions.push_back(rg);
    Report r = check(in);
    INFO(summary(r));
    CHECK(std::fabs(r.helixLength - 2 * kPi * 4) < 1e-9);
    CHECK(r.implicitJoins == 1);
    CHECK(std::fabs(r.joinMax - 1) < 1e-9);
    CHECK(std::fabs(r.cutLength - 4) < 1e-9); // the join is swept as part of the cut
    CHECK(r.uncutReachable > 300);
}
