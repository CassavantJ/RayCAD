// Golden files of linking's integer output (task T-051; results identical across OS jobs). Each golden is
// tests/golden/link/<name>.txt: the toText dump of link_test_util.h (one move per line, integers only, '\n' line ends)
// of a linked case shaped like what an operation task hands over: a contour with arc leads at two depths, the same
// with compensation in the control and line leads, a pocket's offset rings at two depths (helix, step-in, stay-down),
// a slot's centre line at three depths (zigzag ramps), facing strokes (zigzag staying down beside the stock, and one
// way retracting), and a thread mill's helical pass with a radial lead-in in a drilled hole. They must be
// byte-identical on Windows (MSVC) and Linux (GCC); no floating-point value reaches them except through roundNm /
// toNm (half to even).
//
// To make them again after a reviewed change: set RAY_UPDATE_LINK_GOLDENS=1 and run ray_cam_link_tests "[golden]",
// then read the diff.
#include "link_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ray::cam::link::test {
namespace {

std::string goldenPath(const std::string& name)
{
    return std::string(RAY_LINK_GOLDEN_DIR) + "/" + name + ".txt";
}

bool updating()
{
    const char* v = std::getenv("RAY_UPDATE_LINK_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

void matchesGolden(const std::string& name, const std::string& text)
{
    if (updating()) {
        std::ofstream out(goldenPath(name), std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(out.good());
    }
    std::ifstream in(goldenPath(name), std::ios::binary);
    REQUIRE(in.good());
    std::stringstream stored;
    stored << in.rdbuf();
    INFO("golden " << name << " differs; produced:\n" << text);
    CHECK(stored.str() == text);
}

Linked linked(const std::vector<Pass>& passes, const Options& o, const Stock& s)
{
    JobContext ctx;
    return take(linkPasses(passes, o, s, ctx));
}

// A rectangle with corners rounded by r, counter-clockwise from (x0 + r, y0).
Path rounded(Coord x0, Coord y0, Coord x1, Coord y1, Coord r)
{
    return closedPath({x0 + r, y0}, {line({x1 - r, y0}), arc({x1, y0 + r}, {x1 - r, y0 + r}, r, true),
                                     line({x1, y1 - r}), arc({x1 - r, y1}, {x1 - r, y1 - r}, r, true),
                                     line({x0 + r, y1}), arc({x0, y1 - r}, {x0 + r, y1 - r}, r, true),
                                     line({x0, y0 + r}), arc({x0 + r, y0}, {x0 + r, y0 + r}, r, true)});
}

} // namespace

TEST_CASE("golden: contour with arc leads at two depths, and with compensation in the control", "[link][golden]")
{
    // The part 40 x 30 at (10, 10); a 6 mm tool runs around it (counter-clockwise, the part on its left: the lead
    // comes from the right). The stock is the part grown by 2 mm, so the leads start in air.
    const Stock stock = boxStock(8 * kMm, 8 * kMm, 52 * kMm, 42 * kMm, 0);
    const Path ring = rounded(7 * kMm, 7 * kMm, 53 * kMm, 43 * kMm, 3 * kMm);
    Regions room = regionOf(rect(-100 * kMm, -100 * kMm, 200 * kMm, 200 * kMm),
                            {geom2d::reverse(rect(10 * kMm, 10 * kMm, 50 * kMm, 40 * kMm))});
    JobContext ctx;
    room = take(geom2d::offset(room, -3 * kMm, ctx));
    std::vector<Pass> passes;
    for (int level = 0; level < 2; ++level) {
        Pass p = passFromPath(geom2d::startAt(ring, 20 * kMm), -3 * kMm * (level + 1), kCutFeed);
        p.cluster = 0;
        p.level = level;
        p.room = room;
        p.leadIn = Lead{LeadKind::Arc, 4 * kMm, 90, 2 * kMm, Side::Right, kMm};
        p.leadOut = p.leadIn;
        passes.push_back(p);
    }
    Options o = baseOptions();
    o.rampAngleDeg = 3;
    matchesGolden("contour_arc_leads", toText(linked(passes, o, stock)));
    for (Pass& p : passes) {
        // G42 (the tool right of the part, which lies on the left): the straight parts switch it on and off.
        for (Move& m : p.moves)
            m.compensation = 2;
        p.leadIn = Lead{LeadKind::Arc, 4 * kMm, 90, 4 * kMm, Side::Right, 0};
        p.leadOut = p.leadIn;
    }
    o.rapidRetract = true;
    matchesGolden("contour_compensation", toText(linked(passes, o, stock)));
}

TEST_CASE("golden: pocket rings at two depths, helix, step-in and stay-down", "[link][golden]")
{
    const Stock stock = boxStock(0, 0, 60 * kMm, 40 * kMm, 0);
    const Regions pocket = regionOf(rect(10 * kMm, 10 * kMm, 45 * kMm, 32 * kMm));
    JobContext ctx;
    const Regions room = take(geom2d::offset(pocket, -3 * kMm, ctx));
    std::vector<Pass> passes;
    for (int level = 0; level < 2; ++level)
        for (int ring = 0; ring < 10; ++ring) {
            const Regions rs = take(geom2d::offset(room, -2'500'000 * ring, ctx));
            if (rs.empty())
                break;
            Pass p = passFromPath(rs[0].outer, -2'500'000 * (level + 1), kCutFeed);
            p.rotatable = true;
            p.cluster = 0;
            p.level = level;
            p.room = room;
            passes.push_back(p);
        }
    Options o = baseOptions();
    o.helixDiameter = 5 * kMm;
    o.helixMinDiameter = 3 * kMm;
    o.rampAngleDeg = 2;
    o.maxStayDown = 12 * kMm;
    o.stepIn = true;
    matchesGolden("pocket_rings", toText(linked(passes, o, stock)));
}

TEST_CASE("golden: slot centre line at three depths with zigzag ramps", "[link][golden]")
{
    const Stock stock = boxStock(0, 0, 60 * kMm, 30 * kMm, 0);
    std::vector<Pass> passes;
    for (int level = 0; level < 3; ++level) {
        Pass p = passFromPath(openPath({10 * kMm, 15 * kMm}, {line({50 * kMm, 15 * kMm})}), -2 * kMm * (level + 1),
                              kCutFeed);
        p.reversible = true;
        p.cluster = 0;
        p.level = level;
        passes.push_back(p);
    }
    Options o = baseOptions(8 * kMm);
    o.entryOrder = {EntryKind::Ramp};
    o.rampAngleDeg = 4;
    o.entryClearance = kMm / 2;
    matchesGolden("slot_ramps", toText(linked(passes, o, stock)));
}

TEST_CASE("golden: facing strokes, zigzag staying down beside the stock and one way retracting", "[link][golden]")
{
    const Stock stock = boxStock(0, 0, 50 * kMm, 30 * kMm, 0);
    std::vector<Pass> zigzag, oneWay;
    for (int k = 0; k < 7; ++k) {
        const Coord y = 2 * kMm + 4'500'000 * k;
        const Pt a{-8 * kMm, y}, b{58 * kMm, y};
        zigzag.push_back(passFromPath(openPath(k % 2 == 0 ? a : b, {line(k % 2 == 0 ? b : a)}), -kMm, kCutFeed));
        oneWay.push_back(passFromPath(openPath(a, {line(b)}), -kMm, kCutFeed));
    }
    Options o = baseOptions(10 * kMm);
    o.keepOrder = true;
    o.maxStayDown = 10 * kMm;
    matchesGolden("face_zigzag", toText(linked(zigzag, o, stock)));
    o.maxStayDown = 100 * kMm;
    matchesGolden("face_one_way", toText(linked(oneWay, o, stock)));
}

TEST_CASE("golden: thread mill helix with a radial lead-in in a drilled hole", "[link][golden]")
{
    // A 12 mm hole drilled to -15 mm (an earlier operation: given as cleared), an internal thread milled by a 6 mm
    // tool on a 5 mm radius from -12 up to -10 (one turn, two half-circle helical arcs), entered from the hole centre.
    const Pt c{20 * kMm, 20 * kMm};
    Stock stock = boxStock(0, 0, 40 * kMm, 40 * kMm, 0);
    stock.cleared.push_back(Cleared{-15 * kMm, regionOf(Path::circle(c, 6 * kMm, true))});
    Pass p;
    p.start = Pt3{c.x + 5 * kMm, c.y, -12 * kMm};
    Move half{};
    half.kind = MoveKind::ArcCCW;
    half.role = MoveRole::Cut;
    half.feedMicronPerMin = 400'000;
    half.i = c.x;
    half.j = c.y;
    half.x = c.x - 5 * kMm;
    half.y = c.y;
    half.z = half.k = -11 * kMm;
    p.moves.push_back(half);
    half.x = c.x + 5 * kMm;
    half.z = half.k = -10 * kMm;
    p.moves.push_back(half);
    p.room = regionOf(Path::circle(c, 5 * kMm, true));
    p.leadIn = Lead{LeadKind::Arc, 2'500'000, 180, 0, Side::Left, 0};
    p.leadOut = p.leadIn;
    Options o = baseOptions();
    o.entryOrder = {};
    matchesGolden("threadmill_helix", toText(linked({p}, o, stock)));
}

} // namespace ray::cam::link::test
