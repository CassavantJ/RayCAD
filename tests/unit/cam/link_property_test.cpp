// Linking (task T-051), the acceptance properties on seeded random cases (the seed is printed with every failure):
//   1. no rapid below the feed plane inside the stock bounds (a rapid straight up out of the cut, rapidRetract, aside);
//   2. no link crossing uncut stock: rapids, links, retracts and approaches never meet material, and a step-in cuts
//      only inside the tool's disc at the pass start.
// Both are judged by an independent height field (link_test_util.h) replaying the linked moves. Two families:
//   "random passes"  strokes, rings and circles anywhere in and around a box of stock, at random depths, with every
//                    option drawn (stay-down and its lift, step-in, rapid retract, entry orders, helix and ramp sizes,
//                    leads, pre-drilled points, clusters, sequences);
//   "random pockets" a block with a rectangular pocket and maybe a round island, built with the kernel, cut by offset
//                    rings at one to three depths, entered by helix or ramp, linked by step-in and stay-down.
// The first cases of each family are also run through the toolpath invariant checker of tests/unit/cam/invariants
// (rapidLow at the feed plane, rapidStock, plunge, rampAngle, helix, arc, leadTangent, heights, gouge on the pockets'
// part, and the rest). Every case is linked twice and must give the same moves. A refusal is a valid outcome (it names
// its cause); at least half the cases must link.
#include "link_test_util.h"

#include "checker.h"

#include <ray/kernel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>

namespace ray::cam::link::test {
namespace {

lib::Tool flatMill(Coord diameter)
{
    lib::Tool t;
    t.type = lib::ToolType::FlatEndMill;
    t.flutes = 3;
    t.diameter = toMm(diameter);
    t.shankDiameter = t.diameter;
    t.fluteLength = 25;
    t.shoulderLength = 25;
    t.overallLength = 70;
    return t;
}

invariants::Claims claimsFor(const Options& o, const ResolvedGeometry& g)
{
    invariants::Claims c;
    c.spindleRpm = 6000;
    c.rapidFloorZ = toMm(o.feedPlane);
    c.heights.clearance = toMm(o.clearance);
    c.heights.retract = toMm(o.retract);
    c.heights.feedPlane = toMm(o.feedPlane);
    c.centreCutting = o.centreCutting;
    if (o.rampAngleDeg > 0)
        c.maxRampAngleDeg = o.rampAngleDeg;
    if (o.helixDiameter > 0)
        c.helixMinDiameter = toMm(o.helixMinDiameter > 0 ? o.helixMinDiameter : o.helixDiameter);
    (void)g;
    return c;
}

Toolpath toolpathOf(const Linked& l)
{
    Toolpath t;
    t.moves = l.moves;
    t.startX = l.start.x;
    t.startY = l.start.y;
    t.startZ = l.start.z;
    return t;
}

std::vector<HeightField::StepCheck> stepsOf(const Linked& l)
{
    std::vector<HeightField::StepCheck> s;
    for (std::size_t k = 0; k < l.order.size(); ++k)
        if (l.entered[k] == Entered::Step)
            s.push_back({l.passMove[k] - 1, HeightField::passPath(l, l.passMove[k])});
    return s;
}

struct Tally {
    int linked = 0, refused = 0, checked = 0;
    int entered[7] = {};
};

// The shared checks of one linked case.
void judge(const Linked& l, const Linked& again, const Options& o, HeightField field, const std::vector<Pass>& passes)
{
    // Deterministic.
    CHECK(toText(l) == toText(again));
    // The two properties.
    HeightField::Replay r;
    r.radius = o.toolRadius;
    r.feedPlane = o.feedPlane;
    r.rapidRetract = o.rapidRetract;
    const std::vector<FieldIssue> issues = field.replay(l, r, stepsOf(l));
    INFO("height field:\n" << describe(issues));
    CHECK(issues.empty());
    // Every pass once, in a valid order; a pass cut as given appears whole.
    std::vector<std::size_t> idx;
    for (const Placement& p : l.order)
        idx.push_back(p.pass);
    if (!o.keepOrder) // keepOrder cuts the passes as given, whatever their levels say
        CHECK(validOrder(passes, idx));
    for (const Placement& p : l.order)
        if (!p.reversed && p.startS == 0)
            CHECK(containsRun(l.moves, passes[p.pass].moves));
    // Starts and ends at the clearance; no rapid carries a feed, every feed move has one.
    CHECK(l.start.z == o.clearance);
    CHECK(l.moves.back().z == o.clearance);
    for (const Move& m : l.moves)
        CHECK((m.kind == MoveKind::Rapid ? m.feedMicronPerMin == 0 : m.feedMicronPerMin > 0));
}

void checkWithInvariants(const Linked& l, const Options& o, const ResolvedGeometry& g, Coord toolDiameter,
                         bool preDrilled)
{
    const Toolpath t = toolpathOf(l);
    const lib::Tool tool = flatMill(toolDiameter);
    std::vector<invariants::Finding> f =
        invariants::check(invariants::CheckInput{t, g, tool, nullptr, nullptr, claimsFor(o, g)});
    // The checker does not know pre-drilled holes: their plunges read as plunges into stock.
    if (preDrilled)
        std::erase_if(f, [](const invariants::Finding& x) { return x.id == "plunge"; });
    INFO("invariant checker:\n" << invariants::describe(f));
    CHECK(f.empty());
}

} // namespace

namespace {

void randomPasses(std::uint64_t seeds, std::uint64_t checked)
{
    Tally tally;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        INFO("seed " << seed);
        Rng rng{seed * 1000003};
        const Coord w = rng.between(40 * kMm, 80 * kMm), h = rng.between(30 * kMm, 60 * kMm);
        const Stock stock = boxStock(0, 0, w, h, 0);
        Options o = baseOptions(rng.between(3 * kMm, 10 * kMm));
        const Coord r = o.toolRadius;
        o.centreCutting = rng.chance(0.3);
        o.rapidRetract = rng.chance(0.5);
        const double sd = rng.uniform();
        o.maxStayDown = sd < 0.3 ? 0 : (sd < 0.6 ? 20 * kMm : 1000 * kMm);
        o.stayDownLift = rng.chance(0.3) ? 300'000 : 0;
        o.stepIn = rng.chance(0.5);
        o.stayDownEntry = seed % 3 == 0; // T-055; from the seed, so the cases drawn before it stay the same
        if (rng.chance(0.8)) {
            o.helixDiameter = rng.between(r, 3 * r);
            o.helixMinDiameter = rng.chance(0.5) ? rng.between(r / 2, o.helixDiameter) : 0;
        }
        o.rampAngleDeg = rng.chance(0.85) ? rng.between(2.0, 10.0) : 0;
        o.entryClearance = rng.chance(0.3) ? kMm / 2 : 0;
        std::vector<EntryKind> kinds{EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill, EntryKind::Plunge};
        o.entryOrder.clear();
        for (const EntryKind k : kinds)
            if (rng.chance(0.75))
                o.entryOrder.push_back(k);
        if (rng.chance(0.3))
            std::reverse(o.entryOrder.begin(), o.entryOrder.end());
        o.keepOrder = rng.chance(0.15);
        const int n = static_cast<int>(rng.between(2, 12));
        std::vector<Pass> passes;
        for (int k = 0; k < n; ++k) {
            const Pt a{rng.between(-10 * kMm, w + 10 * kMm), rng.between(-10 * kMm, h + 10 * kMm)};
            const Nm z = -rng.between(kMm / 2, 8 * kMm);
            Pass p;
            const double kind = rng.uniform();
            if (kind < 0.45) {
                Pt b{rng.between(-10 * kMm, w + 10 * kMm), rng.between(-10 * kMm, h + 10 * kMm)};
                if (b == a)
                    b.x += kMm;
                p = passFromPath(openPath(a, {line(b)}), z, kCutFeed);
                p.reversible = rng.chance(0.5);
            } else if (kind < 0.75) {
                p = passFromPath(
                    rect(a.x, a.y, a.x + rng.between(3 * kMm, 30 * kMm), a.y + rng.between(3 * kMm, 30 * kMm)), z,
                    kCutFeed);
                p.rotatable = rng.chance(0.6);
                p.reversible = rng.chance(0.3);
            } else {
                p = passFromPath(Path::circle(a, rng.between(kMm, 15 * kMm), rng.chance(0.5)), z, kCutFeed);
                p.rotatable = rng.chance(0.6);
            }
            if (rng.chance(0.5))
                p.room = unbounded();
            if (p.room && rng.chance(0.4)) {
                p.leadIn = Lead{rng.chance(0.7) ? LeadKind::Arc : LeadKind::Line,
                                rng.between(kMm, 6 * kMm),
                                90,
                                rng.chance(0.3) ? 2 * kMm : 0,
                                rng.chance(0.5) ? Side::Left : Side::Right,
                                0};
                p.leadIn.length = p.leadIn.kind == LeadKind::Line ? rng.between(kMm, 5 * kMm) : p.leadIn.length;
                p.leadOut = p.leadIn;
            }
            if (rng.chance(0.4)) {
                p.cluster = static_cast<int>(rng.between(0, 2));
                p.level = static_cast<int>(rng.between(0, 2));
            }
            p.sequence = rng.chance(0.1) ? 1 : 0;
            if (rng.chance(0.1))
                o.preDrills.push_back(PreDrill{Pt{p.start.x, p.start.y}, 2 * r + kMm, -9 * kMm});
            passes.push_back(p);
        }
        JobContext ctx;
        auto l = linkPasses(passes, o, stock, ctx);
        if (!l) {
            ++tally.refused;
            INFO("refused: " << l.error().message);
            CHECK(!l.error().message.empty());
            CHECK((l.error().code == ErrorCode::ToolpathFailed || l.error().code == ErrorCode::InvalidArgument));
            if (std::getenv("RAY_LINK_DEBUG") != nullptr)
                std::printf("seed %llu refused: %s\n", static_cast<unsigned long long>(seed),
                            l.error().message.c_str());
            continue;
        }
        ++tally.linked;
        for (const Entered e : l->entered)
            ++tally.entered[static_cast<int>(e)];
        auto again = linkPasses(passes, o, stock, ctx);
        REQUIRE(again.has_value());
        judge(*l, *again, o, HeightField(0, 0, w, h, 0, 100'000), passes);
        if (seed <= checked) {
            ResolvedGeometry g;
            g.wcsFromModel = Xform::identity();
            g.stockBounds = Box3{{0, 0, -20}, {toMm(w), toMm(h), 0}};
            bool preDrilled = false;
            for (const Entered e : l->entered)
                preDrilled = preDrilled || e == Entered::PreDrill;
            checkWithInvariants(*l, o, g, 2 * r, preDrilled);
            ++tally.checked;
        }
    }
    std::printf("link property (random passes): %d linked, %d refused, %d through the checker; entries: free %d, "
                "stay-down %d, step %d, helix %d, ramp %d, pre-drill %d, plunge %d\n",
                tally.linked, tally.refused, tally.checked, tally.entered[0], tally.entered[1], tally.entered[2],
                tally.entered[3], tally.entered[4], tally.entered[5], tally.entered[6]);
    CHECK(tally.linked * 2 >= static_cast<int>(seeds));
    // Every way in was exercised (but step-in, which needs rings a stepover apart: the pocket property's).
    for (int k = 0; k < 7; ++k)
        if (k != static_cast<int>(Entered::Step))
            CHECK(tally.entered[k] > 0);
}

kernel::Shape kbox(Vec3 a, Vec3 b)
{
    auto s = kernel::makeBox(a, b);
    REQUIRE(s);
    return *s;
}
kernel::Shape kcut(const kernel::Shape& a, const kernel::Shape& b)
{
    JobContext ctx;
    auto r = kernel::boolean(kernel::BoolOp::Cut, a, b, ctx);
    REQUIRE(r);
    return r->shape;
}

void randomPockets(std::uint64_t seeds, std::uint64_t checked)
{
    Tally tally;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        INFO("seed " << seed);
        Rng rng{seed * 2654435761ull};
        const Coord w = rng.between(50 * kMm, 90 * kMm), h = rng.between(40 * kMm, 70 * kMm);
        const Coord px0 = rng.between(5 * kMm, 15 * kMm), py0 = rng.between(5 * kMm, 15 * kMm);
        const Coord px1 = rng.between(px0 + 25 * kMm, w - 5 * kMm), py1 = rng.between(py0 + 20 * kMm, h - 5 * kMm);
        const Nm depth = rng.between(2 * kMm, 8 * kMm);
        const bool island = rng.chance(0.5);
        const Pt ic{(px0 + px1) / 2, (py0 + py1) / 2};
        const Coord ir = rng.between(3 * kMm, std::min(px1 - px0, py1 - py0) / 5);
        // The part: a block with the pocket (and the island standing in it), built with the kernel.
        kernel::Shape pocketSolid = kbox({toMm(px0), toMm(py0), -toMm(depth)}, {toMm(px1), toMm(py1), 1});
        if (island) {
            auto cyl = kernel::makeCylinder(Vec3{toMm(ic.x), toMm(ic.y), -30}, Vec3{0, 0, 1}, toMm(ir), 40);
            REQUIRE(cyl);
            pocketSolid = kcut(pocketSolid, *cyl);
        }
        const kernel::Shape part = kcut(kbox({0, 0, -20}, {toMm(w), toMm(h), 0}), pocketSolid);
        ResolvedGeometry g;
        g.wcsFromModel = Xform::identity();
        g.partShapes = {part};
        g.stockBounds = Box3{{0, 0, -20}, {toMm(w), toMm(h), 0}};
        const Stock stock = stockOf(g);

        Options o = baseOptions(rng.between(3 * kMm, 8 * kMm));
        const Coord r = o.toolRadius;
        o.helixDiameter = rng.between(r, 3 * r);
        o.helixMinDiameter = r;
        o.rampAngleDeg = rng.between(2.0, 5.0);
        o.entryOrder = {EntryKind::Helix, EntryKind::Ramp};
        const Coord stepover = rng.between(6 * r / 10, 14 * r / 10);
        o.maxStayDown = 3 * stepover;
        o.stepIn = true;
        o.stayDownLift = rng.chance(0.3) ? 200'000 : 0;
        o.entryClearance = rng.chance(0.3) ? kMm / 2 : 0;
        o.rapidRetract = rng.chance(0.5);

        Regions pocket = regionOf(rect(px0, py0, px1, py1));
        if (island)
            pocket[0].holes.push_back(geom2d::reverse(Path::circle(ic, ir, true)));
        JobContext ctx;
        const Regions room = take(geom2d::offset(pocket, -r, ctx));
        if (room.empty())
            continue;
        std::vector<Pass> passes;
        const int levels = static_cast<int>(rng.between(1, 3));
        for (int level = 0; level < levels; ++level) {
            const Nm z = -depth * (level + 1) / levels;
            for (int ring = 0; ring < 40; ++ring) {
                const Regions rings = take(geom2d::offset(room, -stepover * ring, ctx));
                if (rings.empty())
                    break;
                for (const geom2d::Region& rg : rings) {
                    std::vector<Path> loops{rg.outer};
                    loops.insert(loops.end(), rg.holes.begin(), rg.holes.end());
                    for (const Path& loop : loops) {
                        Pass p = passFromPath(loop, z, kCutFeed);
                        p.rotatable = true;
                        p.cluster = 0;
                        p.level = level;
                        p.room = room;
                        passes.push_back(p);
                    }
                }
            }
        }
        auto l = linkPasses(passes, o, stock, ctx);
        if (!l) {
            ++tally.refused;
            INFO("refused: " << l.error().message);
            CHECK(!l.error().message.empty());
            continue;
        }
        ++tally.linked;
        for (const Entered e : l->entered)
            ++tally.entered[static_cast<int>(e)];
        auto again = linkPasses(passes, o, stock, ctx);
        REQUIRE(again.has_value());
        judge(*l, *again, o, HeightField(0, 0, w, h, 0, 100'000), passes);
        if (seed <= checked) {
            checkWithInvariants(*l, o, g, 2 * r, false);
            ++tally.checked;
        }
    }
    std::printf("link property (random pockets): %d linked, %d refused, %d through the checker; entries: free %d, "
                "stay-down %d, step %d, helix %d, ramp %d\n",
                tally.linked, tally.refused, tally.checked, tally.entered[0], tally.entered[1], tally.entered[2],
                tally.entered[3], tally.entered[4]);
    CHECK(tally.linked * 2 >= static_cast<int>(seeds));
    CHECK(tally.entered[2] > 0); // step-in between rings happened
    CHECK(tally.entered[3] > 0);
}

} // namespace

// The quick seed sets (the quick tier), and many more seeds hidden ("[.link-long]": run by hand, and before a change
// to linking is merged).
TEST_CASE("link property: random passes never rapid low over the stock and never link through uncut stock",
          "[link][property]")
{
    randomPasses(150, 16);
}
TEST_CASE("link property: random pockets, rings linked by step-in and stay-down, entered by helix or ramp",
          "[link][property]")
{
    randomPockets(30, 10);
}
TEST_CASE("link property, long: 3000 random pass sets and 300 random pockets", "[.link-long]")
{
    randomPasses(3000, 150);
    randomPockets(300, 60);
}

} // namespace ray::cam::link::test
