# render: tier detection, theme tokens, viewport renderer. Qt is allowed here and in src/ui only.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui)

add_library(ray_render STATIC
    render_module.cpp
    tier.cpp
    tier_detect.cpp
    tier_internal.h
    theme.cpp
    theme_internal.h)
ray_module(ray_render)
# On Windows tier_detect.cpp loads dxgi.dll and d3d11.dll at run time, so no system library is linked here.
target_link_libraries(ray_render PRIVATE ray_core ray_kernel ray_sim Qt6::Core Qt6::Gui)
