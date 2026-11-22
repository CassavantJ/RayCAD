# The tool library manager (task T-046).
#   ray_ui_cam_tools_tests       Catch2 driving the real Qt Quick window with the real application session and its
#                                default library stack: browsing (and, [wallclock], 5,000 tools), filters, editing,
#                                the side view, conflicts on a shared folder, the document's tools, the keyboard.
#   ray_ui_cam_tools_unit_tests  the non-visual part (src/ui/cam/tools_logic.h): no window.
# The window tests put their windows on RAY_TEST_SCREEN and raise the graphics driver's threads, as the other window
# tests do (tests/CMakeLists.txt); both are set up here so that list there stays as it is.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_cam_tools_tests tools_test.cpp
    "${CMAKE_SOURCE_DIR}/tests/support/driver_threads_listener.cpp"
    "${CMAKE_SOURCE_DIR}/tests/ui/screen/test_screen_listener.cpp")
target_link_libraries(ray_ui_cam_tools_tests PRIVATE ray_ui ray_render ray_api ray_ui_test_screen Qt6::Core Qt6::Gui
                                                     Qt6::Qml Qt6::Quick Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, ToolLibraryModel, ...) and the CAM engine tests' test-only operation type and
# library JSON (tests/unit/cam/engine_test_util.h).
target_include_directories(ray_ui_cam_tools_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui" "${CMAKE_SOURCE_DIR}/tests/unit/cam")
target_compile_definitions(ray_ui_cam_tools_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
set_property(TARGET ray_ui_cam_tools_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_cam_tools_tests COMMAND ray_ui_cam_tools_tests "~[wallclock]")
set_tests_properties(ray_ui_cam_tools_tests PROPERTIES TIMEOUT 900)
# Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
set_tests_properties(ray_ui_cam_tools_tests PROPERTIES SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_tools_tests QT)
get_test_property(ray_ui_cam_tools_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_tools_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")

add_executable(ray_ui_cam_tools_unit_tests tools_unit_test.cpp)
target_link_libraries(ray_ui_cam_tools_unit_tests PRIVATE ray_ui ray_api Qt6::Core Catch2::Catch2WithMain)
target_include_directories(ray_ui_cam_tools_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_cam_tools_unit_tests PRIVATE RAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_property(TARGET ray_ui_cam_tools_unit_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_tools_unit_tests COMMAND ray_ui_cam_tools_unit_tests)
ray_test_runtime_path(ray_ui_cam_tools_unit_tests QT)
