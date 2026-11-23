// The thread milling operation (task T-056): the direction of travel for every hand, location, cut mode and spindle;
// the basic thread depth; the helix with exact pitch and exact radius in IR units; internal single-form and multi-form
// and external threads against hand-computed values; and each refusal on its own (validate and generate give the same
// words).
#include "engrave_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::ops;
using namespace ray::cam::test;

namespace {

// A block 60 x 40 x 20 (top Z 20) with
//   T1 (15, 20) 6.8 through        (M8x1.25 tap drill)
//   T2 (40, 20) 10.2 blind, full diameter to Z 4, 118 degree point (M12x1.75 tap drill)
//   T3 (52, 8) 5 blind to Z 8, flat floor (M6x1 tap drill)
const kernel::Shape& threadBlock()
{
    static const kernel::Shape s = [] {
        kernel::Shape b = box(Vec3{0, 0, 0}, Vec3{60, 40, 20});
        b = cut(b, revolved(15, 20, {{0, 21}, {3.4, 21}, {3.4, -1}, {0, -1}}));
        b = cut(b, revolved(40, 20, {{0, 21}, {5.1, 21}, {5.1, 4}, {0, 4 - 5.1 * kTip118}}));
        b = cut(b, revolved(52, 8, {{0, 21}, {2.5, 21}, {2.5, 8}, {0, 8}}));
        return b;
    }();
    return s;
}

const kernel::Shape& pin()
{
    static const kernel::Shape s = pinPlate(12, 38);
    return s;
}

OpCase internal(const std::string& params, lib::Tool t,
                const std::vector<std::pair<double, double>>& holes = {{15, 20}})
{
    std::vector<kernel::SubId> faces;
    for (const auto& [x, y] : holes) {
        faces.push_back(holeFace(threadBlock(), x, y));
    }
    return threadCase(threadBlock(), faces, params, std::move(t));
}

OpCase external(const std::string& params, lib::Tool t)
{
    const auto faces = cylinderFaces(pin(), 30, 20, 6);
    REQUIRE_FALSE(faces.empty());
    return threadCase(pin(), faces, params, std::move(t));
}

// The helical arcs of a toolpath (role Cut).
std::vector<std::size_t> cutArcs(const Toolpath& p)
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < p.moves.size(); ++i) {
        const Move& m = p.moves[i];
        if (m.role == MoveRole::Cut && (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW)) {
            out.push_back(i);
        }
    }
    return out;
}

// Every full revolution of a pass (two arcs from (x + R, y) back to it) changes Z by exactly `pitch`, about (x, y),
// with its ends exactly on the circle; returns the radii of the passes in order.
std::vector<Nm> checkRevolutions(const Toolpath& p, Nm x, Nm y, Nm pitch, bool ccw, bool up)
{
    std::vector<Nm> radii;
    Nm px = p.startX, py = p.startY, pz = p.startZ;
    for (std::size_t i = 0; i < p.moves.size(); ++i) {
        const Move& m = p.moves[i];
        if (m.role == MoveRole::Cut && (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW)) {
            INFO("move " << i);
            CHECK(m.kind == (ccw ? MoveKind::ArcCCW : MoveKind::ArcCW));
            CHECK(m.i == x);
            CHECK(m.j == y);
            CHECK(m.k == m.z);
            if (py == y && px > x && m.y == y && m.x < x) {
                // The first half: to (x - R, y).
                CHECK(px - x == x - m.x);
                CHECK((m.z - pz) * 2 == (up ? pitch : -pitch));
                const Move& next = p.moves.at(i + 1);
                REQUIRE(next.role == MoveRole::Cut);
                CHECK(next.x == px);
                CHECK(next.y == y);
                CHECK(next.z - pz == (up ? pitch : -pitch));
                if (radii.empty() || radii.back() != px - x || p.moves.at(i - 1).role != MoveRole::Cut) {
                    radii.push_back(px - x);
                }
            }
        }
        px = m.x;
        py = m.y;
        pz = m.z;
    }
    return radii;
}

} // namespace

TEST_CASE("thread milling: the direction of travel for every hand, location, cut mode and spindle", "[cam][threadmill]")
{
    struct Row {
        bool internal, rightHand, climb, cw;
        bool ccw, up;
    };
    // M3 (cw): climb inside is counter-clockwise, around a boss clockwise; conventional and M4 each reverse it. A
    // right-hand thread rises going counter-clockwise.
    const Row rows[] = {
        {true, true, true, true, true, true},      {true, false, true, true, true, false},
        {true, true, false, true, false, false},   {true, false, false, true, false, true},
        {false, true, true, true, false, false},   {false, false, true, true, false, true},
        {false, true, false, true, true, true},    {false, false, false, true, true, false},
        {true, true, true, false, false, false},   {true, false, true, false, false, true},
        {true, true, false, false, true, true},    {true, false, false, false, true, false},
        {false, true, true, false, true, true},    {false, false, true, false, true, false},
        {false, true, false, false, false, false}, {false, false, false, false, false, true},
    };
    for (const Row& r : rows) {
        INFO("internal " << r.internal << " right " << r.rightHand << " climb " << r.climb << " M3 " << r.cw);
        const ThreadTravel t = threadTravel(r.internal, r.rightHand, r.climb, r.cw);
        CHECK(t.ccw == r.ccw);
        CHECK(t.up == r.up);
    }
}

TEST_CASE("thread milling: the basic thread depth of ISO 68-1", "[cam][threadmill]")
{
    // H = sqrt(3) / 2 P; inside 5/8 H = 0.5412659 P, outside 17/24 H = 0.6134347 P.
    CHECK(basicThreadDepthNm(1'000'000, true) == 541'266);
    CHECK(basicThreadDepthNm(1'000'000, false) == 613'435);
    CHECK(basicThreadDepthNm(1'250'000, true) == 676'582);
    CHECK(basicThreadDepthNm(1'750'000, false) == 1'073'511);
}

TEST_CASE("thread milling: a helix rises exactly one pitch per revolution, its ends exactly on the circle",
          "[cam][threadmill]")
{
    // An odd pitch in nm (13 TPI = 1.953846 mm): each half turn takes 976923 or 976924, each whole turn exactly P.
    const Nm pitch = 1'953'846 + 1;
    const auto moves = helixMoves(10'000'000, 5'000'000, 2'000'000, true, -7'000'000, pitch, 5, 0, 0, 90'000);
    REQUIRE(moves.size() == 10);
    for (std::size_t k = 0; k < moves.size(); ++k) {
        const Move& m = moves[k];
        CHECK(m.kind == MoveKind::ArcCCW);
        CHECK(m.i == 10'000'000);
        CHECK(m.j == 5'000'000);
        CHECK(m.y == 5'000'000);
        CHECK(m.x == (k % 2 == 0 ? 8'000'000 : 12'000'000));
        CHECK(m.feedMicronPerMin == 90'000);
        if (k % 2 == 1) {
            CHECK(m.z == -7'000'000 + static_cast<Nm>((k + 1) / 2) * pitch);
        } else {
            CHECK(m.z == -7'000'000 + static_cast<Nm>(k / 2) * pitch + 976'924); // 976923.5 rounded half to even
        }
    }
    // Down, clockwise, with a 15 degree overlap: the overlap ends on the circle, its Z change P * 15 / 360.
    const auto down = helixMoves(0, 0, 3'000'000, false, 0, -1'000'000, 1, 15, -41'667, 60'000);
    REQUIRE(down.size() == 3);
    CHECK(down[1].z == -1'000'000);
    CHECK(down[2].z == -1'041'667);
    CHECK(down[2].kind == MoveKind::ArcCW);
    CHECK(down[2].x == 2'897'777); // 3 mm cos 15 = 2.8977774
    CHECK(down[2].y == -776'457);  // -3 mm sin 15 = -0.7764571
}

TEST_CASE("thread milling inside: M8x1.25, single-form, two radial passes and a spring pass", "[cam][threadmill]")
{
    OpCase c = internal(R"({"pitch":1.25,"diameter":8,"radialPasses":2,"springPass":true})", threadMill(5));
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{15, 20, 4.01, -1, 30});
        k.direction = invariants::Direction::Climb;
    });
    // Right hand, climb, M3: counter-clockwise and up. Rf = (8 - 5) / 2 = 1.5 mm; h = 676582 nm (5/8 H); the first
    // pass at 1.5 mm - 2 round(676582 / 4) (338292, an even inset) = 1.161708 mm.
    const std::vector<Nm> radii = checkRevolutions(p, 15'000'000, 20'000'000, 1'250'000, true, true);
    CHECK(radii == std::vector<Nm>{1'161'708, 1'500'000, 1'500'000});
    // The thread's diameter: 2 Rf + the tool = 8 mm exactly. 16 revolutions each (the 20 mm bore / 1.25).
    CHECK(cutArcs(p).size() == 3 * 16 * 2);
    // The lead-in starts at the hole's centre, at the bottom of the thread (the through bore's exit, Z 0), and is a
    // half circle of radius R / 2 that meets the helix tangentially.
    std::size_t first = cutArcs(p).front();
    const Move& lead = p.moves.at(first - 1);
    CHECK(lead.role == MoveRole::LeadIn);
    CHECK(lead.kind == MoveKind::ArcCCW);
    CHECK(lead.x == 15'000'000 + 1'161'708);
    CHECK(lead.z == 0);
    const Move& before = p.moves.at(first - 2);
    CHECK(std::abs(before.x - 15'000'000) <= 1);
    CHECK(before.y == 20'000'000);
    // The feed at the tool centre: 200 mm/min at the edge times R / (R + 2.5 mm): 75 mm/min at Rf.
    for (const std::size_t i : cutArcs(p)) {
        const Move& m = p.moves[i];
        if (m.x == 15'000'000 + 1'500'000 || m.x == 15'000'000 - 1'500'000) {
            CHECK(m.feedMicronPerMin == 75'000);
        }
    }
    // The tool never rises above the feed plane inside the hole and stays down between passes in the hole.
    std::size_t rapidsInHole = 0;
    for (std::size_t i = first; i < p.moves.size(); ++i) {
        rapidsInHole += p.moves[i].kind == MoveKind::Rapid && p.moves[i].z < 20'000'000 ? 1 : 0;
    }
    CHECK(rapidsInHole == 0);
}

TEST_CASE("thread milling inside: multi-form, one revolution plus overlap per step of the tool", "[cam][threadmill]")
{
    // M12x1.75 in the 10.2 mm blind hole (full diameter to Z 4, bore 16 long): a 5.5 mm full-form tool with 10 mm of
    // teeth has floor(10 / 1.75) = 5 teeth, steps of 8.75 mm, ceil(16 / 8.75) = 2 steps; overlap 15 degrees =
    // 1.75 * 15 / 360 = 0.0729167 -> 72917 nm.
    OpCase c = internal(R"({"form":"multi","pitch":1.75,"diameter":12,"overlap":15})", threadMill(5.5, 1.75, 10, 30),
                        {{40, 20}});
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{40, 20, 6.01, 3.99, 40});
    });
    const auto arcs = cutArcs(p);
    REQUIRE(arcs.size() == 2 * 3); // two steps, each two half turns and the overlap
    const Nm rf = 3'250'000;       // (12 - 5.5) / 2
    for (int s = 0; s < 2; ++s) {
        const Move& a = p.moves[arcs[3 * s]];
        const Move& b = p.moves[arcs[3 * s + 1]];
        const Move& o = p.moves[arcs[3 * s + 2]];
        const Nm zb = 4'000'000 + s * 8'750'000;
        CHECK(a.x == 40'000'000 - rf);
        CHECK(a.z == zb + 875'000);
        CHECK(b.x == 40'000'000 + rf);
        CHECK(b.z == zb + 1'750'000);
        CHECK(o.z == zb + 1'750'000 + 72'917);
        CHECK(o.kind == MoveKind::ArcCCW);
        CHECK(std::abs(std::hypot(static_cast<double>(o.x - 40'000'000), static_cast<double>(o.y - 20'000'000)) -
                       static_cast<double>(rf)) < 0.75);
    }
    // The tool goes from the first step to the second without leaving the hole.
    for (std::size_t i = arcs.front(); i < arcs.back(); ++i) {
        CHECK(p.moves[i].kind != MoveKind::Rapid);
    }
}

TEST_CASE("thread milling outside: an M12x1.75 left-hand thread on a pin", "[cam][threadmill]")
{
    OpCase c = external(R"({"location":"external","pitch":1.75,"leftHand":true})", threadMill(6));
    const Toolpath p = c.path([](invariants::Claims& k) {
        k.initialStock = invariants::InitialStock::Part;
        k.allowedCuts.push_back(invariants::AllowedCut{30, 20, 6.5, 3.99, 30}); // the pin, and a grid square
        k.direction = invariants::Direction::Climb;
    });
    // Outside, left hand, climb, M3: clockwise and up. h = 17/24 H = 1073511 nm; Rf = (12 - 2h + 6) / 2 = 7926489 nm.
    // The pin is 6 long (Z 4 to Z 10): 4 revolutions from Z 4.
    const std::vector<Nm> radii = checkRevolutions(p, 30'000'000, 20'000'000, 1'750'000, false, true);
    CHECK(radii == std::vector<Nm>{7'926'489});
    CHECK(cutArcs(p).size() == 4 * 2);
    CHECK(p.moves.at(cutArcs(p).front()).z == 4'000'000 + 875'000);
    // The lead comes from outside: radius (6 + h) / 2 = 3536756 nm (3536755.5 rounded half to even), so the tool starts
    // 7.073512 mm outside the pass.
    const Move& lead = p.moves.at(cutArcs(p).front() - 1);
    CHECK(lead.role == MoveRole::LeadIn);
    CHECK(lead.kind == MoveKind::ArcCCW);
    const Move& before = p.moves.at(cutArcs(p).front() - 2);
    CHECK(std::abs(before.x - (30'000'000 + 7'926'489 + 2 * 3'536'756)) <= 1);
    // The centre feed outside is faster than the edge's: 200 * R / (R - 3 mm).
    const Move& arc = p.moves.at(cutArcs(p).front());
    CHECK(arc.feedMicronPerMin == std::llround(200'000.0 * 7'926'489 / (7'926'489 - 3'000'000)));
}

TEST_CASE("thread milling refuses, naming the cause", "[cam][threadmill][validate]")
{
    const auto refused = [](const OpCase& c, ErrorCode code, const std::string& words) {
        const Status v = c.validate();
        REQUIRE_FALSE(v);
        CHECK(v.error().code == code);
        CHECK_THAT(v.error().message, ContainsSubstring(words));
        auto g = c.generate();
        REQUIRE_FALSE(g);
        CHECK(g.error().message == v.error().message);
    };
    const std::string m8 = R"({"pitch":1.25,"diameter":8})";
    SECTION("the tool")
    {
        refused(internal(m8, tool(lib::ToolType::FlatEndMill, 5, 0)), ErrorCode::InvalidArgument,
                "Thread milling needs a thread mill");
        refused(internal(m8, threadMill(5, 1.5, 10)), ErrorCode::InvalidArgument,
                "The thread mill's pitch (1.5 mm) does not match the thread's (1.25 mm)");
        refused(internal(R"({"form":"multi","pitch":1.25,"diameter":8})", threadMill(5)), ErrorCode::InvalidArgument,
                "has none (it is single-form)");
        refused(internal(R"({"form":"multi","pitch":1.25,"diameter":8})", threadMill(5, 1.25, 1)),
                ErrorCode::InvalidArgument, "are shorter than one pitch");
        refused(internal(m8, threadMill(7)), ErrorCode::InvalidArgument,
                "The 7 mm thread mill does not fit the 6.8 mm hole at X15 Y20");
        refused(internal(R"({"pitch":0.5,"diameter":6.805})", threadMill(6.79)), ErrorCode::InvalidArgument,
                "The 6.79 mm thread mill is too large for the 6.805 mm thread at the hole at X15 Y20");
    }
    SECTION("thread data")
    {
        refused(internal(R"({})", threadMill(5)), ErrorCode::InvalidArgument,
                "The model gives no thread for the hole at X15 Y20");
        refused(internal(R"({"pitch":1.25})", threadMill(5)), ErrorCode::InvalidArgument,
                "enter the thread's major (nominal) diameter");
        refused(internal(R"({"pitch":1.25,"diameter":6.5})", threadMill(5)), ErrorCode::InvalidArgument,
                "is not smaller than the 6.5 mm thread");
        refused(internal(R"({"pitch":1.25,"diameter":8,"threadLength":25})", threadMill(5)), ErrorCode::InvalidArgument,
                "is 25 mm long, but the hole's bore is 20 mm long");
        refused(internal(R"({"pitch":1,"diameter":6,"threadLength":13})", threadMill(3), {{52, 8}}),
                ErrorCode::InvalidArgument, "but the drilled bore is 12 mm long: thread less deep, or drill deeper");
        refused(internal(R"({"pitch":1.25,"diameter":8,"threadDepth":4})", threadMill(5)), ErrorCode::InvalidArgument,
                "leaves no minor diameter");
        refused(external(R"({"location":"external"})", threadMill(6)), ErrorCode::InvalidArgument,
                "Enter the pitch of the thread on the boss at X30 Y20");
    }
    SECTION("the wrong kind of face")
    {
        refused(external(R"({"pitch":1.75})", threadMill(6)), ErrorCode::ToolpathFailed,
                "is not part of a hole that can be threaded");
        refused(internal(R"({"location":"external","pitch":1.25})", threadMill(5)), ErrorCode::ToolpathFailed,
                "is the wall of a hole, not of a boss");
        OpCase top = threadCase(threadBlock(), topEdges(threadBlock(), 20, 15, 20, 3.4), m8, threadMill(5));
        refused(top, ErrorCode::InvalidArgument, "Selection 1 is not a face");
        refused(threadCase(threadBlock(), {}, m8, threadMill(5)), ErrorCode::InvalidArgument,
                "Pick the holes to thread");
    }
    SECTION("the shank, the stick-out and the heights")
    {
        lib::Tool stubby = threadMill(5, 0, 1.5, 10);
        stubby.shankDiameter = 6;
        refused(internal(m8, stubby), ErrorCode::InvalidArgument,
                "above its 1.5 mm of flutes the thread mill is 5 mm wide");
        lib::Tool shortReach = threadMill(5, 0, 1.5, 10);
        shortReach.fluteLength = 25; // teeth over the whole thread, then a 6 mm shank from 10 mm up
        shortReach.shankDiameter = 6;
        refused(internal(m8, shortReach), ErrorCode::InvalidArgument,
                "the thread mill's 6 mm shank starts 10 mm above its tip and would rub the bore");
        OpCase s = internal(m8, threadMill(5));
        s.op.tool.stickOut = 20.5;
        refused(s, ErrorCode::InvalidArgument, "it sticks out only 20.5 mm");
        OpCase h = internal(m8, threadMill(5));
        h.op.heights.feedPlane = -1;
        refused(h, ErrorCode::InvalidArgument, "must not be below that top");
        h.op.heights.feedPlane = 3;
        refused(h, ErrorCode::InvalidArgument, "The retract height (2) must not be below the feed plane (3)");
        h.op.heights.feedPlane = 2;
        h.op.heights.clearance = 1;
        refused(h, ErrorCode::InvalidArgument, "The clearance height (1) must not be below the retract height (2)");
        OpCase low = internal(R"({"pitch":1.5,"diameter":8.5,"threadLength":19})", threadMill(5));
        low.op.heights.feedPlane = 0;
        low.op.heights.retract = 0;
        refused(low, ErrorCode::InvalidArgument, "above the feed plane (Z20): raise the feed plane");
    }
    SECTION("cutting data")
    {
        OpCase c = internal(m8, threadMill(5));
        c.op.cutting.clockwise = false;
        refused(c, ErrorCode::InvalidArgument, "counter-clockwise (M4)");
        c.op.cutting.clockwise = true;
        c.op.cutting.feedCut = 0;
        refused(c, ErrorCode::InvalidArgument, "Set a cutting feed");
    }
}

TEST_CASE("thread milling warns about a bore smaller than the minor diameter", "[cam][threadmill][validate]")
{
    // 6.8 mm for an M8x1 thread: the basic minor diameter is 8 - 2 * 0.541266 = 6.917 mm.
    auto w = internal(R"({"pitch":1,"diameter":8})", threadMill(5)).check();
    REQUIRE(w);
    REQUIRE(w->size() == 1);
    CHECK_THAT(w->front(), ContainsSubstring("is smaller than the thread's minor diameter (6.917 mm)"));
}

// A problem of geom2d::sweptArea (T-050) found by T-056 and NOT fixed here: the area a disc of radius 3 mm sweeps along
// a whole circle of radius 3 mm (its inner offset is the centre) together with a lead-out arc ending 1 nm from the
// centre comes out as a disc of radius 7.5 mm (176.7 mm2) instead of 6 mm (113.1 mm2); either path alone is right.
// Linking's cut-area model (link.cpp Material) is built from such sweeps, so a thread-milling pass whose radius equals
// the tool's (an M12 thread with a 6 mm thread mill) with its lead-out gets a wrong cut area. The thread milling
// operation does not rely on it (threadmill.h LINKING). [!shouldfail]: this case starts failing, i.e. reports, when
// geom2d is fixed.
TEST_CASE("known problem: geom2d::sweptArea of a circle of the disc's radius with another path",
          "[cam][threadmill][!shouldfail]")
{
    JobContext ctx;
    const geom2d::Pt c{40'000'000, 20'000'000};
    geom2d::Path full;
    full.start = {43'000'000, 20'000'000};
    full.spans = {geom2d::Span{geom2d::SpanKind::ArcCCW, {37'000'000, 20'000'000}, c, 3'000'000},
                  geom2d::Span{geom2d::SpanKind::ArcCCW, {43'000'000, 20'000'000}, c, 3'000'000}};
    geom2d::Path out; // a lead-out: a half circle of radius 1.5 mm from 15 degrees on the circle back to its centre
    out.start = {42'897'777, 20'776'457};
    out.spans = {geom2d::Span{geom2d::SpanKind::ArcCCW, {39'999'999, 19'999'999}, {41'448'888, 20'388'228}, 1'500'000}};
    auto alone = geom2d::sweptArea(std::vector<geom2d::Path>{full}, 3'000'000, ctx);
    REQUIRE(alone);
    CHECK(std::abs(geom2d::area(*alone) / 1e12 - 36 * 3.14159265358979323846) < 1e-3);
    auto both = geom2d::sweptArea(std::vector<geom2d::Path>{full, out}, 3'000'000, ctx);
    REQUIRE(both);
    CHECK(std::abs(geom2d::area(*both) / 1e12 - 36 * 3.14159265358979323846) < 1e-3); // fails: 176.7 mm2
}
