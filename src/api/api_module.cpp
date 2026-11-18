// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/api.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* kernel_module() noexcept;
const char* naming_module() noexcept;
const char* sketch_module() noexcept;
const char* lib_module() noexcept;
const char* doc_module() noexcept;
const char* store_module() noexcept;
const char* cam_module() noexcept;
const char* sim_module() noexcept;
const char* post_module() noexcept;

const char* api_module() noexcept
{
    return "api";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> api_links()
{
    return {core_module(), kernel_module(), naming_module(), sketch_module(), lib_module(),
            doc_module(),  store_module(),  cam_module(),    sim_module(),    post_module()};
}
} // namespace ray::detail
