// Private to src/kernel and its tests. Not a public interface.
#pragma once
#include <ray/core.h>

#include <string>
#include <vector>

namespace ray::kernel::detail {

// The OCCT toolkits ray_kernel links, as listed in src/kernel/CMakeLists.txt.
std::vector<std::string> occtToolkits();

// Does a small piece of real work in every toolkit from occtToolkits() and returns their names in the
// order they were exercised. On Windows the toolkits are delay-loaded, so this is also the check that
// every DLL is present: a missing one is reported as ErrorCode::KernelFailure, not as a crash.
Result<std::vector<std::string>> occtSmoke();

// OCCT version the kernel was compiled against, e.g. "8.0.1".
std::string occtVersion();

} // namespace ray::kernel::detail
