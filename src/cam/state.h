// CAM state helpers shared by state.cpp and engine.cpp (task T-032). Private to src/cam.
#pragma once

#include <ray/cam.h>

#include <optional>
#include <string>

// Returns the error of a Status expression.
#define RAY_CAM_TRY(expr)                                                                                              \
    do {                                                                                                               \
        if (auto ray_cam_status_ = (expr); !ray_cam_status_) {                                                         \
            return tl::unexpected(std::move(ray_cam_status_.error()));                                                 \
        }                                                                                                              \
    } while (false)

namespace ray::cam::detail {

// The setup that lists `op`, and the operation's index in its list; null when no setup lists it.
const Setup* setupOf(const CamState& state, const OperationId& op, std::size_t* setupIndex = nullptr);
const Setup* findSetup(const CamState& state, const SetupId& id, std::size_t* setupIndex = nullptr);

// The built body `id` of the snapshot; null when it is not built.
const naming::ResolverBody* bodyOf(const CamSnapshot& snapshot, const BodyId& id);

// A reference resolved for CAM: the elements it stands for now, or ReferenceBroken saying why (in words the review
// panel can show; `what` names the reference, e.g. "geometry 2 of 'Drill 1'"). Usable for CAM means
// naming::Resolved::postableWithoutReview(): Exact, or a Repaired the user confirmed.
Result<naming::Resolved> resolveForCam(const naming::IResolver& resolver, const CamSnapshot& snapshot,
                                       const naming::TopoRef& ref, const std::string& what);

// The embedded items an operation's generator receives; NotFound naming the item when it is missing.
struct GenerateItems {
    const lib::Tool* tool = nullptr;
    const lib::Holder* holder = nullptr; // null when the operation has none
    const lib::MachineDef* machine = nullptr;
};
Result<GenerateItems> itemsOf(const CamState& state, const Setup& setup, const Operation& op);
// The same reduced to what the input hash covers (cam.h generatorTool, generatorHolder, generatorMachine): the copies
// the engine hands a generator in GenerateInput.
struct GeneratorItems {
    lib::Tool tool;
    std::optional<lib::Holder> holder; // empty when the operation has none
    lib::MachineDef machine;
};
Result<GeneratorItems> generatorItemsOf(const CamState& state, const Setup& setup, const Operation& op);

// "'Rough pocket'" or the id when the name is empty: how messages name an operation or a setup.
std::string label(const Operation& op);
std::string label(const Setup& setup);

} // namespace ray::cam::detail
