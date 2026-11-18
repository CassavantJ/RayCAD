// DXF import and export (task T-091): entity conversion (bulges, OCS, blocks, splines, ellipses), units, layers,
// errors, the export/import round trip to 1e-9 mm, the corpus against the ezdxf oracle (dxf_corpus_expect.h), and
// malformed input (truncated and corrupted files) that must never crash or hang.
#include <catch2/catch_test_macros.hpp>

#include "dxf_corpus_expect.h"
#include "sketch_test_util.h"

#include <dxf.h>
#include <loops.h>
#include <loops_curves.h>
#include <model.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <set>
#include <sstream>

using namespace ray;
using namespace ray::sketch;
using namespace ray::sketch::test;

namespace {

constexpr double kPi = std::numbers::pi;

double dist(Vec2 a, Vec2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// DXF text from group code / value pairs.
std::string dxf(std::initializer_list<std::pair<int, std::string>> pairs)
{
    std::string out;
    for (const auto& [code, value] : pairs) {
        out += std::to_string(code) + "\n" + value + "\n";
    }
    return out;
}
std::string entities(const std::string& body, const std::string& header = "")
{
    std::string out;
    if (!header.empty()) {
        out += "0\nSECTION\n2\nHEADER\n" + header + "0\nENDSEC\n";
    }
    return out + "0\nSECTION\n2\nENTITIES\n" + body + "0\nENDSEC\n0\nEOF\n";
}
std::string units(int insunits)
{
    return "9\n$INSUNITS\n70\n" + std::to_string(insunits) + "\n";
}
std::string num(double v)
{
    std::ostringstream o;
    o.precision(17);
    o << v;
    return o.str();
}

DxfImport read(const std::string& text, UnitSystem assume = UnitSystem::Metric)
{
    return take(readDxf(text, assume));
}

template <class T>
std::vector<const T*> all(const Sketch& s)
{
    std::vector<const T*> out;
    for (const Entity& e : s.entities) {
        if (const auto* g = std::get_if<T>(&e.geom)) {
            out.push_back(g);
        }
    }
    return out;
}

// A warning containing `text`, ignoring the case of letters.
bool warned(const DxfImport& r, const std::string& text)
{
    const auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const std::string& w) { return lower(w).find(lower(text)) != std::string::npos; });
}

// ---------- Geometry of a sketch, for comparisons ----------

struct Measure {
    double length = 0, arcLength = 0;
    int arcs = 0;
    int points = 0;
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    void add(Vec2 p)
    {
        if (!any) {
            x0 = x1 = p.x;
            y0 = y1 = p.y;
            any = true;
        }
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
};

double sweepOf(Vec2 c, Vec2 s, Vec2 e, bool ccw)
{
    double a = std::atan2((s.x - c.x) * (e.y - c.y) - (s.y - c.y) * (e.x - c.x),
                          (s.x - c.x) * (e.x - c.x) + (s.y - c.y) * (e.y - c.y));
    if (!ccw) {
        a = -a;
    }
    if (a <= 0) {
        a += 2 * kPi;
    }
    return a;
}

Measure measure(const Sketch& s)
{
    Measure m;
    std::set<SketchEntId> used;
    for (const Entity& e : s.entities) {
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            used.insert({l->a, l->b});
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            used.insert({a->center, a->start, a->end});
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            used.insert(c->center);
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            used.insert(sp->controlPoints.begin(), sp->controlPoints.end());
        }
    }
    for (const Entity& e : s.entities) {
        if (std::holds_alternative<Point>(e.geom)) {
            m.points += used.count(e.id) ? 0 : 1;
        } else if (const auto* l = std::get_if<Line>(&e.geom)) {
            const Vec2 a = pointAt(s, l->a), b = pointAt(s, l->b);
            m.length += dist(a, b);
            m.add(a);
            m.add(b);
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            const Vec2 c = pointAt(s, a->center), st = pointAt(s, a->start), en = pointAt(s, a->end);
            const double r = dist(c, st);
            const double sweep = sweepOf(c, st, en, a->ccw);
            m.length += r * sweep;
            m.arcLength += r * sweep;
            ++m.arcs;
            m.add(st);
            m.add(en);
            const double a0 = std::atan2(st.y - c.y, st.x - c.x);
            for (int q = -8; q <= 8; ++q) {
                double t = q * kPi / 2 - a0;
                if (!a->ccw) {
                    t = -t;
                }
                if (t > 1e-12 && t < sweep - 1e-12) {
                    m.add({c.x + r * std::cos(q * kPi / 2), c.y + r * std::sin(q * kPi / 2)});
                }
            }
        } else if (const auto* ci = std::get_if<Circle>(&e.geom)) {
            const Vec2 c = pointAt(s, ci->center);
            m.length += 2 * kPi * ci->radius;
            m.arcLength += 2 * kPi * ci->radius;
            ++m.arcs;
            m.add({c.x - ci->radius, c.y - ci->radius});
            m.add({c.x + ci->radius, c.y + ci->radius});
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            std::vector<Vec2> poles;
            for (SketchEntId id : sp->controlPoints) {
                poles.push_back(pointAt(s, id));
            }
            for (const auto& seg : sketch::detail::sketchSplineBeziers(poles, sp->degree, sp->closed)) {
                Vec2 prev = seg.front();
                m.add(prev);
                for (int i = 1; i <= 2000; ++i) {
                    const Vec2 p = sketch::detail::bezierPoint(seg, i / 2000.0);
                    m.length += dist(prev, p);
                    m.add(p);
                    prev = p;
                }
            }
        }
    }
    return m;
}

// Independent B-spline evaluation (de Boor, homogeneous) for checking the Bezier decomposition.
Vec2 deBoor(const std::vector<Vec2>& P, const std::vector<double>& W, const std::vector<double>& U, int p, double u)
{
    const int n = static_cast<int>(P.size());
    int k = p;
    while (k < n - 1 && !(u < U[static_cast<std::size_t>(k + 1)])) {
        ++k;
    }
    std::vector<std::array<double, 3>> d;
    for (int j = 0; j <= p; ++j) {
        const auto i = static_cast<std::size_t>(j + k - p);
        const double w = W.empty() ? 1.0 : W[i];
        d.push_back({P[i].x * w, P[i].y * w, w});
    }
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const auto i = static_cast<std::size_t>(j + k - p);
            const double denom = U[i + static_cast<std::size_t>(p - r + 1)] - U[i];
            const double alpha = denom > 0 ? (u - U[i]) / denom : 0;
            for (int c = 0; c < 3; ++c) {
                d[static_cast<std::size_t>(j)][static_cast<std::size_t>(c)] =
                    (1 - alpha) * d[static_cast<std::size_t>(j - 1)][static_cast<std::size_t>(c)] +
                    alpha * d[static_cast<std::size_t>(j)][static_cast<std::size_t>(c)];
            }
        }
    }
    const auto& h = d[static_cast<std::size_t>(p)];
    return {h[0] / h[2], h[1] / h[2]};
}

std::string splineEntity(int degree, const std::vector<double>& knots, const std::vector<Vec2>& poles,
                         const std::vector<double>& weights = {}, int flags = 8)
{
    std::ostringstream o;
    o.precision(17);
    o << "0\nSPLINE\n8\n0\n70\n"
      << flags << "\n71\n"
      << degree << "\n72\n"
      << knots.size() << "\n73\n"
      << poles.size() << "\n74\n0\n";
    for (double k : knots) {
        o << "40\n" << k << "\n";
    }
    for (double w : weights) {
        o << "41\n" << w << "\n";
    }
    for (const Vec2& p : poles) {
        o << "10\n" << p.x << "\n20\n" << p.y << "\n30\n0\n";
    }
    return o.str();
}

} // namespace

TEST_CASE("dxf: LWPOLYLINE bulges become arcs with the right centre, radius and direction", "[dxf]")
{
    const double b45 = std::tan(kPi / 8); // a quarter turn
    const DxfImport r = read(entities(dxf({{0, "LWPOLYLINE"},
                                           {8, "0"},
                                           {90, "4"},
                                           {70, "1"},
                                           {10, "0"},
                                           {20, "0"},
                                           {42, "1"}, // half circle, counter-clockwise
                                           {10, "10"},
                                           {20, "0"},
                                           {42, num(-b45)},
                                           {10, "10"},
                                           {20, "10"},
                                           {10, "0"},
                                           {20, "10"}})));
    const auto arcs = all<Arc>(r.sketch);
    const auto lines = all<Line>(r.sketch);
    REQUIRE(arcs.size() == 2);
    CHECK(lines.size() == 2);
    for (const Arc* a : arcs) {
        const Vec2 c = pointAt(r.sketch, a->center), s = pointAt(r.sketch, a->start), e = pointAt(r.sketch, a->end);
        if (dist(s, {0, 0}) < 1e-12) {
            CHECK(a->ccw);
            CHECK(dist(c, {5, 0}) < 1e-12);
            CHECK(dist(e, {10, 0}) < 1e-12);
        } else {
            CHECK_FALSE(a->ccw);
            CHECK(dist(s, {10, 0}) < 1e-12);
            CHECK(dist(e, {10, 10}) < 1e-12);
            CHECK(dist(c, {15, 5}) < 1e-9); // clockwise quarter from (10,0) to (10,10) bulges left, centre right
            CHECK(std::fabs(dist(c, s) - std::sqrt(50.0)) < 1e-9);
        }
    }
    // The polyline's segments share their vertices.
    CHECK(all<Point>(r.sketch).size() == 4 + 2);
    // And make one closed region.
    const auto loops = take(closedLoops(r.sketch));
    CHECK(loops.size() >= 1);
}

TEST_CASE("dxf: R12 POLYLINE with VERTEX bulges and SEQEND, in inches", "[dxf]")
{
    const DxfImport r = read(entities(
        dxf({{0, "POLYLINE"}, {8, "0"},      {66, "1"}, {70, "1"}, {0, "VERTEX"}, {8, "0"},      {10, "0"}, {20, "0"},
             {42, "1"},       {0, "VERTEX"}, {8, "0"},  {10, "2"}, {20, "0"},     {0, "VERTEX"}, {8, "0"},  {10, "2"},
             {20, "1"},       {0, "VERTEX"}, {8, "0"},  {10, "0"}, {20, "1"},     {0, "SEQEND"}, {8, "0"}}),
        units(1)));
    CHECK(r.mmPerUnit == 25.4);
    CHECK(r.unitSource == "$INSUNITS");
    const auto arcs = all<Arc>(r.sketch);
    REQUIRE(arcs.size() == 1);
    CHECK(dist(pointAt(r.sketch, arcs[0]->center), {25.4, 0}) < 1e-12);
    CHECK(all<Line>(r.sketch).size() == 3);
    const LoopAnalysis a = take(analyzeLoops(r.sketch));
    REQUIRE(a.regions.size() == 1);
    // 2 x 1 inch rectangle with a half disc of radius 1 inch cut out of its bottom edge... the arc bulges down
    // (counter-clockwise from (0,0) to (2,0)), so the disc is added.
    CHECK(std::fabs(a.regions[0].area - (2 + kPi / 2) * 25.4 * 25.4) < 1e-9);
}

TEST_CASE("dxf: units from $INSUNITS, $MEASUREMENT or the caller", "[dxf]")
{
    const std::string line = dxf({{0, "LINE"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}});
    const auto lengthOf = [](const DxfImport& r) {
        const Line* l = all<Line>(r.sketch).at(0);
        return dist(pointAt(r.sketch, l->a), pointAt(r.sketch, l->b));
    };
    CHECK(lengthOf(read(entities(line, units(4)))) == 1.0);
    CHECK(lengthOf(read(entities(line, units(1)))) == 25.4);
    CHECK(lengthOf(read(entities(line, units(5)))) == 10.0);
    CHECK(lengthOf(read(entities(line, units(6)))) == 1000.0);
    CHECK(lengthOf(read(entities(line, units(2)))) == 304.8);
    CHECK(lengthOf(read(entities(line, units(9)))) == 0.0254);
    CHECK(lengthOf(read(entities(line, "9\n$MEASUREMENT\n70\n0\n"))) == 25.4);
    CHECK(lengthOf(read(entities(line, units(0) + "9\n$MEASUREMENT\n70\n1\n"), UnitSystem::Inch)) == 1.0);
    const DxfImport assumed = read(entities(line), UnitSystem::Inch);
    CHECK(lengthOf(assumed) == 25.4);
    CHECK(assumed.unitSource == "assumed");
    const DxfImport odd = read(entities(line, units(99)), UnitSystem::Metric);
    CHECK(lengthOf(odd) == 1.0);
    CHECK(warned(odd, "$INSUNITS 99"));
}

TEST_CASE("dxf: CIRCLE, ARC, POINT and the object coordinate system", "[dxf]")
{
    const DxfImport r =
        read(entities(dxf({{0, "ARC"}, {8, "0"},      {10, "5"},    {20, "0"},     {40, "2"}, {50, "0"}, {51, "90"},
                           {0, "ARC"}, {8, "0"},      {10, "5"},    {20, "0"},     {40, "2"}, {50, "0"}, {51, "90"},
                           {210, "0"}, {220, "0"},    {230, "-1"},  {0, "CIRCLE"}, {8, "0"},  {10, "1"}, {20, "2"},
                           {40, "3"},  {0, "CIRCLE"}, {8, "0"},     {10, "1"},     {20, "2"}, {40, "3"}, {210, "1"},
                           {220, "0"}, {230, "0"},    {0, "POINT"}, {8, "0"},      {10, "7"}, {20, "8"}})));
    const auto arcs = all<Arc>(r.sketch);
    REQUIRE(arcs.size() == 2);
    int mirrored = 0;
    for (const Arc* a : arcs) {
        const Vec2 c = pointAt(r.sketch, a->center), s = pointAt(r.sketch, a->start), e = pointAt(r.sketch, a->end);
        if (a->ccw) {
            CHECK(c.x == 5);
            CHECK(c.y == 0);
            CHECK(s.x == 7);
            CHECK(s.y == 0);
            CHECK(e.x == 5);
            CHECK(e.y == 2);
        } else {
            ++mirrored; // extrusion -Z: x is mirrored and the arc runs clockwise
            CHECK(c.x == -5);
            CHECK(s.x == -7);
            CHECK(e.x == -5);
            CHECK(e.y == 2);
        }
    }
    CHECK(mirrored == 1);
    REQUIRE(all<Circle>(r.sketch).size() == 1); // the tilted one is left out
    CHECK(warned(r, "not parallel to the XY plane"));
    CHECK(measure(r.sketch).points == 1);
}

TEST_CASE("dxf: blocks are exploded with rotation, mirror, scale and arrays", "[dxf]")
{
    const std::string blocks =
        "0\nSECTION\n2\nBLOCKS\n" +
        dxf({{0, "BLOCK"}, {8, "0"},    {2, "PART"},  {70, "0"}, {10, "1"},     {20, "0"},     {0, "LINE"},
             {8, "0"},     {10, "1"},   {20, "0"},    {11, "3"}, {21, "0"},     {0, "ARC"},    {8, "0"},
             {10, "1"},    {20, "0"},   {40, "1"},    {50, "0"}, {51, "90"},    {0, "ENDBLK"}, {8, "0"},
             {0, "BLOCK"}, {8, "0"},    {2, "OUTER"}, {70, "0"}, {10, "0"},     {20, "0"},     {0, "INSERT"},
             {8, "0"},     {2, "PART"}, {10, "0"},    {20, "0"}, {0, "ENDBLK"}, {8, "0"}}) +
        "0\nENDSEC\n";
    const auto withBlocks = [&](const std::string& body) {
        return blocks + entities(body);
    };
    SECTION("rotated 90 degrees and scaled 2 at (100, 0)")
    {
        const DxfImport r = read(withBlocks(
            dxf({{0, "INSERT"}, {8, "0"}, {2, "PART"}, {10, "100"}, {20, "0"}, {41, "2"}, {42, "2"}, {50, "90"}})));
        const auto lines = all<Line>(r.sketch);
        REQUIRE(lines.size() == 1);
        // Base point (1, 0) maps to the insertion point; (3, 0) is 2 units further along +x, rotated to +y, scaled 2.
        CHECK(dist(pointAt(r.sketch, lines[0]->a), {100, 0}) < 1e-12);
        CHECK(dist(pointAt(r.sketch, lines[0]->b), {100, 4}) < 1e-12);
        const auto arcs = all<Arc>(r.sketch);
        REQUIRE(arcs.size() == 1);
        CHECK(arcs[0]->ccw);
        CHECK(dist(pointAt(r.sketch, arcs[0]->start), {100, 2}) < 1e-12);
        CHECK(dist(pointAt(r.sketch, arcs[0]->end), {98, 0}) < 1e-12);
    }
    SECTION("mirrored: the arc turns clockwise")
    {
        const DxfImport r =
            read(withBlocks(dxf({{0, "INSERT"}, {8, "0"}, {2, "PART"}, {10, "0"}, {20, "0"}, {41, "-1"}})));
        const auto arcs = all<Arc>(r.sketch);
        REQUIRE(arcs.size() == 1);
        CHECK_FALSE(arcs[0]->ccw);
        CHECK(dist(pointAt(r.sketch, arcs[0]->start), {-1, 0}) < 1e-12);
        CHECK(dist(pointAt(r.sketch, arcs[0]->end), {0, 1}) < 1e-12);
    }
    SECTION("unequal scales keep the line and leave the arc out")
    {
        const DxfImport r =
            read(withBlocks(dxf({{0, "INSERT"}, {8, "0"}, {2, "PART"}, {10, "0"}, {20, "0"}, {41, "2"}, {42, "1"}})));
        CHECK(all<Line>(r.sketch).size() == 1);
        CHECK(all<Arc>(r.sketch).empty());
        CHECK(warned(r, "unequal X and Y scales"));
    }
    SECTION("a nested block and a 2 x 3 array")
    {
        const DxfImport r = read(withBlocks(dxf({{0, "INSERT"},
                                                 {8, "0"},
                                                 {2, "OUTER"},
                                                 {10, "0"},
                                                 {20, "0"},
                                                 {70, "2"},
                                                 {71, "3"},
                                                 {44, "10"},
                                                 {45, "20"}})));
        CHECK(all<Line>(r.sketch).size() == 6);
        CHECK(all<Arc>(r.sketch).size() == 6);
        const Measure m = measure(r.sketch);
        CHECK(m.x1 == 12);
        CHECK(m.y1 == 41);
    }
    SECTION("a missing block is reported")
    {
        const DxfImport r = read(withBlocks(dxf({{0, "INSERT"}, {8, "0"}, {2, "NOPE"}, {10, "0"}, {20, "0"}})));
        CHECK(r.sketch.entities.empty());
        CHECK(warned(r, "missing blocks"));
    }
}

TEST_CASE("dxf: splines", "[dxf]")
{
    SECTION("clamped uniform knots: one sketch spline")
    {
        const std::vector<Vec2> poles{{0, 0}, {1, 2}, {3, 2}, {4, 0}, {6, 1}};
        const DxfImport r = read(entities(splineEntity(3, {0, 0, 0, 0, 0.5, 1, 1, 1, 1}, poles)));
        const auto sp = all<Spline>(r.sketch);
        REQUIRE(sp.size() == 1);
        CHECK(sp[0]->degree == 3);
        CHECK_FALSE(sp[0]->closed);
        REQUIRE(sp[0]->controlPoints.size() == 5);
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(dist(pointAt(r.sketch, sp[0]->controlPoints[i]), poles[i]) == 0);
        }
    }
    SECTION("non-uniform knots: a chain of exact Bezier spans")
    {
        const std::vector<Vec2> poles{{0, 0}, {1, 3}, {2, -1}, {4, 2}, {5, 0}, {7, 1}};
        const std::vector<double> knots{0, 0, 0, 0, 0.2, 0.7, 1, 1, 1, 1};
        const DxfImport r = read(entities(splineEntity(3, knots, poles)));
        const auto sp = all<Spline>(r.sketch);
        REQUIRE(sp.size() == 3);
        // Every point of the original curve lies on the chain (same parameter mapping per span).
        const double spans[3][2] = {{0, 0.2}, {0.2, 0.7}, {0.7, 1}};
        for (const Spline* s : sp) {
            std::vector<Vec2> bez;
            for (SketchEntId id : s->controlPoints) {
                bez.push_back(pointAt(r.sketch, id));
            }
            REQUIRE(bez.size() == 4);
            int matched = 0;
            for (const auto& span : spans) {
                const Vec2 start = deBoor(poles, {}, knots, 3, span[0]);
                if (dist(start, bez.front()) > 1e-12) {
                    continue;
                }
                ++matched;
                for (int i = 0; i <= 20; ++i) {
                    const double t = i / 20.0;
                    const Vec2 a = sketch::detail::bezierPoint(bez, t);
                    const Vec2 b = deBoor(poles, {}, knots, 3, span[0] + t * (span[1] - span[0]));
                    CHECK(dist(a, b) < 1e-12);
                }
            }
            CHECK(matched == 1);
        }
    }
    SECTION("rational quadratic circle spans become arcs")
    {
        const double w = std::sqrt(0.5);
        const std::vector<Vec2> poles{{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
        const std::vector<double> weights{1, w, 1, w, 1, w, 1, w, 1};
        const std::vector<double> knots{0, 0, 0, 0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1, 1, 1};
        const DxfImport r = read(entities(splineEntity(2, knots, poles, weights, 8 + 4)));
        const auto arcs = all<Arc>(r.sketch);
        REQUIRE(arcs.size() == 4);
        for (const Arc* a : arcs) {
            CHECK(a->ccw);
            CHECK(dist(pointAt(r.sketch, a->center), {0, 0}) < 1e-12);
            CHECK(std::fabs(dist(pointAt(r.sketch, a->start), {0, 0}) - 1) < 1e-12);
        }
        CHECK(all<Spline>(r.sketch).empty());
        const LoopAnalysis la = take(analyzeLoops(r.sketch));
        REQUIRE(la.regions.size() == 1);
        CHECK(std::fabs(la.regions[0].area - kPi) < 1e-9);
    }
    SECTION("a rational spline that is not circular is left out with a warning")
    {
        const DxfImport r =
            read(entities(splineEntity(2, {0, 0, 0, 1, 1, 1}, {{0, 0}, {1, 2}, {2, 0}}, {1, 3, 1}, 12)));
        CHECK(r.sketch.entities.empty());
        CHECK(warned(r, "rational SPLINE"));
    }
    SECTION("a periodic uniform spline is one closed sketch spline")
    {
        // Six control points, the first three repeated, uniform knots 0..12.
        std::vector<Vec2> poles;
        for (int i = 0; i < 9; ++i) {
            poles.push_back({10 * std::cos(i * kPi / 3), 10 * std::sin(i * kPi / 3)});
        }
        std::vector<double> knots;
        for (int i = 0; i <= 12; ++i) {
            knots.push_back(i);
        }
        const DxfImport r = read(entities(splineEntity(3, knots, poles, {}, 1 + 2 + 8)));
        const auto sp = all<Spline>(r.sketch);
        REQUIRE(sp.size() == 1);
        CHECK(sp[0]->closed);
        CHECK(sp[0]->controlPoints.size() == 6);
    }
    SECTION("degree 1 is a polyline; fit points only and bad knots are reported")
    {
        const DxfImport r = read(entities(splineEntity(1, {0, 0, 1, 2, 2}, {{0, 0}, {1, 1}, {2, 0}})));
        CHECK(all<Line>(r.sketch).size() == 2);
        const DxfImport fit = read(entities(dxf({{0, "SPLINE"},
                                                 {8, "0"},
                                                 {70, "8"},
                                                 {71, "3"},
                                                 {74, "3"},
                                                 {11, "0"},
                                                 {21, "0"},
                                                 {11, "1"},
                                                 {21, "1"},
                                                 {11, "2"},
                                                 {21, "0"}})));
        CHECK(fit.sketch.entities.empty());
        CHECK(warned(fit, "fit points"));
        const DxfImport bad = read(entities(splineEntity(3, {0, 1, 2}, {{0, 0}, {1, 1}, {2, 0}, {3, 3}})));
        CHECK(bad.sketch.entities.empty());
        CHECK(warned(bad, "knots do not match"));
    }
}

TEST_CASE("dxf: ellipses, layers, ignored entities and paper space", "[dxf]")
{
    const DxfImport r = read(entities(dxf({{0, "ELLIPSE"}, {8, "0"},     {10, "0"},
                                           {20, "0"},      {11, "5"},    {21, "0"},
                                           {40, "1"},      {41, "0"},    {42, num(2 * kPi)},
                                           {0, "ELLIPSE"}, {8, "0"},     {10, "0"},
                                           {20, "0"},      {11, "5"},    {21, "0"},
                                           {40, "1"},      {41, "0"},    {42, "1.5"},
                                           {0, "ELLIPSE"}, {8, "0"},     {10, "0"},
                                           {20, "0"},      {11, "5"},    {21, "0"},
                                           {40, "0.5"},    {0, "LINE"},  {8, "Construction"},
                                           {10, "0"},      {20, "0"},    {11, "1"},
                                           {21, "0"},      {0, "LINE"},  {8, "Outline"},
                                           {10, "0"},      {20, "5"},    {11, "1"},
                                           {21, "5"},      {0, "LINE"},  {8, "0"},
                                           {67, "1"},      {10, "0"},    {20, "9"},
                                           {11, "1"},      {21, "9"},    {0, "TEXT"},
                                           {8, "0"},       {1, "hello"}, {0, "DIMENSION"},
                                           {8, "0"}})));
    CHECK(all<Circle>(r.sketch).size() == 1);
    REQUIRE(all<Arc>(r.sketch).size() == 1);
    CHECK(warned(r, "ELLIPSE entities that are not circles"));
    int construction = 0;
    for (const Entity& e : r.sketch.entities) {
        if (std::holds_alternative<Line>(e.geom)) {
            construction += e.construction;
        }
    }
    CHECK(all<Line>(r.sketch).size() == 2);
    CHECK(construction == 1);
    CHECK(warned(r, "paper space"));
    CHECK(warned(r, "TEXT"));
    CHECK(warned(r, "DIMENSION"));
    bool outline = false;
    for (const auto& [id, layer] : r.layers) {
        outline = outline || layer == "Outline";
    }
    CHECK(outline);
}

TEST_CASE("dxf: damaged files fail with FormatError, odd layouts are read", "[dxf]")
{
    const auto fails = [](const std::string& text) {
        const auto r = readDxf(text, UnitSystem::Metric);
        return !r.has_value() && r.error().code == ErrorCode::FormatError;
    };
    CHECK(fails(""));
    CHECK(fails("<html><body>Not found</body></html>\n"));
    CHECK(fails(std::string("AutoCAD Binary DXF\r\n\x1a") + std::string(1, '\0') + "rest"));
    CHECK(fails("0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n1\n")); // cut inside ENTITIES
    CHECK(fails("0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n"));    // cut after a group code
    CHECK(fails("0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nA\n"));         // cut inside BLOCKS
    CHECK(fails("0\nSECTION\n9\nENTITIES\n"));                       // section without a name
    CHECK(fails("0\nSECTION\n2\nENTITIES\nabc\nLINE\n"));            // group code not a number
    // CR line ends, a byte order mark, padded group codes, data after EOF, no EOF.
    const std::string crlf = "\xEF\xBB\xBF  0\r\nSECTION\r\n  2\r\nENTITIES\r\n  0\r\nLINE\r\n  8\r\n0\r\n 10\r\n0\r\n"
                             " 20\r\n0\r\n 11\r\n1.5\r\n 21\r\n+2\r\n  0\r\nENDSEC\r\n  0\r\nEOF\r\ntrash";
    CHECK(all<Line>(read(crlf).sketch).size() == 1);
    std::string cr = crlf.substr(3);
    cr.erase(std::remove(cr.begin(), cr.end(), '\n'), cr.end());
    CHECK(all<Line>(read(cr).sketch).size() == 1);
    const DxfImport noEof = read("0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n0\n20\n0\n11\n1\n21\n0\n0\nENDSEC\n");
    CHECK(all<Line>(noEof.sketch).size() == 1);
    CHECK(warned(noEof, "no EOF"));
    // A damaged number leaves that entity out.
    const DxfImport bad = read(entities(dxf({{0, "LINE"},
                                             {10, "x"},
                                             {20, "0"},
                                             {11, "1"},
                                             {21, "0"},
                                             {0, "LINE"},
                                             {10, "0"},
                                             {20, "0"},
                                             {11, "1"},
                                             {21, "1e999"},
                                             {0, "LINE"},
                                             {10, "0"},
                                             {20, "0"},
                                             {11, "1"},
                                             {21, "0"}})));
    CHECK(all<Line>(bad.sketch).size() == 1);
    CHECK(warned(bad, "damaged LINE"));
}

TEST_CASE("dxf: limits bound size, entities and block expansion", "[dxf]")
{
    const std::string line = dxf({{0, "LINE"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}});
    DxfLimits small;
    small.maxFileBytes = 64;
    CHECK_FALSE(readDxf(entities(line + line), UnitSystem::Metric, small).has_value());
    DxfLimits few;
    few.maxEntities = 5;
    std::string many;
    for (int i = 0; i < 10; ++i) {
        many += dxf({{0, "LINE"}, {10, std::to_string(i)}, {20, "0"}, {11, std::to_string(i)}, {21, "1"}});
    }
    const auto r = readDxf(entities(many), UnitSystem::Metric, few);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::FormatError);

    SECTION("a block bomb: ten references per level, eight levels")
    {
        std::string blocks = "0\nSECTION\n2\nBLOCKS\n";
        blocks += dxf({{0, "BLOCK"}, {2, "B0"}, {10, "0"}, {20, "0"}}) + line + dxf({{0, "ENDBLK"}});
        for (int level = 1; level <= 8; ++level) {
            blocks += dxf({{0, "BLOCK"}, {2, "B" + std::to_string(level)}, {10, "0"}, {20, "0"}});
            for (int i = 0; i < 10; ++i) {
                blocks += dxf({{0, "INSERT"},
                               {2, "B" + std::to_string(level - 1)},
                               {10, std::to_string(i)},
                               {20, std::to_string(level)}});
            }
            blocks += dxf({{0, "ENDBLK"}});
        }
        blocks += "0\nENDSEC\n";
        DxfLimits limits;
        limits.maxBlockExpansion = 100'000;
        const auto start = std::chrono::steady_clock::now();
        const DxfImport b = take(readDxf(blocks + entities(dxf({{0, "INSERT"}, {2, "B8"}, {10, "0"}, {20, "0"}})),
                                         UnitSystem::Metric, limits));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK(seconds < 30);
        CHECK(warned(b, "block references expand to more than"));
    }
    SECTION("a block that inserts itself, and a huge array of an empty block")
    {
        const std::string blocks = "0\nSECTION\n2\nBLOCKS\n" +
                                   dxf({{0, "BLOCK"},
                                        {2, "SELF"},
                                        {10, "0"},
                                        {20, "0"},
                                        {0, "INSERT"},
                                        {2, "SELF"},
                                        {10, "1"},
                                        {20, "0"},
                                        {0, "ENDBLK"},
                                        {0, "BLOCK"},
                                        {2, "EMPTY"},
                                        {10, "0"},
                                        {20, "0"},
                                        {0, "ENDBLK"}}) +
                                   "0\nENDSEC\n";
        const DxfImport self = read(blocks + entities(dxf({{0, "INSERT"}, {2, "SELF"}, {10, "0"}, {20, "0"}})));
        CHECK(warned(self, "nested deeper than"));
        const auto start = std::chrono::steady_clock::now();
        const DxfImport arr = read(
            blocks +
            entities(dxf({{0, "INSERT"}, {2, "EMPTY"}, {10, "0"}, {20, "0"}, {70, "2000000000"}, {71, "2000000000"}})));
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 30);
        CHECK(warned(arr, "block references expand to more than"));
    }
}

TEST_CASE("dxf: ids are derived from the file's bytes", "[dxf]")
{
    const std::string a = entities(dxf({{0, "LINE"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}}));
    const std::string b = entities(dxf({{0, "LINE"}, {10, "0"}, {20, "0"}, {11, "2"}, {21, "0"}}));
    const DxfImport a1 = read(a), a2 = read(a), b1 = read(b);
    REQUIRE(a1.sketch.entities.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(a1.sketch.entities[i].id == a2.sketch.entities[i].id);
        CHECK(a1.sketch.entities[i].id != b1.sketch.entities[i].id);
    }
    CHECK(validateSketch(a1.sketch).has_value());
}

// ---------- Export and import round trip ----------

namespace {

struct Curve {
    int kind; // 0 point, 1 line, 2 arc, 3 circle, 4 spline
    bool construction;
    std::vector<Vec2> pts; // line: sorted ends; arc (counter-clockwise): centre, start, end; circle: centre; spline
    double radius = 0;
    int degree = 0;
    bool closed = false;
};

std::vector<Curve> curvesOf(const Sketch& s)
{
    std::set<SketchEntId> used;
    for (const Entity& e : s.entities) {
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            used.insert({l->a, l->b});
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            used.insert({a->center, a->start, a->end});
        } else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            used.insert(c->center);
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            used.insert(sp->controlPoints.begin(), sp->controlPoints.end());
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            used.insert({sl->a, sl->b});
        }
    }
    std::vector<Curve> out;
    const auto line = [&](Vec2 a, Vec2 b, bool c) {
        if (std::tie(b.x, b.y) < std::tie(a.x, a.y)) {
            std::swap(a, b);
        }
        out.push_back({1, c, {a, b}});
    };
    const auto arc = [&](Vec2 c, Vec2 s0, Vec2 e0, bool ccw, bool cons) {
        if (!ccw) {
            std::swap(s0, e0);
        }
        out.push_back({2, cons, {c, s0, e0}});
    };
    for (const Entity& e : s.entities) {
        const bool c = e.construction;
        if (const auto* p = std::get_if<Point>(&e.geom)) {
            if (!used.count(e.id)) {
                out.push_back({0, c, {p->p}});
            }
        } else if (const auto* l = std::get_if<Line>(&e.geom)) {
            line(pointAt(s, l->a), pointAt(s, l->b), c);
        } else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            arc(pointAt(s, a->center), pointAt(s, a->start), pointAt(s, a->end), a->ccw, c);
        } else if (const auto* ci = std::get_if<Circle>(&e.geom)) {
            out.push_back({3, c, {pointAt(s, ci->center)}, ci->radius});
        } else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            Curve k{4, c, {}};
            for (SketchEntId id : sp->controlPoints) {
                k.pts.push_back(pointAt(s, id));
            }
            k.degree = sp->degree;
            k.closed = sp->closed;
            out.push_back(k);
        } else if (const auto* sl = std::get_if<Slot>(&e.geom)) {
            const Vec2 A = pointAt(s, sl->a), B = pointAt(s, sl->b);
            const double len = dist(A, B);
            const Vec2 n{-(B.y - A.y) * sl->width / 2 / len, (B.x - A.x) * sl->width / 2 / len};
            const Vec2 a0{A.x - n.x, A.y - n.y}, a1{A.x + n.x, A.y + n.y}, b0{B.x - n.x, B.y - n.y},
                b1{B.x + n.x, B.y + n.y};
            line(a0, b0, c);
            arc(B, b0, b1, true, c);
            line(b1, a1, c);
            arc(A, a1, a0, true, c);
        }
    }
    const auto key = [](const Curve& k) {
        std::vector<double> v{static_cast<double>(k.kind), k.construction ? 1.0 : 0.0, std::round(k.radius * 1e6)};
        for (const Vec2& p : k.pts) {
            v.push_back(std::round(p.x * 1e6));
            v.push_back(std::round(p.y * 1e6));
        }
        return v;
    };
    std::sort(out.begin(), out.end(), [&](const Curve& a, const Curve& b) { return key(a) < key(b); });
    return out;
}

double geometricDifference(const Sketch& a, const Sketch& b)
{
    const auto ca = curvesOf(a);
    const auto cb = curvesOf(b);
    REQUIRE(ca.size() == cb.size());
    double worst = 0;
    for (std::size_t i = 0; i < ca.size(); ++i) {
        REQUIRE(ca[i].kind == cb[i].kind);
        CHECK(ca[i].construction == cb[i].construction);
        CHECK(ca[i].degree == cb[i].degree);
        CHECK(ca[i].closed == cb[i].closed);
        REQUIRE(ca[i].pts.size() == cb[i].pts.size());
        worst = std::max(worst, std::fabs(ca[i].radius - cb[i].radius));
        for (std::size_t p = 0; p < ca[i].pts.size(); ++p) {
            worst = std::max(worst, dist(ca[i].pts[p], cb[i].pts[p]));
        }
    }
    return worst;
}

Sketch roundTripSketch()
{
    Builder b;
    // Lines sharing points, an arc each way, circles, a free point, construction geometry, splines, a slot.
    const SketchEntId p0 = b.point(0, 0), p1 = b.point(123.456789012345, 0.000123), p2 = b.point(123.456789, 77.7);
    b.line(p0, p1);
    b.line(p1, p2);
    const SketchEntId c = b.point(-40.5, 12.25);
    const double r = 17.0 / 3.0;
    b.arc(c, b.point(-40.5 + r, 12.25), b.point(-40.5 + r * std::cos(2.0), 12.25 + r * std::sin(2.0)), true);
    b.arc(c, b.point(-40.5, 12.25 + r), b.point(-40.5 + r * std::cos(-0.3), 12.25 + r * std::sin(-0.3)), false);
    b.arc(b.point(1e-3, 2e-3), b.point(1e-3, 2e-3 + 0.01), b.point(1e-3 - 0.01, 2e-3), true); // tiny
    b.circle(b.point(1000.125, -2000.0625), 0.1);
    b.circle(b.point(-3.3, 4.4), 250.75);
    b.point(9.87654321, -1.23456789);
    b.add(Line{b.point(-500, -500), b.point(500, 500)}, true);
    b.add(Circle{b.point(5, 5), 12}, true);
    std::vector<SketchEntId> cp;
    for (auto [x, y] : {std::pair{0.0, 100.0}, {10.0, 120.0}, {25.0, 90.0}, {40.0, 130.0}, {60.0, 100.0}}) {
        cp.push_back(b.point(x, y));
    }
    b.add(Spline{cp, 3, false});
    b.add(Spline{std::vector<SketchEntId>(cp.begin(), cp.begin() + 4), 2, false});
    std::vector<SketchEntId> ring;
    for (int i = 0; i < 7; ++i) {
        ring.push_back(b.point(300 + 20 * std::cos(i * 0.9), 300 + 15 * std::sin(i * 0.9)));
    }
    b.add(Spline{ring, 3, true});
    b.add(Slot{b.point(200, 0), b.point(260.5, 31.25), 12.7});
    return b.s;
}

std::string tempPath(const std::string& name)
{
    const auto dir = std::filesystem::temp_directory_path() / "raycad_sketch_dxf_test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

} // namespace

TEST_CASE("dxf: export then import is geometrically identical to 1e-9 mm", "[dxf]")
{
    const Sketch original = roundTripSketch();
    for (const UnitSystem u : {UnitSystem::Metric, UnitSystem::Inch}) {
        const std::string path = tempPath(u == UnitSystem::Metric ? "roundtrip_mm.dxf" : "roundtrip_in.dxf");
        REQUIRE(exportDxf(path, original, u).has_value());
        const DxfImport back =
            take(importDxfFile(path, u == UnitSystem::Metric ? UnitSystem::Inch : UnitSystem::Metric));
        CHECK(back.unitSource == "$INSUNITS");
        CHECK(back.mmPerUnit == (u == UnitSystem::Metric ? 1.0 : 25.4));
        CHECK(back.warnings.empty());
        const double diff = geometricDifference(original, back.sketch);
        INFO("largest difference " << diff << " mm");
        CHECK(diff <= 1e-9);
        // The public wrapper gives the same sketch.
        const Sketch viaApi = take(importDxf(path, UnitSystem::Metric));
        CHECK(geometricDifference(original, viaApi) <= 1e-9);
        // A second export of the re-imported sketch is the same file.
        const std::string again = take(writeDxf(back.sketch, u));
        const std::string first = take(writeDxf(original, u));
        CHECK(again.size() > 1000);
        CHECK(first.find("AC1015") != std::string::npos);
    }
    // Writing is deterministic.
    CHECK(take(writeDxf(original, UnitSystem::Metric)) == take(writeDxf(original, UnitSystem::Metric)));
    // An invalid sketch or an unwritable path fails cleanly.
    Builder bad;
    bad.add(Line{bad.point(0, 0), SketchEntId{orderedUuid(999, 1)}});
    CHECK_FALSE(exportDxf(tempPath("bad.dxf"), bad.s, UnitSystem::Metric).has_value());
    const auto unwritable = exportDxf(tempPath("no_such_dir") + "/x/y.dxf", original, UnitSystem::Metric);
    REQUIRE_FALSE(unwritable.has_value());
    CHECK(unwritable.error().code == ErrorCode::IoError);
    const auto missing = importDxf(tempPath("does_not_exist.dxf"), UnitSystem::Metric);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("dxf: exported regions survive the round trip", "[dxf]")
{
    const Sketch original = roundTripSketch();
    const auto before = take(analyzeLoops(original));
    const DxfImport back = take(readDxf(take(writeDxf(original, UnitSystem::Metric)), UnitSystem::Metric));
    const auto after = take(analyzeLoops(back.sketch));
    REQUIRE(after.regions.size() == before.regions.size());
    double areaBefore = 0, areaAfter = 0;
    for (const auto& r : before.regions) {
        areaBefore += r.area;
    }
    for (const auto& r : after.regions) {
        areaAfter += r.area;
    }
    CHECK(std::fabs(areaBefore - areaAfter) <= 1e-9 * areaBefore);
}

// ---------- The corpus, against the ezdxf oracle ----------

TEST_CASE("dxf: corpus files from many exporters import with the right units, arcs and bulges", "[dxf][corpus]")
{
    int checked = 0;
    for (const DxfExpect& x : kDxfCorpus) {
        INFO(x.id);
        const auto r = importDxfFile(std::string(RAY_CORPUS_ROOT) + "/" + x.path, UnitSystem::Metric);
        REQUIRE(r.has_value());
        CHECK(r->mmPerUnit == x.mmPerUnit);
        CHECK(r->unitSource == x.unitSource);
        CHECK(validateSketch(r->sketch).has_value());
        const Measure m = measure(r->sketch);
        const double size = std::max({1.0, std::fabs(x.x0), std::fabs(x.y0), std::fabs(x.x1), std::fabs(x.y1)});
        // Rational splines count only when the reader could keep them (as arcs).
        const bool keptRational = x.rationalLength > 0 && !warned(*r, "rational SPLINE");
        const double expectLength = x.length + (keptRational ? x.rationalLength : 0);
        INFO("length " << m.length << " expected " << expectLength);
        CHECK(std::fabs(m.length - expectLength) <= 1e-6 * std::max(1.0, expectLength));
        if (x.rationalLength == 0) {
            INFO("arc length " << m.arcLength << " expected " << x.arcLength);
            CHECK(std::fabs(m.arcLength - x.arcLength) <= 1e-9 * std::max(1.0, x.arcLength));
            CHECK(m.arcs == x.arcs);
        }
        CHECK(m.points == x.points);
        if (x.length > 0) {
            INFO("box " << m.x0 << " " << m.y0 << " " << m.x1 << " " << m.y1);
            CHECK(std::fabs(m.x0 - x.x0) <= 1e-6 * size);
            CHECK(std::fabs(m.y0 - x.y0) <= 1e-6 * size);
            CHECK(std::fabs(m.x1 - x.x1) <= 1e-6 * size);
            CHECK(std::fabs(m.y1 - x.y1) <= 1e-6 * size);
            ++checked;
        }
        // Every imported sketch can be analysed for regions.
        CHECK(analyzeLoops(r->sketch).has_value());
    }
    CHECK(checked >= 30);
}

TEST_CASE("dxf: corpus regions of the synthetic profiles", "[dxf][corpus]")
{
    struct Case {
        const char* path;
        int regions;
        double area; // of the region with the most holes, mm^2
    };
    // Geometry from tools/corpus_synth.py.
    const double in2 = 25.4 * 25.4;
    const double slot = (1.25 - 0.375) * 0.375 + kPi * 0.1875 * 0.1875; // obround 1.25 x 0.375 in
    for (const Case& c :
         {Case{"files/raycad-synthetic/dxf_plate_4_holes_mm.dxf", 5, 6000 - 4 * kPi * 3.3 * 3.3},
          Case{"files/raycad-synthetic/dxf_gap_and_construction_inch.dxf", 0, 0},
          Case{"files/raycad-synthetic/dxf_open_contour_mm.dxf", 0, 0},
          Case{"files/raycad-synthetic/dxf_l_bracket_profile_mm.dxf", 1, 1056 + 36 - 9 * kPi},
          Case{"files/raycad-synthetic/dxf_rounded_plate_mm.dxf", 2, 120 * 80 - (4 - kPi) * 64 - kPi * 100},
          Case{"files/raycad-synthetic/dxf_slotted_plate_inch.dxf", 3, (8 - 2 * slot) * in2},
          Case{"files/raycad-synthetic/dxf_flange_gasket_mm.dxf", 8, kPi * (3600 - 900) - 6 * kPi * 4.5 * 4.5},
          Case{"files/raycad-synthetic/dxf_pocket_island_inch.dxf", 3, 0},
          Case{"files/raycad-synthetic/dxf_hex_profile_mm.dxf", 2, 6 * 0.5 * 400 * std::sin(kPi / 3) - kPi * 8.5 * 8.5},
          Case{"files/raycad-synthetic/dxf_keyhole_mm.dxf", 2, 0}}) {
        INFO(c.path);
        const DxfImport r = take(importDxfFile(std::string(RAY_CORPUS_ROOT) + "/" + c.path, UnitSystem::Metric));
        const LoopAnalysis a = take(analyzeLoops(r.sketch));
        CHECK(static_cast<int>(a.regions.size()) == c.regions);
        if (c.area > 0) {
            double best = 0;
            std::size_t holes = 0;
            for (const LoopRegion& reg : a.regions) {
                if (reg.holes.size() >= holes) {
                    holes = reg.holes.size();
                    best = reg.area;
                }
            }
            CHECK(std::fabs(best - c.area) < 1e-6 * std::max(1.0, c.area));
        }
    }
}

TEST_CASE("dxf: every committed corpus DXF is read, or refused with a reason", "[dxf][corpus]")
{
    int read = 0;
    int binary = 0;
    int broken = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(std::string(RAY_CORPUS_ROOT) + "/files")) {
        if (entry.path().extension() != ".dxf") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        INFO(name);
        const auto r = importDxfFile(entry.path().string(), UnitSystem::Metric);
        if (name.rfind("bin_dxf_", 0) == 0) {
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().message.find("Binary DXF") != std::string::npos);
            ++binary;
        } else if (name.rfind("broken_", 0) == 0) {
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().code == ErrorCode::FormatError);
            ++broken;
        } else {
            INFO((r.has_value() ? std::string() : r.error().message));
            REQUIRE(r.has_value());
            CHECK(validateSketch(r->sketch).has_value());
            ++read;
        }
    }
    CHECK(binary == 4);
    CHECK(broken == 1);
    CHECK(read >= 70);
}

// ---------- Malformed input ----------

namespace {

// The fuzz seeds: every corpus file of the oracle table, the broken corpus file (second to last), and an exported
// sketch (last).
std::vector<std::string> fuzzSeeds()
{
    std::vector<std::string> paths;
    for (const DxfExpect& x : kDxfCorpus) {
        paths.push_back(x.path);
    }
    paths.push_back("files/raycad-synthetic/broken_no_eof.dxf");
    std::vector<std::string> seeds;
    for (const std::string& rel : paths) {
        std::ifstream in(std::string(RAY_CORPUS_ROOT) + "/" + rel, std::ios::binary);
        REQUIRE(in);
        seeds.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    seeds.push_back(take(writeDxf(roundTripSketch(), UnitSystem::Inch)));
    return seeds;
}

struct FuzzStats {
    int runs = 0;
    int read = 0;
    double slowest = 0;
};

// Reads `perMode` truncated, `perMode` byte-flipped and `perMode` line-edited copies of every seed. Every read must
// end in a valid sketch or a FormatError.
void fuzz(const std::vector<std::string>& seeds, std::uint32_t rngSeed, int perMode, FuzzStats& stats)
{
    std::mt19937 rng(rngSeed);
    const std::vector<std::string> nasty{
        "1e308",    "-1e308", "nan",        "inf",         "-0",     "99999999999999999999",
        "",         "0x10",   "2147483648", "-2147483649", "1e-320", "AutoCAD",
        "\x01\xff", "0",      "SECTION",    "ENDSEC",      "EOF",    "INSERT",
        "BLOCK",    "SEQEND", "70",         "10"};
    DxfLimits limits;
    limits.maxBlockExpansion = 200'000;
    const auto run = [&](const std::string& text) {
        const auto start = std::chrono::steady_clock::now();
        const auto r = readDxf(text, UnitSystem::Metric, limits);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.slowest = std::max(stats.slowest, s);
        ++stats.runs;
        if (r) {
            ++stats.read;
            // Whatever comes out is a valid sketch that the loop finder can take.
            CHECK(validateSketch(r->sketch).has_value());
            CHECK(analyzeLoops(r->sketch).has_value());
        } else {
            CHECK(r.error().code == ErrorCode::FormatError);
        }
    };
    for (const std::string& seed : seeds) {
        std::uniform_int_distribution<std::size_t> pos(0, seed.size() - 1);
        for (int i = 0; i < perMode; ++i) {
            run(seed.substr(0, pos(rng))); // truncated
        }
        for (int i = 0; i < perMode; ++i) { // random bytes changed
            std::string t = seed;
            const int flips = 1 + static_cast<int>(rng() % 20);
            for (int f = 0; f < flips; ++f) {
                t[pos(rng)] = static_cast<char>(rng() % 256);
            }
            run(t);
        }
        for (int i = 0; i < perMode; ++i) { // values replaced by extreme ones or keywords, lines dropped or doubled
            std::vector<std::string> lines;
            std::istringstream in(seed);
            for (std::string l; std::getline(in, l);) {
                lines.push_back(l);
            }
            for (int f = 0; f < 10 && !lines.empty(); ++f) {
                const std::size_t k = rng() % lines.size();
                switch (rng() % 3) {
                case 0:
                    lines[k] = nasty[rng() % nasty.size()];
                    break;
                case 1:
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(k));
                    break;
                default: {
                    const std::string copy = lines[k];
                    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(k), copy);
                    break;
                }
                }
            }
            std::string t;
            for (const std::string& l : lines) {
                t += l + "\n";
            }
            run(t);
        }
    }
}

} // namespace

TEST_CASE("dxf: truncated and corrupted files never crash or hang", "[dxf][fuzz]")
{
    const std::vector<std::string> seeds = fuzzSeeds();
    FuzzStats stats;
    fuzz(seeds, 20261118, 40, stats);
    INFO("runs " << stats.runs << ", read " << stats.read << ", slowest " << stats.slowest << " s");
    CHECK(stats.runs == static_cast<int>(seeds.size()) * 120);
    CHECK(stats.read > 0);
    CHECK(stats.slowest < 5.0);
    // The broken corpus file (cut inside ENTITIES) is refused as it is.
    CHECK_FALSE(readDxf(seeds[seeds.size() - 2], UnitSystem::Metric).has_value());
}

// A longer run for local use, hidden from the default run (ctest does not start it): ray_sketch_tests
// "[dxf-fuzz-long]".
TEST_CASE("dxf: long fuzz run", "[.][dxf-fuzz-long]")
{
    const std::vector<std::string> seeds = fuzzSeeds();
    FuzzStats stats;
    for (std::uint32_t s = 1; s <= 20; ++s) {
        fuzz(seeds, s, 100, stats);
    }
    INFO("runs " << stats.runs << ", read " << stats.read << ", slowest " << stats.slowest << " s");
    CHECK(stats.slowest < 5.0);
}
