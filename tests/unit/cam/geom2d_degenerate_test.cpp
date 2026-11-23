// geom2d on degenerate and near-degenerate input (shared-code fixes after T-056): sweeps of circles and arcs whose
// radius equals the disc's or differs from it by a few nanometres (their inner offset is their centre, or a circle of a
// few nanometres), together with leads and lines that end within a few nanometres of that centre, so that several
// exact curves touch tangentially at one point. The rebuild after Clipper2 must not turn a micrometre piece between two
// ill-conditioned corners into a whole circle (geom2d_clip.cpp "DEGENERATE CORNERS"). Checked against an independent
// distance (geom2d_test_util.h, its own arithmetic): points clearly within the disc's radius of a path are inside the
// swept area, points clearly farther are outside, and where every path lies inside the circle the area is the
// analytic one.
#include "geom2d_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace ray::cam::geom2d::test {
namespace {

// The thread milling case T-056 found: a whole circle of the disc's radius (two half arcs, open) and a radial lead-out
// (half a circle of half that radius) from 15 degrees on it back to 1 nm from its centre.
struct ThreadCase {
    Path full, out;
};
ThreadCase threadCase()
{
    const Pt c{40'000'000, 20'000'000};
    ThreadCase t;
    t.full = openPath({43'000'000, 20'000'000}, {arc({37'000'000, 20'000'000}, c, 3'000'000, true),
                                                 arc({43'000'000, 20'000'000}, c, 3'000'000, true)});
    t.out =
        openPath({42'897'777, 20'776'457}, {arc({39'999'999, 19'999'999}, {41'448'888, 20'388'228}, 1'500'000, true)});
    return t;
}

Pt onCircleAt(Pt c, double r, double a)
{
    return Pt{c.x + static_cast<Coord>(std::llround(r * std::cos(a))),
              c.y + static_cast<Coord>(std::llround(r * std::sin(a)))};
}

double minDistance(P2 p, const std::vector<Path>& paths)
{
    double d = std::numeric_limits<double>::infinity();
    for (const Path& path : paths)
        d = std::min(d, pathDistance(p, path));
    return d;
}

} // namespace

TEST_CASE("sweptArea of a circle of the disc's radius with a lead-out ending 1 nm from its centre",
          "[geom2d][offset][degenerate]")
{
    // Found by T-056 (thread milling): 176.7 mm2 (a whole 4.5 mm circle too many) instead of the 6 mm disc's 113.1.
    JobContext ctx;
    const ThreadCase t = threadCase();
    const double disc = 36 * kPiD; // mm2
    const Regions alone = take(sweptArea(std::vector<Path>{t.full}, 3'000'000, ctx));
    CHECK(std::abs(area(alone) / 1e12 - disc) < 1e-3);
    const Regions both = take(sweptArea(std::vector<Path>{t.full, t.out}, 3'000'000, ctx));
    INFO(toText(both));
    CHECK(std::abs(area(both) / 1e12 - disc) < 1e-3);
    REQUIRE(both.size() == 1);
    CHECK(both[0].holes.empty());
    // The union of the two sweeps made apart is the same disc, and so is the union with the disc itself.
    const Regions lead = take(sweptArea(std::vector<Path>{t.out}, 3'000'000, ctx));
    const Regions united = take(unite(alone, lead, ctx));
    CHECK(std::abs(area(united) / 1e12 - disc) < 1e-3);
    const Regions again = take(unite(both, alone, ctx));
    CHECK(std::abs(area(again) / 1e12 - disc) < 1e-3);
    // Nothing of it reaches outside the 6 mm circle, and its centre is inside.
    CHECK(locate(Pt{40'000'000, 20'000'000}, both) == Where::Inside);
    CHECK(locate(Pt{46'000'100, 20'000'000}, both) == Where::Outside);
    CHECK(locate(Pt{45'990'000, 20'000'000}, both) == Where::Inside);
}

TEST_CASE("sweeps of circles and arcs of about the disc's radius with paths ending near their centre (2,000 seeded "
          "cases)",
          "[geom2d][offset][degenerate]")
{
    JobContext ctx;
    int checkedPoints = 0, areaCases = 0;
    for (std::uint64_t n = 0; n < 2'000; ++n) {
        const std::uint64_t seed = 7'000'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        const Coord r = rng.between(200'000, 20'000'000);               // the disc: 0.2 .. 20 mm
        const Coord R = std::max<Coord>(2'000, r + rng.between(-3, 3)); // the circle: within 3 nm of it
        const Pt c{rng.between(-100'000'000, 100'000'000), rng.between(-100'000'000, 100'000'000)};
        const double phi = rng.between(-kPiD, kPiD);
        const Pt q = onCircleAt(c, static_cast<double>(R), phi);
        const Pt qa = onCircleAt(c, static_cast<double>(R), phi + kPiD);
        const bool ccw = (rng.next() & 1) != 0;
        std::vector<Path> paths;
        // The whole circle is swept and every path lies inside it, so the area is the analytic one.
        bool inside = true;
        // The circle, or part of it.
        switch (rng.next() % 4) {
        case 0:
            paths.push_back(Path::circle(c, R, ccw));
            break;
        case 1: // two half arcs from q, open (as a thread milling pass is made)
            paths.push_back(openPath(q, {arc(qa, c, R, ccw), arc(q, c, R, ccw)}));
            break;
        case 2: // the same, closed
            paths.push_back(closedPath(q, {arc(qa, c, R, ccw), arc(q, c, R, ccw)}));
            break;
        default: {          // an arc of the circle from q
            inside = false; // (the area is not the whole disc's)
            const double sweep = rng.between(0.3, 6.0);
            paths.push_back(
                openPath(q, {arc(onCircleAt(c, static_cast<double>(R), phi + (ccw ? sweep : -sweep)), c, R, ccw)}));
            break;
        }
        }
        // One or two paths ending (or starting) within a few nanometres of the centre.
        const int leads = 1 + static_cast<int>(rng.next() % 2);
        for (int k = 0; k < leads; ++k) {
            const Pt near{c.x + rng.between(-3, 3), c.y + rng.between(-3, 3)};
            const double a = k == 0 ? phi : rng.between(-kPiD, kPiD);
            const Pt from = onCircleAt(c, static_cast<double>(R), a);
            switch (rng.next() % 4) {
            case 0:
            case 1: { // a radial half-circle lead of half the radius, out (from the circle to the centre) or in
                const Pt mid{(from.x + c.x) / 2, (from.y + c.y) / 2};
                const Coord lr =
                    static_cast<Coord>(std::llround(std::hypot(double(from.x - mid.x), double(from.y - mid.y))));
                const bool lccw = (rng.next() & 1) != 0;
                if (rng.next() % 2 == 0)
                    paths.push_back(openPath(from, {arc(near, mid, lr, lccw)}));
                else
                    paths.push_back(openPath(near, {arc(from, mid, lr, !lccw)}));
                break;
            }
            case 2: // a straight radius
                paths.push_back(openPath(from, {line(near)}));
                break;
            default: { // a line from outside the circle to the centre
                const Pt far = onCircleAt(c, static_cast<double>(R) * rng.between(1.2, 3.0), rng.between(-kPiD, kPiD));
                paths.push_back(openPath(far, {line(near)}));
                inside = false;
                break;
            }
            }
        }
        const Regions swept = take(sweptArea(paths, r, ctx));
        std::string text;
        for (const Path& p : paths)
            text += toText(p);
        INFO("r " << r << " R " << R << "\npaths:\n" << text << "swept:\n" << toText(swept));
        REQUIRE_FALSE(swept.empty());
        if (inside) {
            // The disc of radius R + r with, at most, a hole of a few nanometres.
            const double expected = kPiD * double(R + r) * double(R + r);
            CHECK(std::abs(area(swept) - expected) <= 2 * kPiD * double(R + r) * 20.0);
            ++areaCases;
        }
        // Points clearly inside or outside by the independent distance.
        const double margin = 1'000.0;
        const Coord reach = 3 * (R + r);
        std::vector<Pt> probes;
        for (int k = 0; k < 120; ++k)
            probes.push_back({c.x + rng.between(-reach, reach), c.y + rng.between(-reach, reach)});
        // Around the centre, and around the point where every curve touches the outer circle.
        for (int k = 0; k < 20; ++k) {
            probes.push_back({c.x + rng.between(-20'000, 20'000), c.y + rng.between(-20'000, 20'000)});
            const double t = phi + rng.between(-0.002, 0.002);
            probes.push_back(onCircleAt(c, double(R + r) + rng.between(-5'000.0, 5'000.0), t));
        }
        for (const Pt& p : probes) {
            const double d = minDistance(p2(p), paths);
            if (std::abs(d - double(r)) < margin)
                continue;
            const Where w = locate(p, swept);
            INFO("probe " << p.x << " " << p.y << " at " << d << " nm from the paths");
            CHECK(w == (d < double(r) ? Where::Inside : Where::Outside));
            ++checkedPoints;
        }
    }
    CHECK(areaCases > 800);
    CHECK(checkedPoints > 200'000);
}

TEST_CASE("fitArcs: both ends of every arc on its circle as the toolpath IR wants (2,000 seeded polylines)",
          "[geom2d][fit][degenerate]")
{
    // Polylines that are not samples of integer arcs (ellipses, spirals, noisy arcs), so the best circle of a run
    // generally misses its ends by up to the tolerance: every arc that comes back has both ends within 1.25 nm of its
    // circle and their distances within 1.5 nm of each other (the IR's invariant checker allows 2 nm), and the fit is
    // still within the tolerance of the polyline both ways.
    int arcs = 0;
    for (std::uint64_t n = 0; n < 2'000; ++n) {
        const std::uint64_t seed = 8'000'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        const double cx = rng.between(-1e8, 1e8), cy = rng.between(-1e8, 1e8);
        const double a = rng.between(1e6, 6e7), b = a * rng.between(0.3, 1.0), tilt = rng.between(0.0, kPiD);
        const double t0 = rng.between(-kPiD, kPiD), span = rng.between(0.3, 2 * kPiD);
        const double grow = rng.between(0.0, 0.3);    // a spiral when > 0
        const double noise = rng.between(0.0, 300.0); // nm
        const int count = static_cast<int>(rng.between(Coord{8}, Coord{160}));
        std::vector<Pt> pts;
        for (int k = 0; k <= count; ++k) {
            const double t = t0 + span * k / count;
            const double s = 1.0 + grow * (t - t0) / (2 * kPiD);
            const double ex = a * s * std::cos(t), ey = b * s * std::sin(t);
            pts.push_back({static_cast<Coord>(std::llround(cx + ex * std::cos(tilt) - ey * std::sin(tilt) +
                                                           rng.between(-noise, noise))),
                           static_cast<Coord>(std::llround(cy + ex * std::sin(tilt) + ey * std::cos(tilt) +
                                                           rng.between(-noise, noise)))});
        }
        FitOptions opt;
        opt.tolerance = rng.between(Coord{200}, Coord{20'000});
        const bool closed = n % 7 == 0 && span > 6.0;
        const Path fit = fitArcs(pts, closed, opt);
        INFO("tolerance " << opt.tolerance << "\n" << toText(fit));
        Pt cur = fit.start;
        for (const Span& s : fit.spans) {
            if (s.kind != SpanKind::Line) {
                ++arcs;
                const double r = static_cast<double>(s.radius);
                const double d0 = dist(p2(cur), p2(s.centre)), d1 = dist(p2(s.end), p2(s.centre));
                CHECK(std::abs(d0 - r) <= 1.25);
                CHECK(std::abs(d1 - r) <= 1.25);
                CHECK(std::abs(d0 - d1) <= 1.5);
            }
            cur = s.end;
        }
        Path poly;
        poly.start = pts[0];
        poly.closed = closed;
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (pts[i] != pts[i - 1])
                poly.spans.push_back(line(pts[i]));
        if (closed && poly.spans.back().end != poly.start)
            poly.spans.push_back(line(poly.start));
        CHECK(oneWay(fit, poly, 16) <= double(opt.tolerance) + 1.5);
        CHECK(oneWay(poly, fit, 2) <= double(opt.tolerance) + 1.5);
    }
    CHECK(arcs > 2'000);
}

} // namespace ray::cam::geom2d::test
