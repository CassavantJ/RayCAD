// Linking limits (shared-code fixes after T-056), each on seeded cases against independent arithmetic:
//   1. a rotatable pass starts on a vertex or at least geom2d::kMinArcChord (lines: kMinLinkMove) from the ends of the
//      span it splits (link.h Pass::rotatable);
//   2. no link, retract or approach move is shorter than kMinLinkMove, with radial leads whose rounded ends at a hole's
//      centre are a nanometre or two apart (link.h SHORT MOVES);
//   3. a tool with a pointed or round end (Options::tipHeight) never meets material outside its cuts: replayed on an
//      independent height field that knows the cone (link.h THE TOOL'S END), and the same cases with the tool taken as
//      a cylinder do meet it (the fault the rule removes).
#include "link_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ray::cam::link::test {
namespace {

Linked run(const std::vector<Pass>& passes, const Options& o, const Stock& s)
{
    JobContext ctx;
    return take(linkPasses(passes, o, s, ctx));
}

double dist3(const Move& a, const Move& b)
{
    return std::hypot(std::hypot(double(b.x - a.x), double(b.y - a.y)), double(b.z - a.z));
}
double distXY2(Pt a, Pt b)
{
    return std::hypot(double(b.x - a.x), double(b.y - a.y));
}
Pt onCircleAt(Pt c, double r, double a)
{
    return Pt{c.x + static_cast<Coord>(std::llround(r * std::cos(a))),
              c.y + static_cast<Coord>(std::llround(r * std::sin(a)))};
}
// Counter-clockwise rectangle with corner radius r, starting at (x0, y0 + r).
Path roundedRect(Coord x0, Coord y0, Coord x1, Coord y1, Coord r)
{
    return closedPath({x0, y0 + r}, {arc({x0 + r, y0}, {x0 + r, y0 + r}, r, true), line({x1 - r, y0}),
                                     arc({x1, y0 + r}, {x1 - r, y0 + r}, r, true), line({x1, y1 - r}),
                                     arc({x1 - r, y1}, {x1 - r, y1 - r}, r, true), line({x0 + r, y1}),
                                     arc({x0, y1 - r}, {x0 + r, y1 - r}, r, true), line({x0, y0 + r})});
}

} // namespace

TEST_CASE("link: a rotatable pass starts on a vertex or keeps kMinArcChord from it (1,000 seeded cases)",
          "[link][order][limits]")
{
    int snapped = 0, inside = 0;
    for (std::uint64_t n = 0; n < 1'000; ++n) {
        const std::uint64_t seed = 9'100'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // A ring: a rounded rectangle (lines and arcs) or a circle, every span far longer than the limits.
        const Coord x0 = rng.between(-50'000'000, 50'000'000), y0 = rng.between(-50'000'000, 50'000'000);
        Path ring;
        if (rng.chance(0.7)) {
            const Coord w = rng.between(2'000'000, 40'000'000), h = rng.between(2'000'000, 40'000'000);
            const Coord r = rng.between(50'000, std::min(w, h) / 2 - 100'000);
            ring = roundedRect(x0, y0, x0 + w, y0 + h, r);
        } else {
            ring = Path::circle({x0, y0}, rng.between(100'000, 20'000'000), true);
        }
        // The tool comes from outside the ring, from beside one of its vertices: its nearest point on the ring lies
        // within +-30 um of that vertex along the ring.
        std::vector<Pt> vertices{ring.start};
        for (const Span& s : ring.spans)
            vertices.push_back(s.end);
        vertices.pop_back(); // the last end is the start
        const std::size_t vi = static_cast<std::size_t>(rng.next() % vertices.size());
        const Pt v = vertices[vi];
        const geom2d::Span& after = ring.spans[vi];
        Vec2 t; // the direction of travel at v
        if (after.kind == SpanKind::Line) {
            const double len = distXY2(v, after.end);
            t = Vec2{double(after.end.x - v.x) / len, double(after.end.y - v.y) / len};
        } else {
            const double rx = double(v.x - after.centre.x), ry = double(v.y - after.centre.y), rl = std::hypot(rx, ry);
            t = Vec2{-ry / rl, rx / rl}; // counter-clockwise
        }
        const double along = rng.between(-30'000.0, 30'000.0), out = rng.between(1e6, 4e6);
        // Outside a counter-clockwise ring is on the right of travel.
        const Pt from{v.x + static_cast<Coord>(std::llround(along * t.x + out * t.y)),
                      v.y + static_cast<Coord>(std::llround(along * t.y - out * t.x))};
        const Pt far{from.x + static_cast<Coord>(std::llround(3e6 * t.y)),
                     from.y + static_cast<Coord>(std::llround(-3e6 * t.x))};
        Pass approach = passFromPath(openPath(far, {line(from)}), -kMm, kCutFeed);
        approach.sequence = 0;
        Pass rotating = passFromPath(ring, -kMm, kCutFeed);
        rotating.rotatable = true;
        rotating.sequence = 1;
        Options o = baseOptions();
        o.entryOrder = {EntryKind::Plunge};
        o.centreCutting = true;
        const Linked l = run({approach, rotating}, o, Stock{});
        REQUIRE(l.order.size() == 2);
        REQUIRE(l.order[1].pass == 1);
        // The ring's moves, from its plunge.
        const std::size_t k0 = l.passMove[1];
        Move prev = l.moves[k0 - 1];
        const Pt start{prev.x, prev.y};
        const bool onVertex = std::find(vertices.begin(), vertices.end(), start) != vertices.end();
        snapped += onVertex ? 1 : 0;
        inside += onVertex ? 0 : 1;
        INFO("start " << start.x << " " << start.y << " at s " << l.order[1].startS << (onVertex ? " (a vertex)" : ""));
        std::size_t cuts = 0;
        for (std::size_t k = k0; k < l.moves.size() && l.moves[k].role == MoveRole::Cut; ++k) {
            const Move& m = l.moves[k];
            const double chord = distXY2(Pt{prev.x, prev.y}, Pt{m.x, m.y});
            INFO("move " << k << " chord " << chord);
            if (m.kind == MoveKind::Linear)
                CHECK(chord >= double(kMinLinkMove));
            else
                CHECK(chord >= double(geom2d::kMinArcChord) - 1.0); // the split point is rounded
            prev = m;
            ++cuts;
        }
        CHECK(cuts >= ring.spans.size());
        CHECK(Pt{prev.x, prev.y} == start);
    }
    // Both ways are exercised: starts moved onto a vertex and starts kept inside a span.
    CHECK(snapped > 200);
    CHECK(inside > 200);
}

TEST_CASE("link: no link move shorter than kMinLinkMove between radial leads at a hole's centre (500 seeded cases)",
          "[link][limits]")
{
    int apart = 0;
    for (std::uint64_t n = 0; n < 500; ++n) {
        const std::uint64_t seed = 9'200'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // Thread-milling-like passes in a drilled hole: whole circles of radius R about c starting at a random angle
        // (the start rounded, so the radial leads' ends at the centre are rounded points too), at random heights in a
        // random order (links go up and down), each with a half-circle lead of radius R / 2 towards the centre.
        const Pt c{rng.between(10'000'000, 40'000'000), rng.between(10'000'000, 40'000'000)};
        const Coord R = rng.between(1'000'000, 6'000'000);
        const double a = rng.between(-kPiD, kPiD);
        const Pt s = onCircleAt(c, double(R), a);
        const bool ccw = rng.chance(0.5);
        // Each pass: a whole turn from s and an overlap on to e (as a multi-form thread milling step).
        const double overlap = rng.between(0.1, 1.5);
        const Pt e = onCircleAt(c, double(R), a + (ccw ? overlap : -overlap));
        const Coord toolR = R / 2;
        std::vector<Pass> passes;
        const int count = static_cast<int>(rng.between(2, 5));
        for (int k = 0; k < count; ++k) {
            Pass p = passFromPath(openPath(s, {arc(s, c, R, ccw), arc(e, c, R, ccw)}),
                                  -rng.between(2'000'000, 12'000'000), kCutFeed);
            Lead lead;
            lead.kind = LeadKind::Arc;
            lead.radius = R / 2;
            lead.minRadius = lead.radius;
            lead.sweepDeg = 180;
            lead.side = ccw ? Side::Left : Side::Right; // towards the centre
            p.leadIn = lead;
            p.leadOut = lead;
            p.room = regionOf(Path::circle(c, R, true));
            passes.push_back(std::move(p));
        }
        Stock stock = boxStock(0, 0, 50 * kMm, 50 * kMm, 0);
        stock.cleared.push_back(Cleared{-20 * kMm, regionOf(Path::circle(c, R + toolR + 10'000, true))});
        Options o = baseOptions(2 * toolR);
        o.entryOrder = {EntryKind::Plunge};
        o.centreCutting = true;
        o.keepOrder = true;
        o.maxStayDown = rng.chance(0.7) ? geom2d::kMaxCoord : 0; // stay down, or retract every time
        o.rapidRetract = rng.chance(0.3);
        const Linked l = run(passes, o, stock);
        INFO(toText(l));
        for (std::size_t k = 1; k < l.moves.size(); ++k) {
            const Move& m = l.moves[k];
            const bool linking =
                m.role == MoveRole::Link || m.role == MoveRole::Retract || m.role == MoveRole::Approach;
            if (linking && (m.kind == MoveKind::Linear || m.kind == MoveKind::Rapid)) {
                INFO("move " << k);
                CHECK(dist3(l.moves[k - 1], m) >= double(kMinLinkMove));
            }
            if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
                // Every arc still starts and ends on its circle (nothing was merged into an arc).
                const Move& p = l.moves[k - 1];
                const double r0 = std::hypot(double(p.x - m.i), double(p.y - m.j));
                const double r1 = std::hypot(double(m.x - m.i), double(m.y - m.j));
                INFO("arc " << k);
                CHECK(std::abs(r0 - r1) <= 2.0);
            }
        }
        // How often a lead-out's end and the next lead-in's start are apart at all (the case the rule is for): the
        // leads as linking makes them (leadPath), at the pass's start and end in its direction of travel.
        const auto tangent = [&](Pt q) {
            const double rx = double(q.x - c.x), ry = double(q.y - c.y);
            return ccw ? Vec2{-ry, rx} : Vec2{ry, -rx};
        };
        const Path in = leadPath(s, tangent(s), passes[0].leadIn, passes[0].leadIn.radius, true);
        const Path out = leadPath(e, tangent(e), passes[0].leadOut, passes[0].leadOut.radius, false);
        apart += in.start != out.spans.back().end ? 1 : 0;
    }
    CHECK(apart > 20);
}

namespace {

// ---------- An independent height field that knows the tool's cone ----------
// The tool's lower surface at distance d from its axis lies tipHeight * d / R above its tip (a cone of half angle
// atan(R / tipHeight) reaching the radius R at tipHeight; tipHeight 0: flat). Cutting moves lower every cell within R
// of their path to that surface; every other move must not meet a cell standing above it (by more than 1 nm, and
// ignoring the last `margin` of the radius, linking's kFreeTolerance graze). Plain double arithmetic, nothing of
// geom2d.
class ConeField {
public:
    ConeField(Coord x0, Coord y0, Coord x1, Coord y1, Nm top, Coord cell)
        : x0_(double(x0)), y0_(double(y0)), cell_(double(cell)), nx_(int((x1 - x0) / cell)), ny_(int((y1 - y0) / cell))
    {
        top_.assign(std::size_t(nx_) * std::size_t(ny_), double(top));
    }
    // The first non-cutting move that meets material, or -1.
    long firstHit(const Linked& l, Coord radius, Nm tipHeight, double margin = 1'500)
    {
        const double R = double(radius), slope = R > 0 ? double(tipHeight) / R : 0;
        double cx = double(l.start.x), cy = double(l.start.y), cz = double(l.start.z);
        for (std::size_t k = 0; k < l.moves.size(); ++k) {
            const Move& m = l.moves[k];
            const bool cuts = m.kind != MoveKind::Rapid &&
                              (m.role == MoveRole::Cut || m.role == MoveRole::Plunge || m.role == MoveRole::Ramp ||
                               m.role == MoveRole::LeadIn || m.role == MoveRole::LeadOut);
            const double ex = double(m.x), ey = double(m.y), ez = double(m.z);
            const double len = std::hypot(std::hypot(ex - cx, ey - cy), ez - cz);
            const int steps = std::max(1, int(std::ceil(len / (cell_ / 2))));
            for (int i = 0; i <= steps; ++i) {
                const double f = double(i) / steps;
                const double px = cx + (ex - cx) * f, py = cy + (ey - cy) * f, pz = cz + (ez - cz) * f;
                bool hit = false;
                forCells(px, py, R, [&](double d, double& t) {
                    const double surface = pz + slope * d;
                    if (cuts)
                        t = std::min(t, surface);
                    else if (d <= R - margin && t > surface + 1.0)
                        hit = true;
                });
                if (hit)
                    return long(k);
            }
            cx = ex;
            cy = ey;
            cz = ez;
        }
        return -1;
    }

private:
    template <class F>
    void forCells(double px, double py, double R, F f)
    {
        const int i0 = std::max(0, int(std::floor((px - R - x0_) / cell_)));
        const int i1 = std::min(nx_ - 1, int(std::ceil((px + R - x0_) / cell_)));
        const int j0 = std::max(0, int(std::floor((py - R - y0_) / cell_)));
        const int j1 = std::min(ny_ - 1, int(std::ceil((py + R - y0_) / cell_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double x = x0_ + (i + 0.5) * cell_, y = y0_ + (j + 0.5) * cell_;
                const double d = std::hypot(x - px, y - py);
                if (d <= R)
                    f(d, top_[std::size_t(j) * std::size_t(nx_) + std::size_t(i)]);
            }
    }
    double x0_, y0_, cell_;
    int nx_, ny_;
    std::vector<double> top_;
};

} // namespace

TEST_CASE("link: a pointed tool never meets material outside its cuts, with the feed plane below the stock top (150 "
          "seeded cases)",
          "[link][limits]")
{
    int cylinderHits = 0;
    for (std::uint64_t n = 0; n < 150; ++n) {
        const std::uint64_t seed = 9'300'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // Grooves engraved with a V tool: parallel and crossing lines close together (so the area they sweep with the
        // tool's full radius holds whole discs off their centre lines), at random depths, in random order; links stay
        // down where they may; the feed plane below the stock's top, so rapids may go down into cut area.
        const Coord R = rng.between(1'000'000, 3'000'000);
        const Nm tip = static_cast<Nm>(double(R) / std::tan(rng.between(30.0, 60.0) * kPiD / 180.0)); // half angle
        std::vector<Pass> passes;
        const int count = static_cast<int>(rng.between(3, 8));
        for (int k = 0; k < count; ++k) {
            const Nm z = -rng.between(100'000, 800'000);
            const Coord gap = rng.between(R / 4, R);
            Pt a, b;
            if (rng.chance(0.6)) { // across X, stacked in Y
                const Coord y = 20 * kMm + (k - count / 2) * gap;
                a = {rng.between(8 * kMm, 14 * kMm), y};
                b = {rng.between(26 * kMm, 32 * kMm), y};
            } else { // across Y, stacked in X
                const Coord x = 20 * kMm + (k - count / 2) * gap;
                a = {x, rng.between(8 * kMm, 14 * kMm)};
                b = {x, rng.between(26 * kMm, 32 * kMm)};
            }
            Pass p = passFromPath(openPath(a, {line(b)}), z, kCutFeed);
            p.reversible = rng.chance(0.5);
            passes.push_back(std::move(p));
        }
        const Stock stock = boxStock(0, 0, 40 * kMm, 40 * kMm, 0);
        Options o = baseOptions(2 * R);
        o.feedPlane = -rng.between(0, 50'000);
        o.retract = 5 * kMm;
        o.entryOrder = {EntryKind::Plunge};
        o.centreCutting = true;
        o.maxStayDown = rng.chance(0.5) ? 30 * kMm : 0;
        o.tipHeight = tip;
        const Linked l = run(passes, o, stock);
        ConeField field(0, 0, 40 * kMm, 40 * kMm, 0, 150'000);
        const long hit = field.firstHit(l, R, tip);
        INFO("R " << R << " tip height " << tip << "\n" << toText(l));
        CHECK(hit < 0);
        // The same passes linked with the tool taken as a cylinder.
        o.tipHeight = 0;
        const Linked c = run(passes, o, stock);
        ConeField again(0, 0, 40 * kMm, 40 * kMm, 0, 150'000);
        cylinderHits += again.firstHit(c, R, tip) >= 0 ? 1 : 0;
    }
    // The fault the rule removes: as a cylinder, linking goes into the grooves' narrow part in many of these cases.
    CHECK(cylinderHits > 15);
}

namespace {

// The slope of a move from `from` (|dz| over its XY length; an arc's length at the smaller of its two radii).
double slopeOf(const Move& from, const Move& m)
{
    const double dz = std::abs(double(m.z - from.z));
    double len = 0;
    if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
        const double r0 = std::hypot(double(from.x - m.i), double(from.y - m.j));
        const double r1 = std::hypot(double(m.x - m.i), double(m.y - m.j));
        const double a0 = std::atan2(double(from.y - m.j), double(from.x - m.i));
        const double a1 = std::atan2(double(m.y - m.j), double(m.x - m.i));
        double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
        while (sweep <= 0)
            sweep += 2 * kPiD;
        if (m.x == from.x && m.y == from.y)
            sweep = 2 * kPiD;
        len = std::min(r0, r1) * sweep;
    } else {
        len = std::hypot(double(m.x - from.x), double(m.y - from.y));
    }
    return len > 0 ? dz / len : (dz > 0 ? std::numeric_limits<double>::infinity() : 0.0);
}

} // namespace

TEST_CASE("link: appendSloped with a slope never makes a piece steeper (2,000 seeded paths)", "[link][limits]")
{
    int refusedCases = 0, clamped = 0;
    for (std::uint64_t n = 0; n < 2'000; ++n) {
        const std::uint64_t seed = 9'400'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // A path of short and long pieces: lines of 0.1 um to 2 mm and arcs.
        Path p = openPath({rng.between(-10'000'000, 10'000'000), rng.between(-10'000'000, 10'000'000)}, {});
        Pt at = p.start;
        const int pieces = static_cast<int>(rng.between(1, 120));
        for (int k = 0; k < pieces; ++k) {
            const double a = rng.between(-kPiD, kPiD);
            const double len = rng.chance(0.5) ? rng.between(100.0, 20'000.0) : rng.between(20'000.0, 2'000'000.0);
            const Pt next{at.x + static_cast<Coord>(std::llround(len * std::cos(a))),
                          at.y + static_cast<Coord>(std::llround(len * std::sin(a)))};
            if (next == at)
                continue;
            if (rng.chance(0.3) && len > 50'000) {
                // A half circle on the segment as diameter.
                const Pt c{(at.x + next.x) / 2, (at.y + next.y) / 2};
                const Pt q = onCircleAt(c, std::hypot(double(at.x - c.x), double(at.y - c.y)),
                                        std::atan2(double(at.y - c.y), double(at.x - c.x)) + kPiD / 2);
                p.spans.push_back(arc(q, c, Coord(std::llround(distXY2(at, c))), true));
                at = q;
            } else {
                p.spans.push_back(line(next));
                at = next;
            }
        }
        if (p.spans.empty())
            continue;
        const double slope = std::tan(rng.between(0.3, 30.0) * kPiD / 180.0);
        const double length = geom2d::length(p);
        // Mostly just long enough (as linking makes ramps: 1e-4 longer than the angle needs), sometimes too short.
        const double need = rng.chance(0.85) ? length * slope / (1 + 1e-4) : length * slope * rng.between(1.0, 1.2);
        const Nm z0 = rng.between(-2'000'000, 2'000'000);
        const Nm z1 = z0 + (rng.chance(0.5) ? -1 : 1) * static_cast<Nm>(need);
        std::vector<Move> out;
        if (!appendSloped(p, z0, z1, slope, MoveRole::Ramp, kCutFeed, out)) {
            CHECK(out.empty());
            ++refusedCases;
            continue;
        }
        std::vector<Move> plain;
        appendSloped(p, z0, z1, MoveRole::Ramp, kCutFeed, plain);
        REQUIRE(out.size() == plain.size());
        Move from{};
        from.x = p.start.x;
        from.y = p.start.y;
        from.z = z0;
        for (std::size_t k = 0; k < out.size(); ++k) {
            INFO("piece " << k);
            CHECK(slopeOf(from, out[k]) <= slope);
            CHECK(out[k].x == plain[k].x);
            CHECK(out[k].y == plain[k].y);
            clamped += out[k].z != plain[k].z ? 1 : 0;
            from = out[k];
        }
        CHECK(out.back().z == z1);
    }
    CHECK(refusedCases > 50); // the too-short paths are refused, not made steeper
    CHECK(clamped > 50);      // and the rounding rule is exercised
}

TEST_CASE("link: ramps and helices never descend steeper than the ramp angle (300 seeded passes)", "[link][limits]")
{
    for (std::uint64_t n = 0; n < 300; ++n) {
        const std::uint64_t seed = 9'500'000 + n;
        Rng rng{seed};
        INFO("seed " << seed);
        // A ring of many short lines (a pocket ring after fitting gaps), entered by a helix or a ramp along it.
        const Pt c{20 * kMm, 20 * kMm};
        const double radius = rng.between(3e6, 12e6);
        const int count = static_cast<int>(rng.between(40, 400));
        Path ring = closedPath(onCircleAt(c, radius, 0), {});
        for (int k = 1; k <= count; ++k)
            ring.spans.push_back(line(k == count ? ring.start : onCircleAt(c, radius, 2 * kPiD * k / count)));
        Pass pass = passFromPath(ring, -rng.between(500'000, 4'000'000), kCutFeed);
        pass.room = unbounded();
        Options o = baseOptions(2 * rng.between(500'000, 2'000'000));
        o.rampAngleDeg = rng.between(0.5, 12.0);
        o.helixDiameter = static_cast<Coord>(radius);
        o.helixMinDiameter = o.helixDiameter / 2;
        o.minRampRun = 1'000'000;
        o.entryOrder =
            rng.chance(0.5) ? std::vector<EntryKind>{EntryKind::Helix} : std::vector<EntryKind>{EntryKind::Ramp};
        const Linked l = run({pass}, o, boxStock(0, 0, 40 * kMm, 40 * kMm, 0));
        const double tanA = std::tan(o.rampAngleDeg * kPiD / 180.0);
        int ramps = 0;
        for (std::size_t k = 1; k < l.moves.size(); ++k)
            if (l.moves[k].role == MoveRole::Ramp && l.moves[k].z != l.moves[k - 1].z) {
                INFO("move " << k);
                CHECK(std::atan(slopeOf(l.moves[k - 1], l.moves[k])) <= std::atan(tanA));
                ++ramps;
            }
        CHECK(ramps > 0);
    }
}

TEST_CASE("link: the tool's tip height must not be negative", "[link][limits]")
{
    Options o = baseOptions();
    o.tipHeight = -1;
    JobContext ctx;
    const std::vector<Pass> passes{passFromPath(openPath({0, 0}, {line({10 * kMm, 0})}), -kMm, kCutFeed)};
    auto r = linkPasses(passes, o, Stock{}, ctx);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::InvalidArgument);
}

} // namespace ray::cam::link::test
