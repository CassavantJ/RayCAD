// The deliberate fault behind directedit_testing.h DirectEditFault (task T-080). A translation unit of its own, so
// <windows.h> (and its macros) stay out of directedit.cpp.
#include "directedit_internal.h"

#include <Standard_Failure.hxx>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ray::kernel::detail {

void raiseTestFault()
{
#if defined(_WIN32)
    // What an access violation inside OCCT looks like to the guard, without touching memory.
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    throw Standard_Failure("deliberate test fault");
}

} // namespace ray::kernel::detail
