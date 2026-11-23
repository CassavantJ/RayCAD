// The drilling operation (task T-035) in the seeded property harness (task T-058): random plates with holes, a tool
// that fits the cycle the parameters drew, the checker with the claims drilling makes.
#include "property.h"

#include <algorithm>
#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

std::string cycleOf(const std::string& params)
{
    for (const char* c : {"spot", "drill", "peck", "chipBreak", "tap", "bore", "ream"}) {
        if (params.find(std::string("\"cycle\":\"") + c + "\"") != std::string::npos) {
            return c;
        }
    }
    return "drill";
}

// The holes the case selected.
std::vector<Feature::Hole> selectedHoles(const PropertyCase& c)
{
    std::vector<Feature::Hole> out;
    for (const auto& [face, hole] : holeFaces(c)) {
        if (std::find(c.geom.selected.begin(), c.geom.selected.end(), face) != c.geom.selected.end()) {
            out.push_back(hole);
        }
    }
    return out;
}

// ISO 261 coarse thread whose tap drill (major - pitch, rounded as the part family's holes are) is d.
std::pair<double, double> threadFor(double d)
{
    static const std::vector<std::pair<double, double>> coarse = {{4, 0.7},  {5, 0.8},  {6, 1},
                                                                  {8, 1.25}, {10, 1.5}, {12, 1.75}};
    for (const auto& [major, pitch] : coarse) {
        if (std::abs(major - pitch - d) < 0.1) {
            return {major, pitch};
        }
    }
    return {d + 1, 1};
}

PropertyPlugin drillPlugin()
{
    PropertyPlugin p;
    p.type = "drill";
    p.tools = {lib::ToolType::Drill};
    p.needs = {Feature::Kind::Holes};
    p.geometry = pick::holes;
    p.ranges = {{"depth", {0.5, 3}}, {"peck", {0.5, 3}},       {"peckClearance", {0, 1}},
                {"dwell", {0, 1}},   {"breakthrough", {0, 2}}, {"tapLeadPitches", {0, 3}}};
    p.fixed = {{"spotDiameter", "0"}, {"allowOtherTools", "false"}};
    p.adjust = [](PropertyCase& c) {
        // One hole size per case (a tap, a reamer or a boring bar fits one size), the tool made for the cycle.
        const auto holes = holeFaces(c);
        if (holes.empty()) {
            return;
        }
        const double d = holes.front().second.diameter;
        std::vector<kernel::SubId> keep;
        for (const auto& [face, hole] : holes) {
            if (hole.diameter == d) {
                keep.push_back(face);
            }
        }
        c.geom.selected = keep;
        c.geom.selectedShapeIndex.assign(keep.size(), 0);
        const std::string cycle = cycleOf(c.op.paramsJson);
        lib::Tool t = c.tool;
        if (cycle == "spot") {
            t = randomTool(c.rng, lib::ToolType::SpotDrill, std::ceil(d) + 2);
            // A spot to a value would cut a chamfer the model does not have: spot to the hole.
            const std::string from = "\"depthFrom\":\"value\"";
            if (const auto at = c.op.paramsJson.find(from); at != std::string::npos) {
                c.op.paramsJson.replace(at, from.size(), "\"depthFrom\":\"hole\"");
            }
        } else if (cycle == "tap") {
            const auto [major, pitch] = threadFor(d);
            t = randomTool(c.rng, lib::ToolType::Tap, major);
            t.threadPitch = pitch;
        } else if (cycle == "bore") {
            t = randomTool(c.rng, lib::ToolType::BoringBar, d - c.rng.pick(std::vector<double>{0, 0.05}));
        } else if (cycle == "ream") {
            t = randomTool(c.rng, lib::ToolType::Reamer, d);
        } else {
            t = randomTool(c.rng, lib::ToolType::Drill, d - c.rng.pick(std::vector<double>{0, 0.1, 0.5}));
        }
        t.fluteLength = std::max(t.fluteLength, c.part.thickness + 5);
        t.shoulderLength = t.fluteLength;
        t.overallLength = t.fluteLength + 40;
        c.tool = t;
        c.op.tool.stickOut = std::round(t.fluteLength + c.rng.uniform(2, 15));
        if (cycle == "tap") {
            c.op.cutting.spindleRpm = std::round(c.op.cutting.spindleRpm / 10); // a tapping speed, whole
        }
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        // Earlier operations left the part: the holes are there. A tap cuts its thread into the bore's wall.
        k.initialStock = InitialStock::Part;
        if (c.tool.type == lib::ToolType::Tap) {
            for (const Feature::Hole& h : selectedHoles(c)) {
                k.allowedCuts.push_back(AllowedCut{h.x, h.y, c.tool.diameter / 2 + 0.01,
                                                   h.through ? -c.part.thickness - 50 : h.bottomZ, 50});
            }
        }
        return k;
    };
    p.minGenerated = 0.6;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("drill", "[property_drill]", drillPlugin())

TEST_CASE("invariant property: drill, the checker sees what drilling cuts",
          "[cam][invariants][property][property_drill]")
{
    // A tap cuts its thread past the modelled bore: without the claim that this is intended, every tapping case is a
    // gouge. This shows the drill cases are checked against the part, not passed vacuously.
    PropertyPlugin tap = drillPlugin();
    tap.fixed["cycle"] = "\"tap\"";
    tap.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.initialStock = InitialStock::Part;
        return k;
    };
    const PropertyOutcome o = runProperty(tap, seedsFor("drill", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[gouge]") != std::string::npos);
    }
}

TEST_CASE("invariant property: drill, a short stick-out is refused or kept clear",
          "[cam][invariants][property][property_drill]")
{
    // The holder 2 to 8 mm past the flutes, holes up to 25 deep: the operation must refuse (its holder clearance) or
    // keep the holder 0.5 mm clear of part and stock.
    PropertyPlugin p = drillPlugin();
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        c.withHolder = true;
        c.tool.fluteLength = std::round(c.rng.uniform(4, 12));
        c.tool.shoulderLength = c.tool.fluteLength;
        c.op.tool.stickOut = c.tool.fluteLength + std::round(c.rng.uniform(2, 8));
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("drill-short", 8));
    UNSCOPED_INFO(o.cases << " cases, " << o.generated << " toolpaths, " << o.refused << " refused");
    for (const std::string& r : o.refusals) {
        UNSCOPED_INFO("refused " << r);
    }
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.failures.empty());
}

// Found by T-058 as a product gap and since fixed: the drill type used to ignore the spindle direction, so a spindle
// set to turn counter-clockwise with a right-hand drill gave a toolpath. It now refuses, and this case holds it to
// that.
TEST_CASE("drilling with the spindle turning backwards is refused", "[cam][invariants][property][property_drill]")
{
    PropertyPlugin p = drillPlugin();
    p.fixed["cycle"] = "\"drill\"";
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        c.op.cutting.clockwise = false;
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("drill-ccw", 2));
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.generated == 0); // the operation should refuse
    CHECK(o.failures.empty());
}
