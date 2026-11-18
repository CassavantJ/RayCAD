// Stub translation unit (task T-001): one real symbol per dependency edge of the ui module.
#include <ray/api.h>
#include <ray/render.h>

#include <vector>

namespace ray::detail {
const char* api_module() noexcept;
const char* render_module() noexcept;

const char* ui_module() noexcept
{
    return "ui";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> ui_links()
{
    return {api_module(), render_module()};
}
} // namespace ray::detail
