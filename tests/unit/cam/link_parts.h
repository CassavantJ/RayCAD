// Pass sets shaped like the operations of the benchmark parts (PLAN.md 12.3: BP1 fixture plate, BP2 manifold block,
// BP3 steel bracket), for the ordering test (task T-051). Each is the passes one operation hands to linking, in
// nanometres in the setup WCS (origin at the stock's top front left corner, inch dimensions converted exactly), small
// enough for a brute-force best order. The layouts follow the plan's descriptions; positions the plan leaves open are
// chosen here and say so.
#pragma once
#include "link_test_util.h"

#include <string>
#include <vector>

namespace ray::cam::link::test {

inline constexpr Coord kInch = 25'400'000;

struct PartCase {
    std::string name;
    std::vector<Pass> passes;
    Options options;
    Stock stock;
};

namespace parts_detail {

inline Coord in(double inches)
{
    return geom2d::roundNm(inches * static_cast<double>(kInch));
}
inline Pass ring(Path p, Nm z, int cluster = -1, int level = 0)
{
    Pass r = passFromPath(p, z, kCutFeed);
    r.rotatable = true;
    r.cluster = cluster;
    r.level = level;
    return r;
}
inline Pass stroke(Pt a, Pt b, Nm z, int cluster = -1, int level = 0)
{
    Pass s = passFromPath(openPath(a, {line(b)}), z, kCutFeed);
    s.reversible = true;
    s.cluster = cluster;
    s.level = level;
    return s;
}
inline Options plungeOptions(Coord toolDiameter, Nm top)
{
    Options o = baseOptions(toolDiameter);
    o.clearance = top + in(1.0);
    o.retract = top + in(0.2);
    o.feedPlane = top + in(0.05);
    o.entryOrder = {EntryKind::Plunge};
    o.centreCutting = true;
    return o;
}

} // namespace parts_detail

// BP1, setup 1 (6.25 x 4.25 x 0.75 in plate, 6.00 x 4.00 part): the operations that link several passes.
inline std::vector<PartCase> benchmarkCases()
{
    using namespace parts_detail;
    std::vector<PartCase> cases;
    const Stock bp1 = boxStock(0, 0, in(6.25), in(4.25), 0);
    // Four 0.44 x 0.26 counterbores (1/4 in tool: the centre circles 0.095 in in radius) on a 5.0 x 3.0 in pattern
    // centred on the plate (the pattern is this test's choice).
    {
        PartCase c{"BP1 counterbores", {}, plungeOptions(in(0.25), 0), bp1};
        for (const double x : {0.625, 5.625})
            for (const double y : {0.625, 3.625})
                c.passes.push_back(ring(Path::circle({in(x), in(y)}, in(0.095), true), -in(0.26)));
        cases.push_back(c);
    }
    // Two 2.0 x 1.5 in pockets 0.375 deep (1/2 in tool), two rings each at two depths: the wall ring and one ring a
    // stepover (0.3 in) inside, a cluster per pocket so each pocket's depths stay in order.
    {
        PartCase c{"BP1 pockets", {}, plungeOptions(in(0.5), 0), bp1};
        int cluster = 0;
        for (const double cx : {1.75, 4.5}) {
            for (int level = 0; level < 2; ++level) {
                const Nm z = -in(0.1875 * (level + 1));
                for (const double inset : {0.25, 0.55}) {
                    c.passes.push_back(ring(rect(in(cx - 1.0 + inset), in(2.125 - 0.75 + inset), in(cx + 1.0 - inset),
                                                 in(2.125 + 0.75 - inset)),
                                            z, cluster, level));
                }
            }
            ++cluster;
        }
        cases.push_back(c);
    }
    // 0.02 in chamfer of the eight holes: four 0.266 through holes and four 1/4-20 tapped holes (#7 drill, 0.201) at
    // 0.5 in from each part corner and on the counterbore pattern (this test's layout), a 90 degree chamfer mill.
    {
        PartCase c{"BP1 hole chamfers", {}, plungeOptions(in(0.25), 0), bp1};
        for (const double x : {0.625, 5.625})
            for (const double y : {0.625, 3.625})
                c.passes.push_back(ring(Path::circle({in(x), in(y)}, in(0.153), true), -in(0.03)));
        for (const double x : {2.0, 4.25})
            for (const double y : {0.9, 3.35})
                c.passes.push_back(ring(Path::circle({in(x), in(y)}, in(0.12), true), -in(0.03)));
        cases.push_back(c);
    }
    // BP2 (3.25 x 3.25 x 2.1 in block): engraving a part number, eight straight strokes (1/16 in engraver).
    {
        const Stock bp2 = boxStock(0, 0, in(3.25), in(3.25), 0);
        PartCase c{"BP2 engraving", {}, plungeOptions(in(0.0625), 0), bp2};
        const double x0 = 0.6, y0 = 2.6, h = 0.25, w = 0.16, gap = 0.24;
        for (int k = 0; k < 4; ++k) {
            const double x = x0 + gap * k;
            c.passes.push_back(stroke({in(x), in(y0)}, {in(x), in(y0 + h)}, -in(0.005)));
            c.passes.push_back(
                stroke({in(x), in(y0 + h)}, {in(x + w), in(y0 + h * (k % 2 == 0 ? 1.0 : 0.5))}, -in(0.005)));
        }
        cases.push_back(c);
    }
    // BP2: the open-sided step 0.5 deep, four zigzag strokes across it at two depths (1/2 in tool).
    {
        const Stock bp2 = boxStock(0, 0, in(3.25), in(3.25), 0);
        PartCase c{"BP2 step", {}, plungeOptions(in(0.5), 0), bp2};
        for (int level = 0; level < 2; ++level)
            for (int k = 0; k < 4; ++k) {
                const double y = 0.4 + 0.35 * k;
                c.passes.push_back(stroke({-in(0.3), in(y)}, {in(1.0), in(y)}, -in(0.25 * (level + 1)), 0, level));
            }
        cases.push_back(c);
    }
    // BP2: the 1.500 in circular pocket 0.75 deep, three rings at two depths (3/8 in tool).
    {
        const Stock bp2 = boxStock(0, 0, in(3.25), in(3.25), 0);
        PartCase c{"BP2 circular pocket", {}, plungeOptions(in(0.375), 0), bp2};
        for (int level = 0; level < 2; ++level)
            for (const double r : {0.15, 0.36, 0.5625})
                c.passes.push_back(
                    ring(Path::circle({in(2.1), in(1.4)}, in(r), true), -in(0.375 * (level + 1)), 0, level));
        cases.push_back(c);
    }
    // BP3 (4.25 x 2.25 x 1.0 in bracket): the stepped profile as two open chains (front and back), three depths
    // each, and the 0.375 in through slot's centre line at two depths (3/8 in tool; positions this test's choice).
    {
        const Stock bp3 = boxStock(0, 0, in(4.25), in(2.25), 0);
        PartCase c{"BP3 profile and slot", {}, plungeOptions(in(0.375), 0), bp3};
        for (int level = 0; level < 3; ++level) {
            const Nm z = -in(0.3 * (level + 1));
            Pass front = passFromPath(openPath({-in(0.2), in(0.3)}, {line({in(2.0), in(0.3)}), line({in(2.0), in(0.6)}),
                                                                     line({in(4.45), in(0.6)})}),
                                      z, kCutFeed);
            front.reversible = true;
            front.cluster = 0;
            front.level = level;
            c.passes.push_back(front);
            Pass back = passFromPath(openPath({-in(0.2), in(1.95)}, {line({in(4.45), in(1.95)})}), z, kCutFeed);
            back.reversible = true;
            back.cluster = 1;
            back.level = level;
            c.passes.push_back(back);
        }
        for (int level = 0; level < 2; ++level)
            c.passes.push_back(stroke({in(2.6), in(1.1)}, {in(3.6), in(1.1)}, -in(0.5 * (level + 1)), 2, level));
        cases.push_back(c);
    }
    // BP3, setup 2: the 0.25 deep back pocket, two rings at two depths (1/4 in tool).
    {
        const Stock bp3 = boxStock(0, 0, in(4.25), in(2.25), 0);
        PartCase c{"BP3 back pocket", {}, plungeOptions(in(0.25), 0), bp3};
        for (int level = 0; level < 2; ++level)
            for (const double inset : {0.0, 0.2})
                c.passes.push_back(ring(rect(in(0.8 + inset), in(0.6 + inset), in(2.4 - inset), in(1.65 - inset)),
                                        -in(0.125 * (level + 1)), 0, level));
        cases.push_back(c);
    }
    return cases;
}

} // namespace ray::cam::link::test
