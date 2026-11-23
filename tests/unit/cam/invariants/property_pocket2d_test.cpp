// The 2D pocket operation (task T-054) in the seeded property harness (task T-058): random plates with pockets (some
// with islands), slots, open steps and bosses, every floor picked, a flat or bull nose end mill that fits the narrowest
// of them, parameters drawn from the schema; the checker with the claims pocketing makes: the floors cleared to the
// scallop limit (plus a bull nose's corner radius, which stands at the walls), stock to leave on walls and floors, the
// cut direction, ramp and helix limits.
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

// The value of a key of the harness's flat params JSON ("k":v,...), as text; empty when absent.
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

// The narrowest width the tool must pass through in the case's closed features: a pocket's or slot's smaller side,
// the band between a pocket and its island or round a boss.
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

PropertyPlugin pocketPlugin()
{
    PropertyPlugin p;
    p.type = "pocket2d";
    p.tools = {lib::ToolType::FlatEndMill, lib::ToolType::BullEndMill};
    p.needs = {Feature::Kind::Pocket};
    p.geometry = pick::pocketFloors;
    p.ranges = {{"stepover", {30, 85}},          {"stepdown", {1.5, 5}},        {"stockToLeaveWall", {0, 0.4}},
                {"stockToLeaveFloor", {0, 0.3}}, {"wallAllowance", {0.1, 0.4}}, {"floorAllowance", {0.1, 0.4}},
                {"helixDiameter", {40, 150}},    {"rampAngle", {1, 6}},         {"preDrillDiameter", {0, 0}}};
    p.adjust = [](PropertyCase& c) {
        std::string& json = c.op.paramsJson;
        // Pre-drilled entries need holes an earlier operation made, which the random parts do not have
        // (pocket2d_test.cpp covers them).
        if (param(json, "entry") == "\"preDrill\"") {
            setParam(json, "entry", c.rng.coin() ? "\"helix\"" : "\"ramp\"");
        }
        // A tool that fits the narrowest closed feature with what it leaves on the walls.
        const double leave = number(json, "stockToLeaveWall", 0) +
                             (param(json, "finishWalls") == "true" ? number(json, "wallAllowance", 0.2) : 0);
        const double room = narrowest(c) - 2 * leave;
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
        // A bull nose's stepover stays within its flat.
        if (c.tool.type == lib::ToolType::BullEndMill) {
            const double flat = 100 * (c.tool.diameter - 2 * c.tool.cornerRadius) / c.tool.diameter;
            const double s = number(json, "stepover", 45);
            if (s >= 0.95 * flat) {
                setParam(json, "stepover", std::to_string(std::floor(0.9 * flat)));
            }
        }
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        const std::string& json = c.op.paramsJson;
        k.stockToLeaveWall = number(json, "stockToLeaveWall", 0);
        k.stockToLeaveFloor = number(json, "stockToLeaveFloor", 0);
        k.direction = param(json, "cutMode") == "\"conventional\"" ? Direction::Conventional : Direction::Climb;
        k.maxRampAngleDeg = number(json, "rampAngle", 2) + 1e-6;
        k.helixMinDiameter = 0.3 * c.tool.diameter - 1e-6;
        const double corner = c.tool.type == lib::ToolType::BullEndMill ? c.tool.cornerRadius : 0;
        k.clears = coverFeatures(
            c, {Feature::Kind::Pocket, Feature::Kind::Slot, Feature::Kind::Step, Feature::Kind::Boss}, 0.01 + corner);
        return k;
    };
    p.minGenerated = 0.5;
    // Pocketing makes thousands of moves: a 0.2 mm stock grid keeps the quick seeds quick (slivers under 0.2 mm can
    // pass unseen; the unit tests check the coverage at the default 0.1 mm).
    p.options.stockCell = 0.2;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("pocket2d", pocketPlugin())

// One case by its seed, for a failure to be looked at alone: RAY_POCKET2D_SEED=<seed>, run "[.pocket2d-seed]".
TEST_CASE("invariant property: pocket2d, one seed", "[.pocket2d-seed]")
{
    const char* seed = std::getenv("RAY_POCKET2D_SEED");
    if (seed == nullptr) {
        SKIP("RAY_POCKET2D_SEED is not set");
    }
    const PropertyOutcome o = runProperty(pocketPlugin(), {std::stoull(seed)});
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    for (const std::string& f : o.refusals) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.failures.empty());
}

TEST_CASE("invariant property: pocket2d, the checker sees what pocketing leaves", "[cam][invariants][property]")
{
    // The coverage claims are not passed vacuously: the operation leaves 0.3 mm on the floors, the claims say it
    // leaves none, and every toolpath is then found keeping stock above the floor.
    PropertyPlugin p = pocketPlugin();
    p.tools = {lib::ToolType::FlatEndMill}; // a bull nose's claim allows its corner radius above the floor
    p.fixed["stockToLeaveFloor"] = "0.3";
    const auto claims = p.claims;
    p.claims = [claims](const PropertyCase& c) {
        Claims k = claims(c);
        k.stockToLeaveFloor = 0;
        return k;
    };
    const PropertyOutcome o = runProperty(p, seedsFor("pocket2d", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[coverage]") != std::string::npos);
    }
}
