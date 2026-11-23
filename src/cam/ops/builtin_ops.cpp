// The operation types src/cam ships (cam.h registerBuiltinOperationTypes, task T-035). Each new type in src/cam/ops
// adds its line here (docs/operation_types.md).
#include "drill.h"
#include "engrave.h"    // T-056
#include "threadmill.h" // T-056

namespace ray::cam {

void registerBuiltinOperationTypes(OperationRegistry& registry)
{
    registry.add(ops::makeDrillOperationType());
    registry.add(ops::makeEngraveOperationType());    // T-056
    registry.add(ops::makeThreadmillOperationType()); // T-056
}

} // namespace ray::cam
