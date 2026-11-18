// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/core.h>

#include <vector>

namespace ray::detail {

const char* core_module() noexcept
{
    return "core";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> core_links()
{
    return {};
}
} // namespace ray::detail
