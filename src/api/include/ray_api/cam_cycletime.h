// The cycle-time estimate (cam::estimateCycleTime, task T-059) for the desktop CAM workspace, without Qt: what one
// setup's estimate is made of, taken from the engine's statuses, and the estimate itself, to run on a worker.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls cam or lib itself. Not one of the contract
// headers in include/ray/.
#pragma once

#include <ray/api.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ray::api::cam_cycletime {

// One setup's operations as the estimate takes them. Cheap to make (no toolpath is copied or hashed), so it is made on
// the UI thread from what the caller already holds, and compared with the one an estimate was made from.
struct SetupInput {
    SetupId setup;
    // The setup's operations that are not suppressed and are up to date (OpStatus::UpToDate with a toolpath), in
    // program order, with their toolpaths and tool numbers (parallel).
    std::vector<OperationId> operations;
    std::vector<std::shared_ptr<const cam::Toolpath>> toolpaths;
    std::vector<int> toolNumbers;
    // Every operation that is not suppressed is there: the setup's total is the program's.
    bool complete = false;
    // The setup's embedded machine (a MachineDef with nothing known when it has none: every default applies), and
    // its JSON, which `same` compares.
    lib::MachineDef machine;
    std::string machineJson;
    // The same toolpaths (by identity), tool numbers and machine: an estimate of one is the estimate of the other.
    bool same(const SetupInput& other) const;
};

// Every setup of `cam` in program order, from the engine's statuses (ICamEngine::status or statusAsync).
std::vector<SetupInput> setupInputs(const cam::CamState& cam, const std::map<OperationId, cam::OpState>& statuses);

// cam::estimateCycleTime of the input with the default options; CycleTime::operations parallels input.operations.
// Pure and quick (well under a second for a million moves), but call it on a worker.
cam::CycleTime estimate(const SetupInput& input);

} // namespace ray::api::cam_cycletime
