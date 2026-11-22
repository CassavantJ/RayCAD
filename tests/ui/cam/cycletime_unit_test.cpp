// T-059: how the CAM workspace shows a cycle-time estimate (src/ui/cam/cycletime_logic.h) and what a setup's estimate
// is made of (ray_api/cam_cycletime.h), without a window.
#include "cam/cycletime_logic.h"

#include <ray_api/cam_cycletime.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <memory>

using namespace ray;
namespace camui = ray::ui::camui;

TEST_CASE("cycle time: h:mm:ss, rounded to the second", "[cam][cycletime]")
{
    CHECK(camui::formatDuration(0) == QStringLiteral("0:00:00"));
    CHECK(camui::formatDuration(7.4) == QStringLiteral("0:00:07"));
    CHECK(camui::formatDuration(7.5) == QStringLiteral("0:00:08"));
    CHECK(camui::formatDuration(59.6) == QStringLiteral("0:01:00"));
    CHECK(camui::formatDuration(3723) == QStringLiteral("1:02:03"));
    CHECK(camui::formatDuration(100000) == QStringLiteral("27:46:40"));
    CHECK(camui::formatDuration(-1).isEmpty());
    CHECK(camui::formatDuration(std::numeric_limits<double>::quiet_NaN()).isEmpty());
    CHECK(camui::formatDuration(std::numeric_limits<double>::infinity()).isEmpty());
    CHECK(camui::cycleTimeShort(65) == QStringLiteral("est. 0:01:05"));
    CHECK(camui::cycleTimeShort(-1).isEmpty());
}

TEST_CASE("cycle time: the full text, marked as an estimate, with what it assumed", "[cam][cycletime]")
{
    cam::CycleTimePart p;
    p.cutSec = 600;
    p.rapidSec = 62;
    p.dwellSec = 5;
    p.toolChangeSec = 12.6;
    p.toolChanges = 3;
    p.totalSec = p.cutSec + p.rapidSec + p.dwellSec + p.toolChangeSec;
    cam::CycleTimeAssumptions none;
    CHECK(camui::assumptionNotes(none).isEmpty());
    CHECK(camui::cycleTimeDetails(p, none, true) ==
          QStringLiteral("Estimated cycle time 0:11:20 (cutting 0:10:00, rapids 0:01:02, dwells 0:00:05, "
                         "3 tool changes 0:00:13)."));
    cam::CycleTimeAssumptions all;
    all.accelerationUnknown = all.rapidUnknown = all.maxFeedUnknown = all.toolChangeUnknown = all.feedMissing = true;
    const QStringList notes = camui::assumptionNotes(all);
    REQUIRE(notes.size() == 5);
    CHECK(notes[0] == QStringLiteral("Machine accelerations unknown: 1000 mm/s² assumed."));
    CHECK(notes[1] == QStringLiteral("Machine rapid rates unknown: 10000 mm/min assumed."));
    CHECK(notes[3] == QStringLiteral("Tool change time unknown: 10 s assumed."));
    cam::CycleTimeAssumptions accel;
    accel.accelerationUnknown = true;
    p.dwellSec = 0;
    p.toolChanges = 1;
    p.toolChangeSec = 4.2;
    p.totalSec = p.cutSec + p.rapidSec + p.toolChangeSec;
    CHECK(camui::cycleTimeDetails(p, accel, false) ==
          QStringLiteral("Estimated cycle time 0:11:06 (cutting 0:10:00, rapids 0:01:02, 1 tool change 0:00:04). "
                         "Only the generated toolpaths are counted.\n"
                         "Machine accelerations unknown: 1000 mm/s² assumed."));
}

TEST_CASE("cycle time: a setup's input, from the engine's statuses", "[cam][cycletime]")
{
    const auto id = [](int n) {
        return Uuid::fromString("00000000-0000-4000-8000-00000000000" + std::to_string(n));
    };
    cam::CamState cam;
    lib::MachineDef m;
    m.id = MachineId{id(9)};
    m.toolChangeSec = 2;
    cam.machines[m.id] = {m};
    cam::Setup s;
    s.id = SetupId{id(1)};
    s.machine = m.id;
    for (int n = 2; n <= 5; ++n) {
        cam::Operation op;
        op.id = OperationId{id(n)};
        op.tool.toolNumber = n;
        op.suppressed = n == 4;
        cam.operations[op.id] = op;
        s.operations.push_back(op.id);
    }
    cam.setups.push_back(s);
    // Operation 2 and 5 up to date, 3 stale, 4 suppressed (up to date or not, it is left out).
    auto tp = std::make_shared<cam::Toolpath>();
    cam::Move mv{};
    mv.kind = cam::MoveKind::Linear;
    mv.x = 100'000'000;
    mv.feedMicronPerMin = 3'000'000;
    tp->moves.push_back(mv);
    std::map<OperationId, cam::OpState> st;
    st[OperationId{id(2)}] = {cam::OpStatus::UpToDate, "", tp, -1};
    st[OperationId{id(3)}] = {cam::OpStatus::Stale, "", tp, -1};
    st[OperationId{id(4)}] = {cam::OpStatus::UpToDate, "", tp, -1};
    st[OperationId{id(5)}] = {cam::OpStatus::UpToDate, "", tp, -1};
    const auto inputs = api::cam_cycletime::setupInputs(cam, st);
    REQUIRE(inputs.size() == 1);
    const api::cam_cycletime::SetupInput& in = inputs[0];
    CHECK(in.setup == s.id);
    CHECK(in.operations == std::vector<OperationId>{OperationId{id(2)}, OperationId{id(5)}});
    CHECK(in.toolNumbers == std::vector<int>{2, 5});
    CHECK_FALSE(in.complete); // operation 3 is stale
    CHECK(in.machine.toolChangeSec == 2);
    CHECK(in.same(inputs[0]));
    // Operation 3 generated: complete, and not the same input any more.
    st[OperationId{id(3)}].status = cam::OpStatus::UpToDate;
    const auto again = api::cam_cycletime::setupInputs(cam, st);
    CHECK(again[0].complete);
    CHECK_FALSE(again[0].same(in));
    // The estimate: three operations, three tool numbers, so three tool changes of 2 s.
    const cam::CycleTime t = api::cam_cycletime::estimate(again[0]);
    REQUIRE(t.operations.size() == 3);
    CHECK(t.toolChanges == 3);
    CHECK(t.toolChangeSec == 6);
    CHECK(t.cutSec > 0);
    CHECK(t.assumptions.accelerationUnknown); // the machine has no axes
}
