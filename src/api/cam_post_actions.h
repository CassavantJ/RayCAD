// The cam.post action (task T-047, cam_post_actions.cpp).
#pragma once

#include <ray/core.h>

namespace ray::api::detail {

class AppImpl;
Status registerCamPostActions(AppImpl& app);

} // namespace ray::api::detail
