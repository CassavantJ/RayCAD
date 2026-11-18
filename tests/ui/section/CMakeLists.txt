# Section view and draft analysis in the application (task T-023): Catch2 driving a real Qt Quick window whose
# viewport renders. Needs a scene graph with QRhi (Direct3D 11 on Windows by default); where there is none
# (QT_QUICK_BACKEND=software on Linux CI) every test skips with a message.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_section_tests section_test.cpp)
target_link_libraries(ray_ui_section_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                   Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, SectionModel, CommandCatalog, ViewportItem, SceneController).
target_include_directories(ray_ui_section_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_section_tests
                           PRIVATE RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_ui_section_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_section_tests COMMAND ray_ui_section_tests)
set_tests_properties(ray_ui_section_tests PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_section_tests QT)
get_test_property(ray_ui_section_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
# Frames must not wait for a display's vertical blank (a sleeping or locked display blocks the present).
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_section_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
