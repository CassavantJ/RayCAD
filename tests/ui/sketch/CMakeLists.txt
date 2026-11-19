# The sketch drawing tools (task T-092): Catch2 driving the real Qt Quick window with the real application session.
# The tests draw through the viewport, so they need a scene graph with QRhi; where there is none they skip.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_sketch_tests sketch_test.cpp sketch_tools_test.cpp)
target_link_libraries(ray_ui_sketch_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                  Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, SketchModel, the tool engine, ...).
target_include_directories(ray_ui_sketch_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_sketch_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
set_property(TARGET ray_ui_sketch_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_sketch_tests COMMAND ray_ui_sketch_tests)
set_tests_properties(ray_ui_sketch_tests PROPERTIES TIMEOUT 600)
# Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
set_tests_properties(ray_ui_sketch_tests PROPERTIES SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_sketch_tests QT)
get_test_property(ray_ui_sketch_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_sketch_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
