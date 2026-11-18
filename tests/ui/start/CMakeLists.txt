# The start screen, file handling, jobs and crash recovery (task T-019): Catch2 driving real Qt Quick windows with
# the real application session. Every window gets its own per-user data folder, and RAYCAD_USER_DATA_DIR points at a
# temporary folder too, so the real user profile is never read or written.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick Test)

add_executable(ray_ui_start_tests start_test.cpp)
# ray_store: the lock test holds a document's write lock the way another session would.
target_link_libraries(ray_ui_start_tests PRIVATE ray_ui ray_render ray_api ray_store Qt6::Core Qt6::Gui Qt6::Qml
                                                 Qt6::Quick Qt6::Test Catch2::Catch2)
# The private headers of src/ui (AppHost, Session, Files, SceneController, the recovery interface).
target_include_directories(ray_ui_start_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
target_compile_definitions(ray_ui_start_tests PRIVATE
    RAY_KERNEL_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data"
    RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_RAYCAD_EXE="$<TARGET_FILE:raycad>")
add_dependencies(ray_ui_start_tests raycad)
set_property(TARGET ray_ui_start_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_start_tests COMMAND ray_ui_start_tests)
set_tests_properties(ray_ui_start_tests PROPERTIES TIMEOUT 900)
ray_test_runtime_path(ray_ui_start_tests QT)
# As for the viewport tests: frames must not wait for a display's vertical blank (a sleeping or locked display would
# block Direct3D's present, and meshes only show, and picks are only answered, with frames).
get_test_property(ray_ui_start_tests ENVIRONMENT_MODIFICATION _mods)
if(NOT _mods)
    set(_mods)
endif()
list(APPEND _mods "QSG_NO_VSYNC=set:1")
set_tests_properties(ray_ui_start_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
