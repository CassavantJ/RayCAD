# render: tier detection, theme tokens, viewport renderer. Qt is allowed here and in src/ui only.
# GuiPrivate carries the Qt RHI headers (<rhi/qrhi.h>); ShaderTools brings qt_add_shaders (qsb at build time).
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui GuiPrivate ShaderTools)

add_library(ray_render STATIC
    render_module.cpp
    tier.cpp
    tier_detect.cpp
    tier_internal.h
    theme.cpp
    theme_internal.h
    include/ray_render/viewport.h
    include/ray_render/viewport_rhi.h
    viewport_frontend.h
    viewport_frontend.cpp
    viewport_camera.cpp
    viewport_runtime.cpp
    viewport_offscreen.cpp
    viewport_hosted.cpp
    pick.h
    pick.cpp
    backdrop.h
    backdrop.cpp
    highlight.h
    highlight.cpp
    section.h
    section.cpp
    rhi/pick_pass.h
    rhi/pick_pass.cpp
    rhi/backdrop_pass.h
    rhi/backdrop_pass.cpp
    rhi/rhi_util.h
    rhi/rhi_util.cpp
    rhi/rhi_device.h
    rhi/rhi_device.cpp
    rhi/frame_pass.h
    rhi/scene_renderer.h
    rhi/scene_renderer.cpp)
ray_module(ray_render)
# ray_render/viewport.h (no Qt) and ray_render/viewport_rhi.h (the Qt Quick seam for src/ui).
target_include_directories(ray_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# On Windows tier_detect.cpp loads dxgi.dll and d3d11.dll at run time, so no system library is linked here.
target_link_libraries(ray_render PRIVATE ray_core ray_kernel ray_sim Qt6::Core Qt6::Gui Qt6::GuiPrivate)

# Viewport shaders: Vulkan-style GLSL baked by qsb for every RHI backend (PLAN.md 6.3): SPIR-V (Vulkan), GLSL 120
# and 150 (OpenGL 2.1 and 3.2+), HLSL shader model 5.0 (Direct3D 11 and 12) and MSL 1.2 (Metal). They land in the
# resource system under :/ray/render/shaders/. frame.glsl is #included by every stage.
qt_add_shaders(ray_render "ray_render_shaders"
    PREFIX "/ray/render"
    GLSL "120,150"
    HLSL 50
    MSL 12
    FILES
        shaders/background.vert
        shaders/background.frag
        shaders/mesh.vert
        shaders/mesh.frag
        shaders/line.vert
        shaders/line.frag
        shaders/wideline.vert
        shaders/wideline.frag
        shaders/cap.vert
        shaders/cap.frag
        shaders/pick_mesh.vert
        shaders/pick_line.vert
        shaders/pick_wideline.vert
        shaders/pick_cap.vert
        shaders/pick.frag
        shaders/backdrop.vert
        shaders/backdrop_down.frag
        shaders/backdrop_blur7.frag
        shaders/backdrop_blur11.frag
        shaders/backdrop_blur15.frag
        shaders/backdrop_blur23.frag)
# backdrop.glsl is #included by the backdrop_*.frag variants (T-016).
set_source_files_properties(shaders/frame.glsl shaders/backdrop.glsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(ray_render PRIVATE shaders/frame.glsl shaders/backdrop.glsl)
