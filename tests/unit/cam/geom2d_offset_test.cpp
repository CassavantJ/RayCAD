// Offsets, open offsets, swept areas and booleans of geom2d (task T-050) on analytic cases: a rectangle, a circle, a
// slot, a rounded rectangle, an L shape with an inside corner, and an island merging with the wall when the tool does
// not fit. Acceptance: exact to 1 um with arcs as arcs; the results here are compared exactly (to the nanometre where
// an end point is irrational), arcs must have the exact centre and radius, and no polygon edge may be kept literally.
#include "geom2d_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace ray::cam::geom2d::test {
namespace {

Regions grow(const Regions& rs, Coord d)
{
    JobContext ctx;
    return take(offset(rs, d, ctx));
}

void requireSame(const Regions& got, const Regions& want)
{
    INFO("got:\n" << toText(got) << "want:\n" << toText(canonicalize(want)));
    CHECK(got == canonicalize(want));
}

// Rounds a millimetre value to nanometres (round half even).
Coord nm(double mm)
{
    return toCoord(mm);
}

} // namespace

TEST_CASE("offset: rectangle grown gets round corners, shrunk stays sharp", "[geom2d][offset]")
{
    const std::size_t literalBefore = literalEdgeCount();
    const Regions box{region(rect(0, 0, 100 * kMm, 60 * kMm))};
    const Coord d = 5 * kMm;
    requireSame(grow(box, d),
                {region(closedPath(
                    {-d, 0}, {arc({0, -d}, {0, 0}, d, true), line({100 * kMm, -d}),
                              arc({100 * kMm + d, 0}, {100 * kMm, 0}, d, true), line({100 * kMm + d, 60 * kMm}),
                              arc({100 * kMm, 60 * kMm + d}, {100 * kMm, 60 * kMm}, d, true), line({0, 60 * kMm + d}),
                              arc({-d, 60 * kMm}, {0, 60 * kMm}, d, true), line({-d, 0})}))});
    requireSame(grow(box, -d), {region(rect(d, d, 100 * kMm - d, 60 * kMm - d))});
    // A tool wider than the box: nothing left.
    CHECK(grow(box, -30 * kMm).empty());
    CHECK(grow(box, -31 * kMm).empty());
    CHECK(literalEdgeCount() == literalBefore);
}

TEST_CASE("offset: a circle stays a circle of radius r +- d", "[geom2d][offset]")
{
    const Pt c{12'345'678, -7'654'321};
    const Regions disc{region(Path::circle(c, 10 * kMm, true))};
    requireSame(grow(disc, 3 * kMm), {region(Path::circle(c, 13 * kMm, true))});
    requireSame(grow(disc, -3 * kMm), {region(Path::circle(c, 7 * kMm, true))});
    requireSame(grow(disc, -1), {region(Path::circle(c, 10 * kMm - 1, true))});
    CHECK(grow(disc, -10 * kMm).empty());
    CHECK(grow(disc, -12 * kMm).empty());
    // A circular hole in a plate: growing the plate shrinks the hole.
    const Regions plate{
        region(rect(-50 * kMm, -50 * kMm, 50 * kMm, 50 * kMm), {reverse(Path::circle({0, 0}, 8 * kMm, true))})};
    requireSame(grow(plate, -3 * kMm), {region(rect(-47 * kMm, -47 * kMm, 47 * kMm, 47 * kMm),
                                               {reverse(Path::circle({0, 0}, 11 * kMm, true))})});
}

TEST_CASE("offset: a slot shrinks to a slot of smaller radius, then to nothing", "[geom2d][offset]")
{
    const Regions s{region(slot(0, 30 * kMm, 0, 5 * kMm))};
    requireSame(grow(s, -2 * kMm), {region(slot(0, 30 * kMm, 0, 3 * kMm))});
    requireSame(grow(s, -4 * kMm), {region(slot(0, 30 * kMm, 0, 1 * kMm))});
    requireSame(grow(s, 1 * kMm), {region(slot(0, 30 * kMm, 0, 6 * kMm))});
    CHECK(grow(s, -5 * kMm).empty());
    CHECK(grow(s, -6 * kMm).empty());
}

TEST_CASE("offset: a rounded rectangle keeps its arc centres; past the corner radius the corners turn sharp",
          "[geom2d][offset]")
{
    const Regions rr{region(roundedRect(10 * kMm, 10 * kMm, 90 * kMm, 70 * kMm, 6 * kMm))};
    requireSame(grow(rr, -2 * kMm),
                {region(closedPath(
                    {12 * kMm, 16 * kMm},
                    {arc({16 * kMm, 12 * kMm}, {16 * kMm, 16 * kMm}, 4 * kMm, true), line({84 * kMm, 12 * kMm}),
                     arc({88 * kMm, 16 * kMm}, {84 * kMm, 16 * kMm}, 4 * kMm, true), line({88 * kMm, 64 * kMm}),
                     arc({84 * kMm, 68 * kMm}, {84 * kMm, 64 * kMm}, 4 * kMm, true), line({16 * kMm, 68 * kMm}),
                     arc({12 * kMm, 64 * kMm}, {16 * kMm, 64 * kMm}, 4 * kMm, true), line({12 * kMm, 16 * kMm})}))});
    requireSame(grow(rr, -6 * kMm), {region(rect(16 * kMm, 16 * kMm, 84 * kMm, 64 * kMm))});
    requireSame(grow(rr, -8 * kMm), {region(rect(18 * kMm, 18 * kMm, 82 * kMm, 62 * kMm))});
    requireSame(grow(rr, 4 * kMm), {region(roundedRect(6 * kMm, 6 * kMm, 94 * kMm, 74 * kMm, 10 * kMm))});
}

TEST_CASE("offset: an L shape: the inside corner is sharp when grown and an arc about it when shrunk",
          "[geom2d][offset]")
{
    const Path l = closedPath({0, 0}, {line({60 * kMm, 0}), line({60 * kMm, 20 * kMm}), line({20 * kMm, 20 * kMm}),
                                       line({20 * kMm, 60 * kMm}), line({0, 60 * kMm}), line({0, 0})});
    const Coord d = 5 * kMm;
    requireSame(grow({region(l)}, d),
                {region(closedPath(
                    {-d, 0}, {arc({0, -d}, {0, 0}, d, true), line({60 * kMm, -d}),
                              arc({65 * kMm, 0}, {60 * kMm, 0}, d, true), line({65 * kMm, 20 * kMm}),
                              arc({60 * kMm, 25 * kMm}, {60 * kMm, 20 * kMm}, d, true), line({25 * kMm, 25 * kMm}),
                              line({25 * kMm, 60 * kMm}), arc({20 * kMm, 65 * kMm}, {20 * kMm, 60 * kMm}, d, true),
                              line({0, 65 * kMm}), arc({-d, 60 * kMm}, {0, 60 * kMm}, d, true), line({-d, 0})}))});
    requireSame(grow({region(l)}, -d),
                {region(closedPath({d, d}, {line({55 * kMm, d}), line({55 * kMm, 15 * kMm}), line({20 * kMm, 15 * kMm}),
                                            arc({15 * kMm, 20 * kMm}, {20 * kMm, 20 * kMm}, d, false),
                                            line({15 * kMm, 55 * kMm}), line({d, 55 * kMm}), line({d, d})}))});
}

TEST_CASE("offset: an island merges with the wall when the tool does not fit between them", "[geom2d][offset]")
{
    // Pocket 0..100 with an island 40..60 (a hole of the region the tool centre may cover).
    const Regions pocket{
        region(rect(0, 0, 100 * kMm, 100 * kMm), {reverse(rect(40 * kMm, 40 * kMm, 60 * kMm, 60 * kMm))})};
    SECTION("the tool fits: a ring between the wall and the island")
    {
        const Coord r = 15 * kMm;
        const Path island =
            closedPath({25 * kMm, 40 * kMm},
                       {line({25 * kMm, 60 * kMm}), arc({40 * kMm, 75 * kMm}, {40 * kMm, 60 * kMm}, r, false),
                        line({60 * kMm, 75 * kMm}), arc({75 * kMm, 60 * kMm}, {60 * kMm, 60 * kMm}, r, false),
                        line({75 * kMm, 40 * kMm}), arc({60 * kMm, 25 * kMm}, {60 * kMm, 40 * kMm}, r, false),
                        line({40 * kMm, 25 * kMm}), arc({25 * kMm, 40 * kMm}, {40 * kMm, 40 * kMm}, r, false)});
        requireSame(grow(pocket, -r), {region(rect(r, r, 100 * kMm - r, 100 * kMm - r), {island})});
    }
    SECTION("the tool does not fit: four corner pockets, each two lines and an arc of the grown island")
    {
        const Coord r = 22 * kMm;
        const Coord far = nm(40.0 - std::sqrt(22.0 * 22.0 - 18.0 * 18.0)); // where x = 22 meets the island's arc
        // The four corners by symmetry about x = 50 and y = 50.
        Regions want;
        for (const int sx : {0, 1})
            for (const int sy : {0, 1}) {
                const auto X = [&](Coord x) {
                    return sx ? 100 * kMm - x : x;
                };
                const auto Y = [&](Coord y) {
                    return sy ? 100 * kMm - y : y;
                };
                const bool mirrored = sx != sy; // an odd number of mirrors turns every direction round
                Path corner = closedPath({X(r), Y(r)}, {line({X(far), Y(r)}),
                                                        arc({X(r), Y(far)}, {X(40 * kMm), Y(40 * kMm)}, r, mirrored),
                                                        line({X(r), Y(r)})});
                if (mirrored)
                    corner = reverse(corner);
                want.push_back(region(corner));
            }
        const Regions got = grow(pocket, -r);
        requireSame(got, want);
        CHECK(worstArcEndError(got) <= 1.0);
    }
    SECTION("a still larger tool removes everything")
    {
        CHECK(grow(pocket, -25 * kMm).empty());
    }
}

TEST_CASE("offsetOpen: the tool-centre path of an open chain on either side", "[geom2d][offset]")
{
    JobContext ctx;
    const Coord d = 5 * kMm;
    const Path straight = openPath({0, 0}, {line({100 * kMm, 0})});
    CHECK(take(offsetOpen(straight, d, Side::Left, ctx)) == Paths{openPath({0, d}, {line({100 * kMm, d})})});
    CHECK(take(offsetOpen(straight, d, Side::Right, ctx)) == Paths{openPath({0, -d}, {line({100 * kMm, -d})})});
    // A left turn: on the left (inside) the offsets meet at a sharp corner, on the right an arc goes round it.
    const Path bend = openPath({0, 0}, {line({50 * kMm, 0}), line({50 * kMm, 50 * kMm})});
    CHECK(take(offsetOpen(bend, d, Side::Left, ctx)) ==
          Paths{openPath({0, d}, {line({45 * kMm, d}), line({45 * kMm, 50 * kMm})})});
    CHECK(take(offsetOpen(bend, d, Side::Right, ctx)) ==
          Paths{openPath({0, -d}, {line({50 * kMm, -d}), arc({55 * kMm, 0}, {50 * kMm, 0}, d, true),
                                   line({55 * kMm, 50 * kMm})})});
    // An arc of radius 20 offset to its outside and inside: radius 25 and 15 about the same centre.
    const Path bow = openPath({20 * kMm, 0}, {arc({0, 20 * kMm}, {0, 0}, 20 * kMm, true)});
    CHECK(take(offsetOpen(bow, d, Side::Right, ctx)) ==
          Paths{openPath({25 * kMm, 0}, {arc({0, 25 * kMm}, {0, 0}, 25 * kMm, true)})});
    CHECK(take(offsetOpen(bow, d, Side::Left, ctx)) ==
          Paths{openPath({15 * kMm, 0}, {arc({0, 15 * kMm}, {0, 0}, 15 * kMm, true)})});
    CHECK(offsetOpen(bow, 0, Side::Left, ctx).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("sweptArea: the area a tool covers along a path", "[geom2d][offset]")
{
    JobContext ctx;
    const Path straight = openPath({0, 0}, {line({100 * kMm, 0})});
    const Regions swept = take(sweptArea(std::vector<Path>{straight}, 5 * kMm, ctx));
    requireSame(swept, {region(slot(0, 100 * kMm, 0, 5 * kMm))});
    const double mm2 = area(swept) / (1e6 * 1e6);
    CHECK(std::abs(mm2 - (1000.0 + kPiD * 25.0)) < 1e-6);
    // A square pass with a tool large enough to cover its inside: one region without a hole.
    const Path square = rect(0, 0, 20 * kMm, 20 * kMm);
    const Regions covered = take(sweptArea(std::vector<Path>{square}, 10 * kMm, ctx));
    REQUIRE(covered.size() == 1);
    CHECK(covered[0].holes.empty());
    // A smaller tool leaves a square hole in the middle.
    const Regions ring = take(sweptArea(std::vector<Path>{square}, 4 * kMm, ctx));
    REQUIRE(ring.size() == 1);
    REQUIRE(ring[0].holes.size() == 1);
    CHECK(ring[0].holes[0] == canonicalize(reverse(rect(4 * kMm, 4 * kMm, 16 * kMm, 16 * kMm))));
}

TEST_CASE("booleans: union, intersection and difference keep arcs exact", "[geom2d][offset]")
{
    JobContext ctx;
    const std::size_t literalBefore = literalEdgeCount();
    const Regions plate{region(rect(0, 0, 100 * kMm, 60 * kMm))};
    const Regions disc{region(Path::circle({50 * kMm, 30 * kMm}, 10 * kMm, true))};
    requireSame(
        take(subtract(plate, disc, ctx)),
        {region(rect(0, 0, 100 * kMm, 60 * kMm), {reverse(Path::circle({50 * kMm, 30 * kMm}, 10 * kMm, true))})});
    requireSame(take(intersect(plate, disc, ctx)), disc);
    requireSame(take(unite(plate, disc, ctx)), plate);
    // Two overlapping circles: two arcs meeting at x = 7.5, y = +-sqrt(100 - 56.25).
    const Regions two = take(unite({region(Path::circle({0, 0}, 10 * kMm, true))},
                                   {region(Path::circle({15 * kMm, 0}, 10 * kMm, true))}, ctx));
    const Coord y = nm(std::sqrt(100.0 - 56.25));
    requireSame(two, {region(closedPath({nm(7.5), -y}, {arc({nm(7.5), y}, {15 * kMm, 0}, 10 * kMm, true),
                                                        arc({nm(7.5), -y}, {0, 0}, 10 * kMm, true)}))});
    const Regions lens = take(intersect({region(Path::circle({0, 0}, 10 * kMm, true))},
                                        {region(Path::circle({15 * kMm, 0}, 10 * kMm, true))}, ctx));
    requireSame(lens, {region(closedPath({nm(7.5), -y}, {arc({nm(7.5), y}, {0, 0}, 10 * kMm, true),
                                                         arc({nm(7.5), -y}, {15 * kMm, 0}, 10 * kMm, true)}))});
    // A rectangle cut by a circle across its edge: a line, an arc of the circle, a line.
    const Regions bite = take(subtract({region(rect(0, 0, 40 * kMm, 20 * kMm))},
                                       {region(Path::circle({40 * kMm, 10 * kMm}, 5 * kMm, true))}, ctx));
    requireSame(bite, {region(closedPath({0, 0}, {line({40 * kMm, 0}), line({40 * kMm, 5 * kMm}),
                                                  arc({40 * kMm, 15 * kMm}, {40 * kMm, 10 * kMm}, 5 * kMm, false),
                                                  line({40 * kMm, 20 * kMm}), line({0, 20 * kMm}), line({0, 0})}))});
    CHECK(literalEdgeCount() == literalBefore);
}

TEST_CASE("offset and booleans: input checks and cancellation", "[geom2d][offset]")
{
    JobContext ctx;
    const Regions far{region(rect(0, 0, 2 * kMaxCoord, 10))};
    CHECK(offset(far, 1, ctx).error().code == ErrorCode::InvalidArgument);
    CHECK(offset({region(rect(0, 0, 10, 10))}, kMaxCoord + 1, ctx).error().code == ErrorCode::InvalidArgument);
    CancelSource source;
    source.cancel();
    JobContext cancelled{source.token(), {}};
    const Regions box{region(rect(0, 0, 10 * kMm, 10 * kMm))};
    CHECK(offset(box, kMm, cancelled).error().code == ErrorCode::Cancelled);
    CHECK(unite(box, box, cancelled).error().code == ErrorCode::Cancelled);
    CHECK(sweptArea(std::vector<Path>{box[0].outer}, kMm, cancelled).error().code == ErrorCode::Cancelled);
}

} // namespace ray::cam::geom2d::test
