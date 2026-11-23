// The 2D adaptive roughing operation (task T-061) in the seeded property harness (task T-058): random plates with
// pockets (some with islands), slots, open steps and bosses, every floor picked, a flat or bull nose end mill that fits
// the narrowest of them, parameters drawn from the schema; the checker with the claims adaptive roughing makes: the
// floors cleared to the scallop limit (plus a bull nose's corner radius, which stands at the walls), stock to leave on
// walls and floors, the ramp angle and the smallest helix. The engagement is not claimed here: a ramp's line and a
// slot narrower than the helix are full-width cuts by design (at the reduced feed); the engagement the passes hold is
// measured on spike S5's pockets (adaptive2d_test.cpp and the engine's tests). Nor is the cut direction: the engine's
// passes are not all climb.
#include "property.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

std::string param(const std::string& json, const std::string& key)
{
    const std::string k = "\"" + key + "\":";
    const auto at = json.find(k);
    if (at == std::string::npos) {
        return {};
    }
    const auto from = at + k.size();
    const auto to = json.find_first_of(",}", from);
    return json.substr(from, to - from);
}
void setParam(std::string& json, const std::string& key, const std::string& value)
{
    const std::string k = "\"" + key + "\":";
    const auto at = json.find(k);
    if (at == std::string::npos) {
        json.insert(json.size() - 1, std::string(json.size() > 2 ? "," : "") + k + value);
        return;
    }
    const auto from = at + k.size();
    const auto to = json.find_first_of(",}", from);
    json.replace(from, to - from, value);
}
double number(const std::string& json, const std::string& key, double fallback)
{
    const std::string v = param(json, key);
    return v.empty() ? fallback : std::stod(v);
}

struct Box2 {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
};
Box2 boxOf(const std::vector<Vec2>& loop)
{
    Box2 b;
    for (const Vec2& p : loop) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

// The narrowest width the tool must pass through in the case's closed features (property_pocket2d_test.cpp).
double narrowest(const PropertyCase& c)
{
    double w = std::numeric_limits<double>::infinity();
    for (const Feature& f : c.part.features) {
        if (f.kind == Feature::Kind::Holes || f.kind == Feature::Kind::Step || f.region.empty()) {
            continue;
        }
        const Box2 o = boxOf(f.region[0]);
        w = std::min({w, o.x1 - o.x0, o.y1 - o.y0});
        for (std::size_t k = 1; k < f.region.size(); ++k) {
            const Box2 i = boxOf(f.region[k]);
            w = std::min({w, i.x0 - o.x0, o.x1 - i.x1, i.y0 - o.y0, o.y1 - i.y1});
        }
    }
    return w;
}

PropertyPlugin adaptivePlugin()
{
    PropertyPlugin p;
    p.type = "adaptive2d";
    p.tools = {lib::ToolType::FlatEndMill, lib::ToolType::BullEndMill};
    p.needs = {Feature::Kind::Pocket};
    p.geometry = pick::pocketFloors;
    p.ranges = {{"stepover", {8, 40}},           {"stepdown", {1.5, 5}},       {"stockToLeaveWall", {0, 0.4}},
                {"stockToLeaveFloor", {0, 0.3}}, {"helixDiameter", {40, 150}}, {"helixMinDiameter", {25, 40}},
                {"rampAngle", {1, 6}},           {"reducedFeed", {20, 100}},   {"preDrillDiameter", {0, 0}}};
    p.adjust = [](PropertyCase& c) {
        std::string& json = c.op.paramsJson;
        // Pre-drilled entries need holes an earlier operation made, which the random parts do not have
        // (adaptive2d_golden_test.cpp covers them).
        if (param(json, "entry") == "\"preDrill\"") {
            setParam(json, "entry", c.rng.coin() ? "\"helix\"" : "\"ramp\"");
        }
        if (number(json, "helixMinDiameter", 30) > number(json, "helixDiameter", 75)) {
            setParam(json, "helixMinDiameter", param(json, "helixDiameter"));
        }
        // A tool that fits the narrowest closed feature with what it leaves on the walls.
        const double room = narrowest(c) - 2 * number(json, "stockToLeaveWall", 0);
        std::vector<double> fit;
        for (const double d : {3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0}) {
            if (d <= 0.8 * room) {
                fit.push_back(d);
            }
        }
        const double d = fit.empty() ? 3.0 : c.rng.pick(fit);
        const lib::ToolType type = c.tool.type;
        c.tool = randomTool(c.rng, type, d);
        c.tool.fluteLength = std::max(c.tool.fluteLength, 5.5);
        c.tool.shoulderLength = std::max(c.tool.shoulderLength, c.tool.fluteLength);
        c.tool.overallLength = std::max(c.tool.overallLength, c.tool.shoulderLength + 30);
        c.op.tool.stickOut = std::round(std::max(c.op.tool.stickOut, c.part.thickness + 6));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        const std::string& json = c.op.paramsJson;
        k.stockToLeaveWall = number(json, "stockToLeaveWall", 0);
        k.stockToLeaveFloor = number(json, "stockToLeaveFloor", 0);
        k.direction = Direction::Any;
        k.maxRampAngleDeg = number(json, "rampAngle", 2);
        k.helixMinDiameter = number(json, "helixMinDiameter", 30) / 100 * c.tool.diameter - 1e-5;
        const double corner = c.tool.type == lib::ToolType::BullEndMill ? c.tool.cornerRadius : 0;
        k.clears = coverFeatures(
            c, {Feature::Kind::Pocket, Feature::Kind::Slot, Feature::Kind::Step, Feature::Kind::Boss}, 0.01 + corner);
        return k;
    };
    p.minGenerated = 0.5;
    p.options.stockCell = 0.2; // as pocket2d: thousands of moves (property_pocket2d_test.cpp)
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("adaptive2d", "[property_adaptive2d]", adaptivePlugin())

// One case by its seed, for a failure to be looked at alone: RAY_ADAPTIVE2D_SEED=<seed>, run "[.adaptive2d-seed]".
TEST_CASE("invariant property: adaptive2d, one seed", "[.adaptive2d-seed]")
{
    const char* seed = std::getenv("RAY_ADAPTIVE2D_SEED");
    if (seed == nullptr) {
        SKIP("RAY_ADAPTIVE2D_SEED is not set");
    }
    const PropertyOutcome o = runProperty(adaptivePlugin(), {std::stoull(seed)});
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    for (const std::string& f : o.refusals) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.failures.empty());
}
