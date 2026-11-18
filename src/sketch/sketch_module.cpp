// Module-graph translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. The sketch model and solver
// are in model.* and solver*.* (task T-090).
#include <ray/sketch.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* kernel_module() noexcept;

const char* sketch_module() noexcept
{
    return "sketch";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> sketch_links()
{
    return {core_module(), kernel_module()};
}
} // namespace ray::detail
