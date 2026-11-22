// Operation templates as actions (task T-057, cam_template_actions.cpp), and the cam.batch document command they apply
// their operations with (one undo step).
#pragma once

#include <ray/cam.h>
#include <ray/core.h>
#include <ray/document.h>

#include <memory>

namespace ray::api::detail {

class AppImpl;
// cam.template.preview, cam.template.apply, cam.template.reapply, cam.template.saveFrom, cam.template.rename and
// cam.template.remove.
Status registerCamTemplateActions(AppImpl& app);

// The document command "cam.batch": {"commands": [{"name": "cam.operation.add", "args": {...}}, ...]}, the CAM
// commands of `base` (cam.operation.*, cam.library.embed, cam.setup.edit) applied in order as ONE command, so one undo
// step; the first that fails fails the whole (naming its index and command) and nothing changes. Changes toolpaths.
std::unique_ptr<doc::ICommandHandler> makeCamBatchCommand(std::shared_ptr<const doc::CommandRegistry> base);
// cam.batch's cam.operation.add / edit sub-commands checked and completed as those actions are (checkOperationParams).
Result<doc::Command> checkBatchParams(const cam::OperationRegistry* types, const doc::DocState& state,
                                      const doc::Command& command);

} // namespace ray::api::detail
