// Property tests of the backplot (task T-037): every chord within the tolerance of its arc (radii 0.01 mm to 10 m, all
// planes, both directions, full circles, helices and radius mismatches), chord counts close to the fewest possible,
// and the move-to-vertex map on random toolpaths of every move kind.
#include "backplot_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

using namespace ray;
using namespace ray::sim;
using namespace ray::sim::test;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Portable uniform [0, 1): the standard distributions differ between standard libraries.
double uni(std::mt19937_64& rng)
{
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

double logUniform(std::mt19937_64& rng, double lo, double hi)
{
    return lo * std::pow(hi / lo, uni(rng));
}

struct V3 {
    double x, y, z;
};
V3 sub(V3 a, V3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double dot(V3 a, V3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double norm(V3 a)
{
    return std::sqrt(dot(a, a));
}

double distToSegment(V3 p, V3 a, V3 b)
{
    const V3 ab = sub(b, a);
    const double len2 = dot(ab, ab);
    double t = len2 > 0 ? dot(sub(p, a), ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return norm(sub(p, {a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t}));
}

// The arc a control runs for a move, from the integer IR alone (independent of src/sim): in-plane axes per G17/G18/
// G19, sweep in (0, 2 pi], radius and axis linear in the angle.
struct RefArc {
    int u, v, w;
    double cu, cv, a0, sweep, r0, r1, w0, w1;
    bool ccw;

    V3 at(double t) const
    {
        const double phi = a0 + (ccw ? sweep : -sweep) * t;
        const double r = r0 + (r1 - r0) * t;
        double p[3];
        p[u] = cu + r * std::cos(phi);
        p[v] = cv + r * std::sin(phi);
        p[w] = w0 + (w1 - w0) * t;
        return {p[0], p[1], p[2]};
    }
};

RefArc refArc(const cam::Toolpath& tp)
{
    const Move& m = tp.moves[0];
    RefArc a{};
    if (m.plane == ArcPlane::XY) {
        a.u = 0, a.v = 1, a.w = 2;
    } else if (m.plane == ArcPlane::XZ) {
        a.u = 2, a.v = 0, a.w = 1;
    } else {
        a.u = 1, a.v = 2, a.w = 0;
    }
    const Nm s[3] = {tp.startX, tp.startY, tp.startZ};
    const Nm e[3] = {m.x, m.y, m.z};
    const Nm c[3] = {m.i, m.j, m.k};
    const auto mmOf = [](Nm v) {
        return static_cast<double>(v) / 1e6;
    };
    a.cu = mmOf(c[a.u]), a.cv = mmOf(c[a.v]);
    const long double su = static_cast<long double>(s[a.u] - c[a.u]) / 1e6L;
    const long double sv = static_cast<long double>(s[a.v] - c[a.v]) / 1e6L;
    const long double eu = static_cast<long double>(e[a.u] - c[a.u]) / 1e6L;
    const long double ev = static_cast<long double>(e[a.v] - c[a.v]) / 1e6L;
    a.ccw = m.kind == MoveKind::ArcCCW;
    a.a0 = static_cast<double>(std::atan2(sv, su));
    long double turn = std::atan2(ev, eu) - std::atan2(sv, su); // counter-clockwise turn, normalised below
    const long double twoPi = 2.0L * static_cast<long double>(kPi);
    if (!a.ccw) {
        turn = -turn;
    }
    while (turn <= 0.0L) {
        turn += twoPi;
    }
    while (turn > twoPi) {
        turn -= twoPi;
    }
    a.sweep = static_cast<double>(turn);
    a.r0 = static_cast<double>(std::sqrt(su * su + sv * sv));
    a.r1 = static_cast<double>(std::sqrt(eu * eu + ev * ev));
    a.w0 = mmOf(s[a.w]);
    a.w1 = mmOf(e[a.w]);
    return a;
}

struct ArcCase {
    cam::Toolpath tp;
    double tol;
};

// A random arc: radius 0.01 mm to 10 m, centre within 1 m of the origin, any plane and direction; 15% full circles,
// 30% helices, 30% with a start/end radius mismatch of up to 0.1% (a control's radius tolerance), 5% with a large
// mismatch (a spiral to between half and twice the radius).
ArcCase randomArc(std::mt19937_64& rng)
{
    const double r0 = logUniform(rng, 0.01, 10000.0);
    const ArcPlane plane = static_cast<ArcPlane>(rng() % 3);
    const bool ccw = (rng() & 1) != 0;
    double c[3] = {(uni(rng) - 0.5) * 2000.0, (uni(rng) - 0.5) * 2000.0, (uni(rng) - 0.5) * 2000.0};
    const int u = plane == ArcPlane::XY ? 0 : plane == ArcPlane::XZ ? 2 : 1;
    const int v = plane == ArcPlane::XY ? 1 : plane == ArcPlane::XZ ? 0 : 2;
    const int w = 3 - u - v;
    const double a0 = uni(rng) * 2 * kPi;
    const double kind = uni(rng);
    const bool full = uni(rng) < 0.15;
    const double sweep = full ? 2 * kPi : uni(rng) * 2 * kPi;
    double r1 = r0;
    if (kind < 0.3) {
        r1 = r0 * (1.0 + (uni(rng) - 0.5) * 0.002);
    } else if (kind < 0.35) {
        r1 = r0 * logUniform(rng, 0.5, 2.0);
    }
    double s[3], e[3];
    s[w] = c[w] + (uni(rng) - 0.5) * 100.0;
    e[w] = uni(rng) < 0.3 ? s[w] + (uni(rng) - 0.5) * 4.0 * r0 : s[w];
    s[u] = c[u] + r0 * std::cos(a0);
    s[v] = c[v] + r0 * std::sin(a0);
    const double a1 = a0 + (ccw ? sweep : -sweep);
    e[u] = c[u] + r1 * std::cos(a1);
    e[v] = c[v] + r1 * std::sin(a1);
    if (full && r1 == r0) {
        e[u] = s[u], e[v] = s[v]; // start equals end exactly
    }
    ArcCase out;
    out.tp = toolpath(s[0], s[1], s[2], {arc(ccw, plane, e[0], e[1], e[2], c[0], c[1], c[2])});
    out.tol = logUniform(rng, 0.0005, 0.2);
    return out;
}

// Rounding of a float coordinate up to |x|, for each of the two ends of a chord.
double floatSlack(double maxAbs)
{
    const double ulp = std::nextafter(static_cast<float>(maxAbs), INFINITY) - static_cast<float>(maxAbs);
    return std::sqrt(3.0) * 0.5 * static_cast<double>(ulp);
}

} // namespace

TEST_CASE("every chord is within the tolerance of its arc, with no more chords than needed",
          "[sim][backplot][arc][property]")
{
    std::mt19937_64 rng(20261121);
    const int cases = 3000;
    int checkedSegments = 0, spirals = 0, helices = 0, fulls = 0;
    double worstRatio = 0.0; // chord error over tolerance, after float slack
    for (int c = 0; c < cases; ++c) {
        const ArcCase ac = randomArc(rng);
        const Backplot bp = buildBackplot(ac.tp, Xform::identity(), ac.tol);
        checkLayout(ac.tp, bp);
        const RefArc ref = refArc(ac.tp);
        spirals += ref.r0 != ref.r1 ? 1 : 0;
        helices += ref.w0 != ref.w1 ? 1 : 0;
        fulls += ref.sweep == 2 * kPi ? 1 : 0;
        const std::uint32_t n = bp.moveFirstVertex[1] - bp.moveFirstVertex[0];
        REQUIRE(bp.lineVertices.size() == n + 1);
        std::vector<V3> pts;
        double maxAbs = 0.0;
        for (const BackplotVertex& bv : bp.lineVertices) {
            pts.push_back({bv.x, bv.y, bv.z});
            maxAbs = std::max({maxAbs, std::abs(static_cast<double>(bv.x)), std::abs(static_cast<double>(bv.y)),
                               std::abs(static_cast<double>(bv.z))});
        }
        const double slack = floatSlack(maxAbs) + 1e-9 * std::max(ref.r0, ref.r1);
        INFO("case " << c << ": r0 " << ref.r0 << " r1 " << ref.r1 << " sweep " << ref.sweep << " plane "
                     << static_cast<int>(ac.tp.moves[0].plane) << (ref.ccw ? " ccw" : " cw") << " tol " << ac.tol
                     << " chords " << n);
        // Vertex k is the arc at k / n.
        double vertexErr = 0.0;
        for (std::uint32_t k = 0; k <= n; ++k) {
            vertexErr = std::max(vertexErr, norm(sub(pts[k], ref.at(static_cast<double>(k) / n))));
        }
        CHECK(vertexErr <= slack);
        // Every point of the arc is within tol of the polyline (its own chord or a neighbour).
        double err = 0.0;
        const int samples = 8;
        for (std::uint32_t k = 1; k <= n; ++k) {
            for (int j = 1; j < samples; ++j) {
                const V3 p = ref.at((static_cast<double>(k - 1) + static_cast<double>(j) / samples) / n);
                double d = distToSegment(p, pts[k - 1], pts[k]);
                if (k > 1) {
                    d = std::min(d, distToSegment(p, pts[k - 2], pts[k - 1]));
                }
                if (k < n) {
                    d = std::min(d, distToSegment(p, pts[k], pts[k + 1]));
                }
                err = std::max(err, d);
            }
            ++checkedSegments;
        }
        CHECK(err <= ac.tol + slack);
        worstRatio = std::max(worstRatio, (err - slack) / ac.tol);
        // Not wasteful: for a circle the fewest chords of at most a quarter turn that meet the tolerance are
        // ceil(sweep / (2 acos(1 - tol / r))); allow the 3% of the trigonometry-free bound and one for rounding up. A
        // spiral is held to the same with its radius replaced by the bound on |P''| it is counted for.
        const double slope = (ref.r1 - ref.r0) / ref.sweep;
        const double rEff = std::sqrt(std::max(ref.r0, ref.r1) * std::max(ref.r0, ref.r1) + 4 * slope * slope);
        const double x = ac.tol / rEff;
        const double exact = x >= 1.0 ? kPi / 2 : std::min(kPi / 2, 2.0 * std::acos(1.0 - x));
        CHECK(static_cast<double>(n) <= 1.03 * ref.sweep / exact + 1.0);
    }
    std::cout << "[sim] " << cases << " random arcs, " << checkedSegments << " chords: worst chord error " << worstRatio
              << " of the tolerance (after float rounding)\n";
    INFO("worst chord error / tolerance " << worstRatio);
    CHECK(worstRatio <= 1.0);
    // The generator covered what it claims.
    CHECK(checkedSegments > 100000);
    CHECK(spirals > cases / 4);
    CHECK(helices > cases / 5);
    CHECK(fulls > cases / 20);
}

TEST_CASE("moveFirstVertex maps every move of random toolpaths", "[sim][backplot][property]")
{
    std::mt19937_64 rng(7);
    for (int round = 0; round < 50; ++round) {
        std::vector<Move> moves;
        const int count = 1 + static_cast<int>(rng() % 400);
        for (int i = 0; i < count; ++i) {
            Move m{};
            // Every kind, plus values outside the enum and planes outside the enum (a corrupt IR must not break it).
            m.kind = static_cast<MoveKind>(rng() % 14);
            m.role = static_cast<MoveRole>(rng() % 9);
            m.plane = static_cast<ArcPlane>(rng() % 4);
            // Within 1 mm of the origin.
            const auto coord = [&] {
                return static_cast<Nm>(rng() % 2'000'001) - 1'000'000;
            };
            // Often the same position again: zero-length moves.
            if (rng() % 8 == 0) {
                m.x = m.y = m.z = 0;
            } else {
                m.x = coord(), m.y = coord(), m.z = coord();
            }
            m.i = coord(), m.j = coord(), m.k = coord();
            m.cycleRetract = coord();
            m.cyclePeck = static_cast<Nm>(rng() % 300'000) - 50'000;
            m.feedMicronPerMin = static_cast<std::int32_t>(rng() % 5'000'000);
            moves.push_back(m);
        }
        const cam::Toolpath tp = toolpath(0, 0, 0, moves);
        const Backplot bp = buildBackplot(tp, Xform::identity(), 0.001);
        checkLayout(tp, bp);
        // Rapid motion has no feed; vertex kinds are move kinds.
        for (std::size_t v = 1; v < bp.lineVertices.size(); ++v) {
            const BackplotVertex& bv = bp.lineVertices[v];
            if (bv.kind == static_cast<std::uint8_t>(MoveKind::Rapid)) {
                CHECK(bv.feed == 0.0f);
            }
        }
        // The exact size was reserved: the count pass and the emit pass agree.
        CHECK(bp.lineVertices.capacity() == bp.lineVertices.size());
    }
}
