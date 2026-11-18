// The measure.* actions (task T-022), private to src/api: registered with the built-in actions.
#pragma once

#include <ray/core.h>

namespace ray::api::detail {

class AppImpl;

// measure.distance, measure.angle, measure.radius, measure.massProperties, measure.boundingBox and measure.selection.
Status registerMeasureActions(AppImpl& app);

} // namespace ray::api::detail
