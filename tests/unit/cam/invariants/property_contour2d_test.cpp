// The 2D contour operation (task T-053) in the seeded property harness (task T-058): random plates with pockets,
// slots, steps and bosses; either the plate's outline (its four top edges, a closed chain the tool runs outside) or
// the floors of the features (their faces' loops); parameters drawn from the schema, with compensation in the
// computer or for wear (the checker reads coordinates as the tool centre, so compensation in the control, where the
// toolpath is the contour itself, is checked in tests/unit/cam/contour2d_*). The claims: the direction asked for, the
// stock to leave, the heights.
#include "property.h"

#include <algorithm>
#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

// The straight edges of the plate's top outline (Z 0, on the plate's sides).
std::vector<kernel::SubId> outlineEdges(PropertyCase& c)
{
    std::vector<kernel::SubId> out;
    const kernel::Shape& s = c.part.shape;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId e{kernel::SubKind::Edge, i};
        const auto info = kernel::curveInfo(s, e);
        if (!info || info->type != kernel::CurveType::Line || std::abs(info->start.z) > 1e-9 ||
            std::abs(info->end.z) > 1e-9) {
            continue;
        }
        const auto onSide = [&](const Vec3& p) {
            return std::abs(p.x) < 1e-9 || std::abs(p.x - c.part.length) < 1e-9 || std::abs(p.y) < 1e-9 ||
                   std::abs(p.y - c.part.width) < 1e-9;
        };
        const bool alongX = std::abs(info->start.y - info->end.y) < 1e-9;
        const bool alongY = std::abs(info->start.x - info->end.x) < 1e-9;
        if (onSide(info->start) && onSide(info->end) && (alongX || alongY) &&
            ((alongX && (std::abs(info->start.y) < 1e-9 || std::abs(info->start.y - c.part.width) < 1e-9)) ||
             (alongY && (std::abs(info->start.x) < 1e-9 || std::abs(info->start.x - c.part.length) < 1e-9)))) {
            out.push_back(e);
        }
    }
    return out;
}

bool has(const std::string& params, const std::string& text)
{
    return params.find(text) != std::string::npos;
}

double number(const std::string& params, const std::string& key)
{
    const std::string pat = "\"" + key + "\":";
    const auto at = params.find(pat);
    return at == std::string::npos ? 0.0 : std::atof(params.c_str() + at + pat.size());
}

// Sets the value of `key` (JSON text) in the flat params object the harness drew.
void setParam(std::string& params, const std::string& key, const std::string& value)
{
    const std::string pat = "\"" + key + "\":";
    const auto at = params.find(pat);
    if (at == std::string::npos) {
        return;
    }
    const std::size_t from = at + pat.size();
    const std::size_t to = params.find_first_of(",}", from);
    params.replace(from, to - from, value);
}

PropertyPlugin contourPlugin()
{
    PropertyPlugin p;
    p.type = "contour2d";
    p.tools = {lib::ToolType::FlatEndMill, lib::ToolType::BullEndMill, lib::ToolType::BallEndMill};
    p.needs = {};
    p.geometry = [](PropertyCase& c) {
        if (c.rng.coin(0.5)) {
            return outlineEdges(c);
        }
        // One feature's floor (several at once are refused together when any one of them cannot be cut).
        std::vector<kernel::SubId> floors = pick::pocketFloors(c);
        if (floors.size() > 1) {
            floors = {c.rng.pick(floors)};
        }
        return floors;
    };
    p.ranges = {{"stockToLeave", {0, 0.5}},
                {"stockToLeaveAxial", {0, 0.3}},
                {"maxStepdown", {1, 6}},
                {"roughStepover", {0.5, 2}},
                {"finishAllowance", {0.1, 0.5}},
                {"leadRadius", {0, 4}},
                {"leadSweep", {30, 180}},
                {"leadLength", {0, 3}},
                {"rampAngle", {2, 15}},
                {"tabCount", {1, 4}},
                {"tabWidth", {2, 8}},
                {"tabHeight", {0.5, 2}},
                {"roughPasses", {1, 2}}};
    // The tool side found from the part: a side drawn at random puts the tool into the part (refused, rightly) more
    // often than not. The given sides are tested in tests/unit/cam/contour2d_rules_test.cpp. The floors' outer loops
    // only: an island's loop is lower than the plate's top, so above the island the tool runs round air beside the
    // pocket's wall, and the checker would judge the direction against that wall (the island loops are tested in the
    // rules and golden tests).
    p.fixed = {{"side", "\"auto\""}, {"faceLoops", "\"outer\""}};
    p.adjust = [](PropertyCase& c) {
        std::string& params = c.op.paramsJson;
        // Compensation in the control programs the contour, which the checker would read as the tool centre.
        if (has(params, "\"compensation\":\"control\"")) {
            setParam(params, "compensation", "\"computer\"");
        }
        // Inside a feature compensation leaves no entry but a plunge (no ramp with G41/G42), which a tool that does not
        // cut at its centre cannot make: wear is tried on the outline, which is entered from outside the stock.
        const bool outline = !c.geom.selected.empty() && c.geom.selected.front().kind == kernel::SubKind::Edge;
        if (!outline && has(params, "\"compensation\":\"wear\"")) {
            setParam(params, "compensation", "\"computer\"");
        }
        // For wear: leads with a straight part (else refused, as the rules ask), no tabs (rectangular tabs of a tool
        // that does not cut at its centre are refused with compensation).
        if (has(params, "\"compensation\":\"wear\"")) {
            setParam(params, "leadLength", "1.5");
            if (has(params, "\"leadType\":\"none\"")) {
                setParam(params, "leadType", "\"line\"");
            }
            setParam(params, "tabs", "false");
        }
        // Triangular tabs come down at up to atan(2 * 2 / 2): a ramp angle that allows it (else refused).
        if (has(params, "\"tabShape\":\"triangular\"")) {
            setParam(params, "rampAngle", "45");
            setParam(params, "tabWidth", "8");
        }
        // Pocket walls: a tool that fits most of the family's features.
        if (!outline) {
            c.tool = randomTool(c.rng, c.tool.type, std::round(c.rng.uniform(3, 7)));
        }
        // Heights from the stock top; the bottom from the chain (the plate's top or a feature's floor): some way down
        // the plate's side, or the floor itself.
        c.op.heights.clearance = std::round(c.rng.uniform(5, 15));
        c.op.heights.retract = std::round(c.rng.uniform(2, 4));
        c.op.heights.feedPlane = 1;
        c.op.heights.top = 0;
        c.op.heights.bottom = outline ? -std::round(c.rng.uniform(1, std::max(1.5, c.part.thickness - 2))) : 0;
        // Flutes long enough for most cases; the stick-out clear of the stock.
        c.tool.fluteLength = std::max(c.tool.fluteLength, c.part.thickness + 4);
        c.tool.shoulderLength = std::max(c.tool.shoulderLength, c.tool.fluteLength);
        c.tool.overallLength = std::max(c.tool.overallLength, c.tool.shoulderLength + 30);
        c.op.tool.stickOut = std::round(c.part.thickness + 6 + c.rng.uniform(0, 10));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.direction = has(c.op.paramsJson, "\"conventional\"") ? Direction::Conventional : Direction::Climb;
        k.stockToLeaveWall = number(c.op.paramsJson, "stockToLeave");
        k.stockToLeaveFloor = number(c.op.paramsJson, "stockToLeaveAxial");
        const double top = c.geom.stockBounds.max.z;
        k.heights.clearance = top + c.op.heights.clearance;
        k.heights.retract = top + c.op.heights.retract;
        k.heights.feedPlane = top + c.op.heights.feedPlane;
        return k;
    };
    p.minGenerated = 0.5;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("contour2d", "[property_contour2d]", contourPlugin())
