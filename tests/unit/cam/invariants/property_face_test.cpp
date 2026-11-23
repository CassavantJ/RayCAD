// The facing operation (task T-052) in the seeded property harness (task T-058): random plates, stock always above the
// part (facing has nothing to do otherwise), a random flat, bull nose or face mill, random parameters from the schema,
// the checker with the claims facing makes: the whole stock box cleared down to the faced height (coverage), nothing
// below it.
#include "property.h"

#include <algorithm>
#include <cmath>

using namespace ray;
using namespace ray::cam;
using namespace ray::cam::invariants;

namespace {

PropertyPlugin facePlugin()
{
    PropertyPlugin p;
    p.type = "face";
    p.tools = {lib::ToolType::FlatEndMill, lib::ToolType::BullEndMill, lib::ToolType::FaceMill};
    p.geometry = pick::none;
    // Every move is checked against part and stock samples, so the cost grows with strokes x passes: stepovers from
    // 1 mm and stepdowns from 0.5 mm keep a case near a second in Release (the rules are the same for finer ones).
    p.ranges = {{"stepover", {1, 2.5}}, {"overhang", {0, 4}}, {"stepdown", {0.5, 1.5}}};
    p.adjust = [](PropertyCase& c) {
        // The plate's top is Z 0. Stock 0.3 to 2 mm above it; faced to the part top or a little above it.
        if (c.geom.stockBounds.max.z < 0.3) {
            c.geom.stockBounds.max.z = std::round(c.rng.uniform(0.3, 2) * 10) / 10;
        }
        c.op.heights.top = 0;
        c.op.heights.bottom = c.rng.coin(0.7) ? 0 : std::round(c.rng.uniform(0, c.geom.stockBounds.max.z) * 5) / 10;
    };
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        const double bottom = c.op.heights.bottom; // above the part top, Z 0
        const double top = c.geom.stockBounds.max.z;
        k.floorZ = bottom;
        k.clears = {ClearClaim{Region::box(), bottom, 0.002, "coverage"}};
        k.heights.clearance = top + c.op.heights.clearance; // face.h LEVELS: from the stock top
        k.heights.retract = top + c.op.heights.retract;
        k.heights.feedPlane = top + c.op.heights.feedPlane;
        k.heights.top = top + c.op.heights.top;
        k.heights.bottom = bottom;
        return k;
    };
    p.minGenerated = 0.6;
    return p;
}

} // namespace

RAY_INVARIANT_PROPERTY_TESTS("face", "[property_face]", facePlugin())

TEST_CASE("invariant property: face, the checker sees what facing leaves", "[cam][invariants][property][property_face]")
{
    // Claimed down to 0.2 mm below what the operation faces: every case must fail its coverage claim. This shows the
    // face cases are checked against the stock, not passed vacuously.
    PropertyPlugin p = facePlugin();
    p.claims = [](const PropertyCase& c) {
        Claims k = defaultClaims(c);
        k.clears = {ClearClaim{Region::box(), c.op.heights.bottom - 0.2, 0.002, "coverage"}};
        return k;
    };
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        c.op.heights.bottom = 0.3; // so 0.2 below it is still above the part
        c.geom.stockBounds.max.z = std::max(c.geom.stockBounds.max.z, 1.0);
    };
    const PropertyOutcome o = runProperty(p, seedsFor("face-short", 3));
    REQUIRE(o.generated > 0);
    CHECK(static_cast<int>(o.failures.size()) == o.generated);
    for (const std::string& f : o.failures) {
        INFO(f);
        CHECK(f.find("[coverage]") != std::string::npos);
    }
}

TEST_CASE("invariant property: face, a short stick-out is refused or kept clear",
          "[cam][invariants][property][property_face]")
{
    // A stick-out of 1 to 3 mm and passes up to 1.5 mm deep: the operation must refuse (its holder clearance, 1 mm) or
    // keep the holder 0.5 mm clear of part and stock.
    PropertyPlugin p = facePlugin();
    const auto adjust = p.adjust;
    p.adjust = [adjust](PropertyCase& c) {
        adjust(c);
        c.withHolder = true;
        c.op.tool.stickOut = std::round(c.rng.uniform(1, 3) * 2) / 2;
    };
    p.minGenerated = 0;
    const PropertyOutcome o = runProperty(p, seedsFor("face-holder", 4));
    for (const std::string& f : o.failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(o.failures.empty());
}
