// How the CAM workspace shows a cycle-time estimate (task T-059, cam.h estimateCycleTime): the text of the CAM tree's
// rows and tooltips and of the post dialog, without a window, so it can be tested on its own. The estimate itself is
// worked out on a worker (ray_api/cam_cycletime.h).
#pragma once

#include <ray/api.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace ray::ui::camui {

// A duration as h:mm:ss, rounded to the nearest second: "0:00:07", "1:02:03", "27:46:40". "" when it is negative or not
// finite.
QString formatDuration(double seconds);
// The quiet mark beside a setup in the tree: "est. 0:12:34" ("" for an invalid duration).
QString cycleTimeShort(double seconds);
// One sentence per assumption the estimate made, in CycleTimeAssumptions' order ("Machine accelerations unknown:
// 1000 mm/s² assumed."); empty when it assumed nothing.
QStringList assumptionNotes(const cam::CycleTimeAssumptions& assumptions);
// The full text (a tooltip, the post dialog): "Estimated cycle time 0:12:34 (cutting 0:10:00, rapids 0:01:02, dwells
// 0:00:05, 3 tool changes 0:00:30)." with "Only the generated toolpaths are counted." when `complete` is false, then
// the assumption notes, each on a line of its own.
QString cycleTimeDetails(const cam::CycleTimePart& part, const cam::CycleTimeAssumptions& assumptions, bool complete);

} // namespace ray::ui::camui
