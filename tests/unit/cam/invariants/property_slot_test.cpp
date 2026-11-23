// The slot operation (task T-055) in the seeded property harness (task T-058): random plates with slots, the floor of
// each slot of one width picked, a flat end mill of that width (sometimes a little narrower or too wide, which the
// operation must accept with a warning or refuse), and the checker with the claims slotting makes: the ramp angle,
// the tool centre inside the slots, the slots' floors cleared.
//
// What the checker judges here: the flutes, shank and holder against the part (both walls, the floor, the ends) to
// its bounds, the ramps' angle and every descent into stock (no plunge with a flat end mill), rapids over stock,
// engagement deeper than the flutes, the tool centre inside the slot. What it cannot: the climb or conventional side
// (a slot has walls on both sides, README.md "Direction on stock"), and coverage only where a disc of the tool fits
// with a stock cell of margin: with the tool exactly as wide as the slot that is nowhere, so the floor claim checks
// little (the generator proves its floor covered itself, slot.h (1)).
#include "mini_json.h"
#include "property.h"

#include <algorithm>
#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

double paramOf(const std::string& params, const std::string& key, double fallback)
{
    const auto v = json::parse(params);
    if (!v) {
        return fallback;
    }
    const json::Value* n = v->find(key);
    return n != nullptr && n->type == json::Value::Type::Number ? n->number : fallback;
}

// The width of a slot feature (the harness's slots run along X: their Y extent).
double widthOf(const Feature& f)
{
    double lo = 1e300, hi = -1e300;
    for (const Vec2& p : f.region.front()) {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
    }
    return hi - lo;
}

// The up-facing planar face of the part at the slot's floor inside the slot's box.
std::optional<kernel::SubId> floorOf(const kernel::Shape& s, const Feature& f)
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Vec2& p : f.region.front()) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId face{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, face);
        const auto sample = kernel::faceSample(s, face);
        if (!info || !sample || info->type != kernel::SurfaceType::Plane || sample->normal.z < 0.999) {
            continue;
        }
        const Box3& b = info->bounds;
        if (std::abs(b.min.z - f.floorZ) < 1e-6 && b.min.x > x0 - 1e-3 && b.max.x < x1 + 1e-3 && b.min.y > y0 - 1e-3 &&
            b.max.y < y1 + 1e-3) {
            return face;
        }
    }
    return std::nullopt;
}

// The slot features of one width (the first slot's) and their floors.
std::vector<std::pair<kernel::SubId, const Feature*>> slotsOfOneWidth(const PropertyCase& c)
{
    std::vector<std::pair<kernel::SubId, const Feature*>> out;
    double width = 0;
    for (const Feature& f : c.part.features) {
        if (f.kind != Feature::Kind::Slot) {
            continue;
        }
        if (width == 0) {
            width = widthOf(f);
        }
        if (std::abs(widthOf(f) - width) > 1e-6) {
            continue;
        }
        if (const auto face = floorOf(c.part.shape, f)) {
            out.emplace_back(*face, &f);
        }
    }
    return out;
}

PropertyPlugin slotPlugin()
{
    PropertyPlugin p;
    p.type = "slot";
    p.tools = {lib::ToolType::FlatEndMill};
    p.needs = {Feature::Kind::Slot};
    p.geometry = [](PropertyCase& c) {
        std::vector<kernel::SubId> out;
        for (const auto& [face, f] : slotsOfOneWidth(c)) {
            out.push_back(face);
        }
        return out;
    };
    p.ranges = {{"stepDown", {0.5, 3}},
                {"floorStock", {0, 0.3}},
                {"rampAngle", {1, 8}},
                {"minRampRun", {0, 3}},
                {"widthTolerance", {0, 0.1}}};
    p.adjust = [](PropertyCase& c) {
        const auto slots = slotsOfOneWidth(c);
        if (slots.empty()) {
            return;
        }
        // Mostly the slot's own width; sometimes a little narrower (accepted within the width tolerance, or refused
        // beyond it) or wider (refused).
        const double w = widthOf(*slots.front().second);
        const double d = w + c.rng.pick(std::vector<double>{0, 0, 0, 0, -0.02, -0.3, 0.2});
        lib::Tool t = randomTool(c.rng, lib::ToolType::FlatEndMill, d);
        t.fluteLength = std::max(t.fluteLength, c.part.thickness + 5);
        t.shoulderLength = t.fluteLength;
        t.overallLength = t.fluteLength + 40;
        c.tool = t;
        c.op.tool.stickOut = std::round(t.fluteLength + c.rng.uniform(2, 15));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.maxRampAngleDeg = paramOf(c.op.paramsJson, "rampAngle", 3);
        // The slots picked (those of the first slot's width): the tool's centre stays inside them, their floors are
        // cleared down to the floor stock.
        std::vector<std::vector<Vec2>> loops;
        for (const auto& [face, f] : slotsOfOneWidth(c)) {
            loops.push_back(f->region.front());
            ClearClaim cl;
            cl.region = Region::of({f->region.front()});
            cl.floorZ = f->floorZ;
            cl.allowedAbove = 0.01 + paramOf(c.op.paramsJson, "floorStock", 0);
            k.clears.push_back(std::move(cl));
        }
        k.containment = ContainClaim{Region::of(loops), 0.01};
        return k;
    };
    p.minGenerated = 0.5;
    return p;
}

} // namespace

// As RAY_INVARIANT_PROPERTY_TESTS("slot", "[property_slot]", slotPlugin()) defines them: CTest runs the cases with the
// tag [property_slot] as a test of their own (ray_cam_invariants_slot, CMakeLists.txt).
TEST_CASE("invariant property: slot, quick seeds", "[cam][invariants][property][property_slot]")
{
    requireProperty(slotPlugin(), kQuickCases);
}
TEST_CASE("invariant property: slot, slow seeds", "[cam][invariants][property][property_slot][.slow]")
{
    requireProperty(slotPlugin(), kSlowCases);
}

TEST_CASE("invariant property: slot, a tool wider than the slot is always refused",
          "[cam][invariants][property][property_slot]")
{
    PropertyPlugin p = slotPlugin();
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        const auto slots = slotsOfOneWidth(c);
        if (!slots.empty()) {
            c.tool.diameter = widthOf(*slots.front().second) + 0.1;
            c.tool.shankDiameter = c.tool.diameter;
        }
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("slot-wide", 4));
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.generated == 0);
    CHECK(o.failures.empty());
    for (const std::string& r : o.refusals) {
        CHECK(r.find("is wider than the") != std::string::npos);
    }
}
