// Stock model volumes against analytic values (task T-070): every case within ONE CELL LAYER, i.e. the cell size times
// the area of the cut's surface, measured on all three grids and on the removed volume cutBatch returns.
#include "tridexel_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <sstream>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

// The stock of these cases: 100 x 80 x 30 mm with its top at z = 0, placed off the origin and off any grid.
const Vec3 kMin{-10.3, 5.7, -30.0};
const Vec3 kMax{89.7, 85.7, 0.0};

struct Case {
    std::string name;
    double exact = 0;   // mm^3
    double surface = 0; // mm^2 of the cut's surface (walls and floor), for the one-cell-layer tolerance
};

// Cuts with `cut` on a fresh stock and checks the volume each grid lost and the returned removed volume.
void check(const Case& c, double cell, const std::function<double(IStockModel&)>& cut)
{
    auto m = blockStock(kMin, kMax, cell);
    const Volumes before = volumes(*m);
    const double returned = cut(*m);
    const Volumes after = volumes(*m);
    const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(*m);
    const double layer = std::max({in.cx, in.cy, in.cz}) * c.surface;
    INFO(c.name << " at cell " << cell << ": exact " << c.exact << " mm3, one cell layer " << layer << " mm3");
    const char* names[3] = {"Z", "X", "Y"};
    for (int g = 0; g < 3; ++g) {
        const double removed = before.v[g] - after.v[g];
        INFO(names[g] << " grid removed " << removed << " (" << 100.0 * (removed - c.exact) / c.exact << " %)");
        CHECK(std::abs(removed - c.exact) <= layer);
    }
    INFO("returned " << returned);
    CHECK(std::abs(returned - c.exact) <= layer);
    // The returned volume is the Z grid's own loss.
    CHECK(std::abs(returned - (before.v[0] - after.v[0])) <= 1e-6 * c.exact + 1e-6);
}

double cutOne(IStockModel& m, const ToolShape& t, Vec3 a, Vec3 b)
{
    return m.cut(t, a, b, false);
}

double profileVolume(const std::vector<Vec2>& p, double depth, double& sideIntegral)
{
    // Integral of pi r(h)^2 and of 2 r(h) over [0, depth] for a profile with heights not decreasing (midpoint rule,
    // fine steps; the radius at a level step is taken from above).
    const int n = 200000;
    double vol = 0;
    sideIntegral = 0;
    for (int i = 0; i < n; ++i) {
        const double h = (i + 0.5) * depth / n;
        double r = 0;
        for (std::size_t k = 0; k + 1 < p.size(); ++k) {
            if (h >= p[k].y && h <= p[k + 1].y && p[k + 1].y > p[k].y) {
                r = p[k].x + (p[k + 1].x - p[k].x) * (h - p[k].y) / (p[k + 1].y - p[k].y);
            }
        }
        vol += kPi * r * r * depth / n;
        sideIntegral += 2 * r * depth / n;
    }
    return vol;
}

} // namespace

TEST_CASE("Flat tools: plunge, straight and angled slots", "[sim][tridexel][volume]")
{
    const double cell = GENERATE(0.2, 0.1);
    const ToolShape t = flat(10.0);
    const double R = 5.0;
    {
        const double D = 12.0;
        check({"plunge, flat 10, 12 deep", kPi * R * R * D, 2 * kPi * R * D + kPi * R * R}, cell,
              [&](IStockModel& m) { return cutOne(m, t, {20.13, 30.77, 5.0}, {20.13, 30.77, -D}); });
    }
    {
        const double D = 6.0, L = 50.0;
        const Case c{"slot along X, flat 10, 6 deep", (2 * R * L + kPi * R * R) * D,
                     (2 * L + 2 * kPi * R) * D + 2 * R * L + kPi * R * R};
        check(c, cell, [&](IStockModel& m) { return cutOne(m, t, {12.31, 40.41, -D}, {12.31 + L, 40.41, -D}); });
        const double a = 15 * kPi / 180;
        check({"slot at 15 deg", c.exact, c.surface}, cell, [&](IStockModel& m) {
            return cutOne(m, t, {10.0, 20.0, -D}, {10.0 + L * std::cos(a), 20.0 + L * std::sin(a), -D});
        });
    }
    {
        // A slot along Y entering from outside the stock: only the part inside counts.
        const double D = 4.0;
        const double inside = 30.0; // from the stock's -Y face at 5.7 to y = 35.7
        check({"slot from outside, flat 10, 4 deep", (2 * R * inside + kPi * R * R / 2) * D,
               (2 * inside + kPi * R) * D + 2 * R * inside + kPi * R * R / 2},
              cell, [&](IStockModel& m) { return cutOne(m, t, {50.0, -20.0, -D}, {50.0, 35.7, -D}); });
    }
}

TEST_CASE("Arcs: XY slots, full circle, XZ and YZ semicircles", "[sim][tridexel][volume]")
{
    const double cell = GENERATE(0.2, 0.1);
    const ToolShape t = flat(6.0);
    const double r = 3.0, Ra = 20.0, D = 5.0;
    const Vec3 c{40.0, 45.0, -D};
    {
        const double th = 120 * kPi / 180;
        const Case k{"arc 120 deg ccw, flat 6", D * (2 * th * Ra * r + kPi * r * r),
                     D * (2 * th * Ra + 2 * kPi * r) + 2 * th * Ra * r + kPi * r * r};
        const Vec3 a{c.x + Ra, c.y, -D}, b{c.x + Ra * std::cos(th), c.y + Ra * std::sin(th), -D};
        check(k, cell, [&](IStockModel& m) { return m.cutArc(t, a, b, c, false, cam::ArcPlane::XY); });
        // The same arc the other way round is the other 240 degrees.
        const double th2 = 2 * kPi - th;
        const Case k2{"arc 240 deg cw", D * (2 * th2 * Ra * r + kPi * r * r),
                      D * (2 * th2 * Ra + 2 * kPi * r) + 2 * th2 * Ra * r + kPi * r * r};
        check(k2, cell, [&](IStockModel& m) { return m.cutArc(t, a, b, c, true, cam::ArcPlane::XY); });
        // A full circle (start == end): an annulus.
        const Case k3{"full circle", D * 4 * kPi * Ra * r, D * 4 * kPi * Ra + 4 * kPi * Ra * r};
        check(k3, cell, [&](IStockModel& m) { return m.cutArc(t, a, a, c, false, cam::ArcPlane::XY); });
        // Through cutBatch, mixed with lines.
        check(k, cell, [&](IStockModel& m) {
            IStockModel::CutMove mv{a, b, false};
            mv.kind = cam::MoveKind::ArcCCW;
            mv.centre = c;
            double v = 0;
            m.cutBatch(t, std::span<const IStockModel::CutMove>(&mv, 1), std::span<double>(&v, 1));
            return v;
        });
    }
    {
        // A lower semicircle of radius 8 under the stock top: XZ (G18, seen from +Y) and YZ (G19, seen from +X). The
        // flat tool's swept region holds pi r Ra (r + Ra); its surface is pi (Ra + r)^2.
        const double ra = 8.0;
        const Case k{"semicircle", kPi * r * ra * (r + ra), kPi * (ra + r) * (ra + r)};
        // XZ: (u, v) = (z, x). From x = cx - 8 to cx + 8 through z = -8: from (u, v) = (0, -8) to (0, 8) through
        // (-8, 0), clockwise.
        check({"XZ semicircle", k.exact, k.surface}, cell, [&](IStockModel& m) {
            return m.cutArc(t, {32.0, 40.0, 0.0}, {48.0, 40.0, 0.0}, {40.0, 40.0, 0.0}, true, cam::ArcPlane::XZ);
        });
        // The opposite turn runs above the stock: nothing.
        {
            auto m = blockStock(kMin, kMax, cell);
            CHECK(m->cutArc(t, {32.0, 40.0, 0.0}, {48.0, 40.0, 0.0}, {40.0, 40.0, 0.0}, false, cam::ArcPlane::XZ) ==
                  0.0);
        }
        // YZ: (u, v) = (y, z). From y = cy - 8 to cy + 8 through z = -8: from (-8, 0) to (8, 0) through (0, -8),
        // counter-clockwise.
        check({"YZ semicircle", k.exact, k.surface}, cell, [&](IStockModel& m) {
            return m.cutArc(t, {40.0, 37.0, 0.0}, {40.0, 53.0, 0.0}, {40.0, 45.0, 0.0}, false, cam::ArcPlane::YZ);
        });
    }
}

TEST_CASE("Helical arcs agree with an analytic reference and with fine lines", "[sim][tridexel][volume]")
{
    const double cell = 0.1;
    const ToolShape t = flat(6.0);
    const double r = 3.0;
    // A helix in XY: radius 2 (smaller than the tool, so no core is left), descending 2 mm per turn for 2.5 turns
    // from the stock top. Reference: per Z ray of a fine grid, the tool's lowest pass over it is the latest angle at
    // which the ray is within r of the centre path (the helix descends), in closed form.
    const double ra = 2.0, pitch = 2.0, turns = 2.5;
    const Vec3 c{40.0, 40.0, 0.0};
    const double total = 2 * kPi * turns;
    double reference = 0;
    {
        const double step = 0.01;
        for (double x = c.x - ra - r; x <= c.x + ra + r; x += step) {
            for (double y = c.y - ra - r; y <= c.y + ra + r; y += step) {
                const double px = x + step / 2 - c.x, py = y + step / 2 - c.y;
                const double rho = std::hypot(px, py);
                // |C(theta) - p| <= r  <=>  cos(theta - phi) >= kappa
                double best = -1;
                if (rho <= 1e-12) {
                    best = ra <= r ? total : -1;
                } else {
                    const double kappa = (ra * ra + rho * rho - r * r) / (2 * ra * rho);
                    if (kappa <= -1) {
                        best = total;
                    } else if (kappa <= 1) {
                        const double phi = std::atan2(py, px), w = std::acos(kappa);
                        // The largest theta <= total in [phi - w, phi + w] + 2 pi k.
                        for (int k = -1; k <= 4; ++k) {
                            const double lo = phi - w + 2 * kPi * k, hi = phi + w + 2 * kPi * k;
                            if (lo <= total && hi >= 0) {
                                best = std::max(best, std::min(hi, total));
                            }
                        }
                    }
                }
                if (best >= 0) {
                    reference += pitch * best / (2 * kPi) * step * step;
                }
            }
        }
    }
    const Vec3 a{c.x + ra, c.y, 0.0}, b{c.x + ra * std::cos(total), c.y + ra * std::sin(total), -pitch * turns};
    // One arc move per half turn (a control would split it so), in one batch.
    std::vector<IStockModel::CutMove> moves;
    for (int h = 0; h < 5; ++h) {
        const double t0 = kPi * h, t1 = kPi * (h + 1);
        IStockModel::CutMove mv{{c.x + ra * std::cos(t0), c.y + ra * std::sin(t0), -pitch * t0 / (2 * kPi)},
                                {c.x + ra * std::cos(t1), c.y + ra * std::sin(t1), -pitch * t1 / (2 * kPi)},
                                false};
        mv.kind = cam::MoveKind::ArcCCW;
        mv.centre = c;
        moves.push_back(mv);
    }
    (void)a;
    (void)b;
    // Surface of the cut: the floor (about the disc of radius ra + r) and the wall, about 2 pi (ra + r) deep 5 mm.
    const double surface = kPi * (ra + r) * (ra + r) + 2 * kPi * (ra + r) * pitch * turns;
    check({"helix in XY", reference, surface}, cell, [&](IStockModel& m) {
        std::vector<double> v(moves.size());
        m.cutBatch(t, moves, v);
        double s = 0;
        for (double x : v) {
            s += x;
        }
        return s;
    });
    // XZ and YZ helices (the tool travels on a vertical circle while moving along Y or X): the arc against the same
    // helix as 0.005 mm lines.
    for (cam::ArcPlane plane : {cam::ArcPlane::XZ, cam::ArcPlane::YZ}) {
        const bool xz = plane == cam::ArcPlane::XZ;
        const double rr = 6.0, adv = 10.0, sweep = 1.5 * kPi;
        // Centre 3 mm below the top; the helix advances `adv` along the plane's normal.
        const auto at = [&](double th) {
            // (u, v): XZ (z, x), YZ (y, z); angle from +u counter-clockwise.
            const double u = rr * std::cos(th), v = rr * std::sin(th), w = adv * th / sweep;
            return xz ? Vec3{40.0 + v, 30.0 + w, -3.0 + u} : Vec3{30.0 + w, 40.0 + u, -3.0 + v};
        };
        const double th0 = kPi; // start at the bottom of u: below the centre for XZ, at -y for YZ
        IStockModel::CutMove mv{at(th0), at(th0 + sweep), false};
        mv.kind = cam::MoveKind::ArcCCW;
        mv.centre = xz ? Vec3{40.0, 30.0, -3.0} : Vec3{30.0, 40.0, -3.0};
        mv.plane = plane;
        auto arcModel = blockStock(kMin, kMax, cell);
        double arcVol = 0;
        arcModel->cutBatch(t, std::span<const IStockModel::CutMove>(&mv, 1), std::span<double>(&arcVol, 1));
        std::vector<IStockModel::CutMove> lines;
        const int n = static_cast<int>(std::ceil(sweep * rr / 0.005));
        for (int i = 0; i < n; ++i) {
            lines.push_back({at(th0 + sweep * i / n), at(th0 + sweep * (i + 1) / n), false});
        }
        auto lineModel = blockStock(kMin, kMax, cell);
        std::vector<double> v(lines.size());
        lineModel->cutBatch(t, lines, v);
        double lineVol = 0;
        for (double x : v) {
            lineVol += x;
        }
        INFO((xz ? "XZ" : "YZ") << " helix: arc " << arcVol << " mm3, fine lines " << lineVol << " mm3");
        CHECK(arcVol > 100.0);
        // The chords' sagitta is a tenth of a cell: their sweep differs by at most that over the swept surface.
        CHECK(std::abs(arcVol - lineVol) <= 0.1 * cell * 2 * (sweep * rr + 2 * r) * (2 * r + 6.0));
    }
}

TEST_CASE("Ball, bull-nose, chamfer, drill and general revolved profiles", "[sim][tridexel][volume]")
{
    const double cell = GENERATE(0.2, 0.1);
    {
        const double R = 3.0, D = 10.0;
        const ToolShape t = ball(6.0);
        check(
            {"ball plunge", 2.0 / 3 * kPi * R * R * R + kPi * R * R * (D - R), 2 * kPi * R * R + 2 * kPi * R * (D - R)},
            cell, [&](IStockModel& m) { return cutOne(m, t, {30.4, 40.6, 2.0}, {30.4, 40.6, -D}); });
        const double d = 5.0, L = 40.0;
        const double groove =
            (kPi * R * R / 2 + 2 * R * (d - R)) * L + 2.0 / 3 * kPi * R * R * R + kPi * R * R * (d - R);
        check({"ball groove", groove, (kPi * R + 2 * (d - R)) * L + 2 * kPi * R * d}, cell,
              [&](IStockModel& m) { return cutOne(m, t, {15.1, 30.3, -d}, {15.1 + L, 30.3, -d}); });
        const double a = 15 * kPi / 180;
        check({"ball groove at 15 deg", groove, (kPi * R + 2 * (d - R)) * L + 2 * kPi * R * d}, cell,
              [&](IStockModel& m) {
                  return cutOne(m, t, {15.1, 30.3, -d}, {15.1 + L * std::cos(a), 30.3 + L * std::sin(a), -d});
              });
    }
    {
        const double R = 5.0, rc = 1.5, D = 8.0, aa = R - rc;
        const ToolShape t = bull(10.0, rc);
        const double plunge =
            kPi * (aa * aa * rc + kPi / 2 * aa * rc * rc + 2.0 / 3 * rc * rc * rc) + kPi * R * R * (D - rc);
        check({"bull-nose plunge", plunge, 2 * kPi * R * D + kPi * R * R}, cell,
              [&](IStockModel& m) { return cutOne(m, t, {50.2, 30.1, 1.0}, {50.2, 30.1, -D}); });
        const double d = 4.0, L = 30.0;
        const double section = 2 * R * d - 2 * rc * rc * (1 - kPi / 4);
        const double plungeD =
            kPi * (aa * aa * rc + kPi / 2 * aa * rc * rc + 2.0 / 3 * rc * rc * rc) + kPi * R * R * (d - rc);
        check({"bull-nose slot", section * L + plungeD, (2 * d + 2 * R) * L + 2 * kPi * R * d + kPi * R * R}, cell,
              [&](IStockModel& m) { return cutOne(m, t, {20.0, 50.0, -d}, {20.0 + L, 50.0, -d}); });
    }
    {
        // Chamfer mill, 90 degrees, tip 1 mm, diameter 12: a V groove 3 mm deep (the cone reaches 5.5 mm).
        const ToolShape t = pointed(lib::ToolType::ChamferMill, 12.0, 90.0, 1.0);
        const double rt = 0.5, d = 3.0, rd = rt + d, L = 40.0;
        const double v = d * (rt + rd) * L + kPi / 3 * d * (rd * rd + rd * rt + rt * rt);
        check({"chamfer V groove", v, (2 * d * std::sqrt(2.0) + 2 * rt) * L + kPi * (rd + rt) * d * std::sqrt(2.0)},
              cell, [&](IStockModel& m) { return cutOne(m, t, {20.0, 30.0, -d}, {20.0 + L, 30.0, -d}); });
        // Diagonal, partly sloped: the same groove dug along a ramp checks the cone against the Z rays' search.
        auto m = blockStock(kMin, kMax, cell);
        const double up = m->cut(t, {20.0, 30.0, -d}, {50.0, 60.0, -1.0}, false);
        CHECK(up > 0);
    }
    {
        // Drill, 118 degrees, diameter 8: a hole 15 deep.
        const ToolShape t = pointed(lib::ToolType::Drill, 8.0, 118.0, 0.0);
        const double R = 4.0, D = 15.0, hc = R / std::tan(59.0 * kPi / 180);
        check(
            {"drill point plunge", kPi / 3 * R * R * hc + kPi * R * R * (D - hc), 2 * kPi * R * D + kPi * R * R * 1.2},
            cell, [&](IStockModel& m) { return cutOne(m, t, {60.6, 60.6, 5.0}, {60.6, 60.6, -D}); });
    }
    {
        // General revolved profiles, built by hand: a tapered ball (ball 1 mm radius, then a 5 degree taper) and a
        // T-slot-like undercut cutter (wide disc at the tip, narrow neck above it: not convex), plunged and run along
        // a straight groove. Exact: plunge pi int r^2 dh; groove L int 2 r dh + the plunge (the two half ends).
        ToolShape taper;
        const double rb = 1.0, half = 5 * kPi / 180;
        for (int i = 0; i <= 64; ++i) {
            const double a = (kPi / 2 - half) * i / 64;
            taper.cuttingProfile.push_back({rb * std::sin(a), rb - rb * std::cos(a)});
        }
        const Vec2 tp = taper.cuttingProfile.back();
        taper.cuttingProfile.push_back({tp.x + 20 * std::tan(half), tp.y + 20});
        ToolShape tslot;
        tslot.cuttingProfile = {{0, 0}, {6, 0}, {6, 2}, {2, 2}, {2, 12}};
        int which = 0;
        for (const ToolShape* t : {&taper, &tslot}) {
            const double d = 8.0, L = 30.0;
            double side = 0;
            const double plunge = profileVolume(t->cuttingProfile, d, side);
            const bool undercut = which++ == 1;
            const std::string name = undercut ? "T-slot cutter" : "tapered ball";
            // Plunged from above, the T-slot cutter's head goes through the whole column: pi 6^2 d. Placed (the groove
            // starts and ends at depth) it leaves the material above its head outside the neck.
            const double fromAbove = undercut ? kPi * 36 * d : plunge;
            check({name + " plunge", fromAbove, 2 * kPi * 6 * d + kPi * 36 * 2}, cell,
                  [&](IStockModel& m) { return cutOne(m, *t, {40.2, 40.2, 1.0}, {40.2, 40.2, -d}); });
            check({name + " groove", side * L + plunge, (2 * d + 12) * L * 2 + 2 * kPi * 6 * d}, cell,
                  [&](IStockModel& m) { return cutOne(m, *t, {20.2, 40.2, -d}, {20.2 + L, 40.2, -d}); });
        }
    }
}

TEST_CASE("Stepped and overlapping cuts count each piece of material once", "[sim][tridexel][volume]")
{
    const double cell = GENERATE(0.2, 0.1);
    const ToolShape t = flat(8.0);
    const double R = 4.0, L = 40.0;
    const auto slot = [&](double d) {
        return (2 * R * L + kPi * R * R) * d;
    };
    const double surface = (2 * L + 2 * kPi * R) * 6 + 2 * R * L + kPi * R * R;
    const auto layer = [&](const IStockModel& m) {
        const sim::detail::TriDexelInfo in = sim::detail::triDexelInfo(m);
        return std::max({in.cx, in.cy, in.cz}) * surface;
    };
    {
        // Step-down: 3 mm, then 6 mm on the same line: the second pass takes only the second 3 mm.
        auto m = blockStock(kMin, kMax, cell);
        const double first = m->cut(t, {10.1, 40.1, -3.0}, {10.1 + L, 40.1, -3.0}, false);
        const double second = m->cut(t, {10.1, 40.1, -6.0}, {10.1 + L, 40.1, -6.0}, false);
        CHECK(std::abs(first - slot(3)) <= layer(*m));
        CHECK(std::abs(second - (slot(6) - slot(3))) <= layer(*m));
        // The same slot again takes nothing.
        CHECK(m->cut(t, {10.1, 40.1, -6.0}, {10.1 + L, 40.1, -6.0}, false) == 0.0);
    }
    {
        // Two slots crossing at right angles in their middles: the second loses the (2R)^2 square they share.
        auto m = blockStock(kMin, kMax, cell);
        const double d = 6.0;
        const double a = m->cut(t, {10.1, 40.1, -d}, {10.1 + L, 40.1, -d}, false);
        const double b = m->cut(t, {30.1, 20.1, -d}, {30.1, 20.1 + L, -d}, false);
        CHECK(std::abs(a - slot(d)) <= layer(*m));
        CHECK(std::abs(b - (slot(d) - 4 * R * R * d)) <= layer(*m));
    }
    {
        // One batch of overlapping moves (a zigzag over one spot): the per-move volumes add up to the union.
        auto m = blockStock(kMin, kMax, cell);
        std::vector<IStockModel::CutMove> mv = {{{20, 40, -5}, {40, 40, -5}, false},
                                                {{40, 40, -5}, {20, 41, -5}, false},
                                                {{20, 41, -5}, {40, 42, -5}, false},
                                                {{40, 42, -5}, {40, 42, -5}, false}};
        const Volumes before = volumes(*m);
        std::vector<double> v(mv.size());
        m->cutBatch(t, mv, v);
        const Volumes after = volumes(*m);
        // The union of the three stadiums, rasterised at 0.01 mm; each alone is 2 R len + pi R^2.
        double unionArea = 0, separate = 0;
        const double step = 0.01;
        const auto inStadium = [&](const IStockModel::CutMove& s, double x, double y) {
            const double dx = s.to.x - s.from.x, dy = s.to.y - s.from.y, l2 = dx * dx + dy * dy;
            const double tt = l2 > 0 ? std::clamp(((x - s.from.x) * dx + (y - s.from.y) * dy) / l2, 0.0, 1.0) : 0.0;
            const double ex = s.from.x + tt * dx - x, ey = s.from.y + tt * dy - y;
            return ex * ex + ey * ey <= R * R;
        };
        for (double x = 15; x < 45; x += step) {
            for (double y = 35; y < 47; y += step) {
                bool any = false;
                for (int k = 0; k < 3; ++k) {
                    any = any || inStadium(mv[static_cast<std::size_t>(k)], x + step / 2, y + step / 2);
                }
                unionArea += any ? step * step : 0.0;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const auto& s = mv[static_cast<std::size_t>(k)];
            separate += (2 * R * std::hypot(s.to.x - s.from.x, s.to.y - s.from.y) + kPi * R * R) * 5;
        }
        INFO("per move " << v[0] << ", " << v[1] << ", " << v[2] << "; union " << unionArea * 5 << "; separately "
                         << separate);
        CHECK(std::abs(v[0] - (2 * R * 20 + kPi * R * R) * 5) <= layer(*m));
        CHECK(v[3] == 0.0);
        // No double counting: the moves' volumes add up to what the stock lost, which is the union.
        CHECK(std::abs(v[0] + v[1] + v[2] - (before.v[0] - after.v[0])) <= 1e-6);
        CHECK(std::abs(v[0] + v[1] + v[2] - unionArea * 5) <= layer(*m));
        CHECK(v[0] + v[1] + v[2] < separate - 100.0);
    }
    {
        // Two plunges at different depths, side by side, and a stepped profile (two diameters) in one move.
        auto m = blockStock(kMin, kMax, cell);
        const double a = m->cut(t, {20.3, 20.3, 1}, {20.3, 20.3, -4}, false);
        const double b = m->cut(t, {40.3, 20.3, 1}, {40.3, 20.3, -9}, false);
        CHECK(std::abs(a - kPi * R * R * 4) <= layer(*m));
        CHECK(std::abs(b - kPi * R * R * 9) <= layer(*m));
        ToolShape step;
        step.cuttingProfile = {{0, 0}, {2, 0}, {2, 3}, {5, 3}, {5, 20}};
        const double c = m->cut(step, {60.3, 50.3, 1}, {60.3, 50.3, -10}, false);
        CHECK(std::abs(c - (kPi * 4 * 3 + kPi * 25 * 7)) <= layer(*m));
    }
}

TEST_CASE("The flute length limits the cut; the shank and holder never cut", "[sim][tridexel][volume]")
{
    const double cell = 0.2;
    // Flutes 5 mm long: a 12 mm plunge removes only the bottom 5 mm under the flutes' last position... and the shank
    // above them leaves the rest of the column in place (the tool is too short).
    const ToolShape t = flat(10.0, 5.0);
    auto m = blockStock(kMin, kMax, cell);
    const double R = 5.0;
    std::vector<IStockModel::CutMove> mv = {{{30.1, 30.1, 2.0}, {30.1, 30.1, -12.0}, false}};
    std::vector<double> v(1);
    std::vector<std::uint8_t> hit(1);
    m->cutBatch(t, mv, v, hit);
    // The flutes swept from z = 7 down to -12: they removed the whole column from the top down to -12.
    CHECK(std::abs(v[0] - kPi * R * R * 12) <= cell * (2 * kPi * R * 12 + kPi * R * R));
    // Going down, its own flutes cleared the way: no contact.
    CHECK(hit[0] == 0);
    // A horizontal move at -12 through untouched stock: the flutes take 5 mm, the shank meets the 7 mm above.
    mv = {{{30.1, 30.1, -12.0}, {60.1, 30.1, -12.0}, false}};
    m->cutBatch(t, mv, v, hit);
    CHECK(hit[0] == 1);
    const double band = (2 * R * 30) * 5;
    CHECK(std::abs(v[0] - band) <= cell * (2 * 30 + 2 * kPi * R) * 5 + cell * 2 * R * 30);
    // The material above the flutes is still there.
    ToolShape shankOnly = t;
    shankOnly.cuttingProfile.clear();
    CHECK(m->intersects(shankOnly, {45.1, 30.1, -12.0}, {45.1, 30.1, -12.0}));
    // Above the remaining material it is free.
    CHECK_FALSE(m->intersects(t, {45.1, 30.1, 1.0}, {80.1, 30.1, 1.0}));
    // A rapid into material is cut like a feed move and reports what it took.
    CHECK(m->cut(t, {70.1, 70.1, -1.0}, {75.1, 70.1, -1.0}, true) > 0);
}
