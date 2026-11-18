// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/store.h>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* doc_module() noexcept;

const char* store_module() noexcept
{
    return "store";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> store_links()
{
    return {core_module(), doc_module()};
}
} // namespace ray::detail
