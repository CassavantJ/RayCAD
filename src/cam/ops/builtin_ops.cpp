// The operation types src/cam ships (cam.h registerBuiltinOperationTypes, task T-035). Each new type in src/cam/ops
// adds its line here (docs/operation_types.md).
#include "contour2d.h"
#include "drill.h"
#include "face.h"

namespace ray::cam {

void registerBuiltinOperationTypes(OperationRegistry& registry)
{
    registry.add(ops::makeDrillOperationType());
    registry.add(ops::makeFaceOperationType()); // T-052
    registry.add(ops::makeContour2dOperationType()); // "contour2d" (T-053)
}

} // namespace ray::cam
