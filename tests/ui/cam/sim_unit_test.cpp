// T-074: the Simulate workspace's non-visual choices (src/ui/cam/sim_logic.h): the stock's display budget per tier
// cell, the playback speeds, the events' titles and severities, the markers merged on the timeline, the clock's text;
// and the CAM tree's rule for asking the engine for a carried stock (sim_carried.h chainReady).
#include "cam/sim_carried.h"
#include "cam/sim_logic.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace ray;
using namespace ray::ui::camui;

TEST_CASE("the stock's display budget follows the tier's simulation cell; playback asks for half", "[sim][logic]")
{
    CHECK(stockTriangleBudget(0.2) == 150'000);  // Low
    CHECK(stockTriangleBudget(0.1) == 250'000);  // Medium
    CHECK(stockTriangleBudget(0.05) == 350'000); // High
    CHECK(stockTriangleBudget(0.01) == 400'000); // capped
    CHECK(stockTriangleBudget(1.0) == 100'000);  // floored
    CHECK(stockTriangleBudget(0) == 250'000);    // not known
    CHECK(playbackCap(250'000) == 125'000);
    CHECK(playbackCap(10'000) == 10'000);
    // GPU memory of the three budgets, as the report gives it (about 86 bytes a triangle).
    CHECK(stockTriangleBudget(0.2) * kStockBytesPerTriangle / 1e6 < 13);
    CHECK(stockTriangleBudget(0.05) * kStockBytesPerTriangle / 1e6 < 31);
}

TEST_CASE("playback speeds are machine-time multiples, slowest first, 10x by default", "[sim][logic]")
{
    const auto speeds = playbackSpeeds();
    REQUIRE(speeds.size() == 9);
    CHECK(speeds.front() == 1);
    CHECK(speeds.back() == 1000);
    for (std::size_t i = 1; i < speeds.size(); ++i)
        CHECK(speeds[i] > speeds[i - 1]);
    CHECK(speeds[kDefaultSpeedIndex] == 10);
    CHECK(speedText(10) == QStringLiteral("10×"));
    CHECK(speedText(1000) == QStringLiteral("1000×"));
}

TEST_CASE("every event kind has a title in a machinist's words and its enumerator's name", "[sim][logic]")
{
    std::set<QString> titles;
    for (int kind = 0; kind <= 15; ++kind) {
        INFO("kind " << kind);
        CHECK(QString::fromLatin1(eventKindName(kind)).size() > 0);
        CHECK_FALSE(eventTitle(kind).isEmpty());
        titles.insert(eventTitle(kind));
    }
    CHECK(titles.size() == 16); // each its own
    CHECK(QString::fromLatin1(eventKindName(5)) == QStringLiteral("HolderHitsFixture"));
    CHECK(eventTitle(5) == QStringLiteral("Holder hits a fixture"));
    CHECK(QString::fromLatin1(eventKindName(13)) == QStringLiteral("ShankHitsPart"));
    // The setup's (after T-074): a fixture against the part and the stock.
    CHECK(QString::fromLatin1(eventKindName(14)) == QStringLiteral("FixtureHitsPart"));
    CHECK(eventTitle(14) == QStringLiteral("Fixture cuts into the part"));
    CHECK(QString::fromLatin1(eventKindName(15)) == QStringLiteral("FixtureHitsStock"));
    CHECK(eventTitle(15) == QStringLiteral("Fixture overlaps the stock"));
    CHECK(QString::fromLatin1(eventKindName(16)).isEmpty());
    CHECK(severityKey(SimSeverity::Error) == QStringLiteral("error"));
    CHECK(severityKey(SimSeverity::Warning) == QStringLiteral("warning"));
    CHECK(severityKey(SimSeverity::Info) == QStringLiteral("info"));
}

TEST_CASE("the slow-seek line: only when thinned and over the tier's budget, the time to a tenth", "[sim][logic]")
{
    CHECK(seekBudgetMs(0.2) == 250);  // Low
    CHECK(seekBudgetMs(0.1) == 150);  // Medium
    CHECK(seekBudgetMs(0.05) == 100); // High
    CHECK(seekBudgetMs(0) == 150);    // not known
    // Within the budget, or not thinned (memory is the cause the line names): nothing.
    CHECK(slowSeekText(true, 90, 0.05).isEmpty());
    CHECK(slowSeekText(true, 100, 0.05).isEmpty());
    CHECK(slowSeekText(false, 600, 0.05).isEmpty());
    CHECK(slowSeekText(true, 240, 0.2).isEmpty());
    // Over it: one plain line, the time rounded.
    const QString high = slowSeekText(true, 614, 0.05);
    CHECK(high == QStringLiteral("Jumping around this program may take up to about 0.6 s at this resolution: it needs "
                                 "more memory for jump points than is allowed. A coarser resolution jumps faster."));
    CHECK(slowSeekText(true, 312, 0.05).contains(QStringLiteral("about 0.3 s")));
    CHECK(slowSeekText(true, 160, 0.1).contains(QStringLiteral("about 0.2 s")));
    CHECK(slowSeekText(true, 4380, 0.05).contains(QStringLiteral("about 4.4 s")));
    CHECK(slowSeekText(true, 12400, 0.05).contains(QStringLiteral("about 12 s")));
    for (const QString& word : {QStringLiteral("keyframe"), QStringLiteral("tier"), QStringLiteral("thinn")})
        CHECK_FALSE(high.contains(word, Qt::CaseInsensitive));
}

TEST_CASE("markers closer than the merge distance become one: at the earliest, the worst severity, the count",
          "[sim][logic]")
{
    const MarkerInput markers[] = {{10, SimSeverity::Warning, 0},
                                   {14, SimSeverity::Error, 1},
                                   {21, SimSeverity::Warning, 2}, // within 8 px of the one before: a chain
                                   {40, SimSeverity::Info, 3},
                                   {200, SimSeverity::Warning, 4}};
    const auto clusters = clusterMarkers(markers);
    REQUIRE(clusters.size() == 3);
    CHECK(clusters[0].x == 10);
    CHECK(clusters[0].severity == SimSeverity::Error);
    CHECK(clusters[0].events == std::vector<int>{0, 1, 2});
    CHECK(clusters[1].events == std::vector<int>{3});
    CHECK(clusters[1].severity == SimSeverity::Info);
    CHECK(clusters[2].x == 200);
    CHECK(clusterMarkers({}).empty());
    // Wider apart than the merge distance: every one its own.
    const auto apart = clusterMarkers(markers, 3);
    CHECK(apart.size() == 5);
}

TEST_CASE("the timeline's clock: tenths of a second under an hour, h:mm:ss above", "[sim][logic]")
{
    CHECK(formatClock(0) == QStringLiteral("0:00.0"));
    CHECK(formatClock(7.44) == QStringLiteral("0:07.4"));
    CHECK(formatClock(723) == QStringLiteral("12:03.0"));
    CHECK(formatClock(59.96) == QStringLiteral("1:00.0"));
    CHECK(formatClock(3723) == QStringLiteral("1:02:03"));
    CHECK(formatClock(-1) == QStringLiteral("0:00.0"));
}

TEST_CASE("the carried stock is asked for only when the earlier setups are up to date", "[sim][logic][carried]")
{
    auto state = std::make_shared<doc::DocState>();
    auto cam = std::make_shared<cam::CamState>();
    cam::Setup one, two, three;
    one.id = SetupId{Uuid::fromString("a0000000-0000-4000-8000-000000000001")};
    two.id = SetupId{Uuid::fromString("a0000000-0000-4000-8000-000000000002")};
    three.id = SetupId{Uuid::fromString("a0000000-0000-4000-8000-000000000003")};
    two.stock = cam::FromSetupStock{one.id};
    three.stock = cam::FromSetupStock{two.id};
    const OperationId a{Uuid::fromString("c0000000-0000-4000-8000-000000000001")};
    const OperationId b{Uuid::fromString("c0000000-0000-4000-8000-000000000002")};
    const OperationId off{Uuid::fromString("c0000000-0000-4000-8000-000000000003")};
    one.operations = {a, off};
    two.operations = {b};
    cam::Operation opA, opB, opOff;
    opA.id = a;
    opB.id = b;
    opOff.id = off;
    opOff.suppressed = true;
    cam->operations = {{a, opA}, {b, opB}, {off, opOff}};
    cam->setups = {one, two, three};
    state->cam = cam;
    auto path = std::make_shared<const cam::Toolpath>();
    std::map<OperationId, cam::OpState> statuses;
    statuses[a] = cam::OpState{cam::OpStatus::UpToDate, {}, path, -1};
    statuses[b] = cam::OpState{cam::OpStatus::Stale, {}, path, -1};
    CHECK_FALSE(chainReady(*state, one.id, statuses));   // its stock is its own
    CHECK(chainReady(*state, two.id, statuses));         // setup 1 up to date (the suppressed one does not count)
    CHECK_FALSE(chainReady(*state, three.id, statuses)); // setup 2 is stale
    statuses[b].status = cam::OpStatus::UpToDate;
    CHECK(chainReady(*state, three.id, statuses));
    statuses.erase(a);
    CHECK_FALSE(chainReady(*state, three.id, statuses)); // never generated, down the chain
}
