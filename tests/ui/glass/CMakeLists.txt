# Glass panels over the viewport (task T-016): Catch2 driving real Qt Quick windows whose viewport renders and blurs.
# Needs a scene graph with QRhi (Direct3D 11 on Windows by default); where there is none (QT_QUICK_BACKEND=software on
# Linux CI) every test skips with a message. Runs at the screen's own scale and at 150 % (QT_SCALE_FACTOR with the
# platform's scaling off, so the device pixel ratio is exactly 1.5: panels must sample the blur at fractional
# pixel positions).
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_glass_tests glass_test.cpp)
target_link_libraries(ray_ui_glass_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                 Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, AppContext, ViewportItem and GlassBackdrop, SceneController).
target_include_directories(ray_ui_glass_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_glass_tests PRIVATE RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_ui_glass_tests PROPERTY FOLDER "tests")

foreach(_scale native 1.5)
    string(REPLACE "." "_" _name "${_scale}")
    set(_test ray_ui_glass_tests_${_name})
    add_test(NAME ${_test} COMMAND ray_ui_glass_tests)
    set_tests_properties(${_test} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 4)
    ray_test_runtime_path(${_test} QT)
    get_test_property(${_test} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    # Frames must not wait for a display's vertical blank (a sleeping or locked display blocks the present).
    list(APPEND _mods "QSG_NO_VSYNC=set:1")
    if(NOT _scale STREQUAL "native")
        list(APPEND _mods "QT_ENABLE_HIGHDPI_SCALING=set:0" "QT_SCALE_FACTOR=set:${_scale}")
    endif()
    set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()
