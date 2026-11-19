# The feature tools, the feature tree and the parameter table (task T-098): Catch2 driving the real Qt Quick window
# with the real application session. The tests draw through the viewport, so they need a scene graph with QRhi; where
# there is none they skip.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_model_tests model_test.cpp)
target_link_libraries(ray_ui_model_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                 Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, FeatureToolModel, FeatureTreeModel, ...).
target_include_directories(ray_ui_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
set_property(TARGET ray_ui_model_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_model_tests COMMAND ray_ui_model_tests)
set_tests_properties(ray_ui_model_tests PROPERTIES TIMEOUT 900)
# Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
set_tests_properties(ray_ui_model_tests PROPERTIES SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_model_tests QT)
get_test_property(ray_ui_model_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_model_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
