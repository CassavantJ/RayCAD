// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/post.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* cam_module() noexcept;
const char* lib_module() noexcept;

const char* post_module() noexcept
{
    return "post";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> post_links()
{
    return {core_module(), cam_module(), lib_module()};
}
} // namespace ray::detail
