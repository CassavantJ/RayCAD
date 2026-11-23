// The adaptive engine's contract (task T-060, engine.h): refusals with a defined error, a failed start with no
// uninitialised data, the shape of the output, progress, and cancel latency at any point of a run ([wallclock]).
#include "adaptive_test_util.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::cam::adaptive2d;
using namespace ray::cam::adaptive2d::test;

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

} // namespace

TEST_CASE("Engagement angle and stepover convert both ways", "[adaptive]")
{
    CHECK(angleForStepover(0.2) == Catch::Approx(53.130102354).epsilon(1e-9));
    CHECK(angleForStepover(0.1) == Catch::Approx(36.869897646).epsilon(1e-9));
    CHECK(angleForStepover(0.5) == Catch::Approx(90.0).epsilon(1e-12));
    CHECK(angleForStepover(1.0) == Catch::Approx(180.0).epsilon(1e-12));
    for (double s : {0.01, 0.05, 0.1, 0.2, 0.35, 0.5, 0.8}) {
        CHECK(stepoverForAngle(angleForStepover(s)) == Catch::Approx(s).epsilon(1e-12));
    }
}

TEST_CASE("Bad input is refused with InvalidArgument naming what is wrong", "[adaptive]")
{
    const Input good = inputFor(sw::s5::caseById(cases(), "p2"), 0.2);
    auto refused = [](const Input& in, const std::string& part) {
        JobContext ctx;
        auto out = runEngine(in, ctx);
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::InvalidArgument);
        INFO(out.error().message);
        CHECK(contains(out.error().message, part));
    };
    Input in = good;
    in.regions.clear();
    refused(in, "no area");
    in = good;
    in.toolDiameter = 0;
    refused(in, "tool diameter");
    in = good;
    in.stepover = 0.005;
    refused(in, "stepover");
    in.stepover = 1.5;
    refused(in, "stepover");
    in = good;
    in.tolerance = 5'000;
    refused(in, "tolerance");
    in.tolerance = 2'000'000;
    refused(in, "tolerance");
    in = good;
    in.stockToLeave = -1;
    refused(in, "must not be negative");
    in = good;
    in.helixDiameter = -1;
    refused(in, "must not be negative");
    in = good;
    in.keepToolDownRatio = -0.5;
    refused(in, "keep-tool-down");
    in = good;
    in.regions = regionsOf({sw::s5::rect(0, 0, 2e6, 10)}); // 2 km: beyond geom2d's range
    refused(in, "area");
    in = good;
    // At the finest grid (0.01 mm tolerance and stepover x diameter 0.01 mm: 480,000 units per mm) a 5 m pocket
    // is beyond the grid's extent (2^31 units: 4.47 m).
    in.regions = regionsOf({sw::s5::rect(0, 0, 5000, 10)});
    in.stock.clear();
    in.toolDiameter = toNm(1.0);
    in.stepover = 0.01;
    in.tolerance = 10'000;
    refused(in, "too large for the engine's grid");
}

TEST_CASE("A failed start returns a defined error, or with allowUnentered a region with every field defined",
          "[adaptive]")
{
    // S5 p4: a 14.5 mm slot with a 12.7 mm tool at 20% load: no helix fits (upstream returned uninitialised fields).
    const Input in = inputFor(sw::s5::caseById(cases(), "p4"), 0.2);
    {
        JobContext ctx;
        auto out = runEngine(in, ctx);
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::ToolpathFailed);
        INFO(out.error().message);
        CHECK(contains(out.error().message, "no entry into the region around (35"));
        CHECK(contains(out.error().message, ", 7.25"));
        CHECK(contains(out.error().message, "smallest helix (diameter 1.588 mm)"));
        CHECK(out.error().context.empty());
    }
    Input allow = in;
    allow.allowUnentered = true;
    JobContext ctx;
    auto out = runEngine(allow, ctx);
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    const Region& r = out->regions[0];
    CHECK(r.entry.kind == EntryMode::None);
    CHECK(r.entry.centre == Pt{});
    CHECK(r.entry.start == Pt{});
    CHECK(r.entry.radius == 0);
    CHECK(r.paths.empty());
    CHECK_FALSE(r.returnClear);
    CHECK(r.clearedAreaMm2 == 0);
    CHECK_FALSE(r.toolBounds.empty()); // T-063 plans its ramp in here
    REQUIRE(out->warnings.size() == 1);
    CHECK(out->warnings[0].code == Warning::Code::NotEntered);
    CHECK(out->stats.gridUnitsPerMm == 480);
    // The same call twice gives the same bytes (upstream's 20 processes gave 20 different files here).
    JobContext ctx2;
    auto again = runEngine(allow, ctx2);
    REQUIRE(again);
    CHECK(toText(*again) == toText(*out));
}

TEST_CASE("A tool that fits nowhere is refused", "[adaptive]")
{
    Input in;
    in.regions = regionsOf({sw::s5::circle(0, 0, 6.0)}); // 12 mm hole, 12.7 mm tool
    in.toolDiameter = toNm(kToolMm);
    in.stepover = 0.2;
    JobContext ctx;
    auto out = runEngine(in, ctx);
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::ToolpathFailed);
    INFO(out.error().message);
    CHECK(contains(out.error().message, "does not fit anywhere"));
}

TEST_CASE("The output of a circular pocket: a helix entry, passes, leads, links and a finishing pass", "[adaptive]")
{
    // S5 p2 (BP2 style, 38.1 mm circle) at 20% load.
    auto out = runCase("p2", 0.2);
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    CHECK(out->warnings.empty());
    const Region& r = out->regions[0];
    CHECK(r.entry.kind == EntryMode::Helix);
    // The largest helix that fits, at most the requested 9.525 mm, at least upstream's minimum (tool / 8).
    CHECK(r.entry.radius <= toNm(0.75 * kToolMm / 2) + 2'100);
    CHECK(r.entry.radius >= toNm(kToolMm / 16) - 2'100);
    CHECK(r.entry.start.x == r.entry.centre.x);
    CHECK(r.entry.start.y == r.entry.centre.y - r.entry.radius);
    REQUIRE_FALSE(r.paths.empty());
    CHECK(r.paths.front().role == Role::Pass);
    CHECK(r.paths.front().points.front() == r.entry.start);
    bool finish = false;
    std::vector<double> passEngagement;
    const Pt centre = ray::cam::geom2d::toPt({41.275, 41.275});
    for (const Path& p : r.paths) {
        REQUIRE_FALSE(p.points.empty());
        REQUIRE(p.engagementDeg.size() == p.points.size());
        CHECK(p.engagementDeg[0] == -1);
        if (p.motion == Motion::LinkRetract) {
            CHECK(p.points.size() == 2);
            CHECK(p.role == Role::Link);
        }
        if (p.motion != Motion::Cut) {
            CHECK(p.role == Role::Link);
        }
        finish = finish || p.role == Role::Finish;
        for (std::size_t i = 0; i < p.points.size(); i++) {
            // Every tool centre stays inside the pocket shrunk by the tool radius, within the 10 um gouge bound of the
            // acceptance (S5 measured up to 8.5 um: the engine's grid is 2.1 um and its offsets round twice).
            double d = std::hypot(double(p.points[i].x - centre.x), double(p.points[i].y - centre.y));
            CHECK(d <= toNm(19.05 - kToolMm / 2) + 10'000);
            if (i > 0) {
                CHECK(p.engagementDeg[i] >= 0);
                CHECK(p.engagementDeg[i] <= 180);
                if (p.role == Role::Pass) {
                    passEngagement.push_back(p.engagementDeg[i]);
                }
            }
        }
    }
    CHECK(finish);
    CHECK(r.paths.back().role == Role::LeadOut);
    // The engine's own estimate of the passes' engagement centres on the target.
    std::sort(passEngagement.begin(), passEngagement.end());
    REQUIRE(passEngagement.size() > 100);
    CHECK(std::fabs(passEngagement[passEngagement.size() / 2] - angleForStepover(0.2)) < 2.0);
    CHECK(r.clearedAreaMm2 > 0.95 * 3.14159 * 19.05 * 19.05);
    CHECK_FALSE(out->cleared.empty());
    CHECK(out->stats.passes > 0);
    CHECK(out->stats.steps > 0);
    CHECK(out->stats.linkSearchLimitHits == 0);
}

TEST_CASE("An open pocket is entered from outside the stock, without a helix", "[adaptive]")
{
    auto out = runCase("p5", 0.2);
    REQUIRE(out);
    REQUIRE(out->regions.size() == 1);
    const Region& r = out->regions[0];
    CHECK(r.entry.kind == EntryMode::FromCleared);
    CHECK(r.entry.radius == 0);
    CHECK(r.entry.centre == r.entry.start);
    REQUIRE_FALSE(r.paths.empty());
    CHECK(r.paths.front().role == Role::LeadIn);
    CHECK(r.paths.front().points.front() == r.entry.start);
}

TEST_CASE("Progress rises to 1 and never falls", "[adaptive]")
{
    std::mutex m;
    std::vector<double> seen;
    JobContext ctx;
    ctx.progress = Progress([&](double f, std::string_view) {
        std::lock_guard lock(m);
        seen.push_back(f);
    });
    auto out = runEngine(inputFor(sw::s5::caseById(cases(), "p6"), 0.2), ctx);
    REQUIRE(out);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == 1.0);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
}

TEST_CASE("Cancel returns Cancelled within 50 ms at any point of a run", "[adaptive][wallclock]")
{
    using Clock = std::chrono::steady_clock;
    for (const char* id : {"p1", "p3"}) {
        const Input in = inputFor(sw::s5::caseById(cases(), id), 0.2);
        // The run's length first, then cancels spread over it.
        auto t0 = Clock::now();
        {
            JobContext ctx;
            REQUIRE(runEngine(in, ctx));
        }
        const double total = std::chrono::duration<double>(Clock::now() - t0).count();
        const int points = std::string(id) == "p1" ? 20 : 8;
        double worst = 0;
        for (int k = 0; k < points; k++) {
            const double at = total * (k + 0.5) / points;
            CancelSource source;
            JobContext ctx{source.token(), {}};
            Result<Output> out = tl::unexpected(Error{ErrorCode::Internal, "", {}});
            std::atomic<bool> done{false};
            Clock::time_point returned;
            std::thread worker([&] {
                out = runEngine(in, ctx);
                returned = Clock::now();
                done = true;
            });
            std::this_thread::sleep_for(std::chrono::duration<double>(at));
            const Clock::time_point cancelled = Clock::now();
            source.cancel();
            worker.join();
            if (!out && out.error().code == ErrorCode::Cancelled) {
                double ms = std::chrono::duration<double, std::milli>(returned - cancelled).count();
                worst = std::max(worst, ms);
                INFO(id << " cancelled at " << at << " s of " << total << " s: returned after " << ms << " ms");
                CHECK(ms < 50.0);
            } else {
                // The run finished before the cancel (the last point of a short run on a fast machine).
                CHECK(out);
            }
        }
        WARN(id << ": worst cancel latency " << worst << " ms over " << points << " points of a " << total << " s run");
    }
}
