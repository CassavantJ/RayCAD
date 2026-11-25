// Adaptive clearing with every region entered (task T-063, entry.h), measured by the independent sweep checker
// (tests/tools/sweepcheck) with a 12.7 mm tool. PLAN.md M4 T-063's acceptance:
//   - a slot 14.5 mm wide (S5's p4) produces a toolpath at 10% and 20% load, entering by a ramp along the slot axis, or
//     at a pre-drilled point when one is given, with 99th-percentile engagement at most 75 degrees and never above 100;
//   - a slot too narrow for any entry returns a failed result naming the minimum width;
//   - tool-down link moves remove 0 mm2 of material (every S5 pocket and the slot, both loads), or are reclassified as
//     cuts (cutLinksThatCut);
//   - wherever engagement exceeds 1.25 x target the move carries a reduced-feed flag (checked against the checker's
//     forward-half reading; its full reading is reported and held at what is measured);
//   - air cutting (cut moves read under 2 degrees) at most 10% of cut length on p1 and p2 (BP1, BP2), 15% on the
//   others.
// The slow cases ([slow]) cover every S5 pocket at both loads; the quick ones the slot at 20% load, the refusals and
// the building blocks.
#include "adaptive_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ray;
using namespace ray::cam::adaptive2d;
using namespace ray::cam::adaptive2d::test;
namespace geom2d = ray::cam::geom2d;
using ray::cam::EntryKind;

namespace {

const std::vector<sw::s5::Case>& cases()
{
    static const std::vector<sw::s5::Case> c = sw::s5::makeCases();
    return c;
}

bool contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

// The pre-drilled hole of the slot tests: 14.3 mm (at least the tool and the smallest helix, 12.7 + 1.5875) at the
// centre of the slot's end radius.
PreDrillHole slotHole()
{
    return PreDrillHole{geom2d::toPt({7.25, 7.25}), toNm(14.3)};
}

EntryOptions drillFirst()
{
    EntryOptions o;
    o.order = {EntryKind::PreDrill, EntryKind::Helix, EntryKind::Ramp};
    o.preDrills = {slotHole()};
    return o;
}

// The slot's acceptance: p99 at most 75, never above 100 (the checker's full reading), no tool-down link cutting, no
// reachable area left, no gouge beyond 10 um, air at most 15%, every reading above 1.25 x target on the forward half
// flagged.
sw::Report acceptSlot(const Output& out, double stepover, const std::vector<PreDrillHole>& holes)
{
    const sw::Report r = sw::check(checkInputFor(sw::s5::caseById(cases(), "p4"), out, stepover, holes));
    WARN("p4 at " << stepover * 100 << "%: " << sw::summary(r) << " | forward p99 " << r.forward.p99 << " max "
                  << r.forward.max << ", unflagged above 1.25x " << r.forward.pctAboveFlagCheckUnflagged << "%");
    INFO(sw::summary(r));
    CHECK(r.p99 <= 75.0);
    CHECK(r.max <= 100.0);
    CHECK(r.cutLength > 100.0);
    CHECK(r.linkRemovedMm2 == 0.0);
    CHECK(r.uncutReachable < 0.005);
    CHECK(r.gougeAnalytic <= 0.010);
    CHECK(r.pctAir <= 15.0);
    CHECK(r.forward.pctAboveFlagCheckUnflagged == 0.0);
    return r;
}

void checkRamp(const Output& out)
{
    REQUIRE(out.regions.size() == 1);
    const Region& r = out.regions[0];
    REQUIRE(r.entry.kind == EntryMode::Ramp);
    REQUIRE(r.paths.size() >= 2);
    const Path& ramp = r.paths.front();
    CHECK(ramp.role == Role::Ramp);
    CHECK(ramp.motion == Motion::Cut);
    REQUIRE(ramp.points.size() == 2);
    CHECK(ramp.points[0] == r.entry.centre);
    CHECK(ramp.points[1] == r.entry.start);
    CHECK(r.paths[1].points.front() == r.entry.start);
    // along the slot's axis (y = 7.25), at least a tool diameter long, inside the tool-centre area
    for (const Pt& p : ramp.points) {
        CHECK(std::llabs(p.y - toNm(7.25)) <= 5'000);
        CHECK(geom2d::locate(p, r.toolBounds) == geom2d::Where::Inside);
    }
    CHECK(std::llabs(ramp.points[1].x - ramp.points[0].x) >= toNm(kToolMm));
    CHECK(ramp.reducedFeed == std::vector<std::uint8_t>{0, 1});
}

} // namespace

TEST_CASE("A slot no helix fits is entered by a ramp along its axis: 20% load", "[adaptive][entry]")
{
    auto out = planCase("p4", 0.2);
    REQUIRE(out);
    checkRamp(*out);
    REQUIRE(out->warnings.size() == 1);
    CHECK(out->warnings[0].code == Warning::Code::EntryFallback);
    CHECK(contains(out->warnings[0].message, "is entered by a ramp: a helix of 9.525 mm does not fit"));
    CHECK(out->stats.engineRuns == 2);
    acceptSlot(*out, 0.2, {});
}

TEST_CASE("A slot no helix fits is entered by a ramp along its axis: 10% load", "[adaptive][entry]")
{
    // At 10% load the smallest helix (1.5875 mm) fits, but a full one does not: the ramp comes before a smaller helix.
    auto out = planCase("p4", 0.1);
    REQUIRE(out);
    checkRamp(*out);
    acceptSlot(*out, 0.1, {});
}

TEST_CASE("A slot is entered at a pre-drilled hole when one is given: 20% load", "[adaptive][entry]")
{
    auto out = planCase("p4", 0.2, drillFirst());
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    const Region& r = out->regions[0];
    CHECK(r.entry.kind == EntryMode::PreDrill);
    CHECK(r.entry.centre == slotHole().centre);
    CHECK(r.entry.radius == slotHole().diameter / 2);
    CHECK(r.entry.start == r.entry.centre);
    // the plunge at the centre, then a tool-down link to the hole's side, where the first pass starts
    REQUIRE(r.paths.size() >= 2);
    CHECK(r.paths[0].role == Role::Link);
    CHECK(r.paths[0].motion == Motion::LinkClear);
    CHECK(r.paths[0].points.front() == r.entry.centre);
    CHECK(r.paths[1].role == Role::Pass);
    CHECK(out->warnings.empty());
    acceptSlot(*out, 0.2, {slotHole()});
}

TEST_CASE("A slot is entered at a pre-drilled hole when one is given: 10% load", "[adaptive][entry][slow]")
{
    auto out = planCase("p4", 0.1, drillFirst());
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    CHECK(out->regions[0].entry.kind == EntryMode::PreDrill);
    acceptSlot(*out, 0.1, {slotHole()});
}

TEST_CASE("A hole narrower than the tool and the smallest helix is not used; the next kind is, with a warning",
          "[adaptive][entry]")
{
    EntryOptions o = drillFirst();
    o.preDrills = {PreDrillHole{geom2d::toPt({35, 7.25}), toNm(14.0)}};
    auto out = planCase("p4", 0.2, o);
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    CHECK(out->regions[0].entry.kind == EntryMode::Ramp);
    REQUIRE(out->warnings.size() == 1);
    INFO(out->warnings[0].message);
    CHECK(out->warnings[0].code == Warning::Code::EntryFallback);
    CHECK(contains(out->warnings[0].message, "a pre-drilled hole of 14.288 mm or a helix of 9.525 mm does not fit"));
}

TEST_CASE("A slot too narrow for any entry returns a failed result naming the minimum width", "[adaptive][entry]")
{
    // 12.8 mm: narrower than the tool and twice the finishing allowance (61 and 30 grid units of 1/480 mm at 20% and
    // 10% load), so the tool centre has nowhere to go.
    sw::s5::Case slot{
        "narrow", "slot 12.8 x 70", {sw::s5::roundedRect(0, 0, 70, 12.8, 6.4)}, {sw::s5::rect(-10, -10, 80, 22.8)}};
    for (auto [stepover, width] : {std::pair{0.2, "12.954"}, std::pair{0.1, "12.825"}}) {
        JobContext ctx;
        auto out = planAdaptive(inputFor(slot, stepover), {}, ctx);
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ToolpathFailed);
        INFO(out.error().message);
        CHECK(contains(out.error().message, std::string("a slot must be wider than ") + width + " mm"));
    }
    // A pocket the tool centre fits but no entry does: a 13.5 mm hole (its centre area 0.55 mm across: no helix of
    // 1.5875 mm, no ramp of 12.7 mm, no pre-drilled hole).
    sw::s5::Case hole{"hole", "circle 13.5", {sw::s5::circle(20, 20, 6.75)}, {sw::s5::rect(0, 0, 40, 40)}};
    JobContext ctx;
    auto out = planAdaptive(inputFor(hole, 0.2), {}, ctx);
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::ToolpathFailed);
    INFO(out.error().message);
    CHECK(contains(out.error().message, "no entry into the region around (20, 19.999) mm"));
    CHECK(contains(out.error().message, "a helix needs the area to be at least 14.542 mm wide"));
    CHECK(contains(out.error().message, "a ramp needs a straight run of at least 12.7 mm"));
    CHECK(contains(out.error().message, "a pre-drilled hole needs to be at least 14.288 mm wide"));
    // allowUnentered: the region comes back not entered, with the same words as a warning
    Input allow = inputFor(hole, 0.2);
    allow.allowUnentered = true;
    JobContext ctx2;
    auto unentered = planAdaptive(allow, {}, ctx2);
    REQUIRE(unentered);
    REQUIRE(unentered->regions.size() == 1);
    CHECK(unentered->regions[0].entry.kind == EntryMode::None);
    CHECK(unentered->regions[0].paths.empty());
    REQUIRE(unentered->warnings.size() == 1);
    CHECK(unentered->warnings[0].code == Warning::Code::NotEntered);
    CHECK(unentered->warnings[0].message == out.error().message);
}

TEST_CASE("A region no helix, ramp or hole enters is plunged into, last, when the order asks for it (after M3)",
          "[adaptive][entry]")
{
    // The 13.5 mm hole of the test above (its centre area 0.55 mm across), with a plunge at the end of the order.
    sw::s5::Case hole{"hole", "circle 13.5", {sw::s5::circle(20, 20, 6.75)}, {sw::s5::rect(0, 0, 40, 40)}};
    EntryOptions o;
    o.order = {EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill, EntryKind::Plunge};
    for (const double stepover : {0.1, 0.2}) {
        INFO("load " << stepover);
        JobContext ctx;
        auto out = planAdaptive(inputFor(hole, stepover), o, ctx);
        REQUIRE(out);
        REQUIRE(out->regions.size() == 1);
        const Region& r = out->regions[0];
        CHECK(r.entry.kind == EntryMode::Plunge);
        CHECK(r.entry.radius == 0);
        CHECK(r.entry.start == r.entry.centre);
        // At the middle of the area, on the engine's grid.
        CHECK(std::llabs(r.entry.centre.x - toNm(20)) <= 5'000);
        CHECK(std::llabs(r.entry.centre.y - toNm(20)) <= 5'000);
        CHECK(geom2d::locate(r.entry.centre, r.toolBounds) == geom2d::Where::Inside);
        // A straight cut to the wall and once round it: every segment full width, every one at the reduced feed.
        REQUIRE(r.paths.size() == 2);
        CHECK(r.paths[0].points.size() == 2);
        CHECK(r.paths[0].points.front() == r.entry.centre);
        CHECK(r.paths[1].points.front() == r.paths[0].points.back());
        CHECK(r.paths[1].points.back() == r.paths[0].points.back());
        for (const Path& p : r.paths) {
            CHECK(p.role == Role::Pass);
            CHECK(p.motion == Motion::Cut);
            REQUIRE(p.reducedFeed.size() == p.points.size());
            CHECK(std::count(p.reducedFeed.begin() + 1, p.reducedFeed.end(), std::uint8_t(1)) ==
                  std::ptrdiff_t(p.points.size() - 1));
            for (std::size_t i = 1; i < p.engagementDeg.size(); ++i) {
                CHECK(p.engagementDeg[i] == 180.0);
            }
        }
        // The wall loop is the area less the tool radius: radius 6.75 - 6.35 = 0.4 (within the input polygon's sag).
        for (const Pt& q : r.paths[1].points) {
            const double d = std::hypot(double(q.x) - 20e6, double(q.y) - 20e6);
            CHECK(std::abs(d - 0.4e6) < 10'000);
        }
        REQUIRE(out->warnings.size() == 1);
        CHECK(out->warnings[0].code == Warning::Code::EntryFallback);
        CHECK(contains(out->warnings[0].message, "is entered by a plunge: a helix of 9.525 mm, a ramp of 12.7 mm, a "
                                                 "pre-drilled hole of 14.288 mm or a helix of 1.588 mm does not fit"));
        CHECK(out->cleared.size() == 1);
        // The sweep checker: nothing the tool reaches is left, nothing gouged, at most a full-width cut.
        const sw::Report rep = sw::check(checkInputFor(hole, *out, stepover));
        INFO(sw::summary(rep));
        CHECK(rep.uncutReachable < 0.005);
        CHECK(rep.gougeAnalytic <= 0.010);
        CHECK(rep.linkRemovedMm2 == 0.0);
        CHECK(rep.max <= 182.0); // the checker's reading of a full-width cut (180) within its sampling
    }
    // Without the plunge in the order: refused as before ("A slot too narrow for any entry ...").
    JobContext ctx;
    CHECK_FALSE(planAdaptive(inputFor(hole, 0.2), {}, ctx));
}

TEST_CASE("A plunge at the end of the order changes nothing where another entry fits (after M3)", "[adaptive][entry]")
{
    EntryOptions plunge;
    plunge.order = {EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill, EntryKind::Plunge};
    for (const char* id : {"p2", "p4"}) { // a helix; a ramp after the helix does not fit
        INFO(id);
        auto without = planCase(id, 0.2);
        JobContext ctx;
        auto with = planAdaptive(inputFor(sw::s5::caseById(cases(), id), 0.2), plunge, ctx);
        REQUIRE(without);
        REQUIRE(with);
        CHECK(toText(*with) == toText(*without));
    }
}
TEST_CASE("Bad entry options are refused with InvalidArgument", "[adaptive][entry]")
{
    const Input good = inputFor(sw::s5::caseById(cases(), "p2"), 0.2);
    auto refused = [&](const Input& in, const EntryOptions& o, const std::string& part) {
        JobContext ctx;
        auto out = planAdaptive(in, o, ctx);
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::InvalidArgument);
        INFO(out.error().message);
        CHECK(contains(out.error().message, part));
    };
    EntryOptions o;
    o.order = {static_cast<EntryKind>(200)};
    refused(good, o, "not a known kind of entry");
    o.order.clear();
    refused(good, o, "no entry kind");
    o = {};
    o.preDrills = {PreDrillHole{{0, 0}, 0}};
    refused(good, o, "positive diameter");
    o = {};
    o.minRampRun = -1;
    refused(good, o, "must not be negative");
    Input seeded = good;
    seeded.seeds.push_back({});
    refused(seeded, {}, "set by the entry planner");
    Input filtered = good;
    filtered.regionsAt.push_back({});
    refused(filtered, {}, "set by the entry planner");
}

TEST_CASE("A pocket a full helix enters gives the engine's own output, flagged", "[adaptive][entry]")
{
    auto planned = planCase("p2", 0.2);
    auto engine = runCase("p2", 0.2);
    REQUIRE(planned);
    REQUIRE(engine);
    CHECK(planned->stats.engineRuns == 1);
    CHECK(planned->stats.linksMadeCuts == 0);
    REQUIRE(planned->regions.size() == engine->regions.size());
    for (std::size_t i = 0; i < planned->regions.size(); i++) {
        const Region& a = planned->regions[i];
        const Region& b = engine->regions[i];
        CHECK(a.entry.kind == b.entry.kind);
        REQUIRE(a.paths.size() == b.paths.size());
        for (std::size_t k = 0; k < a.paths.size(); k++) {
            CHECK(a.paths[k].points == b.paths[k].points);
            CHECK(a.paths[k].motion == b.paths[k].motion);
            CHECK(a.paths[k].reducedFeed.size() == a.paths[k].points.size());
        }
    }
    // the same apart from the flags: the engine's text is the plan's with them taken out
    Output stripped = *planned;
    for (Region& r : stripped.regions) {
        for (Path& p : r.paths) {
            p.reducedFeed.clear();
        }
    }
    CHECK(toText(stripped) == toText(*engine));
}

TEST_CASE("A ramp first enters a wide pocket along its longest run", "[adaptive][entry]")
{
    EntryOptions o;
    o.order = {EntryKind::Ramp, EntryKind::Helix};
    auto out = planCase("p2", 0.2, o);
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    const Region& r = out->regions[0];
    REQUIRE(r.entry.kind == EntryMode::Ramp);
    // the circle's tool-centre area is 25.4 mm across (less the finishing allowance): the ramp spans it
    const Path& ramp = r.paths.front();
    const double length =
        std::hypot(double(ramp.points[1].x - ramp.points[0].x), double(ramp.points[1].y - ramp.points[0].y));
    CHECK(length > toNm(25.0));
    CHECK(length < toNm(25.4));
    o.maxRampRun = toNm(15.0);
    auto capped = planCase("p2", 0.2, o);
    REQUIRE(capped);
    const Path& short_ = capped->regions[0].paths.front();
    CHECK(std::hypot(double(short_.points[1].x - short_.points[0].x),
                     double(short_.points[1].y - short_.points[0].y)) <= toNm(15.0) + 2'100);
    const sw::Report rep = sw::check(checkInputFor(sw::s5::caseById(cases(), "p2"), *capped, 0.2));
    INFO(sw::summary(rep));
    CHECK(rep.uncutReachable < 0.005);
    CHECK(rep.gougeAnalytic <= 0.010);
    CHECK(rep.linkRemovedMm2 == 0.0);
}

TEST_CASE("A tool-down link whose estimate cuts becomes a cut; others stay links", "[adaptive][entry]")
{
    auto path = [](Role role, Motion motion, std::vector<double> engagement) {
        Path p;
        p.role = role;
        p.motion = motion;
        for (std::size_t i = 0; i < engagement.size(); i++) {
            p.points.push_back({Coord(i) * 1'000'000, 0});
        }
        p.engagementDeg = std::move(engagement);
        return p;
    };
    std::vector<Path> paths = {
        path(Role::Link, Motion::LinkClear, {-1, 0, 0.5, 0}), path(Role::Link, Motion::LinkClear, {-1, 0, 3.0, 0}),
        path(Role::LeadIn, Motion::LinkClear, {-1, 0, 0}), path(Role::Link, Motion::LinkRetract, {-1, 40}),
        path(Role::Pass, Motion::Cut, {-1, 50})};
    CHECK(cutLinksThatCut(paths) == 1);
    CHECK(paths[0].motion == Motion::LinkClear);
    CHECK(paths[1].motion == Motion::Cut);
    CHECK(paths[2].motion == Motion::LinkClear);
    CHECK(paths[3].motion == Motion::LinkRetract);
    CHECK(paths[4].motion == Motion::Cut);
}

TEST_CASE("The reduced-feed flag: above 1.25 x target, half a millimetre either side, ramps whole, links never",
          "[adaptive][entry]")
{
    // A pass of 0.2 mm segments; one segment estimated above 1.25 x 40 = 50 degrees.
    Path p;
    p.role = Role::Pass;
    p.motion = Motion::Cut;
    for (int i = 0; i <= 20; i++) {
        p.points.push_back({Coord(i) * 200'000, 0});
        p.engagementDeg.push_back(i == 0 ? -1.0 : i == 10 ? 50.5 : 49.0);
    }
    flagReducedFeed(p, 40.0);
    // segment 10 spans 1.8..2.0 mm; flagged: every segment overlapping 1.3..2.5 mm (7: 1.2..1.4 to 13: 2.4..2.6)
    for (int i = 1; i <= 20; i++) {
        INFO("segment " << i);
        CHECK(p.reducedFeed[std::size_t(i)] == (i >= 7 && i <= 13 ? 1 : 0));
    }
    CHECK(p.reducedFeed[0] == 0);
    // a lead-in's last segment above the target is flagged (and the half millimetre before it: segments 18 to 20,
    // 3.4..4.0 mm, overlap 3.3..4.0); above the target elsewhere is not
    Path in = p;
    in.role = Role::LeadIn;
    std::fill(in.engagementDeg.begin() + 1, in.engagementDeg.end(), 41.0);
    flagReducedFeed(in, 40.0);
    CHECK(in.reducedFeed[20] == 1);
    CHECK(in.reducedFeed[18] == 1);
    CHECK(in.reducedFeed[17] == 1);
    CHECK(in.reducedFeed[16] == 0);
    // links and ramps
    Path link = p;
    link.motion = Motion::LinkClear;
    flagReducedFeed(link, 40.0);
    CHECK(std::count(link.reducedFeed.begin(), link.reducedFeed.end(), 1) == 0);
    Path ramp;
    ramp.role = Role::Ramp;
    ramp.motion = Motion::Cut;
    ramp.points = {{0, 0}, {20'000'000, 0}};
    ramp.engagementDeg = {-1, 180};
    flagReducedFeed(ramp, 40.0);
    CHECK(ramp.reducedFeed == std::vector<std::uint8_t>{0, 1});
}

// ---------- every S5 pocket at both loads ([slow]) ----------
namespace {

struct Row {
    std::string id;
    double stepover;
    sw::Report report;
};

const std::vector<Row>& allRows()
{
    static const std::vector<Row> rows = [] {
        std::vector<Row> out;
        for (double s : {0.2, 0.1}) {
            for (const char* id : {"p1", "p2", "p3", "p4", "p5", "p6"}) {
                auto plan = planCase(id, s);
                REQUIRE(plan);
                out.push_back({id, s, sw::check(checkInputFor(sw::s5::caseById(cases(), id), *plan, s))});
                const sw::Report& r = out.back().report;
                WARN(id << " at " << s * 100 << "%: " << sw::summary(r) << " | flagged " << r.pctFlagged
                        << "%, unflagged above 1.25x " << r.pctAboveFlagCheckUnflagged << "% (max " << r.unflaggedMax
                        << " at " << r.unflaggedMaxAt.first << ", " << r.unflaggedMaxAt.second << "); forward: p99 "
                        << r.forward.p99 << " max " << r.forward.max << ", unflagged above 1.25x "
                        << r.forward.pctAboveFlagCheckUnflagged << "% (max " << r.forward.unflaggedMax << ")");
            }
        }
        return out;
    }();
    return rows;
}

} // namespace

TEST_CASE("Tool-down links remove no material on any S5 pocket at either load", "[adaptive][entry][slow]")
{
    for (const Row& row : allRows()) {
        INFO(row.id << " at " << row.stepover * 100 << "%: " << sw::summary(row.report));
        CHECK(row.report.linkRemovedMm2 == 0.0);
        CHECK(row.report.linkMaxDeg == 0.0);
    }
}

TEST_CASE("Air cutting is at most 10% on BP1 and BP2 and 15% on the other S5 pockets", "[adaptive][entry][slow]")
{
    for (const Row& row : allRows()) {
        INFO(row.id << " at " << row.stepover * 100 << "%: " << sw::summary(row.report));
        CHECK(row.report.pctAir <= (row.id == "p1" || row.id == "p2" ? 10.0 : 15.0));
    }
}

TEST_CASE("Every cut read above 1.25 x target is flagged for reduced feed", "[adaptive][entry][slow]")
{
    for (const Row& row : allRows()) {
        const sw::Report& r = row.report;
        INFO(row.id << " at " << row.stepover * 100 << "%: " << sw::summary(r));
        // the forward-half reading (the edge that can cut): none unflagged
        CHECK(r.forward.pctAboveFlagCheckUnflagged == 0.0);
        // the full reading counts lagged material behind the tool in tight turns (sweepcheck.h, Report::Forward):
        // held at what was measured (at most 0.47% of cut length, the slot at 10% load)
        CHECK(r.pctAboveFlagCheckUnflagged <= 0.5);
        // flags are not everywhere: at most 2% of cut length
        CHECK(r.pctFlagged <= 2.0);
        if (row.id == "p1") {
            // S5's 95.5 degree spike in p1's corner of the tool's radius (T-060's report: 0.10% of cut length above
            // 1.5 x target) is on a flagged move, by either reading
            CHECK(r.max > 90.0);
            CHECK(r.unflaggedMax < 85.0);
            CHECK(r.forward.unflaggedMax < 1.25 * angleForStepover(row.stepover));
        }
    }
}
