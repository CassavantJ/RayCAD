# The viewport item, navigation, selection and model tree (task T-020; files open through the session, T-019):
# Catch2 driving real Qt Quick windows. The viewport needs a scene graph with QRhi (Direct3D 11 on Windows by default); where there is
# none (QT_QUICK_BACKEND=software on Linux CI) the tests that look at pixels or pick skip with a message.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_viewport_tests viewport_test.cpp)
target_link_libraries(ray_ui_viewport_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                    Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, SceneController, SelectionModel, ViewportItem, Files, Session).
target_include_directories(ray_ui_viewport_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_viewport_tests PRIVATE
    RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data"
    RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
set_property(TARGET ray_ui_viewport_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_viewport_tests COMMAND ray_ui_viewport_tests "~[wallclock]")
set_tests_properties(ray_ui_viewport_tests PROPERTIES TIMEOUT 600)
# Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
set_tests_properties(ray_ui_viewport_tests PROPERTIES SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_viewport_tests QT)
# Frames must not wait for a display's vertical blank: on a machine whose display is asleep or locked, Direct3D's
# present blocks and the window draws only when grabbed, so picks (which need a frame) would never be answered.
get_test_property(ray_ui_viewport_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_viewport_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
