// Arc fitting property tests (task T-050 acceptance: "arc fitting reproduces known arcs exactly and never deviates more
// than tolerance: property test, 10,000 cases"). Fixed seeds; every failure prints its case seed.
//
// Two ways arcs come back from polylines:
//   1. offsets and booleans flatten exact curves for Clipper2 and rebuild them from the curve tags: known arcs must
//   come
//      back EXACTLY (same centre, radius and end points). 10,000 random shapes made of known arcs and lines go through
//      Clipper2 (a union with nothing) and must be returned bit for bit.
//   2. fitArcs fits polylines without tags (the adaptive engine's passes, sampled curves): 10,000 polylines sampled
//   from
//      known integer arcs must come back as ONE arc with the same end points and direction; the centre and radius
//      differences are measured and bounded; and 10,000 random polylines (arcs, lines, corners and noise mixed) must be
//      fitted within tolerance both ways (fit to polyline and polyline to fit).
#include "geom2d_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace ray::cam::geom2d::test {
namespace {

// A random shape made of known arcs and lines, anywhere in +-0.5 m.
Path randomShape(Rng& rng)
{
    const Coord x0 = rng.between(-500'000'000, 400'000'000), y0 = rng.between(-500'000'000, 400'000'000);
    switch (rng.next() % 4) {
    case 0:
        return Path::circle({x0, y0}, rng.between(10'000, 50'000'000), true);
    case 1: {
        const Coord w = rng.between(100'000, 80'000'000), h = rng.between(100'000, 80'000'000);
        return roundedRect(x0, y0, x0 + w, y0 + h, rng.between(1'000, std::min(w, h) / 2 - 1'000));
    }
    case 2: {
        // A slot along a random direction: its arc end points are rounded, so they lie up to 0.7 nm off the circle.
        const Coord r = rng.between(50'000, 20'000'000);
        const double len = rng.between(10'000.0, 60'000'000.0), a = rng.between(0.0, 2 * kPiD);
        const Pt c0{x0, y0};
        const Pt c1{x0 + static_cast<Coord>(std::llround(len * std::cos(a))),
                    y0 + static_cast<Coord>(std::llround(len * std::sin(a)))};
        const double ux = std::cos(a), uy = std::sin(a);
        const auto at = [&](Pt c, double s) {
            return Pt{c.x + static_cast<Coord>(std::llround(s * -uy * static_cast<double>(r))),
                      c.y + static_cast<Coord>(std::llround(s * ux * static_cast<double>(r)))};
        };
        return closedPath(
            at(c0, -1), {line(at(c1, -1)), arc(at(c1, 1), c1, r, true), line(at(c0, 1)), arc(at(c0, -1), c0, r, true)});
    }
    default: {
        // A pie slice: two radii and an arc of random sweep.
        const Coord r = rng.between(100'000, 40'000'000);
        const double a0 = rng.between(0.0, 2 * kPiD), sweep = rng.between(0.2, 5.5);
        const Pt c{x0, y0};
        const auto on = [&](double a) {
            return Pt{c.x + static_cast<Coord>(std::llround(static_cast<double>(r) * std::cos(a))),
                      c.y + static_cast<Coord>(std::llround(static_cast<double>(r) * std::sin(a)))};
        };
        return closedPath(c, {line(on(a0)), arc(on(a0 + sweep), c, r, true), line(c)});
    }
    }
}

} // namespace

TEST_CASE("known arcs come back exactly through Clipper2 and the curve tags (10,000 seeded shapes)", "[geom2d][fit][sweeps]")
{
    JobContext ctx;
    const std::size_t literalBefore = literalEdgeCount();
    int checked = 0;
    for (std::uint64_t n = 0; n < 10'000; ++n) {
        const std::uint64_t seed = 1'000'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        Regions shape{region(randomShape(rng))};
        // Half the cases get a circular hole well inside, so holes go through the same machinery.
        if (n % 2 == 0) {
            const Bounds b = bounds(shape[0].outer);
            const Coord span = std::min(b.max.x - b.min.x, b.max.y - b.min.y);
            const auto inside = interiorPoint(shape[0]);
            if (inside && span > 1'000'000) {
                const Coord r = std::max<Coord>(1'000, span / 50);
                shape = take(subtract(shape, {region(Path::circle(*inside, r, true))}, ctx));
            }
        }
        const Regions expected = canonicalize(shape);
        const Regions back = take(unite(shape, {}, ctx));
        INFO("in:\n" << toText(expected) << "out:\n" << toText(back));
        REQUIRE(back == expected);
        ++checked;
    }
    CHECK(checked == 10'000);
    CHECK(literalEdgeCount() == literalBefore);
}

TEST_CASE("fitArcs: polylines sampled from known arcs come back as one arc (10,000 seeded arcs)", "[geom2d][fit][sweeps]")
{
    double worstCentre = 0, worstRadius = 0;
    int exactCentre = 0, cases = 0;
    for (std::uint64_t n = 0; n < 10'000; ++n) {
        const std::uint64_t seed = 2'000'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        const Pt c{rng.between(-200'000'000, 200'000'000), rng.between(-200'000'000, 200'000'000)};
        const Coord r = rng.between(200'000, 100'000'000); // 0.2 .. 100 mm
        const bool ccw = (rng.next() & 1) != 0;
        const double a0 = rng.between(-kPiD, kPiD);
        // At least 10 degrees and long enough to be an arc at all (chord over 10 um, sagitta over the tolerance).
        double sweep = rng.between(10.0, 179.0) * kPiD / 180.0;
        const auto on = [&](double a) {
            return Pt{c.x + static_cast<Coord>(std::llround(static_cast<double>(r) * std::cos(a))),
                      c.y + static_cast<Coord>(std::llround(static_cast<double>(r) * std::sin(a)))};
        };
        const Pt start = on(a0), end = on(a0 + (ccw ? sweep : -sweep));
        const Path source = openPath(start, {arc(end, c, r, ccw)});
        // The polyline's own chords stay within half the fit tolerance of the arc (an arc can only be within the
        // tolerance of a polyline whose chords are).
        const Coord sag = rng.between(50, 500);
        const std::vector<Pt> poly = polygonize(source, sag);
        FitOptions opt;
        const Path fit = fitArcs(poly, false, opt);
        const double sagitta = static_cast<double>(r) * (1 - std::cos(0.5 * sweep));
        if (std::hypot(double(end.x - start.x), double(end.y - start.y)) < double(opt.minArcChord) ||
            sagitta <= double(opt.tolerance) || poly.size() < 3) {
            // Not an arc by the thresholds: whatever came back must still be within tolerance.
            CHECK(oneWay(fit, source) <= double(opt.tolerance) + 1.0);
            continue;
        }
        ++cases;
        INFO("r " << r << " sweep " << sweep << " sag " << sag << " points " << poly.size() << "\n" << toText(fit));
        REQUIRE(fit.spans.size() == 1);
        const Span& s = fit.spans[0];
        CHECK(fit.start == start);
        CHECK(s.end == end);
        CHECK(s.kind == (ccw ? SpanKind::ArcCCW : SpanKind::ArcCW));
        const double dc = std::hypot(double(s.centre.x - c.x), double(s.centre.y - c.y));
        worstCentre = std::max(worstCentre, dc);
        worstRadius = std::max(worstRadius, std::abs(double(s.radius - r)));
        exactCentre += (s.centre == c && s.radius == r) ? 1 : 0;
        // Never further from the source arc than the tolerance, either way.
        CHECK(oneWay(fit, source) <= double(opt.tolerance) + 1.0);
        CHECK(oneWay(source, fit) <= double(opt.tolerance) + 1.0);
    }
    INFO("arcs " << cases << ", exact centre and radius " << exactCentre << ", worst centre " << worstCentre
                 << " nm, worst radius " << worstRadius << " nm");
    CHECK(cases > 9'000);
    // The fitted circle may differ from the source by what the rounding of the samples allows; it stays within the
    // fit tolerance of it (checked above), and this bounds it.
    CHECK(worstRadius <= double(kFitTolerance));
    WARN("fitArcs on " << cases << " sampled arcs: exact centre and radius in " << exactCentre << ", worst centre "
                       << worstCentre << " nm, worst radius " << worstRadius << " nm");
}

TEST_CASE("fitArcs: never deviates more than the tolerance (10,000 seeded random polylines)", "[geom2d][fit][sweeps]")
{
    for (std::uint64_t n = 0; n < 10'000; ++n) {
        const std::uint64_t seed = 3'000'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // A random walk of pieces: arcs (sampled), straight runs, sharp corners and noise.
        std::vector<Pt> pts{{rng.between(-100'000'000, 100'000'000), rng.between(-100'000'000, 100'000'000)}};
        double heading = rng.between(-kPiD, kPiD);
        const int pieces = static_cast<int>(rng.between(Coord{1}, Coord{8}));
        for (int k = 0; k < pieces; ++k) {
            const Pt from = pts.back();
            switch (rng.next() % 4) {
            case 0: { // arc
                const double r = rng.between(50'000.0, 50'000'000.0);
                const double turn = rng.between(-3.0, 3.0);
                const int steps = static_cast<int>(rng.between(Coord{2}, Coord{60}));
                const double side = turn > 0 ? 1 : -1;
                const double cx = from.x - side * r * std::sin(heading), cy = from.y + side * r * std::cos(heading);
                const double a0 = std::atan2(from.y - cy, from.x - cx);
                for (int i = 1; i <= steps; ++i) {
                    const double a = a0 + turn * i / steps;
                    pts.push_back({static_cast<Coord>(std::llround(cx + r * std::cos(a))),
                                   static_cast<Coord>(std::llround(cy + r * std::sin(a)))});
                }
                heading += turn;
                break;
            }
            case 1: { // straight run in several pieces
                const double len = rng.between(1'000.0, 20'000'000.0);
                const int steps = static_cast<int>(rng.between(Coord{1}, Coord{10}));
                for (int i = 1; i <= steps; ++i)
                    pts.push_back({from.x + static_cast<Coord>(std::llround(len * i / steps * std::cos(heading))),
                                   from.y + static_cast<Coord>(std::llround(len * i / steps * std::sin(heading)))});
                break;
            }
            case 2: // corner
                heading += rng.between(-2.5, 2.5);
                break;
            default: { // noise: short jittery steps
                const int steps = static_cast<int>(rng.between(Coord{1}, Coord{20}));
                for (int i = 0; i < steps; ++i) {
                    const Pt last = pts.back();
                    pts.push_back(
                        {last.x + rng.between(-3'000, 3'000) + static_cast<Coord>(5'000 * std::cos(heading)),
                         last.y + rng.between(-3'000, 3'000) + static_cast<Coord>(5'000 * std::sin(heading))});
                }
                break;
            }
            }
        }
        FitOptions opt;
        opt.tolerance = rng.between(Coord{200}, Coord{20'000});
        const bool closed = n % 5 == 0 && pts.size() > 3;
        const Path fit = fitArcs(pts, closed, opt);
        Path poly;
        poly.start = pts[0];
        poly.closed = closed;
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (pts[i] != pts[i - 1])
                poly.spans.push_back(line(pts[i]));
        if (closed && poly.spans.back().end != poly.start)
            poly.spans.push_back(line(poly.start));
        INFO("tolerance " << opt.tolerance << " points " << pts.size() << "\n" << toText(fit));
        const double slack = 1.5; // rounding of the sample points of an arc span in this check
        CHECK(oneWay(fit, poly, 64) <= double(opt.tolerance) + slack);
        CHECK(oneWay(poly, fit, 4) <= double(opt.tolerance) + slack);
        // The rules on every arc: chord, radius, sweep, one direction (an arc whose end equals its start is never
        // produced).
        Pt cur = fit.start;
        for (const Span& s : fit.spans) {
            if (s.kind != SpanKind::Line) {
                CHECK(s.end != cur);
                CHECK(std::hypot(double(s.end.x - cur.x), double(s.end.y - cur.y)) >= double(opt.minArcChord));
                CHECK(s.radius <= opt.maxArcRadius);
                CHECK(sweepOf(cur, s) <= opt.maxSweep + 1e-9);
            }
            cur = s.end;
        }
        CHECK(cur == (closed ? fit.start : pts.back()));
    }
}

TEST_CASE("fitArcs is deterministic and keeps the first point of a closed polyline", "[geom2d][fit]")
{
    std::vector<Pt> pts = polygonize(Path::circle({0, 0}, 10 * kMm, true), 200);
    std::rotate(pts.begin(), pts.begin() + 7, pts.end());
    const Path a = fitArcs(pts, true);
    const Path b = fitArcs(pts, true);
    CHECK(a == b);
    CHECK(a.start == pts[0]);
    CHECK(a.closed);
    REQUIRE(a.spans.size() >= 2);
    // Arcs of at most 180 degrees; what is left at the end may be a chord (within the sag of the circle).
    int lines = 0;
    for (const Span& s : a.spans) {
        if (s.kind == SpanKind::Line) {
            ++lines;
            continue;
        }
        CHECK(s.kind == SpanKind::ArcCCW);
        CHECK(std::abs(double(s.radius - 10 * kMm)) <= 2.0);
    }
    CHECK(lines <= 1);
    CHECK(oneWay(a, Path::circle({0, 0}, 10 * kMm, true)) <= 201.0);
}

} // namespace ray::cam::geom2d::test
