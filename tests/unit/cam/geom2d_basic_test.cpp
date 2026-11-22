// geom2d basics (task T-050): conversion from and to kernel profiles (round trip under 1 nm), measures, point
// location, nesting, canonical form, positions along paths, moves and the text format.
#include "geom2d_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace ray::cam::geom2d::test {

TEST_CASE("roundNm rounds half to even", "[geom2d]")
{
    CHECK(roundNm(0.5) == 0);
    CHECK(roundNm(1.5) == 2);
    CHECK(roundNm(2.5) == 2);
    CHECK(roundNm(-0.5) == 0);
    CHECK(roundNm(-1.5) == -2);
    CHECK(roundNm(-2.5) == -2);
    CHECK(roundNm(2.4999999) == 2);
    CHECK(roundNm(-2.5000001) == -3);
    CHECK(roundNm(std::nan("")) == 0);
}

TEST_CASE("conversion: the round trip through kernel profiles moves no point by 1 nm or more", "[geom2d]")
{
    Rng rng{20261122};
    double worst = 0.0;
    for (int n = 0; n < 2000; ++n) {
        INFO("case " << n);
        // A random rounded rectangle anywhere within +-1 m, corner radius random.
        const Coord x0 = rng.between(-1'000'000'000, 900'000'000), y0 = rng.between(-1'000'000'000, 900'000'000);
        const Coord w = rng.between(2'000'000, 99'000'000), h = rng.between(2'000'000, 99'000'000);
        const Coord r = rng.between(1, std::min(w, h) / 2 - 1);
        const Path p = roundedRect(x0, y0, x0 + w, y0 + h, r);
        const kernel::ProfileLoop loop = toLoop(p);
        // nm -> mm -> nm is exact.
        CHECK(take(fromLoop(loop)) == p);
        // mm -> nm -> mm moves each value by at most 0.5 nm: shift the loop by an odd fraction first.
        kernel::ProfileLoop shifted = loop;
        const double dx = rng.uniform() * 1e-6, dy = rng.uniform() * 1e-6;
        for (kernel::ProfileCurve& c : shifted.curves)
            for (Vec2& v : c.pts) {
                v.x += dx;
                v.y += dy;
            }
        const kernel::ProfileLoop back = toLoop(take(fromLoop(shifted)));
        REQUIRE(back.curves.size() == shifted.curves.size());
        for (std::size_t i = 0; i < back.curves.size(); ++i)
            for (std::size_t k = 0; k < back.curves[i].pts.size(); ++k) {
                worst = std::max(worst, std::abs(back.curves[i].pts[k].x - shifted.curves[i].pts[k].x));
                worst = std::max(worst, std::abs(back.curves[i].pts[k].y - shifted.curves[i].pts[k].y));
            }
    }
    INFO("largest round-trip error " << worst * 1e6 << " nm");
    CHECK(worst < 1e-6);
    CHECK(worst <= 0.5e-6 + 1e-12);
    // A circle loop.
    const Path c = Path::circle({3, -4}, 1234567, false);
    CHECK(take(fromLoop(toLoop(c))) == c);
}

TEST_CASE("fromLoop chains curves in any order and direction and refuses gaps and B-splines", "[geom2d]")
{
    using Kind = kernel::ProfileCurve::Kind;
    const auto L = [](Vec2 a, Vec2 b) {
        return kernel::ProfileCurve{Kind::Line, {a, b}, 0.0, false, 0};
    };
    kernel::ProfileLoop loop;
    loop.curves = {L({0, 0}, {10, 0}), L({0, 10}, {10, 10}) /* reversed */, L({10, 0}, {10, 10}), L({0, 10}, {0, 0})};
    CHECK(take(fromLoop(loop)) == rect(0, 0, 10 * kMm, 10 * kMm));
    kernel::ProfileLoop gap = loop;
    gap.curves[2] = L({10, 0.002}, {10, 10});
    CHECK(fromLoop(gap).error().code == ErrorCode::InvalidArgument);
    kernel::ProfileLoop spline;
    spline.curves = {kernel::ProfileCurve{Kind::Bspline, {{0, 0}, {1, 1}, {2, 0}}, 0.0, false, 0}, L({2, 0}, {0, 0})};
    CHECK(fromLoop(spline).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("measures: area, length, bounds", "[geom2d]")
{
    const Path r = rect(0, 0, 10 * kMm, 5 * kMm);
    CHECK(area(r) == 50.0 * kMm * kMm);
    CHECK(area(reverse(r)) == -50.0 * kMm * kMm);
    CHECK(length(r) == 30.0 * kMm);
    const Path c = Path::circle({0, 0}, kMm, true);
    CHECK(std::abs(area(c) - kPiD * 1e12) < 1e-3);
    CHECK(std::abs(length(c) - 2 * kPiD * 1e6) < 1e-6);
    const Path s = slot(0, 10 * kMm, 0, 2 * kMm);
    CHECK(std::abs(area(s) - (40.0 + kPiD * 4.0) * 1e12) < 1e-2);
    const Bounds b = bounds(s);
    CHECK(b.min == Pt{-2 * kMm, -2 * kMm});
    CHECK(b.max == Pt{12 * kMm, 2 * kMm});
    const Region withHole = region(rect(0, 0, 10 * kMm, 10 * kMm), {reverse(rect(2 * kMm, 2 * kMm, 4 * kMm, 4 * kMm))});
    CHECK(area(withHole) == 96.0 * kMm * kMm);
}

TEST_CASE("locate: inside, outside and on the boundary, lines and arcs", "[geom2d]")
{
    const Path s = slot(0, 10 * kMm, 0, 2 * kMm);
    CHECK(locate(Pt{5 * kMm, 0}, s) == Where::Inside);
    CHECK(locate(Pt{-1 * kMm, 0}, s) == Where::Inside);      // in the left cap
    CHECK(locate(Pt{-2 * kMm - 1, 0}, s) == Where::Outside); // just left of it
    CHECK(locate(Pt{-2 * kMm, 0}, s) == Where::OnBoundary);  // on the arc
    CHECK(locate(Pt{5 * kMm, 2 * kMm}, s) == Where::OnBoundary);
    CHECK(locate(Pt{5 * kMm, 2 * kMm + 1}, s) == Where::Outside);
    CHECK(locate(Pt{11 * kMm, 1 * kMm}, s) == Where::Inside);
    CHECK(locate(Pt{12 * kMm, 2 * kMm}, s) == Where::Outside); // the box corner beyond the cap
    // Clockwise works the same.
    CHECK(locate(Pt{5 * kMm, 0}, reverse(s)) == Where::Inside);
    const Region withHole =
        region(rect(0, 0, 10 * kMm, 10 * kMm), {reverse(Path::circle({5 * kMm, 5 * kMm}, kMm, true))});
    CHECK(locate(Pt{5 * kMm, 5 * kMm}, withHole) == Where::Outside);
    CHECK(locate(Pt{1 * kMm, 1 * kMm}, withHole) == Where::Inside);
    CHECK(locate(Pt{6 * kMm, 5 * kMm}, withHole) == Where::OnBoundary);
    const auto inner = interiorPoint(withHole);
    REQUIRE(inner);
    CHECK(locate(*inner, withHole) == Where::Inside);
}

TEST_CASE("nest and regionsByNesting: even-odd nesting, any orientation, box-pruned", "[geom2d]")
{
    JobContext ctx;
    // An outer square, two holes, an island in one hole, given in a scrambled order and orientations.
    const std::vector<Path> paths{
        Path::circle({30 * kMm, 50 * kMm}, 5 * kMm, true),  // island in hole 1
        reverse(rect(0, 0, 100 * kMm, 100 * kMm)),          // outer, clockwise
        rect(10 * kMm, 30 * kMm, 50 * kMm, 70 * kMm),       // hole 1, counter-clockwise
        Path::circle({75 * kMm, 50 * kMm}, 10 * kMm, true), // hole 2
    };
    const Nesting n = take(nest(paths, ctx));
    CHECK(n.parent == std::vector<int>{2, -1, 1, 1});
    CHECK(n.depth == std::vector<int>{2, 0, 1, 1});
    const Regions rs = regionsByNesting(paths, n);
    REQUIRE(rs.size() == 2);
    CHECK(rs[0].outer == rect(0, 0, 100 * kMm, 100 * kMm));
    REQUIRE(rs[0].holes.size() == 2);
    CHECK(area(rs[0].holes[0]) < 0);
    CHECK(rs[1].outer == Path::circle({30 * kMm, 50 * kMm}, 5 * kMm, true));
    // Same result whatever the input order.
    std::vector<Path> shuffled{paths[3], paths[0], paths[2], paths[1]};
    CHECK(regionsByNesting(shuffled, take(nest(shuffled, ctx))) == rs);
    // A loop touching its parent at a vertex is still inside it.
    const std::vector<Path> touching{
        rect(0, 0, 10 * kMm, 10 * kMm),
        closedPath({0, 0}, {line({5 * kMm, 2 * kMm}), line({2 * kMm, 5 * kMm}), line({0, 0})})};
    CHECK(take(nest(touching, ctx)).parent == std::vector<int>{-1, 0});
}

TEST_CASE("canonicalize: merges, start vertex and orientation", "[geom2d]")
{
    // Collinear pieces and a split arc merge; the start moves to the lowest vertex.
    const Path messy = closedPath({5 * kMm, 0}, {line({10 * kMm, 0}), line({10 * kMm, 10 * kMm}), line({0, 10 * kMm}),
                                                 line({0, 0}), line({5 * kMm, 0})});
    CHECK(canonicalize(messy) == rect(0, 0, 10 * kMm, 10 * kMm));
    const Path halves = closedPath({kMm, 0}, {arc({-kMm, 0}, {0, 0}, kMm, true), arc({kMm, 0}, {0, 0}, kMm, true)});
    CHECK(canonicalize(halves) == Path::circle({0, 0}, kMm, true));
    const Path quarters = closedPath({0, 0}, {line({kMm, 0}), arc({0, kMm}, {0, 0}, kMm, true), line({0, 0})});
    CHECK(canonicalize(quarters) == quarters);
    const Region r = canonicalize(region(reverse(rect(0, 0, 10, 10)), {rect(2, 2, 4, 4)}));
    CHECK(area(r.outer) > 0);
    CHECK(area(r.holes[0]) < 0);
}

TEST_CASE("paths along their length: pointAt, tangentAt, subPath, startAt, polygonize", "[geom2d]")
{
    const Path s = slot(0, 10 * kMm, 0, 2 * kMm); // bottom line, right cap, top line, left cap
    const double total = length(s);
    CHECK(pointAt(s, 0) == Pt{0, -2 * kMm});
    CHECK(pointAt(s, 5 * kMm) == Pt{5 * kMm, -2 * kMm});
    CHECK(pointAt(s, 10 * kMm + kPiD * kMm) == Pt{12 * kMm, 0}); // middle of the right cap
    const Vec2 t = tangentAt(s, 10 * kMm + kPiD * kMm);
    CHECK(std::abs(t.x) < 1e-12);
    CHECK(std::abs(t.y - 1.0) < 1e-12);
    const Path part = subPath(s, 5 * kMm, 15 * kMm + 2 * kPiD * kMm);
    CHECK(part.start == Pt{5 * kMm, -2 * kMm});
    REQUIRE(part.spans.size() == 3);
    CHECK(part.spans[1].kind == SpanKind::ArcCCW);
    CHECK(part.spans[2].end == Pt{5 * kMm, 2 * kMm});
    // Wrapping round a closed path.
    const Path wrap = subPath(s, total - kMm, total + kMm);
    CHECK(wrap.start == pointAt(s, total - kMm));
    CHECK(wrap.spans.back().end == Pt{kMm, -2 * kMm});
    const Path moved = startAt(s, 5 * kMm);
    CHECK(moved.start == Pt{5 * kMm, -2 * kMm});
    CHECK(moved.closed);
    CHECK(std::abs(length(moved) - total) < 1e-6);
    CHECK(moved.spans.back().end == moved.start);
    // Polygonize: every vertex on the arcs, every chord within the sag.
    const std::vector<Pt> poly = polygonize(s, 100);
    double worst = 0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Pt a = poly[i], b = poly[(i + 1) % poly.size()];
        worst = std::max(worst, pathDistance(P2{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)}, s));
    }
    CHECK(worst <= 101.0);
}

TEST_CASE("orientation helpers: tool side for climb and conventional", "[geom2d]")
{
    CHECK(toolSideFor(CutMode::Climb, true) == Side::Left);
    CHECK(toolSideFor(CutMode::Conventional, true) == Side::Right);
    CHECK(toolSideFor(CutMode::Climb, false) == Side::Right);
    const Path r = rect(0, 0, 10, 10);
    CHECK(insideSide(r) == Side::Left);
    // Climb outside a part with M3: the tool must be on the left, the part (inside) on the right: clockwise.
    const Path climbOutside = orientForTool(r, Side::Right, toolSideFor(CutMode::Climb, true));
    CHECK(area(climbOutside) < 0);
    CHECK(orientForTool(r, Side::Left, Side::Left) == r);
}

TEST_CASE("appendMoves: lines and arcs, long arcs split at 180 degrees", "[geom2d]")
{
    std::vector<Move> moves;
    appendMoves(Path::circle({0, 0}, kMm, true), -3 * kMm, MoveRole::Cut, 500'000, moves);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].kind == MoveKind::ArcCCW);
    CHECK(moves[0].x == -kMm);
    CHECK(moves[0].y == 0);
    CHECK(moves[0].i == 0);
    CHECK(moves[0].z == -3 * kMm);
    CHECK(moves[1].x == kMm);
    CHECK(moves[1].feedMicronPerMin == 500'000);
    moves.clear();
    appendMoves(rect(0, 0, kMm, kMm), 0, MoveRole::Cut, 1, moves);
    CHECK(moves.size() == 4);
    CHECK(moves[0].kind == MoveKind::Linear);
}

TEST_CASE("toText is the integer golden format", "[geom2d]")
{
    CHECK(toText(slot(0, 10, 0, 2)) == "P 1 0 -2\nL 10 -2\nA+ 10 2 10 0 2\nL 0 2\nA+ 0 -2 0 0 2\n");
    CHECK(toText(Regions{region(rect(0, 0, 1, 1))}) == "R\nO\nP 1 0 0\nL 1 0\nL 1 1\nL 0 1\nL 0 0\n");
}

} // namespace ray::cam::geom2d::test
