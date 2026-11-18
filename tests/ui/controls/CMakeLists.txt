# The shell, the Theme binding and the base controls (task T-018): Catch2 driving real Qt Quick windows.
# The windows are shown briefly; on Linux CI run with QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software (nothing
# here needs a GPU). The gallery test runs three times, at 100 %, 150 % and 200 % (QT_SCALE_FACTOR with the
# platform's own scaling off, so the device pixel ratio is exactly 1, 1.5 and 2).
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_controls_tests controls_test.cpp)
target_link_libraries(ray_ui_controls_tests PRIVATE ray_ui ray_render Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test
                                                    Catch2::Catch2)
# The private headers of src/ui (AppHost, ThemeObject, AppContext).
target_include_directories(ray_ui_controls_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_controls_tests PRIVATE RAY_THEMES_DIR="${CMAKE_SOURCE_DIR}/resources/themes")
set_property(TARGET ray_ui_controls_tests PROPERTY FOLDER "tests")

# Everything except the gallery scale test, at the screen's own scale.
add_test(NAME ray_ui_controls_tests COMMAND ray_ui_controls_tests "~[scale]")
ray_test_runtime_path(ray_ui_controls_tests QT)

foreach(_scale 1 1.5 2)
    string(REPLACE "." "_" _name "${_scale}")
    add_test(NAME ray_ui_gallery_scale_${_name} COMMAND ray_ui_controls_tests "[scale]")
    ray_test_runtime_path(ray_ui_gallery_scale_${_name} QT)
    # ray_test_runtime_path sets ENVIRONMENT_MODIFICATION; the scale goes through it too.
    get_test_property(ray_ui_gallery_scale_${_name} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    # The monitor's own scaling is switched off so the device pixel ratio is exactly the factor under test.
    list(APPEND _mods "QT_ENABLE_HIGHDPI_SCALING=set:0" "QT_SCALE_FACTOR=set:${_scale}" "RAY_TEST_SCALE=set:${_scale}")
    set_tests_properties(ray_ui_gallery_scale_${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()
