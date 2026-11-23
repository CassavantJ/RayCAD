// The seeded property harness of the invariant checker (task T-058; PLAN.md section 11, "Property tests" and
// "Toolpath invariants"). Any operation type plugs in with a few lines (README.md in this folder says how): for each
// seed the harness builds a random part from a small family with the kernel (a plate with pockets, islands, bosses,
// slots, holes and open edges), a random tool of the plug-in's types, random parameters drawn from the type's own
// schema, generates the toolpath twice (the bytes must match), and runs the checker (checker.h) with the claims the
// plug-in makes. A refusal (generate returning an error) is a valid outcome; a toolpath with any finding is a failure,
// reported with the seed and a description of the case.
#pragma once

#include "checker.h"

#include <ray/cam.h>
#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ray::cam::invariants {

// splitmix64: the same numbers on every platform (the standard distributions are not).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next();
    double uniform(double lo, double hi); // [lo, hi)
    int range(int lo, int hi);            // [lo, hi]
    bool coin(double p = 0.5);
    template <class T>
    const T& pick(const std::vector<T>& v)
    {
        return v[static_cast<std::size_t>(range(0, static_cast<int>(v.size()) - 1))];
    }

private:
    std::uint64_t s_;
};

struct Feature {
    enum class Kind { Pocket, Slot, Holes, Step, Boss } kind = Kind::Pocket;
    std::vector<std::vector<Vec2>> region; // where its floor is, WCS XY (even-odd: an island or a boss is a hole)
    double floorZ = 0;
    double cornerRadius = 0;
    struct Hole {
        double x = 0, y = 0, diameter = 0, bottomZ = 0; // bottomZ: where the full diameter ends (the cone below)
        bool through = false;
    };
    std::vector<Hole> holes;
    std::string describe() const;
};

// A plate [0, length] x [0, width] x [-thickness, 0] (the WCS is the model frame, Z 0 its top) with up to six
// features, each in its own cell of a 2 x 2 or 3 x 2 grid; at least one of each kind in `needs`.
struct RandomPart {
    kernel::Shape shape;
    double length = 0, width = 0, thickness = 0;
    std::vector<Feature> features;
    std::string describe() const;
};
Result<RandomPart> randomPart(Rng& rng, const std::vector<Feature::Kind>& needs = {});

struct PropertyCase {
    std::uint64_t seed = 0;
    RandomPart part;
    ResolvedGeometry geom;
    Operation op;
    lib::Tool tool;
    lib::Holder holder;
    bool withHolder = false;
    lib::MachineDef machine;
    Rng rng{0};

    GenerateInput input() const { return GenerateInput{op, tool, withHolder ? &holder : nullptr, machine, geom}; }
    std::string describe() const;
};

// A random tool of `type` with a diameter near `diameter` (0: random), flute length, shank and the rest to match.
lib::Tool randomTool(Rng& rng, lib::ToolType type, double diameter = 0);
// The plug-in's claims start from these: claimsFor(op, geom), the holder kept 0.5 mm clear.
Claims defaultClaims(const PropertyCase& c);

// A bore face of every hole of the case's part, with the hole.
std::vector<std::pair<kernel::SubId, Feature::Hole>> holeFaces(const PropertyCase& c);

// Geometry pickers: the faces an operation of the case is given (ResolvedGeometry::selected).
namespace pick {
std::vector<kernel::SubId> none(PropertyCase&);
std::vector<kernel::SubId> holes(PropertyCase&);        // a bore face of every hole
std::vector<kernel::SubId> pocketFloors(PropertyCase&); // the floor of every pocket, slot, step and boss
std::vector<kernel::SubId> topFaces(PropertyCase&);     // the up-facing faces at Z 0
std::vector<kernel::SubId> outerWalls(PropertyCase&);   // the plate's four sides
} // namespace pick

// Added by T-055 (chamfers): the cut a chamfer of a level edge removes, as an AllowedCut (part_model.h): a cone along
// the edge's polyline (lines exact, arcs and circles sampled within 1 um) from `depth` below the edge, opening at
// `slope` (tan of the chamfer mill's half angle), grown by `grow` in XY. The shape's frame is taken as the WCS (the
// harness's identity frame). nullopt for an edge that is not a level line, arc or circle.
std::optional<AllowedCut> chamferCut(const kernel::Shape& shape, kernel::SubId edge, double depth, double slope,
                                     double grow);

// Region claims of the case's features (coverage of each floor of the given kinds, to the scallop limit).
std::vector<ClearClaim> coverFeatures(const PropertyCase& c, std::vector<Feature::Kind> kinds, double scallop,
                                      std::string id = "coverage");

struct PropertyPlugin {
    std::string type; // the registered type (registerBuiltinOperationTypes), unless `make` is given
    std::vector<lib::ToolType> tools{lib::ToolType::FlatEndMill};
    std::vector<Feature::Kind> needs; // every random part has at least one feature of each of these kinds
    std::function<std::vector<kernel::SubId>(PropertyCase&)> geometry = pick::none;
    // Numeric parameter ranges (else from the schema: minimum to max(2 x default, minimum + 1), within maximum) and
    // fixed parameter values (JSON text).
    std::map<std::string, std::pair<double, double>> ranges;
    std::map<std::string, std::string> fixed;
    // After the defaults are drawn: adjust the tool, heights, cutting data or parameters (op.paramsJson).
    std::function<void(PropertyCase&)> adjust;
    // What the operation claims about its toolpath. Default: defaultClaims.
    std::function<Claims(const PropertyCase&)> claims;
    // A type that is not registered (the test suite's own reference types).
    std::function<std::unique_ptr<IOperationType>()> make;
    double minGenerated = 0.5; // at least this fraction of the cases must produce a toolpath
    CheckOptions options;
};

struct PropertyOutcome {
    int cases = 0, generated = 0, refused = 0;
    std::vector<std::string> refusals; // "seed: message"
    std::vector<std::string> failures; // seed, case and findings
    double seconds = 0;
    double partSeconds = 0, generateSeconds = 0, checkSeconds = 0; // building parts, generating (twice), checking
};
PropertyOutcome runProperty(const PropertyPlugin& plugin, const std::vector<std::uint64_t>& seeds);

// Fixed seeds: the quick tier runs kQuickCases of them, the slow tier kSlowCases (a superset).
inline constexpr int kQuickCases = 12;
inline constexpr int kSlowCases = 200;
std::vector<std::uint64_t> seedsFor(const std::string& type, int count);

// Runs the plug-in on the seeds and fails the current test case on any failure, with the seed and the case.
inline void requireProperty(const PropertyPlugin& plugin, int count)
{
    const PropertyOutcome o = runProperty(plugin, seedsFor(plugin.type, count));
    UNSCOPED_INFO(plugin.type << ": " << o.cases << " cases, " << o.generated << " toolpaths, " << o.refused
                              << " refused, " << o.seconds << " s (parts " << o.partSeconds << " s, generating "
                              << o.generateSeconds << " s, checking " << o.checkSeconds << " s)");
    for (const std::string& r : o.refusals) {
        UNSCOPED_INFO("refused " << r);
    }
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.failures.empty());
    CHECK(o.generated >= static_cast<int>(plugin.minGenerated * o.cases));
}

} // namespace ray::cam::invariants

// Defines the quick and the slow property test of an operation type: RAY_INVARIANT_PROPERTY_TESTS("pocket2d",
// plugin) where `plugin` is an expression of type PropertyPlugin. The slow one is hidden ([.slow]) from a plain run.
#define RAY_INVARIANT_PROPERTY_TESTS(TYPE, PLUGIN)                                                                     \
    TEST_CASE("invariant property: " TYPE ", quick seeds", "[cam][invariants][property]")                              \
    {                                                                                                                  \
        ::ray::cam::invariants::requireProperty(PLUGIN, ::ray::cam::invariants::kQuickCases);                          \
    }                                                                                                                  \
    TEST_CASE("invariant property: " TYPE ", slow seeds", "[cam][invariants][property][.slow]")                        \
    {                                                                                                                  \
        ::ray::cam::invariants::requireProperty(PLUGIN, ::ray::cam::invariants::kSlowCases);                           \
    }
