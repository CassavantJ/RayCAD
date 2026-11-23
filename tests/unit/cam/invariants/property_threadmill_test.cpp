// The thread milling operation (task T-056) in the seeded property harness (task T-058): internal threads in the
// random plates' holes (an ISO coarse thread whose tap drill the hole is), and external threads on a pin standing in
// a ring pocket of a plate built for the case; random form, hand, direction, radial passes, spring pass, overlap,
// thread length and tool.
//
// WHAT THE CHECKER JUDGES for a thread mill: the toolpath against the part left by drilling (InitialStock::Part, with
// cutting allowed only inside the thread's cylinder: the major diameter inside, the pin outside, over the thread's
// length): no gouge of the bore below the thread, of the counterbore, the floor or the plate; nothing below the drilled
// depth; rapids clear of part and stock; the holder and shank clear; the neck (taken as R - 0.6134 P for a tool with a
// pitch, its shank otherwise) not rubbing remaining stock; heights, feeds, the spindle, machine travel; every arc's
// start and end on one circle within 2 nm (helical arcs included); the claimed climb or conventional direction where
// the tool runs along the hole's or pin's wall; and that two generations give the same bytes.
// WHAT IT CANNOT JUDGE: the thread itself. Its stock grid (0.1 mm) and part grid cannot resolve a thread form, so the
// pitch, the hand, the phase of the helix from pass to pass and step to step, the thread depth reached and the
// thread's diameter are not seen; nor whether the tooth profile fits the thread. Those are proved by the unit tests and
// goldens (threadmill_test.cpp, threadmill_golden_test.cpp: exact pitch per revolution, exact radius) and, through
// the posts, by the round trip (tests/unit/post/roundtrip "thread milling keeps the pitch").
#include "property.h"

#include <ray/kernel.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

// `json` (a flat object of simple values, as the harness writes it) with `key` set to `value`.
std::string withParam(std::string json, const std::string& key, const std::string& value)
{
    const std::string k = "\"" + key + "\":";
    const std::size_t at = json.find(k);
    if (at == std::string::npos) {
        json.insert(json.size() - 1, std::string(json.size() > 2 ? "," : "") + k + value);
        return json;
    }
    const std::size_t from = at + k.size();
    const std::size_t to = json.find_first_of(",}", from);
    json.replace(from, to - from, value);
    return json;
}

bool has(const std::string& json, const std::string& key, const std::string& value)
{
    return json.find("\"" + key + "\":" + value) != std::string::npos;
}

std::string num(double v)
{
    std::string s = std::to_string(std::round(v * 1000) / 1000);
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

// The ISO 261 coarse thread whose tap drill (major - pitch, as the random plates round them) is d.
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

// A thread mill for a thread of `pitch`: single-form (pitch 0) or multi-form, with a neck up to
// `reach` (libraries.h has no neck: it is described as the shank).
lib::Tool threadMill(Rng& rng, double diameter, double pitch, bool multi, double reach)
{
    lib::Tool t = randomTool(rng, lib::ToolType::ThreadMill, diameter);
    t.threadPitch = multi ? pitch : 0;
    t.fluteLength = multi ? std::round(rng.uniform(3, 12)) : 1.5;
    // A neck that clears the thread's crests inside and out (1.4 pitches narrower than the teeth).
    t.shankDiameter = std::max(std::round((diameter - 1.4 * pitch) * 10) / 10, std::round(diameter * 3) / 10);
    t.shoulderLength = std::max(reach, t.fluteLength);
    t.overallLength = t.shoulderLength + 40;
    return t;
}

void commonAdjust(PropertyCase& c, double pitch, double toolDiameter, double reach)
{
    const bool multi = has(c.op.paramsJson, "form", "\"multi\"");
    c.tool = threadMill(c.rng, toolDiameter, pitch, multi, reach);
    c.op.tool.stickOut = std::round(c.tool.shoulderLength + c.rng.uniform(2, 15));
    c.op.cutting.spindleRpm = c.rng.range(1500, 6000);
    c.op.cutting.feedCut = std::round(c.rng.uniform(100, 600));
    // A single-form helix ends up to a pitch above the thread's top: a feed plane at least 2 mm up.
    c.op.heights.feedPlane = std::max(c.op.heights.feedPlane, 2.0);
    c.op.heights.retract = std::max(c.op.heights.retract, c.op.heights.feedPlane);
}

PropertyPlugin internalPlugin()
{
    PropertyPlugin p;
    p.type = "threadmill";
    p.tools = {lib::ToolType::ThreadMill};
    p.needs = {Feature::Kind::Holes};
    p.geometry = pick::holes;
    p.fixed = {{"location", "\"internal\""}, {"threadDepth", "0"}, {"leftHand", "false"}};
    p.ranges = {{"radialPasses", {1, 3}}, {"overlap", {0, 45}}};
    p.adjust = [](PropertyCase& c) {
        // One hole size per case, its coarse thread entered (the random plates' holes have no thread spec).
        const auto holes = holeFaces(c);
        if (holes.empty()) {
            return;
        }
        const Feature::Hole first = holes.front().second;
        std::vector<kernel::SubId> keep;
        for (const auto& [face, hole] : holes) {
            if (hole.diameter == first.diameter) {
                keep.push_back(face);
            }
        }
        c.geom.selected = keep;
        c.geom.selectedShapeIndex.assign(keep.size(), 0);
        const auto [major, pitch] = threadFor(first.diameter);
        std::string params = withParam(c.op.paramsJson, "pitch", num(pitch));
        params = withParam(params, "diameter", num(major));
        params = withParam(params, "leftHand", c.rng.coin() ? "true" : "false");
        // The thread's length: the whole bore, or a part of the shallowest selected hole's bore.
        double shallowest = c.part.thickness;
        for (const auto& [face, hole] : holes) {
            if (hole.diameter == first.diameter && !hole.through) {
                shallowest = std::min(shallowest, -hole.bottomZ);
            }
        }
        params = withParam(params, "threadLength",
                           c.rng.coin(0.4) ? "0" : num(std::round(c.rng.uniform(2, shallowest) * 10) / 10));
        c.op.paramsJson = params;
        const double toolDiameter = std::round(first.diameter * c.rng.uniform(0.55, 0.8) * 10) / 10;
        commonAdjust(c, pitch, toolDiameter, c.part.thickness + c.rng.uniform(2, 10));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.initialStock = InitialStock::Part;
        k.direction =
            has(c.op.paramsJson, "direction", "\"conventional\"") ? Direction::Conventional : Direction::Climb;
        for (const auto& [face, hole] : holeFaces(c)) {
            if (std::find(c.geom.selected.begin(), c.geom.selected.end(), face) == c.geom.selected.end()) {
                continue;
            }
            const double major = threadFor(hole.diameter).first;
            // The thread from the hole's top down; nothing may go below the drilled bore (through: the plate).
            k.allowedCuts.push_back(AllowedCut{hole.x, hole.y, major / 2 + 0.01,
                                               hole.through ? -c.part.thickness - 0.01 : hole.bottomZ, 50});
        }
        return k;
    };
    p.minGenerated = 0.6;
    return p;
}

// A plate with a pin in a ring pocket, replacing the case's random part: the pin of diameter d at the plate's centre,
// standing `height` above the pocket's floor, its top the plate's top (Z 0).
void pinPart(PropertyCase& c, double d, double ringRadius, double height)
{
    const double side = std::round((2 * ringRadius + 12) * 10) / 10;
    const double t = std::round(c.rng.uniform(height + 4, height + 12));
    const double cx = side / 2, cy = side / 2;
    auto plate = kernel::makeBox(Vec3{0, 0, -t}, Vec3{side, side, 0});
    auto ring = kernel::makeCylinder(Vec3{cx, cy, -height}, Vec3{0, 0, 1}, ringRadius, height + 1);
    auto pin = kernel::makeCylinder(Vec3{cx, cy, -height - 1}, Vec3{0, 0, 1}, d / 2, height + 3);
    REQUIRE(plate);
    REQUIRE(ring);
    REQUIRE(pin);
    JobContext ctx;
    auto annulus = kernel::boolean(kernel::BoolOp::Cut, *ring, *pin, ctx);
    REQUIRE(annulus);
    auto shape = kernel::boolean(kernel::BoolOp::Cut, *plate, annulus->shape, ctx);
    REQUIRE(shape);
    c.part.shape = shape->shape;
    c.part.length = side;
    c.part.width = side;
    c.part.thickness = t;
    Feature boss;
    boss.kind = Feature::Kind::Boss;
    boss.floorZ = -height;
    boss.cornerRadius = d / 2; // here: the pin's radius, for the claims
    c.part.features = {boss};
    c.geom.partShapes = {c.part.shape};
    c.geom.stockBounds = Box3{Vec3{0, 0, -t}, Vec3{side, side, 0}};
    c.geom.selected.clear();
    for (std::uint32_t i = 0; i < c.part.shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(c.part.shape, f);
        if (info && info->type == kernel::SurfaceType::Cylinder && std::abs(info->radius - d / 2) < 1e-6) {
            c.geom.selected.push_back(f);
        }
    }
    c.geom.selectedShapeIndex.assign(c.geom.selected.size(), 0);
}

PropertyPlugin externalPlugin()
{
    PropertyPlugin p = internalPlugin();
    p.fixed["location"] = "\"external\"";
    p.needs = {};
    p.geometry = pick::none;
    p.adjust = [](PropertyCase& c) {
        static const std::vector<std::pair<double, double>> coarse = {
            {6, 1}, {8, 1.25}, {10, 1.5}, {12, 1.75}, {16, 2}};
        const auto [d, pitch] = c.rng.pick(coarse);
        const double toolDiameter = std::round(c.rng.uniform(4, 8) * 10) / 10;
        const double height = std::round(c.rng.uniform(4, 10));
        pinPart(c, d, d / 2 + 2 * toolDiameter + 1.5, height);
        std::string params = withParam(c.op.paramsJson, "pitch", num(pitch));
        params = withParam(params, "diameter", c.rng.coin() ? "0" : num(d));
        params = withParam(params, "leftHand", c.rng.coin() ? "true" : "false");
        params = withParam(params, "threadLength",
                           c.rng.coin(0.4) ? "0" : num(std::round(c.rng.uniform(2, height) * 10) / 10));
        c.op.paramsJson = params;
        commonAdjust(c, pitch, toolDiameter, height + c.rng.uniform(2, 10));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.initialStock = InitialStock::Part;
        k.direction =
            has(c.op.paramsJson, "direction", "\"conventional\"") ? Direction::Conventional : Direction::Climb;
        REQUIRE(c.part.features.size() == 1);
        const Feature& boss = c.part.features.front();
        k.allowedCuts.push_back(
            AllowedCut{c.part.length / 2, c.part.width / 2, boss.cornerRadius + 0.5, boss.floorZ - 0.01, 50});
        return k;
    };
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("threadmill", internalPlugin())
RAY_INVARIANT_PROPERTY_TESTS("threadmill outside", externalPlugin())

TEST_CASE("invariant property: threadmill, the checker sees what thread milling cuts", "[cam][invariants][property]")
{
    // Without the claim that cutting the thread is intended, every case is a gouge of the bore: the thread cases are
    // checked against the part, not passed vacuously.
    PropertyPlugin p = internalPlugin();
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.initialStock = InitialStock::Part;
        return k;
    };
    const PropertyOutcome o = runProperty(p, seedsFor("threadmill", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[gouge]") != std::string::npos);
    }
}

TEST_CASE("invariant property: threadmill, the other direction is caught", "[cam][invariants][property]")
{
    // Claiming the opposite of what the operation was asked for is reported along the wall: the direction is judged.
    PropertyPlugin p = internalPlugin();
    const auto claims = p.claims;
    p.claims = [claims](const PropertyCase& c) {
        Claims k = claims(c);
        k.direction = k.direction == Direction::Climb ? Direction::Conventional : Direction::Climb;
        return k;
    };
    const PropertyOutcome o = runProperty(p, seedsFor("threadmill", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[direction]") != std::string::npos);
    }
}
