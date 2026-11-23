// Linking (task T-051), unit cases: each lead, ramp and helix kind with its exact geometry, the entry fall-back order
// helix -> ramp -> pre-drill point -> refuse (and plunge), stay-down and step-in links against the cut area, the
// refusals with the cause they name, compensation in the control, and cam.h's public form.
#include "link_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace ray::cam::link::test {
namespace {

Move mv(MoveKind k, MoveRole r, Nm x, Nm y, Nm z, std::int32_t f = 0)
{
    Move m{};
    m.kind = k;
    m.role = r;
    m.x = x;
    m.y = y;
    m.z = z;
    m.feedMicronPerMin = f;
    return m;
}
Move arcMv(MoveKind k, MoveRole r, Nm x, Nm y, Nm z, Nm cx, Nm cy, std::int32_t f)
{
    Move m = mv(k, r, x, y, z, f);
    m.i = cx;
    m.j = cy;
    m.k = z;
    return m;
}
void sameMoves(const std::vector<Move>& got, const std::vector<Move>& want)
{
    INFO("got:\n" << toText(Linked{{}, got, {}, {}, {}, {}, 0, 0, {}}));
    REQUIRE(got.size() == want.size());
    for (std::size_t k = 0; k < got.size(); ++k) {
        INFO("move " << k);
        CHECK(got[k].kind == want[k].kind);
        CHECK(got[k].role == want[k].role);
        CHECK(got[k].x == want[k].x);
        CHECK(got[k].y == want[k].y);
        CHECK(got[k].z == want[k].z);
        CHECK(got[k].feedMicronPerMin == want[k].feedMicronPerMin);
        CHECK(got[k].compensation == want[k].compensation);
        if (got[k].kind == MoveKind::ArcCW || got[k].kind == MoveKind::ArcCCW) {
            CHECK(got[k].i == want[k].i);
            CHECK(got[k].j == want[k].j);
        }
    }
}
Linked run(std::vector<Pass> passes, const Options& o, const Stock& s)
{
    JobContext ctx;
    return take(linkPasses(passes, o, s, ctx));
}
Error refused(std::vector<Pass> passes, const Options& o, const Stock& s)
{
    JobContext ctx;
    auto r = linkPasses(passes, o, s, ctx);
    REQUIRE(!r.has_value());
    return r.error();
}
Pass linePass(Pt a, Pt b, Nm z)
{
    return passFromPath(openPath(a, {line(b)}), z, kCutFeed);
}
bool contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}
// The sweep of an arc move from `from`, radians, in its direction.
double sweepOfMove(Pt from, const Move& m)
{
    const double a0 = std::atan2(static_cast<double>(from.y - m.j), static_cast<double>(from.x - m.i));
    const double a1 = std::atan2(static_cast<double>(m.y - m.j), static_cast<double>(m.x - m.i));
    double s = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
    while (s <= 0)
        s += 2 * kPiD;
    if (from.x == m.x && from.y == m.y)
        s = 2 * kPiD;
    return s;
}

const Stock kNoStock{};

} // namespace

TEST_CASE("link: heights out of order, a retract under the stock and a pass above the feed plane are refused", "[link]")
{
    const Stock stock = boxStock(0, 0, 50 * kMm, 30 * kMm, 0);
    const Pass p = linePass({-5 * kMm, 10 * kMm}, {55 * kMm, 10 * kMm}, -kMm);
    Options o = baseOptions();
    o.retract = 12 * kMm;
    Error e = refused({p}, o, stock);
    CHECK(e.code == ErrorCode::InvalidArgument);
    CHECK(contains(e.message, "Heights out of order"));
    o = baseOptions();
    o.feedPlane = -2 * kMm;
    o.retract = -kMm;
    e = refused({p}, o, stock);
    CHECK(contains(e.message, "below the top of the stock"));
    o = baseOptions();
    e = refused({linePass({0, 0}, {kMm, 0}, 2 * kMm)}, o, stock);
    CHECK(contains(e.message, "above the feed plane"));
    Pass rapid = p;
    rapid.moves[0].kind = MoveKind::Rapid;
    e = refused({rapid}, o, stock);
    CHECK(contains(e.message, "is a rapid"));
    Pass nofeed = p;
    nofeed.moves[0].feedMicronPerMin = 0;
    CHECK(contains(refused({nofeed}, o, stock).message, "has no feed"));
    o.toolRadius = 0;
    CHECK(contains(refused({p}, o, stock).message, "tool's radius"));
}

TEST_CASE("link: a pass that starts outside the stock is entered from above in air", "[link]")
{
    // Facing with overhang: the stroke starts beyond the stock, so the tool feeds down beside it.
    const Stock stock = boxStock(0, 0, 50 * kMm, 30 * kMm, 0);
    Options o = baseOptions();
    o.keepOrder = true;
    const Linked l = run({linePass({-5 * kMm, 10 * kMm}, {55 * kMm, 10 * kMm}, -kMm)}, o, stock);
    using K = MoveKind;
    using R = MoveRole;
    sameMoves(l.moves, {mv(K::Rapid, R::Approach, -5 * kMm, 10 * kMm, 10 * kMm),
                        mv(K::Rapid, R::Approach, -5 * kMm, 10 * kMm, kMm),
                        mv(K::Linear, R::Approach, -5 * kMm, 10 * kMm, -kMm, 300'000),
                        mv(K::Linear, R::Cut, 55 * kMm, 10 * kMm, -kMm, kCutFeed),
                        mv(K::Linear, R::Retract, 55 * kMm, 10 * kMm, kMm, 1'200'000),
                        mv(K::Rapid, R::Retract, 55 * kMm, 10 * kMm, 10 * kMm)});
    CHECK(l.start == Pt3{-5 * kMm, 10 * kMm, 10 * kMm});
    REQUIRE(l.entered.size() == 1);
    CHECK(l.entered[0] == Entered::Free);
    CHECK(l.rapidLength == 9.0 * kMm + 9.0 * kMm);
    // A rapid retract goes straight up instead.
    o.rapidRetract = true;
    const Linked r = run({linePass({-5 * kMm, 10 * kMm}, {55 * kMm, 10 * kMm}, -kMm)}, o, stock);
    CHECK(r.moves.back().kind == MoveKind::Rapid);
    CHECK(r.moves.back().z == 10 * kMm);
    CHECK(r.moves[r.moves.size() - 2].kind == MoveKind::Linear);
    CHECK(r.moves[r.moves.size() - 2].role == MoveRole::Cut);
}

TEST_CASE("link: tangent arc leads with straight parts, exact geometry", "[link][lead]")
{
    Pass p = linePass({0, 0}, {50 * kMm, 0}, -2 * kMm);
    p.room = unbounded();
    p.leadIn = Lead{LeadKind::Arc, 5 * kMm, 90, 3 * kMm, Side::Left, 0};
    p.leadOut = p.leadIn;
    Options o = baseOptions();
    const Linked l = run({p}, o, kNoStock);
    using K = MoveKind;
    using R = MoveRole;
    sameMoves(l.moves, {mv(K::Rapid, R::Approach, -5 * kMm, 8 * kMm, 10 * kMm),
                        mv(K::Rapid, R::Approach, -5 * kMm, 8 * kMm, kMm),
                        mv(K::Linear, R::Approach, -5 * kMm, 8 * kMm, -2 * kMm, 300'000),
                        mv(K::Linear, R::LeadIn, -5 * kMm, 5 * kMm, -2 * kMm, 800'000),
                        arcMv(K::ArcCCW, R::LeadIn, 0, 0, -2 * kMm, 0, 5 * kMm, 800'000),
                        mv(K::Linear, R::Cut, 50 * kMm, 0, -2 * kMm, kCutFeed),
                        arcMv(K::ArcCCW, R::LeadOut, 55 * kMm, 5 * kMm, -2 * kMm, 50 * kMm, 5 * kMm, 900'000),
                        mv(K::Linear, R::LeadOut, 55 * kMm, 8 * kMm, -2 * kMm, 900'000),
                        mv(K::Linear, R::Retract, 55 * kMm, 8 * kMm, kMm, 1'200'000),
                        mv(K::Rapid, R::Retract, 55 * kMm, 8 * kMm, 10 * kMm)});
    // On the right: clockwise, mirrored.
    p.leadIn = Lead{LeadKind::Arc, 5 * kMm, 90, 0, Side::Right, 0};
    p.leadOut = Lead{};
    const Linked r = run({p}, o, kNoStock);
    CHECK(r.start == Pt3{-5 * kMm, -5 * kMm, 10 * kMm});
    const Move& in = r.moves[3];
    CHECK(in.kind == MoveKind::ArcCW);
    CHECK(in.i == 0);
    CHECK(in.j == -5 * kMm);
    CHECK(in.x == 0);
    CHECK(in.y == 0);
    // A half-circle lead of radius R/2 from the centre of a circle of radius R (thread milling's radial lead-in).
    const Coord R0 = 8 * kMm;
    Pass circle = passFromPath(geom2d::Path::circle({0, 0}, R0, true), -kMm, kCutFeed);
    circle.room = unbounded();
    circle.leadIn = Lead{LeadKind::Arc, R0 / 2, 180, 0, Side::Left, 0};
    const Linked c = run({circle}, o, kNoStock);
    CHECK(c.start == Pt3{0, 0, 10 * kMm});
    CHECK(c.moves[3].kind == MoveKind::ArcCCW);
    CHECK(c.moves[3].i == R0 / 2);
    CHECK(c.moves[3].j == 0);
    CHECK(c.moves[3].x == R0);
    CHECK(c.moves[3].y == 0);
}

TEST_CASE("link: line leads switch compensation in the control on and off", "[link][lead]")
{
    Pass p = linePass({0, 0}, {50 * kMm, 0}, -2 * kMm);
    for (Move& m : p.moves)
        m.compensation = 1;
    p.room = unbounded();
    p.leadIn = Lead{LeadKind::Line, 0, 90, 4 * kMm, Side::Left, 0};
    p.leadOut = p.leadIn;
    const Linked l = run({p}, baseOptions(), kNoStock);
    using K = MoveKind;
    using R = MoveRole;
    Move in = mv(K::Linear, R::LeadIn, 0, 0, -2 * kMm, 800'000);
    in.compensation = 1;
    Move cut = mv(K::Linear, R::Cut, 50 * kMm, 0, -2 * kMm, kCutFeed);
    cut.compensation = 1;
    sameMoves(l.moves, {mv(K::Rapid, R::Approach, -4 * kMm, 0, 10 * kMm), mv(K::Rapid, R::Approach, -4 * kMm, 0, kMm),
                        mv(K::Linear, R::Approach, -4 * kMm, 0, -2 * kMm, 300'000), in, cut,
                        mv(K::Linear, R::LeadOut, 54 * kMm, 0, -2 * kMm, 900'000),
                        mv(K::Linear, R::Retract, 54 * kMm, 0, kMm, 1'200'000),
                        mv(K::Rapid, R::Retract, 54 * kMm, 0, 10 * kMm)});
    // An arc lead without a straight part cannot switch it.
    p.leadIn = Lead{LeadKind::Arc, 5 * kMm, 90, 0, Side::Left, 0};
    const Error e = refused({p}, baseOptions(), kNoStock);
    CHECK(e.code == ErrorCode::ToolpathFailed);
    CHECK(contains(e.message, "needs a straight lead-in"));
    // Nor can a ramp or a helix enter such a pass.
    p.leadIn = Lead{LeadKind::Line, 0, 90, 4 * kMm, Side::Left, 0};
    Options o = baseOptions();
    o.rampAngleDeg = 3;
    o.helixDiameter = 4 * kMm;
    const Error e2 = refused({p}, o, boxStock(-20 * kMm, -20 * kMm, 80 * kMm, 20 * kMm, 0));
    CHECK(contains(e2.message, "cutter compensation in the control is on"));
}

TEST_CASE("link: a lead that does not fit its room is made smaller, then refused", "[link][lead]")
{
    Pass p = linePass({0, 0}, {50 * kMm, 0}, -2 * kMm);
    p.room = regionOf(rect(-3 * kMm, -10 * kMm, 60 * kMm, 10 * kMm));
    p.leadIn = Lead{LeadKind::Arc, 5 * kMm, 90, 0, Side::Left, kMm};
    const Linked l = run({p}, baseOptions(), kNoStock);
    CHECK(l.start == Pt3{-2'500'000, 2'500'000, 10 * kMm});
    REQUIRE(l.warnings.size() == 1);
    CHECK(contains(l.warnings[0], "radius 2.5 mm instead of 5 mm"));
    p.leadIn.minRadius = 0;
    const Error e = refused({p}, baseOptions(), kNoStock);
    CHECK(e.code == ErrorCode::ToolpathFailed);
    CHECK(contains(e.message, "does not fit the room"));
    // Without a room a lead cannot be checked.
    p.room.reset();
    CHECK(contains(refused({p}, baseOptions(), kNoStock).message, "no room"));
}

namespace {
// A 20 mm square ring from (0, 0) counter-clockwise at z, in a big stock.
Pass squareRing(Coord side, Nm z)
{
    Pass p = passFromPath(rect(0, 0, side, side), z, kCutFeed);
    return p;
}
const Stock kBig = boxStock(-50 * kMm, -50 * kMm, 50 * kMm, 50 * kMm, 0);

struct HelixCheck {
    std::size_t first = 0, count = 0;
    double sweep = 0;
};
// The helix moves (role Ramp, z falling) after the approach descent; checks circle, slope and continuity.
HelixCheck checkHelix(const Linked& l, Pt c, Coord r, double angleDeg)
{
    HelixCheck h;
    std::size_t k = 0;
    while (k < l.moves.size() && l.moves[k].role != MoveRole::Ramp)
        ++k;
    REQUIRE(k < l.moves.size());
    h.first = k;
    Pt3 prev{l.moves[k - 1].x, l.moves[k - 1].y, l.moves[k - 1].z};
    const double tanA = std::tan(angleDeg * kPiD / 180);
    for (; k < l.moves.size() && l.moves[k].role == MoveRole::Ramp; ++k) {
        const Move& m = l.moves[k];
        REQUIRE((m.kind == MoveKind::ArcCCW || m.kind == MoveKind::ArcCW));
        CHECK(m.i == c.x);
        CHECK(m.j == c.y);
        CHECK(std::abs(std::hypot(static_cast<double>(m.x - c.x), static_cast<double>(m.y - c.y)) -
                       static_cast<double>(r)) <= 1.0);
        const double sweep = sweepOfMove({prev.x, prev.y}, m);
        CHECK(sweep <= kPiD + 1e-9);
        if (m.z != prev.z) {
            const double slope = static_cast<double>(prev.z - m.z) / (static_cast<double>(r) * sweep);
            CHECK(slope <= tanA);
            CHECK(slope >= tanA * (1 - 3e-4));
            h.sweep += sweep;
        }
        CHECK(m.z <= prev.z);
        prev = Pt3{m.x, m.y, m.z};
        ++h.count;
    }
    return h;
}
} // namespace

TEST_CASE("link: a helix enters a closed pass in material, exact geometry", "[link][helix]")
{
    Pass p = squareRing(20 * kMm, -3 * kMm);
    p.room = unbounded();
    Options o = baseOptions();
    o.helixDiameter = 8 * kMm;
    o.helixMinDiameter = 4 * kMm;
    o.rampAngleDeg = 3;
    const Linked l = run({p}, o, kBig);
    REQUIRE(l.entered[0] == Entered::Helix);
    CHECK(l.helixDiameter[0] == 8 * kMm);
    // Counter-clockwise (climb with M3) about the point 4 mm left of the start direction (+X).
    const Pt c{0, 4 * kMm};
    const HelixCheck h = checkHelix(l, c, 4 * kMm, 3);
    // It descends from the stock top (0) to -3 mm: 3 mm / tan 3 deg, 0.01 % longer, around a 4 mm radius.
    const double length = 3.0 * kMm / std::tan(3 * kPiD / 180) * (1 + kRampSlack);
    CHECK(std::abs(h.sweep - length / (4.0 * kMm)) < 1e-6);
    // The approach feeds to the stock top at the helix start, which lies that sweep before the pass start.
    const Move& down = l.moves[h.first - 1];
    CHECK(down.role == MoveRole::Approach);
    CHECK(down.z == 0);
    const double a0 = -kPiD / 2 - length / (4.0 * kMm);
    CHECK(std::abs(static_cast<double>(down.x) - 4.0 * kMm * std::cos(a0)) <= 1.0);
    CHECK(std::abs(static_cast<double>(down.y) - (4.0 * kMm + 4.0 * kMm * std::sin(a0))) <= 1.0);
    // The last sloped move ends at the pass start at depth, then a flat circle at depth (two half circles).
    const std::size_t flat = h.first + h.count - 2;
    CHECK(l.moves[flat - 1].x == 0);
    CHECK(l.moves[flat - 1].y == 0);
    CHECK(l.moves[flat - 1].z == -3 * kMm);
    CHECK(l.moves[flat].z == -3 * kMm);
    CHECK(l.moves[flat].x == 0);
    CHECK(l.moves[flat].y == 8 * kMm);
    CHECK(l.moves[flat + 1].x == 0);
    CHECK(l.moves[flat + 1].y == 0);
    CHECK(l.moves[flat + 2].role == MoveRole::Cut);
    // Entry clearance: the helix starts that much above the material.
    o.entryClearance = kMm / 2;
    const Linked lc = run({p}, o, kBig);
    const HelixCheck hc = checkHelix(lc, c, 4 * kMm, 3);
    CHECK(lc.moves[hc.first - 1].z == kMm / 2);
}

TEST_CASE("link: a helix that does not fit is made smaller, down to its minimum", "[link][helix]")
{
    Pass p = squareRing(20 * kMm, -3 * kMm);
    p.room = regionOf(rect(-3'200'000, -500'000, 30 * kMm, 30 * kMm));
    Options o = baseOptions();
    o.helixDiameter = 8 * kMm;
    o.helixMinDiameter = 4 * kMm;
    o.rampAngleDeg = 3;
    const Linked l = run({p}, o, kBig);
    REQUIRE(l.entered[0] == Entered::Helix);
    CHECK(l.helixDiameter[0] == 6 * kMm); // 8, then 6 (0.75 x 8), fits x >= -3
    checkHelix(l, Pt{0, 3 * kMm}, 3 * kMm, 3);
}

TEST_CASE("link: entry falls back helix -> ramp -> pre-drilled point -> refuse", "[link][entry]")
{
    Pass p = squareRing(12 * kMm, -3 * kMm);
    p.room = regionOf(rect(-kMm, -kMm, 13 * kMm, 13 * kMm)); // no helix of 4 mm fits
    Options o = baseOptions();
    o.helixDiameter = 8 * kMm;
    o.helixMinDiameter = 4 * kMm;
    o.rampAngleDeg = 3;

    SECTION("ramp along the closed pass, ending at its start")
    {
        const Linked l = run({p}, o, kBig);
        REQUIRE(l.entered[0] == Entered::Ramp);
        // 3 mm at 3 deg needs 57.25 mm: one lap of 48 mm and 9.25 mm before it, from (0, 9.25) down the left side.
        const double need = 3.0 * kMm / std::tan(3 * kPiD / 180) * (1 + kRampSlack);
        const double rest = need - 48.0 * kMm;
        std::size_t k = 0;
        while (l.moves[k].role != MoveRole::Ramp)
            ++k;
        const Move& down = l.moves[k - 1];
        CHECK(down.x == 0);
        CHECK(std::abs(static_cast<double>(down.y) - rest) <= 1.0);
        CHECK(down.z == 0);
        Pt3 prev{down.x, down.y, down.z};
        double run = 0;
        const double tanA = std::tan(3 * kPiD / 180);
        for (; l.moves[k].role == MoveRole::Ramp; ++k) {
            const Move& m = l.moves[k];
            CHECK(m.kind == MoveKind::Linear);
            const double d = std::hypot(static_cast<double>(m.x - prev.x), static_cast<double>(m.y - prev.y));
            CHECK(static_cast<double>(prev.z - m.z) <= d * tanA + 1e-6);
            run += d;
            prev = Pt3{m.x, m.y, m.z};
        }
        CHECK(std::abs(run - need) < 2.0);
        CHECK(prev == Pt3{0, 0, -3 * kMm});
        CHECK(l.moves[k].role == MoveRole::Cut);
    }
    SECTION("a pre-drilled point when the ramp is not allowed")
    {
        o.entryOrder = {EntryKind::Helix, EntryKind::PreDrill};
        o.preDrills = {PreDrill{{0, 0}, 8 * kMm, -5 * kMm}};
        const Linked l = run({p}, o, kBig);
        REQUIRE(l.entered[0] == Entered::PreDrill);
        const Move& plunge = l.moves[l.passMove[0] - 1];
        CHECK(plunge.role == MoveRole::Plunge);
        CHECK(plunge.kind == MoveKind::Linear);
        CHECK(plunge.z == -3 * kMm);
        CHECK(plunge.feedMicronPerMin == 300'000);
        CHECK(l.moves[l.passMove[0] - 2].z == 0);
    }
    SECTION("refused, naming why each failed and the room a helix needs")
    {
        o.entryOrder = {EntryKind::Helix, EntryKind::PreDrill};
        o.preDrills = {PreDrill{{30 * kMm, 30 * kMm}, 8 * kMm, -5 * kMm}, PreDrill{{0, 0}, 8 * kMm, -2 * kMm}};
        const Error e = refused({p}, o, kBig);
        CHECK(e.code == ErrorCode::ToolpathFailed);
        INFO(e.message);
        CHECK(contains(e.message, "Pass 1 cannot be entered at (0, 0), Z -3 mm"));
        CHECK(contains(e.message, "helix: a helix of 8 mm down to 4 mm does not fit the room"));
        CHECK(contains(e.message, "pre-drilled point: no pre-drilled point"));
        CHECK(contains(e.message, "A helix needs a room at least 10 mm wide"));
    }
    SECTION("a plunge only with a centre-cutting tool")
    {
        o.entryOrder = {EntryKind::Plunge};
        CHECK(contains(refused({p}, o, kBig).message, "plunge: the tool does not cut at its centre"));
        o.centreCutting = true;
        const Linked l = run({p}, o, kBig);
        CHECK(l.entered[0] == Entered::Plunge);
        CHECK(l.moves[l.passMove[0] - 1].role == MoveRole::Plunge);
    }
    SECTION("no ramp angle, no helix diameter")
    {
        o.rampAngleDeg = 0;
        o.helixDiameter = 0;
        o.helixMinDiameter = 0;
        const Error e = refused({p}, o, kBig);
        CHECK(contains(e.message, "helix: no helix diameter is set"));
        CHECK(contains(e.message, "ramp: no ramp angle is set"));
    }
}

TEST_CASE("link: a zigzag ramp along an open pass, exact turning point", "[link][ramp]")
{
    Pass p = linePass({0, 0}, {60 * kMm, 0}, -2 * kMm);
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Ramp};
    o.rampAngleDeg = 5;
    const Linked l = run({p}, o, kBig);
    REQUIRE(l.entered[0] == Entered::Ramp);
    // 2 mm at 5 deg: L = 2 / tan 5 (+0.01 %); two legs of L / 2 (more than the 6 mm minimum run).
    const double need = 2.0 * kMm / dmath::tan(5 * kPiD / 180.0) * (1.0 + kRampSlack);
    const double leg = need / 2;
    const Nm turn = geom2d::roundNm(60.0 * kMm * (leg / (60.0 * kMm)));
    using K = MoveKind;
    using R = MoveRole;
    sameMoves(l.moves,
              {mv(K::Rapid, R::Approach, 0, 0, 10 * kMm), mv(K::Rapid, R::Approach, 0, 0, kMm),
               mv(K::Linear, R::Approach, 0, 0, 0, 300'000), mv(K::Linear, R::Ramp, turn, 0, -kMm, 500'000),
               mv(K::Linear, R::Ramp, 0, 0, -2 * kMm, 500'000), mv(K::Linear, R::Cut, 60 * kMm, 0, -2 * kMm, kCutFeed),
               mv(K::Linear, R::Retract, 60 * kMm, 0, kMm, 1'200'000),
               mv(K::Rapid, R::Retract, 60 * kMm, 0, 10 * kMm)});
    CHECK(std::abs(static_cast<double>(turn) - leg) <= 1.0);
    // A short pass: four legs of its whole length.
    const Linked s = run({linePass({0, 0}, {8 * kMm, 0}, -2 * kMm)}, o, kBig);
    REQUIRE(s.entered[0] == Entered::Ramp);
    int legs = 0;
    for (const Move& m : s.moves)
        legs += m.role == MoveRole::Ramp ? 1 : 0;
    CHECK(legs == 4); // ceil(22.86 / 8) = 3, made even
    // Shorter than the minimum run (the tool diameter): refused.
    const Error e = refused({linePass({0, 0}, {4 * kMm, 0}, -2 * kMm)}, o, kBig);
    CHECK(contains(e.message, "ramp: the cut offers 4 mm for the ramp, which needs a run of at least 6 mm"));
    o.minRampRun = 3 * kMm;
    CHECK(run({linePass({0, 0}, {4 * kMm, 0}, -2 * kMm)}, o, kBig).entered[0] == Entered::Ramp);
}

TEST_CASE("link: a second depth enters from the depth above (multi-depth)", "[link][entry]")
{
    // Two depths of one ring: the second is entered from the first's floor, which the first cut.
    Pass a = squareRing(20 * kMm, -2 * kMm);
    Pass b = squareRing(20 * kMm, -4 * kMm);
    a.cluster = b.cluster = 0;
    a.level = 0;
    b.level = 1;
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Ramp};
    o.rampAngleDeg = 3;
    o.keepOrder = true;
    const Linked l = run({a, b}, o, kBig);
    REQUIRE(l.entered.size() == 2);
    CHECK(l.entered[1] == Entered::Ramp);
    // The second ramp starts at the first depth: the approach feeds through the cut area down to -2 mm.
    std::size_t k = l.passMove[0];
    while (l.moves[k].role != MoveRole::Ramp)
        ++k;
    CHECK(l.moves[k - 1].role == MoveRole::Approach);
    CHECK(l.moves[k - 1].z == -2 * kMm);
    // 2 mm at 3 deg is 38.2 mm, less than the 80 mm ring: no lap.
    const double need = 2.0 * kMm / std::tan(3 * kPiD / 180) * (1 + kRampSlack);
    // It starts that far before the pass start along the ring: on the top side, going from (20, 20) to (0, 20).
    CHECK(std::abs(static_cast<double>(l.moves[k - 1].x) - (need - 20.0 * kMm)) <= 1.0);
    CHECK(l.moves[k - 1].y == 20 * kMm);
}

TEST_CASE("link: stayDownEntry keeps a slot's next depth in the cut (T-055)", "[link][staydown]")
{
    // A slot: one open reversible pass per depth along (0, 0) -> (30, 0), at Z -1 and -2, ramped in (5 degrees, 0.5 mm
    // entry clearance). Without the option the second depth retracts to the retract plane and comes back; with it the
    // tool stays at the end of the first depth, feeds up to the ramp's top (Z -0.5) and ramps from there.
    Pass a = linePass({0, 0}, {30 * kMm, 0}, -kMm);
    Pass b = linePass({0, 0}, {30 * kMm, 0}, -2 * kMm);
    a.reversible = b.reversible = true;
    a.cluster = b.cluster = 0;
    a.level = 0;
    b.level = 1;
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Ramp};
    o.rampAngleDeg = 5;
    o.entryClearance = kMm / 2;
    o.maxStayDown = 6 * kMm;
    const Linked off = run({a, b}, o, kBig);
    int retracts = 0;
    for (const Move& m : off.moves) {
        retracts += m.role == MoveRole::Retract ? 1 : 0;
    }
    CHECK(retracts == 4); // feed up and rapid up after each pass
    o.stayDownEntry = true;
    const Linked on = run({a, b}, o, kBig);
    REQUIRE(on.entered.size() == 2);
    CHECK(on.entered[1] == Entered::Ramp);
    CHECK(on.order[1].reversed != on.order[0].reversed); // the second depth runs back
    // After the first pass (ending at its far end, Z -1): one Link feed straight up to Z -0.5, then the ramp.
    std::size_t k = on.passMove[0];
    while (k < on.moves.size() && on.moves[k].role == MoveRole::Cut)
        ++k;
    REQUIRE(k < on.moves.size());
    const Move& up = on.moves[k];
    CHECK(up.role == MoveRole::Link);
    CHECK(up.kind == MoveKind::Linear);
    CHECK(up.z == -kMm / 2);
    CHECK(up.x == on.moves[k - 1].x);
    CHECK(up.feedMicronPerMin == 1'200'000);
    CHECK(on.moves[k + 1].role == MoveRole::Ramp);
    retracts = 0;
    for (const Move& m : on.moves) {
        retracts += m.role == MoveRole::Retract ? 1 : 0;
    }
    CHECK(retracts == 2); // only the final one
    CHECK(on.rapidLength < off.rapidLength);
    // The entry's start farther than maxStayDown: a retract as before.
    o.maxStayDown = 0;
    CHECK(run({a, b}, o, kBig).moves.size() == off.moves.size());
    // A pass beside the first, across uncut stock: its ramp starts in uncut material at the stock top (Z 0) plus the
    // entry clearance, so the move there runs at Z 0.5, over the stock, never through it.
    Pass far = linePass({0, 10 * kMm}, {30 * kMm, 10 * kMm}, -2 * kMm);
    far.reversible = true;
    far.cluster = 0;
    far.level = 1;
    o.maxStayDown = 20 * kMm;
    const Linked across = run({a, far}, o, kBig);
    int sideways = 0;
    for (std::size_t m = 1; m < across.moves.size(); ++m) {
        const Move& mv = across.moves[m];
        const Move& prev = across.moves[m - 1];
        if (mv.role == MoveRole::Link && (mv.x != prev.x || mv.y != prev.y)) {
            ++sideways;
            CHECK(mv.z == kMm / 2);
            CHECK(prev.z == kMm / 2);
        }
    }
    CHECK(sideways == 1);
}

TEST_CASE("link: stay down only through cut area or air, else retract", "[link][staydown]")
{
    const Stock stock = boxStock(0, 0, 50 * kMm, 30 * kMm, 0);
    Options o = baseOptions();
    o.keepOrder = true;
    o.maxStayDown = 40 * kMm;
    o.entryOrder = {EntryKind::Plunge};
    o.centreCutting = true;
    const Pass p1 = linePass({-5 * kMm, 10 * kMm}, {55 * kMm, 10 * kMm}, -kMm);
    const Pass p2 = linePass({55 * kMm, 14 * kMm}, {-5 * kMm, 14 * kMm}, -kMm);
    SECTION("beside the stock (facing strokes)")
    {
        const Linked l = run({p1, p2}, o, stock);
        CHECK(l.entered[1] == Entered::StayDown);
        const Move& link = l.moves[l.passMove[1] - 1];
        CHECK(link.role == MoveRole::Link);
        CHECK(link.kind == MoveKind::Linear);
        CHECK(link.x == 55 * kMm);
        CHECK(link.y == 14 * kMm);
        CHECK(link.z == -kMm);
        CHECK(link.feedMicronPerMin == 1'200'000);
        o.stayDownLift = kMm / 2;
        const Linked lifted = run({p1, p2}, o, stock);
        const std::size_t s = lifted.passMove[1];
        CHECK(lifted.moves[s - 3].z == -kMm / 2);
        CHECK(lifted.moves[s - 3].x == 55 * kMm);
        CHECK(lifted.moves[s - 3].y == 10 * kMm);
        CHECK(lifted.moves[s - 2].z == -kMm / 2);
        CHECK(lifted.moves[s - 2].y == 14 * kMm);
        CHECK(lifted.moves[s - 1].z == -kMm);
    }
    SECTION("back along the cut it just made")
    {
        const Pass back = linePass({20 * kMm, 10 * kMm}, {30 * kMm, 10 * kMm}, -kMm);
        const Linked l = run({p1, back}, o, stock);
        CHECK(l.entered[1] == Entered::StayDown);
    }
    SECTION("not across uncut stock")
    {
        const Pass inside = linePass({20 * kMm, 20 * kMm}, {30 * kMm, 20 * kMm}, -kMm);
        const Linked l = run({p1, inside}, o, stock);
        CHECK(l.entered[1] == Entered::Plunge);
        const Move& up = l.moves[l.passMove[0] + 1];
        CHECK(up.role == MoveRole::Retract);
    }
    SECTION("not farther than maxStayDown")
    {
        o.maxStayDown = 3 * kMm;
        const Linked l = run({p1, p2}, o, stock);
        CHECK(l.entered[1] == Entered::Free);
    }
}

TEST_CASE("link: step in to the next pocket ring with the tool centre in cut area", "[link][staydown]")
{
    const Stock stock = boxStock(0, 0, 50 * kMm, 30 * kMm, 0);
    const Pt c{25 * kMm, 15 * kMm};
    Pass inner = passFromPath(geom2d::Path::circle(c, 3 * kMm, true), -2 * kMm, kCutFeed);
    Pass outer = passFromPath(geom2d::Path::circle(c, 5'500'000, true), -2 * kMm, kCutFeed);
    inner.room = outer.room = unbounded();
    inner.cluster = outer.cluster = 0;
    Options o = baseOptions();
    o.keepOrder = true;
    o.helixDiameter = 4 * kMm;
    o.rampAngleDeg = 3;
    o.maxStayDown = 6 * kMm;
    o.stepIn = true;
    const Linked l = run({inner, outer}, o, stock);
    REQUIRE(l.entered[1] == Entered::Step);
    const Move& step = l.moves[l.passMove[1] - 1];
    CHECK(step.role == MoveRole::Cut);
    CHECK(step.x == 30'500'000);
    CHECK(step.y == 15 * kMm);
    CHECK(step.z == -2 * kMm);
    // A ring two tool radii out: the centre would enter material, so it retracts and enters by itself.
    Pass far = passFromPath(geom2d::Path::circle(c, 7 * kMm, true), -2 * kMm, kCutFeed);
    far.room = unbounded();
    far.cluster = 0;
    const Linked f = run({inner, far}, o, stock);
    CHECK(f.entered[1] == Entered::Helix);
    o.stepIn = false;
    CHECK(run({inner, outer}, o, stock).entered[1] == Entered::Helix);
}

TEST_CASE("link: the order respects sequences and cluster levels; keepOrder keeps it", "[link][order]")
{
    std::vector<Pass> passes;
    for (int k = 0; k < 6; ++k) {
        Pass p = linePass({k * 10 * kMm, 0}, {k * 10 * kMm + 5 * kMm, 0}, -kMm * (1 + k % 2));
        p.cluster = k % 2;
        p.level = 2 - k / 2;
        p.sequence = k == 5 ? 1 : 0;
        passes.push_back(p);
    }
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Plunge};
    o.centreCutting = true;
    JobContext ctx;
    const std::vector<Placement> order = take(orderPasses(passes, o, ctx));
    std::vector<std::size_t> idx;
    for (const Placement& p : order)
        idx.push_back(p.pass);
    CHECK(validOrder(passes, idx));
    CHECK(idx.back() == 5);
    o.keepOrder = true;
    std::vector<Pass> plain = passes;
    for (Pass& p : plain) {
        p.sequence = 0;
        p.cluster = -1;
        p.level = 0;
    }
    const Linked l = run(plain, o, kNoStock);
    for (std::size_t k = 0; k < l.order.size(); ++k)
        CHECK(l.order[k].pass == k);
}

TEST_CASE("link: a rotatable ring starts where the tool is nearest", "[link][order]")
{
    Pass a = linePass({0, 0}, {10 * kMm, 0}, -kMm);
    Pass ring = passFromPath(rect(20 * kMm, -5 * kMm, 30 * kMm, 5 * kMm), -kMm, kCutFeed);
    ring.rotatable = true;
    Options o = baseOptions();
    o.entryOrder = {EntryKind::Plunge};
    o.centreCutting = true;
    o.from = Pt{0, 0};
    const Linked l = run({a, ring}, o, kNoStock);
    REQUIRE(l.order.size() == 2);
    CHECK(l.order[0].pass == 0);
    CHECK(l.order[1].pass == 1);
    // From (10, 0) the nearest point of the ring is (20, 0) on its left side, 35 mm along it from (20, -5).
    CHECK(l.order[1].startS == 35.0 * kMm);
    const Move& down = l.moves[l.passMove[1] - 1];
    CHECK(down.x == 20 * kMm);
    CHECK(down.y == 0);
}

TEST_CASE("link: cancelled before it starts", "[link]")
{
    CancelSource src;
    src.cancel();
    JobContext ctx{src.token(), {}};
    Options o = baseOptions();
    std::vector<Pass> passes{linePass({0, 0}, {10 * kMm, 0}, -kMm), linePass({0, 5 * kMm}, {10 * kMm, 5 * kMm}, -kMm)};
    auto r = linkPasses(passes, o, kNoStock, ctx);
    REQUIRE(!r.has_value());
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("link: cam.h's public form", "[link][api]")
{
    ResolvedGeometry geom;
    geom.stockBounds = Box3{{0, 0, -20}, {50, 30, 0}};
    LinkPass p;
    p.startX = 10 * kMm;
    p.startY = 10 * kMm;
    p.startZ = -kMm;
    p.moves.push_back(mv(MoveKind::Linear, MoveRole::Cut, 40 * kMm, 10 * kMm, -kMm, kCutFeed));
    Heights h;
    h.clearance = 10;
    h.retract = 5;
    h.feedPlane = 1;
    LinkOptions o;
    o.toolDiameter = 6;
    o.centreCutting = true;
    o.entryOrder = {EntryKind::Helix, EntryKind::Plunge};
    o.helixDiameter = 4;
    o.rampAngleDeg = 3;
    o.feedPlunge = 250;
    JobContext ctx;
    const std::vector<Move> moves = take(ray::cam::linkPasses(std::vector<LinkPass>{p}, h, o, geom, ctx));
    REQUIRE(moves.size() == 7);
    CHECK(moves[0].kind == MoveKind::Rapid);
    CHECK(moves[0].z == 10 * kMm);
    CHECK(moves[2].role == MoveRole::Approach);
    CHECK(moves[2].z == 0);
    CHECK(moves[3].role == MoveRole::Plunge); // no room in the public form: the helix gives way to the plunge
    CHECK(moves[3].feedMicronPerMin == 250'000);
    o.leadRadius = 2;
    auto bad = ray::cam::linkPasses(std::vector<LinkPass>{p}, h, o, geom, ctx);
    REQUIRE(!bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
}

} // namespace ray::cam::link::test
