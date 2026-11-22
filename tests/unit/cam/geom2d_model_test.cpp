// Boundaries from the model (task T-050): slices as regions, the pocket above a floor, a face's boundary and chains of
// picked edges with tangent extension, on parts built with the kernel.
#include "geom2d_parts.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ray::cam::geom2d::test {
namespace {

const Xform kIdentity = Xform::identity();

std::optional<kernel::SubId> faceAt(const kernel::Shape& s, double z, bool up)
{
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        const kernel::SurfaceInfo info = take(kernel::surfaceInfo(s, f));
        if (info.type != kernel::SurfaceType::Plane || std::abs(info.centroid.z - z) > 1e-9)
            continue;
        const kernel::FaceSample sample = take(kernel::faceSample(s, f));
        if ((sample.normal.z > 0.5) == up)
            return f;
    }
    return std::nullopt;
}

// The pocket outline 10..90 x 10..70, corner radius 6, counter-clockwise.
Path pocketOutline()
{
    return roundedRect(10 * kMm, 10 * kMm, 90 * kMm, 70 * kMm, 6 * kMm);
}
Path islandOutline()
{
    return roundedRect(40 * kMm, 30 * kMm, 60 * kMm, 50 * kMm, 3 * kMm);
}

// The edges of the pocket's opening at the top (z = 30), excluding the island's.
std::vector<kernel::SubId> openingEdges(const kernel::Shape& s)
{
    std::vector<kernel::SubId> out;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Edge); ++i) {
        const kernel::SubId e{kernel::SubKind::Edge, i};
        const kernel::CurveInfo c = take(kernel::curveInfo(s, e));
        if (std::abs(c.start.z - 30) > 1e-9 || std::abs(c.end.z - 30) > 1e-9)
            continue;
        const auto onPocket = [](const Vec3& p) {
            const bool inBox = p.x >= 10 - 1e-9 && p.x <= 90 + 1e-9 && p.y >= 10 - 1e-9 && p.y <= 70 + 1e-9;
            const bool onIsland = p.x >= 40 - 1e-9 && p.x <= 60 + 1e-9 && p.y >= 30 - 1e-9 && p.y <= 50 + 1e-9;
            return inBox && !onIsland;
        };
        if (onPocket(c.start) && onPocket(c.end))
            out.push_back(e);
    }
    return out;
}

} // namespace

TEST_CASE("sliceRegions: a block with through holes in integer nanometres", "[geom2d][model]")
{
    JobContext ctx;
    const Regions got = take(sliceRegions(blockWithHoles(), kIdentity, 10 * kMm, ctx));
    const Regions want{
        region(rect(0, 0, 100 * kMm, 60 * kMm), {reverse(Path::circle({25 * kMm, 30 * kMm}, 5 * kMm, true)),
                                                 reverse(Path::circle({75 * kMm, 30 * kMm}, 8 * kMm, true))})};
    INFO(toText(got));
    CHECK(got == canonicalize(want));
}

TEST_CASE("pocketAboveFace and faceBoundary: a pocket picked by its floor", "[geom2d][model]")
{
    JobContext ctx;
    const kernel::Shape part = pocketPart();
    const auto floor = faceAt(part, 20, true);
    REQUIRE(floor);
    const FaceBoundary fb = take(faceBoundary(part, *floor, kIdentity));
    CHECK(fb.z == 20 * kMm);
    CHECK(fb.facesUp);
    CHECK(fb.region == canonicalize(region(pocketOutline(), {reverse(islandOutline())})));
    const Pocket pocket = take(pocketAboveFace(part, *floor, kIdentity, ctx));
    CHECK(pocket.z == 20 * kMm);
    INFO(toText(pocket.region));
    CHECK(pocket.region == canonicalize(region(pocketOutline(), {reverse(islandOutline())})));
    // The top face is not a floor with walls round it; the bottom face does not look up.
    const auto top = faceAt(part, 30, true);
    REQUIRE(top);
    CHECK(pocketAboveFace(part, *top, kIdentity, ctx).error().code == ErrorCode::InvalidArgument);
    const auto bottom = faceAt(part, 0, false);
    REQUIRE(bottom);
    CHECK(pocketAboveFace(part, *bottom, kIdentity, ctx).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("chainEdges: closed and open chains, tangent extension, direction", "[geom2d][model]")
{
    JobContext ctx;
    const kernel::Shape part = pocketPart();
    const std::vector<kernel::SubId> opening = openingEdges(part);
    REQUIRE(opening.size() == 8);
    SECTION("all the edges of the opening make one closed counter-clockwise chain")
    {
        const auto chains = take(chainEdges(part, opening, kIdentity, {}, ctx));
        REQUIRE(chains.size() == 1);
        CHECK(chains[0].path == pocketOutline());
        CHECK(chains[0].edges.size() == 8);
        CHECK(chains[0].zMin == 30 * kMm);
        CHECK(chains[0].zMax == 30 * kMm);
    }
    SECTION("one edge, extended tangentially, goes all round")
    {
        const auto single = take(chainEdges(part, std::vector<kernel::SubId>{opening[0]}, kIdentity, {}, ctx));
        REQUIRE(single.size() == 1);
        CHECK_FALSE(single[0].path.closed);
        CHECK(single[0].edges.size() == 1);
        ChainOptions extend;
        extend.tangentExtend = true;
        const auto all = take(chainEdges(part, std::vector<kernel::SubId>{opening[0]}, kIdentity, extend, ctx));
        REQUIRE(all.size() == 1);
        CHECK(all[0].path == pocketOutline());
        CHECK(all[0].edges.size() == 8);
    }
    SECTION("three consecutive edges make an open chain from its lower end")
    {
        // Edges are found in the shape's order; pick three that join: take the chain of all and its first three.
        const auto chains = take(chainEdges(part, opening, kIdentity, {}, ctx));
        REQUIRE(chains.size() == 1);
        const std::vector<kernel::SubId> three(chains[0].edges.begin(), chains[0].edges.begin() + 3);
        const auto open = take(chainEdges(part, three, kIdentity, {}, ctx));
        REQUIRE(open.size() == 1);
        CHECK_FALSE(open[0].path.closed);
        CHECK(open[0].path.spans.size() == 3);
        CHECK(open[0].path.start < open[0].path.spans.back().end);
    }
    SECTION("vertical edges are skipped")
    {
        std::vector<kernel::SubId> vertical;
        for (std::uint32_t i = 0; i < part.count(kernel::SubKind::Edge); ++i) {
            const kernel::CurveInfo c = take(kernel::curveInfo(part, {kernel::SubKind::Edge, i}));
            if (c.type == kernel::CurveType::Line && std::abs(c.start.x - c.end.x) < 1e-12 &&
                std::abs(c.start.y - c.end.y) < 1e-12)
                vertical.push_back({kernel::SubKind::Edge, i});
        }
        REQUIRE_FALSE(vertical.empty());
        CHECK(take(chainEdges(part, vertical, kIdentity, {}, ctx)).empty());
    }
}

TEST_CASE("silhouetteRegions and cancellation of the model calls", "[geom2d][model]")
{
    JobContext ctx;
    const Regions shadow = take(silhouetteRegions(sphere(), kIdentity, ctx));
    CHECK(shadow == Regions{region(Path::circle({0, 0}, 10 * kMm, true))});
    CancelSource source;
    source.cancel();
    JobContext cancelled{source.token(), {}};
    CHECK(sliceRegions(pocketPart(), kIdentity, 25 * kMm, cancelled).error().code == ErrorCode::Cancelled);
    CHECK(silhouetteRegions(pocketPart(), kIdentity, cancelled).error().code == ErrorCode::Cancelled);
    const auto opening = openingEdges(pocketPart());
    CHECK(chainEdges(pocketPart(), opening, kIdentity, {}, cancelled).error().code == ErrorCode::Cancelled);
}

} // namespace ray::cam::geom2d::test
