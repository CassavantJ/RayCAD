// The operation types src/cam ships (cam.h registerBuiltinOperationTypes, task T-035). Each new type in src/cam/ops
// adds its line here (docs/operation_types.md).
#include "contour2d.h"
#include "drill.h"
#include "engrave.h" // T-056
#include "face.h"
#include "threadmill.h" // T-056
// T-055: slot and chamfer2d
#include "chamfer2d.h"
#include "pocket2d.h"
#include "slot.h"

namespace ray::cam {

void registerBuiltinOperationTypes(OperationRegistry& registry)
{
    registry.add(ops::makeDrillOperationType());
    registry.add(ops::makeEngraveOperationType());    // T-056
    registry.add(ops::makeThreadmillOperationType()); // T-056
    registry.add(ops::makeFaceOperationType());       // T-052
    registry.add(ops::makeContour2dOperationType());  // "contour2d" (T-053)
    // T-055: slot and chamfer2d
    registry.add(ops::makeSlotOperationType());
    registry.add(ops::makeChamfer2dOperationType());
    registry.add(ops::makePocket2dOperationType()); // T-054
}

} // namespace ray::cam
