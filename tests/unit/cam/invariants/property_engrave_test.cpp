// The engraving operation (task T-056) in the seeded property harness (task T-058): the rims of the random plates'
// holes (edges of the top face) and a random line of text at a random place and angle on the top face, with random
// depth, step-down, height, spacing and alignment, and an engraving cutter, spot drill, chamfer mill or ball end mill.
//
// The claims: cutting the part is intended only within the groove's width of the rims and within a circle round the
// text (computed here from the number of characters, not from the font code), no deeper than the depth; nothing
// moves below the depth. So the checker judges that the engraving goes where it should and no deeper, plunges only
// with tools that cut at their centre, rapids clear, the holder and shank, arcs (the font's and fitted ones) on one
// circle within 2 nm, feeds, heights and determinism. It cannot judge the letters' shapes: those are the font tests'.
#include "property.h"

#include <ray/kernel.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

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

double param(const std::string& json, const std::string& key, double fallback)
{
    const std::string k = "\"" + key + "\":";
    const std::size_t at = json.find(k);
    return at == std::string::npos ? fallback : std::strtod(json.c_str() + at + k.size(), nullptr);
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

const std::vector<std::string>& texts()
{
    static const std::vector<std::string> t = {"RAY-01", "Ab9",        "#4 (x)",  "M6x1",
                                               "Op 20",  "1/4-20 UNC", "Lot 7.3", "q:p"};
    return t;
}

// The rim edges (circles at Z 0) of the holes of the case.
std::vector<kernel::SubId> rims(PropertyCase& c)
{
    std::vector<kernel::SubId> out;
    const kernel::Shape& s = c.part.shape;
    for (const kernel::SubId face : pick::topFaces(c)) {
        for (const kernel::SubId e : kernel::adjacent(s, face, kernel::SubKind::Edge)) {
            auto ci = kernel::curveInfo(s, e);
            if (!ci || ci->type != kernel::CurveType::Circle) {
                continue;
            }
            for (const Feature& f : c.part.features) {
                for (const Feature::Hole& h : f.holes) {
                    if (std::abs(ci->center.x - h.x) < 1e-6 && std::abs(ci->center.y - h.y) < 1e-6 &&
                        std::abs(ci->radius - h.diameter / 2) < 1e-6 &&
                        std::find(out.begin(), out.end(), e) == out.end()) {
                        out.push_back(e);
                    }
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The groove's width at depth d of the case's tool, from libraries.h's fields (not from the generator's code).
double groove(const lib::Tool& t, double d)
{
    if (t.tipAngleDeg > 0 && t.tipAngleDeg < 180 && t.type != lib::ToolType::BallEndMill) {
        return std::min(t.diameter, t.tipDiameter + 2 * d * std::tan(t.tipAngleDeg * 3.14159265358979323846 / 360));
    }
    return t.diameter;
}

PropertyPlugin engravePlugin()
{
    PropertyPlugin p;
    p.type = "engrave";
    p.tools = {lib::ToolType::EngraveCutter, lib::ToolType::EngraveCutter, lib::ToolType::SpotDrill,
               lib::ToolType::ChamferMill, lib::ToolType::BallEndMill};
    p.needs = {Feature::Kind::Holes};
    p.geometry = rims;
    p.ranges = {{"depth", {0.05, 0.6}}, {"stepDown", {0, 0.4}}, {"textHeight", {2, 8}},    {"textSpacing", {0, 1}},
                {"textX", {0, 0}},      {"textY", {0, 0}},      {"textAngle", {-180, 180}}};
    p.adjust = [](PropertyCase& c) {
        // A small tool (a large one rarely fits the depth within its point), the heights over the plane at Z 0.
        const double d = c.rng.pick(std::vector<double>{3, 3, 6});
        c.tool = randomTool(c.rng, c.tool.type, d);
        c.op.tool.stickOut = std::round(c.tool.fluteLength + c.rng.uniform(5, 25));
        c.op.heights.top = 0;
        c.op.heights.feedPlane = std::max(c.op.heights.feedPlane, 1.0);
        c.op.heights.retract = std::max(c.op.heights.retract, c.op.heights.feedPlane);
        std::string params = c.op.paramsJson;
        if (c.rng.coin(0.75)) {
            params = withParam(params, "text", "\"" + c.rng.pick(texts()) + "\"");
            params = withParam(params, "textX", num(std::round(c.rng.uniform(5, c.part.length - 5))));
            params = withParam(params, "textY", num(std::round(c.rng.uniform(5, c.part.width - 5))));
        }
        if (c.rng.coin(0.2)) {
            c.geom.selected.clear(); // text alone (or nothing: a refusal)
            c.geom.selectedShapeIndex.clear();
        }
        c.op.paramsJson = params;
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        const double depth = param(c.op.paramsJson, "depth", 0.2);
        const double w = groove(c.tool, depth);
        k.floorZ = -depth;
        const auto rimsOf = [&]() {
            std::vector<Feature::Hole> out;
            for (const Feature& f : c.part.features) {
                for (const Feature::Hole& h : f.holes) {
                    out.push_back(h);
                }
            }
            return out;
        };
        if (!c.geom.selected.empty()) {
            for (const Feature::Hole& h : rimsOf()) {
                k.allowedCuts.push_back(AllowedCut{h.x, h.y, h.diameter / 2 + w / 2 + 0.05, -depth - 0.01, 50});
            }
        }
        const std::size_t at = c.op.paramsJson.find("\"text\":\"");
        if (at != std::string::npos) {
            const std::size_t end = c.op.paramsJson.find('"', at + 8);
            const double n = static_cast<double>(end - at - 8);
            const double h = param(c.op.paramsJson, "textHeight", 5);
            const double spacing = param(c.op.paramsJson, "textSpacing", 0);
            // Every glyph point lies within (n x 14/12 x h + n x spacing) + h of the anchor, whatever the alignment and
            // angle (no advance is above 13 units, no point above 12 or below -4 units).
            const double reach = n * (14.0 / 12.0 * h + spacing) + h + w / 2 + 0.05;
            k.allowedCuts.push_back(AllowedCut{param(c.op.paramsJson, "textX", 0), param(c.op.paramsJson, "textY", 0),
                                               reach, -depth - 0.01, 50});
        }
        return k;
    };
    p.minGenerated = 0.5;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("engrave", "[property_engrave]", engravePlugin())

TEST_CASE("invariant property: engrave, the checker sees what engraving cuts",
          "[cam][invariants][property][property_engrave]")
{
    // Without the allowed cuts every engraving is a gouge of the plate's top: the cases are checked against the part.
    PropertyPlugin p = engravePlugin();
    const auto claims = p.claims;
    p.claims = [claims](const PropertyCase& c) {
        Claims k = claims(c);
        k.allowedCuts.clear();
        return k;
    };
    const PropertyOutcome o = runProperty(p, seedsFor("engrave", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[gouge]") != std::string::npos);
    }
}

TEST_CASE("invariant property: engrave, a flat end mill is refused", "[cam][invariants][property][property_engrave]")
{
    PropertyPlugin p = engravePlugin();
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        c.tool = randomTool(c.rng, lib::ToolType::FlatEndMill, 3);
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("engrave-flat", 3));
    CHECK(o.generated == 0);
    CHECK(o.failures.empty());
}
