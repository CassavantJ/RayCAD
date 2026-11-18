// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/cam.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* kernel_module() noexcept;
const char* naming_module() noexcept;
const char* lib_module() noexcept;

const char* cam_module() noexcept
{
    return "cam";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> cam_links()
{
    return {core_module(), kernel_module(), naming_module(), lib_module()};
}
} // namespace ray::detail
