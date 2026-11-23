# The CAM workspace and its setups (task T-044).
#   ray_ui_cam_tests       Catch2 driving the real Qt Quick window with the real application session: the four
#                          interactions on an imported part, the live preview, editing, flips, undo and redo, the status
#                          badges against the engine, switching workspaces. The tests draw through the viewport, so they
#                          need a scene graph with QRhi; where there is none they skip.
#   ray_ui_cam_unit_tests  the non-visual part (src/ui/cam/setup_logic.h): no window.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_cam_tests cam_test.cpp)
target_link_libraries(ray_ui_cam_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                               Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, SetupToolModel, SetupTreeModel, ...) and the CAM engine tests' test-only
# operation type (tests/unit/cam/engine_test_util.h).
target_include_directories(ray_ui_cam_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui" "${CMAKE_SOURCE_DIR}/tests/unit/cam")
target_compile_definitions(ray_ui_cam_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
set_property(TARGET ray_ui_cam_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_cam_tests COMMAND ray_ui_cam_tests "~[wallclock]")
set_tests_properties(ray_ui_cam_tests PROPERTIES TIMEOUT 900)
# Catch2 exits with 4 when every test case skipped (no QRhi scene graph): CTest then reports the test as skipped.
set_tests_properties(ray_ui_cam_tests PROPERTIES SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_tests QT)
get_test_property(ray_ui_cam_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")

# The operation panel (task T-045): Catch2 driving the real window as above. ray_ui_cam_operation_tests runs every case
# but the [scale] and [wallclock] ones; ray_ui_cam_operation_scale_<s> runs the [scale] case at 100 %, 150 % and 200 %
# (QT_SCALE_FACTOR with the platform's own scaling off, as the controls gallery does); the [wallclock] split is made by
# tests/tiers.cmake.
add_executable(ray_ui_cam_operation_tests operation_test.cpp)
target_link_libraries(ray_ui_cam_operation_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml
                                                         Qt6::Quick Qt6::Test Catch2::Catch2)
target_include_directories(ray_ui_cam_operation_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui"
                                                              "${CMAKE_SOURCE_DIR}/tests/unit/cam")
target_compile_definitions(ray_ui_cam_operation_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_SHOTS_DIR="${CMAKE_SOURCE_DIR}/build/shots") # T-061's screenshots
set_property(TARGET ray_ui_cam_operation_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_operation_tests COMMAND ray_ui_cam_operation_tests "~[wallclock]~[scale]")
set_tests_properties(ray_ui_cam_operation_tests PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_operation_tests QT)
get_test_property(ray_ui_cam_operation_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_operation_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
foreach(_scale 1 1.5 2)
    string(REPLACE "." "_" _name "${_scale}")
    add_test(NAME ray_ui_cam_operation_scale_${_name} COMMAND ray_ui_cam_operation_tests "[scale]")
    set_tests_properties(ray_ui_cam_operation_scale_${_name} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 4)
    ray_test_runtime_path(ray_ui_cam_operation_scale_${_name} QT)
    get_test_property(ray_ui_cam_operation_scale_${_name} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    list(APPEND _mods "QSG_NO_VSYNC=set:1" "QT_ENABLE_HIGHDPI_SCALING=set:0" "QT_SCALE_FACTOR=set:${_scale}"
         "RAY_TEST_SCALE=set:${_scale}")
    set_tests_properties(ray_ui_cam_operation_scale_${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()

# Posting, the G-code viewer and file output (task T-047): Catch2 driving the real window as above. The [wallclock] case
# (a 200,000-line program) is split out by tests/tiers.cmake.
add_executable(ray_ui_cam_post_tests post_test.cpp)
target_link_libraries(ray_ui_cam_post_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                    Qt6::Test Catch2::Catch2)
target_include_directories(ray_ui_cam_post_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui" "${CMAKE_SOURCE_DIR}/tests/unit/cam")
target_compile_definitions(ray_ui_cam_post_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_SHIPPED_POSTS_DIR="${CMAKE_SOURCE_DIR}/resources/library/posts")
set_property(TARGET ray_ui_cam_post_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_post_tests COMMAND ray_ui_cam_post_tests "~[wallclock]")
set_tests_properties(ray_ui_cam_post_tests PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_post_tests QT)
get_test_property(ray_ui_cam_post_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_post_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")

# The whole of M2 in one window, as a user does it (import, setup, drill, post, viewer, edit, post again, save and
# reopen): Catch2 driving the real window as above. Its screenshots go to build/shots/m2_flow_*.png.
add_executable(ray_ui_cam_flow_tests flow_test.cpp)
target_link_libraries(ray_ui_cam_flow_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
                                                    Qt6::Test Catch2::Catch2)
# The M3 flow reads its posted programs back with the post tests' independent interpreter (T-043, test code only).
target_link_libraries(ray_ui_cam_flow_tests PRIVATE ray_roundtrip_compare)
target_include_directories(ray_ui_cam_flow_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui"
                                                         "${CMAKE_SOURCE_DIR}/tests/unit/post/roundtrip")
target_compile_definitions(ray_ui_cam_flow_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
                                                         RAY_SHOTS_DIR="${CMAKE_SOURCE_DIR}/build/shots")
set_property(TARGET ray_ui_cam_flow_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_flow_tests COMMAND ray_ui_cam_flow_tests "~[scale]")
set_tests_properties(ray_ui_cam_flow_tests PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_flow_tests QT)
get_test_property(ray_ui_cam_flow_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_flow_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
# M3 in one window (face, pocket, contour, drill, chamfer, two posts read back, save and reopen): its hidden [m3flow]
# case, on its own. Screenshots: build/shots/m3_flow_*.png.
add_test(NAME ray_ui_cam_flow_m3 COMMAND ray_ui_cam_flow_tests "[m3flow]")
set_tests_properties(ray_ui_cam_flow_m3 PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_flow_m3 QT)
get_test_property(ray_ui_cam_flow_m3 ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_flow_m3 PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
# Its [scale] case (no list or check box text cut where it has room, in the setup panel, the operation panel, the post
# dialog and the tool library) at 100 %, 150 % and 200 %, as the operation panel's.
foreach(_scale 1 1.5 2)
    string(REPLACE "." "_" _name "${_scale}")
    add_test(NAME ray_ui_cam_flow_scale_${_name} COMMAND ray_ui_cam_flow_tests "[scale]")
    set_tests_properties(ray_ui_cam_flow_scale_${_name} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 4)
    ray_test_runtime_path(ray_ui_cam_flow_scale_${_name} QT)
    get_test_property(ray_ui_cam_flow_scale_${_name} ENVIRONMENT_MODIFICATION _mods)
    if(NOT _mods)
        set(_mods)
    endif()
    list(APPEND _mods "QSG_NO_VSYNC=set:1" "QT_ENABLE_HIGHDPI_SCALING=set:0" "QT_SCALE_FACTOR=set:${_scale}"
         "RAY_TEST_SCALE=set:${_scale}")
    set_tests_properties(ray_ui_cam_flow_scale_${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
endforeach()

add_executable(ray_ui_cam_unit_tests cam_unit_test.cpp
    operation_unit_test.cpp # T-045: the schema form over every registered type and synthetic schemas, the panel logic
    post_unit_test.cpp # T-047: the viewer's tokens and index, post properties as a form, the gate through cam.post
    cycletime_unit_test.cpp) # T-059: the cycle-time estimate's text, and a setup's input from the engine's statuses
target_link_libraries(ray_ui_cam_unit_tests PRIVATE ray_ui ray_api Qt6::Core Catch2::Catch2WithMain)
target_include_directories(ray_ui_cam_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_include_directories(ray_ui_cam_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/unit/cam") # T-047: test.op
target_compile_definitions(ray_ui_cam_unit_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
                                                         RAY_SHIPPED_POSTS_DIR="${CMAKE_SOURCE_DIR}/resources/library/posts")
set_property(TARGET ray_ui_cam_unit_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_unit_tests COMMAND ray_ui_cam_unit_tests)
ray_test_runtime_path(ray_ui_cam_unit_tests QT)

# Operation templates (task T-057): the template panel and the CAM tree's template buttons in the real window, as the
# flow tests do. Its screenshots go to build/shots/t057_*.png.
add_executable(ray_ui_cam_templates_tests templates_test.cpp)
target_link_libraries(ray_ui_cam_templates_tests PRIVATE ray_ui ray_render ray_api Qt6::Core Qt6::Gui Qt6::Qml
                                                         Qt6::Quick Qt6::Test Catch2::Catch2)
target_include_directories(ray_ui_cam_templates_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_cam_templates_tests PRIVATE RAY_SHOTS_DIR="${CMAKE_SOURCE_DIR}/build/shots")
set_property(TARGET ray_ui_cam_templates_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_cam_templates_tests COMMAND ray_ui_cam_templates_tests)
set_tests_properties(ray_ui_cam_templates_tests PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 4)
ray_test_runtime_path(ray_ui_cam_templates_tests QT)
get_test_property(ray_ui_cam_templates_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_cam_templates_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
