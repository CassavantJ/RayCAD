# The feature tools, the feature tree and the parameter table (task T-098): Catch2 driving the real Qt Quick window
# with the real application session. The tests draw through the viewport, so they need a scene graph with QRhi; where
# there is none they skip.
# T-083: the direct-edit tools and the reference review on imported corpus parts (directedit_test.cpp) and their
# non-visual logic (directedit_unit_test.cpp), in the same program; ctest runs them as ray_ui_directedit_tests (they
# import parts and rebuild them, minutes beside the T-098 suite), the [wallclock] cases of both in
# ray_ui_model_tests_wallclock (tests/tiers.cmake).
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_model_tests model_test.cpp directedit_test.cpp directedit_unit_test.cpp
    replace_import_test.cpp) # T-084
target_link_libraries(ray_ui_model_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                 Qt6::Test Catch2::Catch2)
# T-083: the kernel's S9 cases of a corpus part find the slow fillet deletion (src/kernel/directedit_testing.h).
target_link_libraries(ray_ui_model_tests PRIVATE ray_kernel)
# The private headers of src/ui (AppHost, FeatureToolModel, FeatureTreeModel, ...); T-083: the CAM engine tests'
# test-only operation type (tests/unit/cam/engine_test_util.h) and the kernel's direct-edit test cases.
target_include_directories(ray_ui_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui" "${CMAKE_SOURCE_DIR}/tests/unit/cam"
                                                      "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_ui_model_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
                                                      RAY_CORPUS_ROOT_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
set_property(TARGET ray_ui_model_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_model_tests COMMAND ray_ui_model_tests "~[wallclock]~[directedit]~[replaceimport]")
add_test(NAME ray_ui_directedit_tests COMMAND ray_ui_model_tests "[directedit]~[wallclock]~[scale]")
# T-084: the replace dialog and its summary (replace_import_test.cpp).
add_test(NAME ray_ui_replace_import_tests COMMAND ray_ui_model_tests "[replaceimport]~[wallclock]")
foreach(_test IN ITEMS ray_ui_model_tests ray_ui_directedit_tests ray_ui_replace_import_tests)
    set_tests_properties(${_test} PROPERTIES TIMEOUT 900)
    # Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
    set_tests_properties(${_test} PROPERTIES SKIP_RETURN_CODE 4)
    ray_test_runtime_path(${_test} QT)
    get_test_property(${_test} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    list(APPEND _mods "QSG_NO_VSYNC=set:1")
    set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()
# T-083: the reference review panel's layout (a dozen rows, the title and the footer in view) from the shell's minimum
# window size up, at 100 %, 150 % and 200 % (the monitor's own scaling off, so the device pixel ratio is the factor).
foreach(_scale 1 1.5 2)
    string(REPLACE "." "_" _name "${_scale}")
    add_test(NAME ray_ui_directedit_scale_${_name} COMMAND ray_ui_model_tests "[directedit][scale]")
    set_tests_properties(ray_ui_directedit_scale_${_name} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 4)
    ray_test_runtime_path(ray_ui_directedit_scale_${_name} QT)
    get_test_property(ray_ui_directedit_scale_${_name} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    list(APPEND _mods "QSG_NO_VSYNC=set:1" "QT_ENABLE_HIGHDPI_SCALING=set:0" "QT_SCALE_FACTOR=set:${_scale}"
         "RAY_TEST_SCALE=set:${_scale}")
    set_tests_properties(ray_ui_directedit_scale_${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()
