# Hardware tiers and themes (task T-013) and the viewport (task T-014). tier_internal.h, theme_internal.h and
# ray_render/viewport.h include no Qt header, so these tests do not either. The viewport tests render offscreen
# with their own QRhi and SKIP (not fail) where no graphics API can be created.
add_executable(ray_render_tests
    tier_test.cpp
    theme_test.cpp
    viewport_testing.h
    viewport_scenes.h
    viewport_test.cpp
    viewport_reference_test.cpp
    pick_test.cpp
    backdrop_test.cpp
    body_color_test.cpp)
# Test meshes come from ray::kernel, including the builders in src/kernel/shape_testing.h.
target_link_libraries(ray_render_tests PRIVATE ray_render ray_kernel Catch2::Catch2WithMain)
target_include_directories(ray_render_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/render" "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_render_tests PRIVATE
    RAY_THEMES_DIR="${CMAKE_SOURCE_DIR}/resources/themes"
    RAY_THEME_SCHEMA="${CMAKE_SOURCE_DIR}/schemas/theme.schema.json"
    RAY_RENDER_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference")
ray_add_test(ray_render_tests)
# About 180 s in a debug build on a loaded machine; the presets' default limit is 300 s.
set_tests_properties(ray_render_tests PROPERTIES TIMEOUT 900)
# ray_render links Qt (Core and Gui), so the test needs the Qt DLL folder on Windows, and the vcpkg folder for the
# delay-loaded OCCT toolkits of ray_kernel.
ray_test_runtime_path(ray_render_tests QT)
# Under ThreadSanitizer, races inside Mesa's uninstrumented llvmpipe are suppressed (see tsan.supp).
if(RAY_SANITIZE MATCHES "thread")
    set_property(TEST ray_render_tests APPEND PROPERTY ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
endif()
