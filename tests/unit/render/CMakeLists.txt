# Hardware tiers and themes (task T-013). tier_internal.h and theme_internal.h are private to src/render; they
# include no Qt header, so these tests do not either.
add_executable(ray_render_tests tier_test.cpp theme_test.cpp)
target_link_libraries(ray_render_tests PRIVATE ray_render Catch2::Catch2WithMain)
target_include_directories(ray_render_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/render")
target_compile_definitions(ray_render_tests PRIVATE
    RAY_THEMES_DIR="${CMAKE_SOURCE_DIR}/resources/themes"
    RAY_THEME_SCHEMA="${CMAKE_SOURCE_DIR}/schemas/theme.schema.json")
ray_add_test(ray_render_tests)
# ray_render links Qt (Core and Gui), so the test needs the Qt DLL folder on Windows.
ray_test_runtime_path(ray_render_tests QT)
