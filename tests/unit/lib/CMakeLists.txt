# lib unit tests (tasks T-030, T-031): library folders, the file format and schemas, edits by other processes,
# layered roots and queries, the kill-during-save test and the shipped library in resources/library.
# Hidden tag, run by hand: "[.kill500]" (500 kills during save).

# The helper process the tests spawn and kill (lib_helper.cpp).
add_executable(ray_lib_helper lib_helper.cpp lib_test_util.cpp)
target_link_libraries(ray_lib_helper PRIVATE ray_lib ray_core)
target_include_directories(ray_lib_helper PRIVATE "${CMAKE_SOURCE_DIR}/src/lib")
target_compile_definitions(ray_lib_helper PRIVATE RAY_LIB_HELPER="$<TARGET_FILE:ray_lib_helper>")
set_property(TARGET ray_lib_helper PROPERTY FOLDER "tests")

add_executable(ray_lib_tests
    codec_test.cpp
    kill_test.cpp
    lib_test_util.cpp
    library_test.cpp
    shipped_test.cpp
    stack_test.cpp
)
target_link_libraries(ray_lib_tests PRIVATE ray_lib ray_core Catch2::Catch2WithMain)
# codec.h, schema.h, json.h, tool_query.h, fileio.h and library_internal.h are private to src/lib; test_dispatcher.h is
# the UI-thread double of the job tests.
target_include_directories(ray_lib_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/lib" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_lib_tests PRIVATE RAY_LIB_HELPER="$<TARGET_FILE:ray_lib_helper>"
    RAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(ray_lib_tests ray_lib_helper)
set_property(TARGET ray_lib_tests PROPERTY FOLDER "tests")
# Two processes, as the store tests: the kill test (mostly waiting on child processes) beside the rest.
add_test(NAME ray_lib_tests COMMAND ray_lib_tests "~[kill]")
add_test(NAME ray_lib_kill COMMAND ray_lib_tests "[kill]")
foreach(_t ray_lib_tests ray_lib_kill)
    ray_test_runtime_path(${_t})
endforeach()

# The shipped library (resources/library) is exactly what tools/gen_library.py writes, ids included (task T-031).
add_test(NAME lib_starter_library_generated
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/gen_library.py" --check)
