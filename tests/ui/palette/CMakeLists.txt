# The command palette, keyboard shortcuts and context menus (task T-021): Catch2 driving real Qt Quick windows with
# the real application session (AppHost::startSession, T-019) and a temporary RAYCAD_USER_DATA_DIR per test. The
# context-menu test opens a part in the viewport, so it needs a scene graph with QRhi (it skips with a message where
# there is none).
# The test case tagged [.screenshots] does not run by default: set RAY_UI_SCREENSHOT_DIR and run
#   ray_ui_palette_tests "[screenshots]"
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_palette_tests palette_test.cpp)
# ray_store: one test holds a document's write lock the way another session would.
target_link_libraries(ray_ui_palette_tests PRIVATE ray_ui ray_render ray_api ray_store Qt6::Core Qt6::Gui Qt6::Qml
                                                   Qt6::Quick Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, CommandCatalog, ShortcutManager, PaletteModel, palette::formFields, Files,
# Session).
target_include_directories(ray_ui_palette_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_palette_tests PRIVATE RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_ui_palette_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_palette_tests COMMAND ray_ui_palette_tests)
set_tests_properties(ray_ui_palette_tests PROPERTIES TIMEOUT 600)
ray_test_runtime_path(ray_ui_palette_tests QT)
# As for the viewport tests: frames must not wait for a vertical blank of a display that may be asleep.
get_test_property(ray_ui_palette_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_palette_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
