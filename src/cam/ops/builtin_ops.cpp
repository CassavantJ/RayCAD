// The operation types src/cam ships (cam.h registerBuiltinOperationTypes, task T-035). Each new type in src/cam/ops
// adds its line here (docs/operation_types.md).
#include "drill.h"
// T-055: slot and chamfer2d
#include "chamfer2d.h"
#include "slot.h"

namespace ray::cam {

void registerBuiltinOperationTypes(OperationRegistry& registry)
{
    registry.add(ops::makeDrillOperationType());
    // T-055: slot and chamfer2d
    registry.add(ops::makeSlotOperationType());
    registry.add(ops::makeChamfer2dOperationType());
}

} // namespace ray::cam
