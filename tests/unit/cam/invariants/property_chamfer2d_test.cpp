// The chamfer operation (task T-055) in the seeded property harness (task T-058): random plates, one or two edge loops
// of their top faces picked (the plate's outline with its steps, a pocket's, a slot's, a hole's, a boss's), a chamfer
// mill or a spot drill, and the checker with the claims chamfering makes: cutting the part is intended only inside the
// wedge each picked edge's chamfer removes (invariants::chamferCut), and the cut direction.
//
// What the checker can judge for an angled tool, and what not: it meshes the part and measures the tool as its real
// cone (tool_profile.h), so it sees the tip or the flank going into a wall or floor below or beside the edge, the
// shank or holder touching anything, and any cut outside the chamfer's wedge deeper than its tolerance (0.01 mm, plus
// the 0.02 mm the wedge is grown by). It does not see whether the chamfer is complete (no claim is made on what is left
// on the chamfer's face: the stock grid's 0.1 mm samples cannot resolve a 0.5 mm wedge well), nor its angle beyond "no
// deeper than the wedge", nor a contact shallower than the bounds in README.md.
#include "mini_json.h"
#include "property.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string textOf(const std::string& params, const std::string& key)
{
    const auto v = json::parse(params);
    const json::Value* s = v ? v->find(key) : nullptr;
    return s != nullptr && s->type == json::Value::Type::String ? s->string : std::string();
}
double numberOf(const std::string& params, const std::string& key, double fallback)
{
    const auto v = json::parse(params);
    const json::Value* n = v ? v->find(key) : nullptr;
    return n != nullptr && n->type == json::Value::Type::Number ? n->number : fallback;
}

// The edge loops of the part's top faces (Z 0): each top face's edges, grouped by shared vertices.
std::vector<std::vector<kernel::SubId>> topLoops(const kernel::Shape& s)
{
    std::vector<std::vector<kernel::SubId>> loops;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId face{kernel::SubKind::Face, i};
        const auto info = kernel::surfaceInfo(s, face);
        const auto sample = kernel::faceSample(s, face);
        if (!info || !sample || info->type != kernel::SurfaceType::Plane || sample->normal.z < 0.999 ||
            std::abs(info->bounds.max.z) > 1e-6 || std::abs(info->bounds.min.z) > 1e-6) {
            continue;
        }
        const std::vector<kernel::SubId> edges = kernel::adjacent(s, face, kernel::SubKind::Edge);
        std::vector<std::size_t> parent(edges.size());
        std::iota(parent.begin(), parent.end(), 0);
        const auto root = [&](std::size_t k) {
            while (parent[k] != k) {
                k = parent[k] = parent[parent[k]];
            }
            return k;
        };
        std::map<kernel::SubId, std::size_t> byVertex;
        for (std::size_t k = 0; k < edges.size(); ++k) {
            for (const kernel::SubId v : kernel::adjacent(s, edges[k], kernel::SubKind::Vertex)) {
                const auto [it, fresh] = byVertex.emplace(v, k);
                if (!fresh) {
                    parent[root(k)] = root(it->second);
                }
            }
        }
        std::map<std::size_t, std::vector<kernel::SubId>> groups;
        for (std::size_t k = 0; k < edges.size(); ++k) {
            groups[root(k)].push_back(edges[k]);
        }
        for (auto& [r, g] : groups) {
            loops.push_back(std::move(g));
        }
    }
    return loops;
}

PropertyPlugin chamferPlugin()
{
    PropertyPlugin p;
    p.type = "chamfer2d";
    p.tools = {lib::ToolType::ChamferMill, lib::ToolType::SpotDrill};
    p.needs = {};
    p.geometry = [](PropertyCase& c) {
        const auto loops = topLoops(c.part.shape);
        std::vector<kernel::SubId> out;
        if (loops.empty()) {
            return out;
        }
        const int n = c.rng.range(1, 2);
        for (int k = 0; k < n; ++k) {
            const auto& loop = loops[static_cast<std::size_t>(c.rng.range(0, static_cast<int>(loops.size()) - 1))];
            for (const kernel::SubId e : loop) {
                if (std::find(out.begin(), out.end(), e) == out.end()) {
                    out.push_back(e);
                }
            }
        }
        return out;
    };
    p.ranges = {{"width", {0.1, 1}}, {"tipOffset", {0, 0.5}}, {"extension", {0, 2}}};
    p.adjust = [](PropertyCase& c) {
        c.tool.fluteLength = std::max(c.tool.fluteLength, 4.0);
        c.tool.shoulderLength = std::max(c.tool.shoulderLength, c.tool.fluteLength);
        c.tool.overallLength = std::max(c.tool.overallLength, c.tool.shoulderLength + 30);
        c.op.tool.stickOut = std::round(c.tool.fluteLength + c.rng.uniform(5, 25));
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        // Earlier operations left the part; chamfering it is intended inside each picked edge's wedge.
        k.initialStock = InitialStock::Part;
        const double t = std::tan(c.tool.tipAngleDeg * kPi / 360);
        const double depth = numberOf(c.op.paramsJson, "width", 0.5) / t;
        for (const kernel::SubId e : c.geom.selected) {
            if (auto cut = chamferCut(c.part.shape, e, depth, t, 0.02)) {
                k.allowedCuts.push_back(std::move(*cut));
            }
        }
        k.direction =
            textOf(c.op.paramsJson, "direction") == "conventional" ? Direction::Conventional : Direction::Climb;
        return k;
    };
    p.minGenerated = 0.5;
    return p;
}

} // namespace

// As RAY_INVARIANT_PROPERTY_TESTS("chamfer2d", "[property_chamfer2d]", chamferPlugin()) defines them: CTest runs the
// cases with the tag [property_chamfer2d] as a test of their own (ray_cam_invariants_chamfer2d, CMakeLists.txt).
TEST_CASE("invariant property: chamfer2d, quick seeds", "[cam][invariants][property][property_chamfer2d]")
{
    requireProperty(chamferPlugin(), kQuickCases);
}
TEST_CASE("invariant property: chamfer2d, slow seeds", "[cam][invariants][property][property_chamfer2d][.slow]")
{
    requireProperty(chamferPlugin(), kSlowCases);
}

TEST_CASE("invariant property: chamfer2d, the checker sees what chamfering cuts",
          "[cam][invariants][property][property_chamfer2d]")
{
    // Without the claim that each chamfer's wedge is meant to be cut, every chamfer is a gouge: the cases are checked
    // against the part, not passed vacuously.
    PropertyPlugin p = chamferPlugin();
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.initialStock = InitialStock::Part;
        return k;
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("chamfer2d", 4));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[gouge]") != std::string::npos);
    }
}
