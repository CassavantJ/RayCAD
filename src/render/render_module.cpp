// Stub translation unit (task T-001). It compiles the module's public header and gives the linker
// one real symbol per dependency edge, so the CMake module graph is exercised. No product code yet.
#include <ray/render.h>

#include <QtCore/QtGlobal>
#include <QtGui/QColor>

#include <vector>

namespace ray::detail {
const char* core_module() noexcept;
const char* kernel_module() noexcept;
const char* sim_module() noexcept;

const char* render_module() noexcept
{
    return "render";
}

// The modules this one links directly (PLAN.md section 1.1).
std::vector<const char*> render_links()
{
    return {core_module(), kernel_module(), sim_module()};
}

// One call into Qt GUI, so the Qt link edge is real. Returns the Qt run-time version, e.g. "6.12.0".
const char* render_qt_version() noexcept
{
    return QColor(Qt::black).isValid() ? qVersion() : "";
}
} // namespace ray::detail
