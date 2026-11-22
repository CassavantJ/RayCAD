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
    body_color_test.cpp
    section_test.cpp
    backplot_test.cpp)
# Test meshes come from ray::kernel, including the builders in src/kernel/shape_testing.h; backplots from ray::sim
# (task T-038: hand-made toolpaths through sim::buildBackplot).
target_link_libraries(ray_render_tests PRIVATE ray_render ray_kernel ray_sim Catch2::Catch2WithMain)
target_include_directories(ray_render_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/render" "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_render_tests PRIVATE
    RAY_THEMES_DIR="${CMAKE_SOURCE_DIR}/resources/themes"
    RAY_THEME_SCHEMA="${CMAKE_SOURCE_DIR}/schemas/theme.schema.json"
    RAY_RENDER_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference")
set_property(TARGET ray_render_tests PROPERTY FOLDER "tests")
# The three measurements on a 500k-triangle mesh and the blur at 1920x1080 (frame statistics, pick latency, the cost
# of section caps and of the blur; about three quarters of the suite's time, mostly in the software rasteriser) run
# as tests of their own, so they run beside the rest in a parallel ctest. The 2 M-segment backplot against the
# 500k-triangle baseline (T-038) asserts a frame-time ratio, so it runs alone (RAY_SERIAL_TESTS in tests/tiers.cmake).
add_test(NAME ray_render_tests COMMAND ray_render_tests "~[stats]~[latency]~[cost]")
add_test(NAME ray_render_frame_stats COMMAND ray_render_tests "[stats]")
add_test(NAME ray_render_pick_latency COMMAND ray_render_tests "[latency]")
add_test(NAME ray_render_cost COMMAND ray_render_tests "[cost]~[backplot]")
add_test(NAME ray_render_backplot_cost COMMAND ray_render_tests "[backplot][cost]")
foreach(_t ray_render_tests ray_render_frame_stats ray_render_pick_latency ray_render_cost ray_render_backplot_cost)
    # About 180 s in a debug build on a loaded machine; the presets' default limit is 300 s. Where no graphics API
    # can be created every viewport test skips, and Catch2 exits with 4 when all of a run's test cases did (the
    # three measurement tests on their own): CTest reports that as skipped, as for the Qt Quick tests.
    set_tests_properties(${_t} PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
    # ray_render links Qt (Core and Gui), so the test needs the Qt DLL folder on Windows, and the vcpkg folder for the
    # delay-loaded OCCT toolkits of ray_kernel.
    ray_test_runtime_path(${_t} QT)
    # Under ThreadSanitizer, races inside Mesa's uninstrumented llvmpipe are suppressed (see tsan.supp).
    if(RAY_SANITIZE MATCHES "thread")
        set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
            "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
    endif()
endforeach()
