// The property harness itself (task T-058): a reference facing type (reference_paths.h, wrapped as an operation type)
// passes on the random parts with a coverage claim; a facing type that cuts too deep, and one whose output changes
// from call to call, are reported with their seed and case; the random parts are what their seeds say.
#include "mini_json.h"
#include "property.h"
#include "reference_paths.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

// Facing of the stock top to the part's top (Z 0) with reference_paths.h's zig-zag; "stepover" is a fraction of the
// tool diameter. `sink` faces that much too deep; `drift` makes every call's output differ.
class RefFace final : public IOperationType {
public:
    RefFace(double sink = 0, bool drift = false) : sink_(sink), drift_(drift) {}
    std::string_view typeName() const override { return "ref_face"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","properties":{"stepover":{"type":"number","minimum":0.3,"maximum":0.9,"default":0.6}}})";
    }
    Status validate(const GenerateInput&) const override { return {}; }
    Result<Toolpath> generate(const GenerateInput& in, JobContext&) const override
    {
        const auto params = json::parse(in.op.paramsJson);
        const json::Value* s = params ? params->find("stepover") : nullptr;
        FaceRef r;
        r.stock = in.geom.stockBounds;
        r.toolRadius = in.tool.diameter / 2;
        r.stepover = (s != nullptr ? s->number : 0.6) * in.tool.diameter;
        r.z = -sink_;
        r.clearance = in.geom.stockBounds.max.z + in.op.heights.clearance;
        r.feed = in.op.cutting.feedCut;
        if (drift_) {
            r.z -= 1e-6 * static_cast<double>(calls_.fetch_add(1));
        }
        return zigzagFace(r);
    }

private:
    double sink_;
    bool drift_;
    mutable std::atomic<int> calls_{0};
};

PropertyPlugin facePlugin(double sink = 0, bool drift = false)
{
    PropertyPlugin p;
    p.type = "ref_face";
    p.make = [sink, drift] {
        return std::make_unique<RefFace>(sink, drift);
    };
    p.tools = {lib::ToolType::FlatEndMill, lib::ToolType::FaceMill};
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.clears = {ClearClaim{Region::box(), 0, 0.002, "coverage"}};
        return k;
    };
    p.minGenerated = 1;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("ref_face", "[property_harness]", facePlugin())

TEST_CASE("invariant property: a broken type is reported with its seed and case",
          "[cam][invariants][property][property_harness]")
{
    const std::vector<std::uint64_t> seeds = seedsFor("ref_face", 3);
    const PropertyOutcome deep = runProperty(facePlugin(0.05), seeds);
    CHECK(deep.generated == 3);
    REQUIRE(deep.failures.size() == 3);
    for (const std::string& f : deep.failures) {
        INFO(f);
        CHECK(f.find("seed ") != std::string::npos);
        CHECK(f.find("plate ") != std::string::npos);
        CHECK(f.find("[gouge]") != std::string::npos);
    }
    const PropertyOutcome drift = runProperty(facePlugin(0, true), seeds);
    REQUIRE(drift.failures.size() == 3);
    for (const std::string& f : drift.failures) {
        INFO(f);
        CHECK(f.find("[determinism]") != std::string::npos);
    }
}

TEST_CASE("invariant property: random parts are what their seeds say", "[cam][invariants][property][property_harness]")
{
    for (const std::uint64_t seed : seedsFor("parts", 4)) {
        Rng a(seed), b(seed);
        auto pa = randomPart(a);
        auto pb = randomPart(b);
        REQUIRE(pa);
        REQUIRE(pb);
        CHECK(pa->describe() == pb->describe());
        CHECK(pa->shape.contentHash() == pb->shape.contentHash());
        INFO(pa->describe());
        CHECK(!pa->features.empty());
    }
    // The quick seeds are the first of the slow ones.
    const auto quick = seedsFor("drill", kQuickCases), slow = seedsFor("drill", kSlowCases);
    CHECK(std::equal(quick.begin(), quick.end(), slow.begin()));
}
