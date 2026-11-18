# Settings, units and tier override (task T-024): Catch2 driving real Qt Quick windows with the real application
# session. Every window loads its settings from a temporary user data folder (and RAYCAD_USER_DATA_DIR points at a
# temporary folder too), so the real user profile is never read or written. Tests that need the viewport to draw skip
# where the scene graph has no QRhi.
# Screenshots of every settings section in both themes: set RAY_T024_SHOTS to a folder and run
#   ray_ui_settings_tests "[screenshots]"
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_settings_tests settings_test.cpp)
target_link_libraries(ray_ui_settings_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                    Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, SettingsObject, UnitsObject, Session, SceneController, ViewportItem).
target_include_directories(ray_ui_settings_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_settings_tests PRIVATE
    RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data"
    RAY_RAYCAD_EXE="$<TARGET_FILE:raycad>")
add_dependencies(ray_ui_settings_tests raycad)
set_property(TARGET ray_ui_settings_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_settings_tests COMMAND ray_ui_settings_tests)
set_tests_properties(ray_ui_settings_tests PROPERTIES TIMEOUT 600)
ray_test_runtime_path(ray_ui_settings_tests QT)
# As for the viewport tests: frames must not wait for a vertical blank of a display that may be asleep.
get_test_property(ray_ui_settings_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_settings_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
