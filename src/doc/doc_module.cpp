// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/document.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* kernel_module() noexcept;
const char* naming_module() noexcept;
const char* sketch_module() noexcept;
const char* cam_module() noexcept;

const char* doc_module() noexcept
{
    return "doc";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> doc_links()
{
    return {core_module(), kernel_module(), naming_module(), sketch_module(), cam_module()};
}
} // namespace ray::detail
