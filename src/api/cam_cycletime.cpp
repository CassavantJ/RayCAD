// The cycle-time estimate for the CAM workspace (task T-059): include/ray_api/cam_cycletime.h.
#include <ray_api/cam_cycletime.h>

#include <ray/cam.h>
#include <ray/libraries.h>

namespace ray::api::cam_cycletime {

bool SetupInput::same(const SetupInput& other) const
{
    return setup == other.setup && operations == other.operations && toolpaths == other.toolpaths &&
           toolNumbers == other.toolNumbers && complete == other.complete && machineJson == other.machineJson;
}

std::vector<SetupInput> setupInputs(const cam::CamState& cam, const std::map<OperationId, cam::OpState>& statuses)
{
    std::vector<SetupInput> out;
    out.reserve(cam.setups.size());
    for (const cam::Setup& s : cam.setups) {
        SetupInput in;
        in.setup = s.id;
        if (const auto m = cam.machines.find(s.machine); m != cam.machines.end())
            in.machine = m->second.item;
        in.machineJson = lib::toJson(in.machine);
        in.complete = true;
        for (const OperationId& id : s.operations) {
            const auto op = cam.operations.find(id);
            if (op == cam.operations.end() || op->second.suppressed)
                continue;
            const auto st = statuses.find(id);
            if (st == statuses.end() || st->second.status != cam::OpStatus::UpToDate || !st->second.toolpath) {
                in.complete = false;
                continue;
            }
            in.operations.push_back(id);
            in.toolpaths.push_back(st->second.toolpath);
            in.toolNumbers.push_back(op->second.tool.toolNumber);
        }
        out.push_back(std::move(in));
    }
    return out;
}

cam::CycleTime estimate(const SetupInput& input)
{
    std::vector<cam::CycleTimeOperation> ops;
    ops.reserve(input.toolpaths.size());
    for (std::size_t i = 0; i < input.toolpaths.size(); ++i)
        ops.push_back({input.toolpaths[i].get(), i < input.toolNumbers.size() ? input.toolNumbers[i] : 0});
    return cam::estimateCycleTime(ops, input.machine);
}

} // namespace ray::api::cam_cycletime
